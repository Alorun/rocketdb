#include "version_set.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "dbformat.h"
#include "../wal/log_reader.h"
#include "../wal/log_writer.h"
#include "memtable.h"
#include "../../include/env.h"
#include "../../include/table_builder.h"
#include "../table/merger.h"
#include "../table/two_level_iterator.h"
#include "../util/coding.h"
#include "../util/logging.h"
#include "../../include/options.h"
#include "version_edit.h"
#include "table_cache.h"
#include "filename.h"
#include "../util/logging.h"


namespace rocketdb {

static size_t TargetFileSize(const Options* options) {
    return options->max_file_size;
}

static int64_t MaxGrandParentOverlapBytes(const Options* options) {
    return 10 * TargetFileSize(options);
}

static int64_t ExpandedCompactionByteSizeLimit(const Options* options) {
    return 25 * TargetFileSize(options);
}

static double MaxBytesForLevel(const Options* options, int level) {
    double result = 10. * 1048576.0;
    while (level > 1) {
        result *= 10;
        level--;
    }
    return result;
}

static uint64_t MaxFileSizeForLevel(const Options* options, int level) {
    return TargetFileSize(options);
}

static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
    int sum = 0;
    for (size_t i = 0; i < files.size(); i++) {
        sum += files[i]->file_size;
    }
    return sum;
}

Version::~Version() {
    assert(refs_ == 0);

    prev_->next_ = next_;
    next_->prev_ = prev_;

    for (int level = 0; level < config::kNumLevels; level++) {
        for (size_t i = 0; i < files_[level].size(); i++) {
            FileMetaData* f = files_[level][i];
            assert(f->refs > 0);
            if (f->refs <= 0) {
                delete f;
            }
        }
    }
}

