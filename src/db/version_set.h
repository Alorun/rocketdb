#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "dbformat.h"
#include "table_cache.h"
#include "version_edit.h"
#include "../port/port_stdcxx.h"
#include "../../include/options.h"

namespace rocketdb {

namespace log {
    class Writer;
}

class Comparator;
class Iterator;
class Memtable;
class TableBuilder;
class Version;
class VersionSet;
class WritableFile;
class Compaction;

int FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files, const Slice& key);

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp, bool disjoint_sorted_file, std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key, const Slice* largest_user_key);

class Version {
    public:
        struct GetStats {
            FileMetaData* seek_file;
            int seek_file_level;
        };

        // Append to *iter a sequence of iterators that will yield the contains of this Version when merged together.
        void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

        // Lookup the value for key
        Status Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats);

        // Add stats into the current state, resonsible to reduce allowed_seeks.
        // When allowed_seeks == 0, wake up the thread to compaction.
        bool UpdateStats(const GetStats& stats);

        // Trigger to compaction base on Seek.
        bool RecordReadSample(Slice key);

        // Reference count management.
        void Ref();
        void Unref();

        // Return a level of all files to input.
        void GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inoputs);

        // Returns true iff some file in the specified level overlaps.
        // some part of [*smallest_user_key,*largest_user_key].
        // smallest_user_key==nullptr represents a key smaller than all the DB's keys.
        // largest_user_key==nullptr represents a key largest than all the DB's keys.
        bool OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key);

        // Return the level at which we should place a new memtable compaction.
        // result that covers the range [smallest_user_key,largest_user_key].
        int PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key);

        int NumFiles(int level) const { return files_[level].size(); }

        // Return a human readable string that descrbes this version's contents.
        std::string DebugString() const;

    private:
        friend class Compaction;
        friend class VersionSet;

        class LevelFileNumIterator;

        explicit Version(VersionSet* vset) : vset_(vset), next_(this), prev_(this), refs_(0), file_to_compact_(nullptr),
                file_to_compact_level(-1), compaction_score_(-1), compaction_level_(-1) {}

        Version(const Version&) = delete;
        Version& operator=(const Version&) = delete;

        ~Version();

        Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

        void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg, bool (*func)(void*, int, FileMetaData*));

        VersionSet* vset_;      // Version Set
        Version* next_;         // Next Version
        Version* prev_;         // Prev Version
        int refs_;              // When version's refs == 0, it need to delete

        // Every level of file metadata.
        std::vector<FileMetaData*> files_[config::kNumLevels];

        // Next file to compact based on seek stats(allowed_seeks).
        FileMetaData* file_to_compact_;     
        int file_to_compact_level;

        double compaction_score_;   // If the score > 1, the mean this need to compaction
        int compaction_level_;      // The highest priority level to be compaction
};

class VersionSet {
    public:
        VersionSet(const std::string& dbname, const Options* options, TableCache* table_cache, const InternalKeyComparator*);

        VersionSet(const VersionSet&) = delete;
        Version& operator=(const VersionSet&) = delete;

        ~VersionSet();

        // Apply *edit to the current version to form a new descriptor.
        // It is saved to persistent state and installed as the new current version.
        // Requires holding mute and make sure no other thread concurrently calls LogAndApply().
        Status LogAndApply(VersionEdit* edit, port::Mutex* mu) EXCLUSIVE_LOCKS_REQUIRED(mu);

        // Recover the last saved descriptor from persistent storage (Restart and use).
        Status Recover(bool* save_manifest);

        // Return the current version.
        Version* current() const { return current_; }

        // Return the current manifest file number.
        uint64_t ManifestFileNumber() const { return manifest_file_number_; }

        // Allocate and return a new file number.
        uint64_t NewFileNumber() { return next_file_number_++; }

        // Arrange to reuse "file_number" unless a newer file number has already been allocated.
        void ReuseFileNumber(uint64_t file_number) {
            if (next_file_number_ == file_number + 1) {
                next_file_number_ = file_number;
            }
        }

        // Return the number fo Table files at the specified level.
        int NumLevelFiles(int level) const;

        // Return the combined file size of all files at the specified level.
        int64_t NumLevelBytes(int level) const;

        // Return the last sequence number.
        int64_t LastSequence() const { return last_sequence_; }

        // Set the last sequence number to s.
        void SetLastSequence(uint64_t s) {
            assert(s >= last_sequence_);
            last_sequence_ = s;
        }

        // Mark the specified file number as used.
        void MarkFileNumberUsed(uint64_t number);

        // Return the current log file number.
        uint64_t LogNumber() const { return log_number_; }

