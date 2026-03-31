#include "version_set.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

    // Search 0 level
    std::vector<FileMetaData*> tmp;
    tmp.reserve(files_[0].size());
    for (uint32_t i = 0; i < files_[0].size(); i++) {
        FileMetaData* f = files_[0][i];
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 && ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
            tmp.push_back(f);
        }
    }
    if (!tmp.empty()) {
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
        GetStats* stats;
        const ReadOptions* options;
        Slice ikey;
        FileMetaData* last_file_read;
        int last_file_read_level;

        VersionSet* vset;
        Status s;
        bool found;

        static bool Match(void* arg, int level, FileMetaData* f) {
            State* state = reinterpret_cast<State*>(arg);

            if (state->stats->seek_file == nullptr && state->last_file_read != nullptr) {
                state->stats->seek_file = state->last_file_read;
                state->stats->seek_file_level = state->last_file_read_level;
            }

            state->last_file_read = f;
            state->last_file_read_level = level;

            state->s = state->vset->table_cache->Get(*state->options, f->number, f->file_size, state->ikey,
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

bool Version::UpdateStats(const GetStats& stats) {
    FileMetaData* f = stats.seek_file;
    if (f != nullptr) {
        f->allowed_seeks--;
        if (f->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
            file_to_compact_ = f;
            file_to_compac_level = stats.seek_file_level;
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
    assert(this != &vset_->dummy_sersions_);
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
    if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
        InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
        InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
        std::vector<FileMetaData*> overlaps;
        while (level < config::kMaxMeCompactLevel) {
            if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
                break;
            }
            if (level + 2 < config::kNumLevels) {
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


}