// Binary search for the file containing the key
int FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files, const Slice& key) {
    uint32_t left = 0;
    uint32_t right = files.size();
    while (left < right) {
        uint32_t mid = (left + right) / 2;
        const FileMetaData* f = files[mid];
        // :: For skip the virtual funciton table for the compare query
        if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return right;
}

// If user_key is after the file return true, else return false
static bool AfterFile(const Comparator* ucmp, const Slice* user_key, const FileMetaData* f) {
    return (user_key != nullptr && ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

// If user_key is before the file return true. else return false
static bool BeforeFile(const Comparator* ucmp, const Slice* user_key, const FileMetaData* f) {
    return (user_key != nullptr && ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

// If the scope and file overlap return true, else return false
bool SomeFileOverlapsRange(const InternalKeyComparator &icmp, bool disjoint_sorted_file, std::vector<FileMetaData *> &files,
                           const Slice *smallest_user_key, const Slice *largest_user_key) {
    const Comparator* ucmp = icmp.user_comparator();
    if (!disjoint_sorted_file) {
        for (size_t i = 0; i < files.size(); i++) {
            const FileMetaData* f = files[i];
            if (AfterFile(ucmp, smallest_user_key, f) || BeforeFile(ucmp, largest_user_key, f)) {
                // No overlap
            } else {
                // Overlap
                return true;
            }
        }
        return false;
    }
    
    uint32_t index = 0;
    if (smallest_user_key != nullptr) {
        InternalKey small_key(*smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        index = FindFile(icmp, files, small_key.Encode());
    }

    if (index >= files.size()) {
        return false;
    }

    return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// In which file is the search key located
class Version::LevelFileNumIterator : public Iterator {
    public:
        LevelFileNumIterator(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>* flist)
            : icmp_(icmp), flist_(flist), index_(flist->size()) {}

        bool Valid() const override { return index_ < flist_->size(); }

        void Seek(const Slice& target) override {
            index_ = FindFile(icmp_, *flist_, target);
        }
        void SeekToFirst() override { index_ = 0; }
        void SeekToLast() override { 
            index_ = flist_->empty() ? 0 : flist_->size() - 1;
        }
        void Next() override {
            assert(Valid());
            index_++;
        }
        void Prev() override {
            assert(Valid());
            if (index_ == 0) {
                index_ = flist_->size();  // Marks as invalid
            } else {
                index_--;
            }
        }
        Slice key() const override {
            assert(Valid());
            return (*flist_)[index_]->largest.Encode();
        }
        Slice value() const override {
            assert(Valid());
            EncodeFixed64(value_buf_, (*flist_)[index_]->number);
            EncodeFixed64(value_buf_ + 8, (*flist_)[index_]->file_size);
            return Slice(value_buf_, sizeof(value_buf_));
        }
        Status status() const override { return Status::OK(); }

    private:
        const   InternalKeyComparator icmp_;
        const std::vector<FileMetaData*>* const flist_;
        uint32_t index_;

        mutable char value_buf_[16];
};

// Real to opening files and iterating files to search
static Iterator* GetFileIterator(void* arg, const ReadOptions& options, const Slice& file_value) {
    TableCache* cache = reinterpret_cast<TableCache*>(arg);
    if (file_value.size() != 16) {
        return NewErrorIterator(Status::Corruption("FileReader invoked with unexpected value"));
    } else {
        return cache->NewIterator(options, DecodeFixed64(file_value.data()),
                                  DecodeFixed64(file_value.data() + 8));
    }
}

// It contain two iterator
// One for searching files
// One for opening and iterating throught files to search
Iterator* Version::NewConcatenatingIterator(const ReadOptions& options, int level) const {
    return NewTwoLevelIterator(
        new LevelFileNumIterator(vset_->icmp_, &files_[level]),
                    &GetFileIterator, vset_->table_cache_, options);
}

void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) {
    for (size_t i = 0; i < files_[0].size(); i++) {
        iters->push_back(vset_->table_cache_->NewIterator(options, files_[0][i]->number, files_[0][i]->file_size));
    }

    for (int level = 1; level < config::kNumLevels; level++) {
        if (!files_[level].empty()) {
            iters->push_back(NewConcatenatingIterator(options, level));
        }
    }
}

namespace  {

enum SaverState {
    kNotFound,
    kFound,
    kDeleted,
    kCorrupt,
};
// Responsible for storing the read data
struct Saver {
    SaverState state;
    const Comparator* ucmp;
    Slice user_key;
    std::string* value;
};
}

static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
    Saver* s = reinterpret_cast<Saver*>(arg);
    ParsedInternalKey parsed_key;
    if (!ParseInternalKey(ikey, &parsed_key)) {
        s->state = kCorrupt;
    } else {
        if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
            s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
            if (s->state == kFound) {
                s->value->assign(v.data(), v.size());
            }
        }
    }
}

static bool NewestFirst(FileMetaData* a, FileMetaData* b) {
    return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg, bool (*func)(void*, int, FileMetaData*)) {
    const Comparator* ucmp = vset_->icmp_.user_comparator();

    // Search 0 level in order from newest to oldest
    std::vector<FileMetaData*> tmp;
    tmp.reserve(files_[0].size());
    for (uint32_t i = 0; i < files_[0].size(); i++) {
        FileMetaData* f = files_[0][i];
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 && ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
            tmp.push_back(f);
        }
    }
    if (!tmp.empty()) {
        // Make sure can read th latest data
        std::sort(tmp.begin(), tmp.end(), NewestFirst);
        for (uint32_t i = 0; i < tmp.size(); i++) {
            if (!(*func)(arg, 0, tmp[i])) {
                return;
            }
        }
    }

    // Search other levels
    for (int level = 1; level < config::kNumLevels; level++) {
        size_t num_files = files_[level].size();
        if (num_files == 0) continue;

        uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
        if (index < num_files) {
            FileMetaData* f = files_[level][index];
            if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
                // All of "f" is past any data for user_key
            } else {
                if (!(*func)(arg, level, f)) {
                    return;
                }
            }
        }
    }
}

Status Version::Get(const ReadOptions& options, const LookupKey& k, std::string* value, GetStats* stats) {
    stats->seek_file = nullptr;
    stats->seek_file_level = -1;

    struct State {
        Saver saver;

        // Input
        const ReadOptions* options;
        Slice ikey;
        VersionSet* vset;

        // Optimization
        GetStats* stats;
        FileMetaData* last_file_read;
        int last_file_read_level;

        // Output
        Status s;
        bool found;

        static bool Match(void* arg, int level, FileMetaData* f) {
            State* state = reinterpret_cast<State*>(arg);

            // Last file is invild found, record in stats
            if (state->stats->seek_file == nullptr && state->last_file_read != nullptr) {
                state->stats->seek_file = state->last_file_read;
                state->stats->seek_file_level = state->last_file_read_level;
            }

            state->last_file_read = f;
            state->last_file_read_level = level;

            state->s = state->vset->table_cache_->Get(*state->options, f->number, f->file_size, state->ikey,
                                                      &state->saver, SaveValue);
            if (!state->s.ok()) {
                state->found = true;
                return false;
            }
            switch (state->saver.state) {
                case kNotFound:
                    return true;
                case kFound:
                    state->found = true;
                case kDeleted:
                    return false;
                case kCorrupt:
                    state->s = Status::Corruption("corrupted key for ", state->saver.user_key);
                    state->found = true;
                    return false;
            }

            // No reached
            return false;
        }
    };

    State state;
    state.found =false;
    state.stats = stats;
    state.last_file_read = nullptr;
    state.last_file_read_level = -1;

    state.options = &options;
    state.ikey = k.internal_key();
    state.vset = vset_;

    state.saver.state = kNotFound;
    state.saver.ucmp = vset_->icmp_.user_comparator();
    state.saver.user_key = k.user_key();
    state.saver.value = value;

    ForEachOverlapping(state.saver.user_key, state.ikey, &state, &State::Match);

    return state.found ? state.s : Status::NotFound(Slice());
}

// Optimize invalid file query
bool Version::UpdateStats(const GetStats& stats) {
    FileMetaData* f = stats.seek_file;
    if (f != nullptr) {
        f->allowed_seeks--;
        // File invalid queries exhausted, added to compression queue
        if (f->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
            file_to_compact_ = f;
            file_to_compact_level = stats.seek_file_level;
            return true;
        }
    }
    return false;
}

bool Version::RecordReadSample(Slice internal_key) {
    ParsedInternalKey ikey;
    if (!ParseInternalKey(internal_key, &ikey)) {
        return false;
    }

    struct State {
        GetStats stats;
        int matches;

        static bool Match(void* arg, int level, FileMetaData* f) {
            State* state = reinterpret_cast<State*>(arg);
            state->matches++;
            if (state->matches == 1) {
                state->stats.seek_file = f;
                state->stats.seek_file_level = level;
            }
            return state->matches < 2;
        }
    };

    State state;
    state.matches = 0;
    ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);

    if (state.matches >= 2) {
        return UpdateStats(state.stats);
    }
    return false;
}

void Version::Ref() { ++refs_; }

void Version::Unref() {
    assert(this != &vset_->dummy_versions_);
    assert(refs_ >= 1);
    --refs_;
    if (refs_ == 0) {
        delete this;
    }
}

bool Version::OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key) {
    return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                                 smallest_user_key, largest_user_key);
}

