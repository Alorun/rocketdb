#pragma once

#include <map>
#include <set>
#include <vector>

#include "dbformat.h"
#include "version_edit.h"
#include "../port/port.h"
#include "../port/thread_annotations.h"
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

int FindFile(const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files, const Slice& key);

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp, bool disjoint_sorted_file, std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key, const Slice* largest_user_key);

class Version {
    public:
        struct GetStats {
            FileMetaData* seek_file;
            int seek_file_level;
        };

        // Append to *iter a sequence of iterators that will yield the contains of this Version when merged together
        void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

        // Lookup the value for key
        Status Get(const ReadOptions&, const LookupKey& key, std::string* val, GetStats* stats);

        // Add stats into the current state, resonsible to reduce allowed_seeks
        // When allowed_seeks == 0, wake up the thread to compaction
        bool UpdateStats(const GetStats& stats);

        // Trigger to compaction base on Seek
        bool RecordReadSample(Slice key);

        // Reference count management
        void Ref();
        void Unref();

        // Return a level of all files to input
        void GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end, std::vector<FileMetaData*>* inoputs);

        // Returns true iff some file in the specified level overlaps
        // some part of [*smallest_user_key,*largest_user_key].
        // smallest_user_key==nullptr represents a key smaller than all the DB's keys.
        // largest_user_key==nullptr represents a key largest than all the DB's keys.
        bool OverlapInLevel(int level, const Slice* smallest_user_key, const Slice* largest_user_key);

        // Return the level at which we should place a new memtable compaction
        // result that covers the range [smallest_user_key,largest_user_key].
        int PickLevelForMemTableOutput(const Slice& smallest_user_key, const Slice& largest_user_key);

        int NumFiles(int level) const { return files_[level].size(); }

        // Return a human readable string that descrbes this version's contents
        std::string DebugString() const;

    private:
        friend class Compaction;
        friend class VersionSet;

        class LevelFileNumIterator;

        explicit Version(VersionSet* vset) : vset_(vset), next_(this), prev_(this), refs_(0), file_to_compact_(nullptr),
                file_to_compac_level(-1), compaction_score_(-1), compaction_level_(-1) {}

        Version(const Version&) = delete;
        Version& operator=(const Version&) = delete;

        ~Version();

        Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

        void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg, bool (*func)(void*, int, FileMetaData));

        VersionSet* vset_;      // Version Set
        Version* next_;         // Next Version
        Version* prev_;         // Prev Version
        int refs_;              // When version's refs == 0, it need to delete

        // Every level of file metadata
        std::vector<FileMetaData*> files_[config::kNumLevels];

        // Next file to compact based on seek stats(allowed_seeks)
        FileMetaData* file_to_compact_;     
        int file_to_compac_level;

        double compaction_score_;   // If the score > 1, the mean this need to compaction
        int compaction_level_;      // The highest priority level to be compaction
};





}