        // Return the log file number for the log file that is currently being compacted.
        uint64_t PrevLogNumber() const { return prev_log_number_; } 

        // Pick level and inputs for a new compaction.
        // Return a heap-allocated object that descibes the compaction, or return nullptr.
        // Caller should delete the result.
        Compaction* PickCompaction();

        // Compacting the range [begin,end] in specified level.
        // Return a heap-allocated object that descibes the compaction, or return nullptr.
        // Caller should delete the result.
        Compaction* CompactRange(int level, const InternalKey* begin, const InternalKey* end);

        // Rerturn the maxinum overlapping data at next level for any file.
        int64_t MaxNextLevelOverlappingBytes();

        // Create an iterator that reads over the compaction inputs for "*c".
        Iterator* MakeInputIterator(Compaction* c);

        // Return true iff some level needs a compaction.
        bool NeedsCompaction() const {
            Version* v = current_;
            return (v->compaction_score_ > 1) || (v->file_to_compact_ != nullptr);
        }

        // Add all files listed in any live version to *live.
        void AddLiveFiles(std::set<uint64_t>* live);

        // Return the approximate offset in the database of the data for "key" as of version "v".
        uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

        // Return the human-readable short summary of the number of files per level.
        struct LevelSummaryStorage {
            char buffer[100];
        };
        const char* LevelSummary(LevelSummaryStorage* scratch) const;

    private:
        class Builder;

        friend class Compaction;
        friend class Version;

        // Reuse MANIFEST file for restart database.
        bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

        // Assess whether each level requires compaction.
        void Finalize(Version* v);

        // For PickCompaction, determine the scope of compaction.
        void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest, InternalKey* largest);
        void GetRange2(const std::vector<FileMetaData*>& inputs1, const std::vector<FileMetaData*>& input2,
                       InternalKey* smallest, InternalKey* largest);
        void SetupOtherInputs(Compaction* c);

        // Save current contents to *log.
        Status WriteSnapshot(log::Writer* log);

        // Real update version.
        void AppendVersion(Version* v);

        Env* const env_;
        const std::string dbname_;
        const Options* const options_;
        TableCache* const table_cache_;
        const InternalKeyComparator icmp_;

        uint64_t next_file_number_;
        uint64_t manifest_file_number_;     // Matedata log file number
        uint64_t last_sequence_;
        uint64_t log_number_;               // Currently write log file number
        uint64_t prev_log_number_;          // Prev write log file number (Fault recovery)

        WritableFile* descriptor_file_;     // Manifest driect disk write
        log::Writer* descriptor_log_;       // Manifest formatted and packaged for writing to disk

        Version dummy_versions_;            // fake head version
        Version* current_;                  // The newest version

        // Store the key that was compressed to in the last compression at each layer.
        std::string compact_pointer_[config::kNumLevels];
};

class Compaction {
    public:
        ~Compaction();

        // Return the level that is being compacted.
        int level() const { return level_; }

        // Return the object that holds the edits to the descriptor done by this compaction.
        VersionEdit* edit() { return &edit_; }

        int num_input_files(int which) const { return inputs_[which].size(); }

        // Return the ith input file at "level()+which"
        FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

        // Maximun size of files to build during this compaction.
        uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

        // Is this a trivial compaction that can be implemented by just moving a single input file to the next level.
        bool IsTrivialMove() const;

        // Add all inputs to this compaction as delete operations to *edit.
        void AddInputDeletions(VersionEdit* edit);

        // Return true if the information we have avaible guarantees.
        // The compaction is producing data in "level+1" for which no data exists in levels greater than "level+1".
        // For Delete a key.
        bool IsBaseLevelForKey(const Slice& user_key);

        // Return true iff we should stop building the current output before processing "internal_key".
        // It depends currently compact file size overlapping with grandparent.
        bool ShouldStopBefore(const Slice& internal_key);

        // Release the input version for the compaction, once the compaction is successful.
        void ReleaseInputs();

    private:
        friend class Version;
        friend class VersionSet;

        Compaction(const Options* options, int level);

        int level_;
        uint64_t max_output_file_size_;
        Version* input_version_;
        VersionEdit edit_;

        // File participating in compaction at level and level+1.
        std::vector<FileMetaData*> inputs_[2];

        // State used to check for number of overlapping grandparent files.
        std::vector<FileMetaData*> grandparents_;
        size_t grandparent_index_;      // Index in grandparent_starts_
        bool seen_key_;                 // Some input key has been seen
        int64_t overlapped_bytes_;      // Bytes of overlap between current output and grandparent files

        // State for implementing IsBaseLevelForKey.
        size_t level_ptrs_[config::kNumLevels];
};


}