int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key) {
    int level = 0;
    // If the data ovelaps with L0, it must be placed in L0 first for find the latest data
    if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
        // The seq to cover the maximum area
        InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
        std::vector<FileMetaData*> overlaps;
        while (level < config::kMaxMeCompactLevel) {
            if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
                break;
            }
            if (level + 2 < config::kNumLevels) {
                // Check that file dose not overlop too many grandparent bytes (write enlarged)
                GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
                const int64_t sum = TotalFileSize(overlaps);
                if (sum > MaxGrandParentOverlapBytes(vset_->options_)) {
                    break;
                }
            }
            level++;
        }
    }
    return level;
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inputs) {
    assert(level > 0);
    assert(level < config::kNumLevels);
    inputs->clear();
    Slice user_begin, user_end;
    if (begin != nullptr) {
        user_begin = begin->user_key();
    }
    if (end != nullptr) {
        user_end = end->user_key();
    }
    const Comparator* user_cmp = vset_->icmp_.user_comparator();
    for (size_t i = 0; i < files_[level].size();) {
        FileMetaData* f = files_[level][i++];
        const Slice file_start = f->smallest.user_key();
        const Slice file_limit = f->largest.user_key();
        if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
            // "f" is completely before specified range; skip it
        } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
            // "f" is completely after specified range; skip it
        } else {
            inputs->push_back(f);
            if (level == 0) {
                // Level-0 file may overlap each other, So check if the newly added files has expanded thr range
                // If so, it is necessary to clear and recalculate
                if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
                    user_begin = file_start;
                    inputs->clear();
                    i = 0;
                } else if (end != nullptr && user_cmp->Compare(file_limit, user_end) > 0) {
                    user_end = file_limit;
                    inputs->clear();
                    i = 0;
                }
            }
        }
    }
}

std::string Version::DebugString() const {
    std::string r;
    for (int level = 0; level < config::kNumLevels; level++) {
        // Example:
        //  --- level 1 ---
        //  17:123['a' .. 'd']
        //  20:43['e' .. 'g']
        r.append("--- level");
        AppendNumberTo(&r, level);
        r.append(" ---\n");
        const std::vector<FileMetaData*>& files = files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            r.push_back(' ');
            AppendNumberTo(&r, files[i]->number);
            r.push_back(':');
            AppendNumberTo(&r, files[i]->file_size);
            r.push_back('[');
            r.append(files[i]->smallest.DebugString());
            r.append(" .. ");
            r.append(files[i]->largest.DebugString());
            r.append("]\n");
        }
    }
    return r;
}

class VersionSet::Builder {
    public:
        Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
            base_->Ref();
            BySmallestKey cmp;
            cmp.internal_comparator = &vset_->icmp_;
            for (int level = 0; level < config::kNumLevels; level++) {
                level_[level].added_files = new FileSet(cmp);
            }
        }

        ~Builder() {
            for (int level = 0; level < config::kNumLevels; level++) {
                const FileSet* added = level_[level].added_files;
                std::vector<FileMetaData*> to_unref;
                to_unref.reserve(added->size());
                for (FileSet::const_iterator it = added->begin(); it != added->end(); ++it) {
                    to_unref.push_back(*it);
                }
                delete added;
                for (uint32_t i = 0; i < to_unref.size(); i++) {
                    FileMetaData* f = to_unref[i];
                    f->refs--;
                    if (f->refs <= 0) {
                        delete f;
                    }
                }
            }
            base_->Unref();
        }

        // Apply all of the edits in *edit to the current state
        void Apply(const VersionEdit* edit) {
            // Update compaction pointers
            for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
                const int level = edit->compact_pointers_[i].first;
                vset_->compact_pointer_[level] = edit->compact_pointers_[i].second.Encode().ToString();
            }

            // Delete files
            for (const auto& deleted_file_set_kvp : edit->deleted_files_) {
                const int level = deleted_file_set_kvp.first;
                const uint64_t number = deleted_file_set_kvp.second;
                level_[level].deleted_files.insert(number);
            }

            // Add new files
            for (size_t i = 0; i < edit->new_files_.size(); i++) {
                const int level = edit->new_files_[i].first;
                FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
                f->refs = 1;

                // The cost of invalid queries Vs th costs of file merging
                // We arrange to automatically compact this file after
                // a certain number of seeks.  Let's assume:
                //   (1) One seek costs 10ms
                //   (2) Writing or reading 1MB costs 10ms (100MB/s)
                //   (3) A compaction of 1MB does 25MB of IO:
                //         1MB read from this level
                //         10-12MB read from next level (boundaries may be misaligned)
                //         10-12MB written to next level
                // This implies that 25 seeks cost the same as the compaction
                // of 1MB of data.  I.e., one seek costs approximately the
                // same as the compaction of 40KB of data.  We are a little
                // conservative and allow approximately one seek for every 16KB
                // of data before triggering a compaction.
                f->allowed_seeks = static_cast<int>((f->file_size / 16384U));
                if (f->allowed_seeks < 100) f->allowed_seeks = 100;

                level_[level].deleted_files.erase(f->number);
                level_[level].added_files->insert(f);
            }
        }

        void SaveTo(Version* v) {
            BySmallestKey cmp;
            cmp.internal_comparator = &vset_->icmp_;
            for (int level = 0; level < config::kNumLevels; level++) {
                // Merge the set of added files with the set of pre_existing files
                // Drop any deleted files. Store the result in *v
                const std::vector<FileMetaData*>& base_files = base_->files_[level];
                std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
                std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
                const FileSet* added_files = level_[level].added_files;
                v->files_[level].reserve(base_files.size() + added_files->size());
                for (const auto& added_file : *added_files) {
                    for (std::vector<FileMetaData*>::const_iterator bpos = std::upper_bound(base_iter, base_end, added_file, cmp); 
                        base_iter != bpos; ++base_iter) {
                            MaybeAddFile(v, level, *base_iter);
                        }
                    MaybeAddFile(v, level, added_file);
                }
                for (; base_iter != base_end; ++base_iter) {
                    MaybeAddFile(v, level, *base_iter);
                }

#ifndef NDEBUG                
                // Make sure there is no overlap in levels > 0
                if (level > 0) {
                    for (uint32_t i = 1; i < v->files_[level].size(); i++) {
                        const InternalKey& prev_end = v->files_[level][i - 1]->largest;
                        const InternalKey& this_begin = v->files_[level][i]->smallest;
                        if(vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
                            std::fprintf(stderr, "overlapping ranges in same level % s vs. %s\n",
                                          prev_end.DebugString().c_str(), this_begin.DebugString().c_str());
                            std::abort();
                        }
                    }
                }
#endif
            }
        }

        // Added new version files checks
        // v is builded for next version, so not do is deletion
        void MaybeAddFile(Version* v, int level, FileMetaData* f) {
            if (level_[level].deleted_files.count(f->number) > 0) {
                // File is deleted: do nothing
            } else {
                std::vector<FileMetaData*>* files = &v->files_[level];
                if (level > 0 && !files->empty()) {
                    // Must not overlap
                    assert(vset_->icmp_.Compare((*files)[files->size() - 1]->largest, f->smallest) < 0);
                }
                f->refs++;
                files->push_back(f);
            }
        }


    private:
        // Custom sort for set
        struct BySmallestKey {
            const InternalKeyComparator* internal_comparator;

            bool operator()(FileMetaData* f1, FileMetaData* f2) const {
                int r = internal_comparator->Compare(f1->smallest, f2->smallest);
                if (r != 0) {
                    return (r < 0);
                } else {
                    return (f1->number < f2->number);
                }
            }
        };

        typedef std::set<FileMetaData*, BySmallestKey> FileSet;

        struct LevelState {
            std::set<uint64_t> deleted_files;
            FileSet* added_files;
        };

        VersionSet* vset_;
        Version* base_;
        LevelState level_[config::kNumLevels];
};

VersionSet::VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache, const InternalKeyComparator* cmp)
        : env_(options->env), 
          dbname_(dbname), 
          options_(options), 
          table_cache_(table_cache), 
          icmp_(*cmp), 
          next_file_number_(2), 
          manifest_file_number_(0), 
          last_sequence_(0), 
          log_number_(0),
          prev_log_number_(0), 
          descriptor_file_(nullptr),
          descriptor_log_(nullptr),
          dummy_versions_(this),
          current_(nullptr) {
    AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
    current_->Unref();
    assert(dummy_versions_.next_ == &dummy_versions_);
    delete descriptor_log_;
    delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
    assert(v->refs_ == 0);
    assert(v != current_);
    if (current_ != nullptr) {
        current_->Unref();
    }
    current_ = v;
    v->Ref();

    // Append to linked list (Double linked list)
    v->prev_ = dummy_versions_.prev_;
    v->next_ = &dummy_versions_;
    v->prev_->next_ = v;
    v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
    if (edit->has_log_number_) {
        assert(edit->log_number_ >= log_number_);
        assert(edit->log_number_ < next_file_number_);
    } else {
        edit->SetLogNumber(log_number_);
    }

    if (!edit->has_prev_log_number_) {
        edit->SetPrevLogNumber(prev_log_number_);
    }

    edit->SetNextFile(next_file_number_);
    edit->SetLastSequence(last_sequence_);

    Version* v = new Version(this);
    {
        Builder buider(this, current_);
        buider.Apply(edit);
        buider.SaveTo(v);
    }
    Finalize(v);

    // Initialize new descriptor log file.
    // if necessary by creating a temporary file that contains a snapshot of the current version.
    std::string new_manifest_file;
    Status s;
    if (descriptor_log_ == nullptr) {
        assert(descriptor_file_ == nullptr);
        new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
        s = env_->NewWriteFile(new_manifest_file, &descriptor_file_);
        if (s.ok()) {
            descriptor_log_ = new log::Writer(descriptor_file_);
            s = WriteSnapshot(descriptor_log_);
        }
    }

    // Unlock during expensive MANIFEST log write
    {
        mu->Unlock();

        // Write new record to MANIFEST log
        if (s.ok()) {
            std::string record;
            edit->EncodeTo(&record);
            s = descriptor_log_->AddRecord(record);
            if (s.ok()) {
                s = descriptor_file_->Sync();
            }
            if (!s.ok()) {
                Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
            }
        }

        // If we just created a new descriptor fill.
        // install it by writing a new CURRENT file that points to it.
        if (s.ok() && !new_manifest_file.empty()) {
            s = SetCurrentFile(env_, dbname_, manifest_file_number_);
        }

        mu->Lock();
    }

    // Install the new version
    if (s.ok()) {
        AppendVersion(v);
        log_number_ = edit->log_number_;
        prev_log_number_ = edit->prev_log_number_;
    } else {
        delete v;
        if (!new_manifest_file.empty()) {
            delete descriptor_log_;
            delete descriptor_file_;
            descriptor_log_ = nullptr;
            descriptor_file_ = nullptr;
            env_->RemoveFile(new_manifest_file);
        }
    }

    return s;
}

Status VersionSet::Recover(bool* save_manifest) {
    struct LogReporter : public log::Reader::Reporter {
        Status* status;
        void Corruption(size_t bytes, const Status& s) override {
            if (this->status->ok()) *this->status = s;
        }

    };

    std::string current;
    Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
    if (!s.ok()) {
        return s;
    }
    if (current.empty() || current[current.size() - 1] != '\n') {
        return Status::Corruption("CURRENT file dose not end with newline");
    }
    current.resize(current.size() - 1);

    std::string dsname = dbname_ + "/" + current;
    SequentialFile* file;
    s = env_->NewSequentialFile(dsname, &file);
    if (!s.ok()) {
        if (s.IsNotFound()) {
            return Status::Corruption("CURRENT points to  a non-existent file", s.ToString());
        }
        return s;
    }

    bool have_log_number = false;
    bool have_prev_log_number = false;
    bool have_next_file = false;
    bool have_last_sequence = false;
    uint64_t next_file = 0;
    uint64_t last_sequence = 0;
    uint64_t log_number = 0;
    uint64_t prev_log_number = 0;
    Builder builder(this, current_);
    int read_records = 0;

    {
        LogReporter reporter;
        reporter.status = &s;
        log::Reader reader(file, &reporter, true, 0);
        Slice record;
        std::string scratch;
        while (reader.ReaderRecord(&record, &scratch)) {
            ++read_records;
            VersionEdit edit;
            s = edit.DecodeFrom(record);
            if (s.ok()) {
                if (edit.has_comparator_ && edit.comparator_ != icmp_.user_comparator()->Name()) {
                    s = Status::InvalidArgument(edit.comparator_ + " does not match existing comparator ",
                                                icmp_.user_comparator()->Name());
                }
            }

            if (s.ok()) {
                builder.Apply(&edit);
            }

            if (edit.has_log_number_) {
                log_number = edit.log_number_;
                have_log_number = true;
            }

            if (edit.has_prev_log_number_) {
                prev_log_number = edit.prev_log_number_;
                have_prev_log_number = true;
            }

            if (edit.has_next_file_number_) {
                next_file_number_ = edit.next_file_number_;
                have_next_file = true;
            }

            if (edit.has_last_sequence_) {
                last_sequence_ = edit.last_sequence_;
                have_last_sequence = true;
            }
        }
    }
    delete file;
    file = nullptr;

    if (s.ok()) {
        if (!have_next_file) {
            s = Status::Corruption("no meta-nextfile entry in descriptor");
        } else if (!have_log_number) {
            s = Status::Corruption("no meta-lognumber entry in descriptor");
        } else if (!have_last_sequence) {
            s = Status::Corruption("no last-sequence-number entry in descriptor");
        }

        if (!have_prev_log_number) {
            prev_log_number = 0;
        }

        MarkFileNumberUsed(prev_log_number);
        MarkFileNumberUsed(log_number);
    }

    if (s.ok()) {
        Version* v = new Version(this);
        builder.SaveTo(v);
        Finalize(v);
        AppendVersion(v);
        manifest_file_number_ = next_file;
        next_file_number_ = next_file + 1;
        last_sequence_ = last_sequence;
        log_number = log_number;
        prev_log_number_ = prev_log_number;

        if (ReuseManifest(dsname, current)) {

        } else {
            *save_manifest = true;
        }
    } else {
        std::string error = s.ToString();
        Log(options_->info_log, "Error recovering version set with %d record: %s", read_records, error.c_str());
    }

    return s;
}

bool VersionSet::ReuseManifest(const std::string& dsname, const std::string& dscbase) {
    if (!options_->reuse_logs) {
        return false;
    }
    FileType manifest_type;
    uint64_t manifest_number;
    uint64_t manifest_size;
    if (!ParseFileName(dscbase, &manifest_number, &manifest_type) || manifest_type != kDescriptorFile ||
        !env_->GetFileSize(dsname, &manifest_size).ok() || 
        // Make new compacted MANIFEST if old one is too big
        manifest_size >= TargetFileSize(options_)) {
        return false;
    }

    assert(descriptor_file_ == nullptr);
    assert(descriptor_log_ == nullptr);
    Status r = env_->NewAppendableFile(dsname, &descriptor_file_);
    if (!r.ok()) {
        Log(options_->info_log, "Reuse MANIFEST: %s\n", r.ToString().c_str());
        assert(descriptor_file_ == nullptr);
        return false;
    }

    Log(options_->info_log, "Reusing MANISEST %s\n", dsname.c_str());
    descriptor_log_ = new log::Writer(descriptor_file_, manifest_size);
    manifest_file_number_ = manifest_number;
    return true;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) {
        next_file_number_ = number + 1;
    }
}

void VersionSet::Finalize(Version* v) {
    // Precomputed best level for mext compaction
    int best_level = -1;
    double best_score = -1;

    for (int level = 0; level < config::kNumLevels - 1; level++) {
        double score;
        if (level == 0) {
            score = v->files_[level].size() / static_cast<double>(config::kLO_CompactionTrigger);
        } else {
            const uint64_t level_bytes = TotalFileSize(v->files_[level]);
            score = static_cast<double>(level_bytes) / MaxBytesForLevel(options_, level);
        }

        if (score > best_score) {
            best_level = level;
            best_score = score;
        }
    }

    v->compaction_level_ = best_level;
    v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
    // Save metadata
    VersionEdit edit;
    edit.SetComparatorName(icmp_.user_comparator()->Name());

    // Save compaction pointers
    for (int level = 0; level < config::kNumLevels; level++) {
        if (!compact_pointer_[level].empty()) {
            InternalKey key;
            key.DecodeFrom(compact_pointer_[level]);
            edit.SetCompectorPointer(level, key);
        }
    }

    // Save files
    for (int level = 0; level < config::kNumLevels; level++) {
        const std::vector<FileMetaData*>& files = current_->files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            const FileMetaData* f = files[i];
            edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
        }
    }

    std::string record;
    edit.EncodeTo(&record);
    return log->AddRecord(record);
}

int VersionSet::NumLevelFiles(int level) const {
    assert(level >= 0);
    assert(level <= config::kNumLevels);
    return current_->files_[level].size();
}


const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
    // Update code if kNumlevels changes
    static_assert(config::kNumLevels == 7, ""); 
    std::snprintf(
            scratch->buffer, sizeof(scratch->buffer), "files[ %d %d %d %d %d %d %d ]",
            int(current_->files_[0].size()), int(current_->files_[1].size()), int(current_->files_[2].size()),
            int(current_->files_[3].size()), int(current_->files_[4].size()), int(current_->files_[5].size()),
            int(current_->files_[6].size()));
    return scratch->buffer;
}

uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
    uint64_t result = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
        const std::vector<FileMetaData*>& files = v->files_[level];
        for (size_t i = 0; i < files.size(); i++) {
            if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
                result += files[i]->file_size;
            } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
                if (level > 0) {
                    break;
                }
            } else {
                Table* tableptr;
                Iterator* iter = table_cache_->NewIterator(ReadOptions(), files[i]->number,
                                                           files[i]->file_size, &tableptr);
                if (tableptr != nullptr) {
                    result += tableptr->ApproximateOffsetOf(ikey.Encode());
                }                                            
                delete iter;
            }
        }
    }
    return result;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
    for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
        for (int level = 0; level < config::kNumLevels; level++) {
            const std::vector<FileMetaData*>& files = v->files_[level];
            for (size_t i = 0; i < files.size(); i++) {
                live->insert(files[i]->number);
            }
        }
    }
}

int64_t VersionSet::NumLevelBytes(int level) const {
    assert(level >= 0);
    assert(level < config::kNumLevels);
    return TotalFileSize(current_->files_[level]);
}

int64_t VersionSet::MaxNextLevelOverlappingBytes() {
    int64_t result = 0;
    std::vector<FileMetaData*> overlaps;
    for (int level = 1; level < config::kNumLevels - 1; level++) {
        for (size_t i = 0; i < current_->files_[level].size(); i++) {
            const FileMetaData* f = current_->files_[level][i];
            current_->GetOverlappingInputs(level, &f->smallest, &f->largest, &overlaps);
            const int64_t sum = TotalFileSize(overlaps);
            if (sum > result) {
                result = sum;
            }
        }
    }
    return result;
}

// Get the range of input keys.
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest, InternalKey* largest) {
    assert(!inputs.empty());
    smallest->Clear();
    largest->Clear();
    for (size_t i = 0; i < inputs.size(); i++) {
        FileMetaData* f = inputs[i];
        if (i == 0) {
            *smallest = f->smallest;
            *largest = f->largest;
        } else {
            if (icmp_.Compare(f->smallest, *smallest) < 0) {
                *smallest = f->smallest;
            } 
            if (icmp_.Compare(f->largest, *largest) > 0) {
                *largest = f->largest;
            }
        }
    }
}

void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1, const std::vector<FileMetaData*>& inputs2, 
                           InternalKey* smallest, InternalKey* largest) {
    std::vector<FileMetaData*> all = inputs1;
    all.insert(all.end(), inputs2.begin(), inputs2.end());
    GetRange(all, smallest, largest);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
    ReadOptions options;
    options.verify_checksums = options_->paranoid_checks;
    options.fill_cache = false;

    // Level-0 files have to be merged together, every file need a iterator.
    // For other levels, we will make a concatenating iterator per level.
    const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
    Iterator** list = new Iterator*[space];
    int num = 0;
    for (int which = 0; which < 2; which++) {
        if (!c->inputs_[which].empty()) {
            if (c->level() + which == 0) {
                const std::vector<FileMetaData*>& files = c->inputs_[which];
                for (size_t i = 0; i < files.size(); i++) {
                    list[num++] = table_cache_->NewIterator(options, files[i]->number, files[i]->file_size);
                }
            } else {
                list[num++] = NewTwoLevelIterator(new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]), 
                                  &GetFileIterator, table_cache_, options);
            }
        }
    }
    assert(num <= space);
    Iterator* result = NewMergingIterator(&icmp_, list, num);
    delete[] list;
    return result;
}

Compaction* VersionSet::PickCompaction() {
    Compaction* c;
    int level;

    // We prefer compactions triggered by too much data in a level over the compaction triggered by seeks.
    // Priority writing pause(size) > read amplification(seek)
    const bool size_compaction = (current_->compaction_score_ >= 1);
    const bool seek_compaction = (current_->file_to_compact_ != nullptr);
    if (size_compaction) {
        level = current_->compaction_level_;
        assert(level >= 0);
        assert(level + 1< config::kNumLevels);
        c = new Compaction(options_, level);

        // Pick the first file that comes after compact_pointer_[level].
        for (size_t i = 0; i < current_->files_[level].size(); i++) {
            FileMetaData* f = current_->files_[level][i];
            // Why f need largest? 
            // After the previous merge, some new file may have generated.
            // The pointer is no longer the largest key in the file.
            if (compact_pointer_[level].empty() || icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
                c->inputs_[0].push_back(f);
                break;
            }
        }
        if (c->inputs_[0].empty()) {
            // Take the beginning of the key space
            c->inputs_[0].push_back(current_->files_[level][0]);
        }
    } else if (seek_compaction) {
        level = current_->file_to_compact_level;
        c = new Compaction(options_, level);
        c->inputs_[0].push_back(current_->file_to_compact_);
    } else {
        return nullptr;
    }

    c->input_version_ = current_;
    c->input_version_->Ref();

    // File in level 0 may overlap each other, so pick up all overlapping ones.
    if (level == 0) {
        InternalKey smallest, largest;
        GetRange(c->inputs_[0], &smallest, &largest);
        current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
        assert(!c->inputs_[0].empty());
    }

    SetupOtherInputs(c);

    return c;
}

// Finds the largest key in a vector of files
bool FindLargestKey(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files, InternalKey* largest_key) {
    if (files.empty()) {
        return false;
    }
    *largest_key = files[0]->largest;
    for (size_t i = 0; i < files.size(); i++) {
        FileMetaData* f = files[i];
        if (icmp.Compare(f->largest, *largest_key) > 0) {
            *largest_key = f->largest;
        }
    }
    return true;
}

// Prevent old versions of identical key from remaining in their original level during compaction.
// Cause otherwise cause stale data to be read.
FileMetaData* FindSmallestBoundaryFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& level_files,
                                       const InternalKey& largest_key) {
    const Comparator* user_cmp = icmp.user_comparator();
    FileMetaData* smallest_boundary_file = nullptr;
    for (size_t i = 0; i < level_files.size(); i++) {
        FileMetaData* f = level_files[i];
        if (icmp.Compare(f->smallest, largest_key) > 0 && user_cmp->Compare(f->smallest.user_key(), largest_key.user_key()) == 0) {
            if (smallest_boundary_file == nullptr || icmp.Compare(f->smallest, smallest_boundary_file->smallest) < 0) {
                smallest_boundary_file = f;
            }
        }
    }
    return smallest_boundary_file;
}

// Circular calls to FindSmallestBoundaryFile prevent the cascading spread of chain reactions
void AddBoundaryInputs(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& level_files, 
                       std::vector<FileMetaData*>* compaction_files) {
    InternalKey largest_key;
    
    if (!FindLargestKey(icmp, level_files, &largest_key)) {
        return;
    }

    bool continue_searching = true;
    while (continue_searching) {
        FileMetaData* smallest_boundary_file = FindSmallestBoundaryFile(icmp, level_files, largest_key);

        if (smallest_boundary_file != NULL) {
            compaction_files->push_back(smallest_boundary_file);
            largest_key = smallest_boundary_file->largest;
        } else {
            continue_searching = false;
        }
    }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
    const int level = c->level();
    InternalKey smallest, largest;

    AddBoundaryInputs(icmp_, current_->files_[level], &c->inputs_[0]);
    GetRange(c->inputs_[0], &smallest, &largest);

    current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);
    AddBoundaryInputs(icmp_, current_->files_[level + 1], &c->inputs_[1]);

    InternalKey all_start, all_limit;
    GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

    if (!c->inputs_[1].empty()) {
        std::vector<FileMetaData*> expanded0;
        current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
        AddBoundaryInputs(icmp_, current_->files_[level], &expanded0);
        const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
        const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
        const int64_t expanded0_size = TotalFileSize(expanded0);
        if (expanded0.size() > c->inputs_[0].size() && inputs1_size + expanded0_size < ExpandedCompactionByteSizeLimit(options_)) {
            InternalKey new_start, new_limit;
            GetRange(expanded0, &new_start, &new_limit);
            std::vector<FileMetaData*> expanded1;
            current_->GetOverlappingInputs(level + 1, &new_start, &new_limit, &expanded1);
            AddBoundaryInputs(icmp_, current_->files_[level + 1], &expanded1);
            if (expanded1.size() == c->inputs_[1].size()) {
                Log(options_->info_log, "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
                    level, int(c->inputs_[0].size()), int(c->inputs_[1].size()), long(inputs0_size), long(inputs1_size),
                    int(expanded0.size()), int(expanded1.size()), long(expanded0_size), long(inputs1_size));
                smallest = new_start;
                largest = new_limit;
                c->inputs_[0] = expanded0;
                c->inputs_[1] = expanded1;
                GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
            }
        }
    }

    if (level + 2 < config::kNumLevels) {
        current_->GetOverlappingInputs(level + 1, &all_start, &all_limit, &c->grandparents_);
    }

    compact_pointer_[level] = largest.Encode().ToString();
    c->edit_.SetCompectorPointer(level, largest);
}

Compaction* VersionSet::CompactRange(int level, const InternalKey* begin, const InternalKey* end) {
    std::vector<FileMetaData*> inputs;
    current_->GetOverlappingInputs(level, begin, end, &inputs);
    if (inputs.empty()) {
        return nullptr;
    }

    if (level > 0) {
        const uint64_t limit = MaxFileSizeForLevel(options_, level);
        uint64_t total = 0;
        for (size_t i = 0; i < inputs.size(); i++) {
            uint64_t s = inputs[i]->file_size;
            total += s;
            if (total >= limit) {
                inputs.resize(i + 1);
                break;
            }
        }
    }

    Compaction* c = new Compaction(options_, level);
    c->input_version_ = current_;
    c->input_version_->Ref();
    c->inputs_[0] = inputs;
    SetupOtherInputs(c);
    return c;
}

Compaction::~Compaction() {
    if (input_version_ != nullptr) {
    }
}

bool Compaction::IsTrivialMove() const {
    const VersionSet* vset = input_version_->vset_;

    return (num_input_files(0) == 1 && num_input_files(1) == 0 && 
            TotalFileSize(grandparents_) <= MaxGrandParentOverlapBytes(vset->options_));
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
    for (int which = 0; which < 2; which++) {
        for (size_t i = 0; i < inputs_[which].size(); i++) {
            edit->RemoveFile(level_ + which, inputs_[which][i]->number);
        }
    }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
    const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
    for (int level = level_ + 2; level < config::kNumLevels; level++) {
        const std::vector<FileMetaData*>& files = input_version_->files_[level];
        while (level_ptrs_[level] < files.size()) {
            FileMetaData* f = files[level_ptrs_[level]];
            if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
                if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
                    return false;
                }
                 break;
            }
           level_ptrs_[level]++;
        }
    }
    return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key) {
    const VersionSet* vset = input_version_->vset_;
    const InternalKeyComparator* icmp = &vset->icmp_;
    while (grandparent_index_ < grandparents_.size() &&
           icmp->Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0) {
        if (seen_key_) {
            overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
        }
        grandparent_index_++;
    }
    seen_key_ = true;

    if (overlapped_bytes_ > MaxGrandParentOverlapBytes(vset->options_)) {
        overlapped_bytes_ = 0;
        return true;
    } else {
        return false;
    }
}

void Compaction::ReleaseInputs() {
    if (input_version_ != nullptr) {
        input_version_->Unref();
        input_version_ = nullptr;
    }
}

}
