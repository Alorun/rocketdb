#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <set>
#include <string>

#include "dbformat.h"
#include "memtable.h"
#include "snapshot.h"
#include "../wal/log_writer.h"
#include "../port/port.h"
#include "../port/thread_annotations.h"
#include "../../include/db.h"
#include "../../include/env.h"
#include "table_cache.h"
#include "../port/port_stdcxx.h"
#include "version_edit.h"
#include "version_set.h"

namespace rocketdb {

class DBImpl : public DB {
    public:
        DBImpl(const Options& options, const std::string& dbname);

        DBImpl(const DBImpl&) = delete;
        DBImpl& operator=(const DBImpl&) = delete;

        ~DBImpl();

        Status Put(const WriteOptions&, const Slice& key, const Slice& value) override;
        Status Delete(const WriteOptions&, const Slice& key) override;
        Status Write(const WriteOptions& options, WriteBatch* updates) override;
        Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;

    private:
        friend class DB;
        struct CompactionState;
        struct Writer;

        struct ManualCompaction {
            int level;
            bool done;
            const InternalKey* begin;
            const InternalKey* end;
            InternalKey tmp_storage;
        };

        struct CompactionStats {
            CompactionStats() : micros(0), bytes_read(0), bytes_written(0) {}

            void Add(const CompactionStats& c) {
                this->micros += c.micros;
                this->bytes_read += c.bytes_read;
                this->bytes_written += c.bytes_written;
            }

            int64_t micros;
            int64_t bytes_read;
            int64_t bytes_written;
        };

        Iterator* NewInternalIterator(const ReadOptions&, SequenceNumber* latest_snapshot, uint32_t* seed);

        Status NewDB();

        Status Recover(VersionEdit* edit, bool* save_manifest) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

        // Used in scenarios where errors can be tolerated(last wal log, delete sst failure)
        void MaybeIgnoreError(Status* s) const;

        // Delete any unneeded files and stale in-memory entries
        void RemoveObsoleteFiles() EXCLUSIVE_LOCKS_REQUIRED(mutex_);   

        const Comparator* user_comparator() const {
            return internal_comparator_.user_comparator();
        }

        // Constant after construction
        Env* const env_;
        const InternalKeyComparator internal_comparator_;
        const InternalFilterPolicy internal_filter_policy_;
        const Options options_;  // options_.comparator == &internal_comparator_
        // Do you Create your logs and cache?
        // If so, you are responnsible for deleting them.
        const bool owns_info_log_;
        const bool owns_cache_;
        const std::string dbname_;

        // table_cache_ provides its own synchronization
        TableCache* const table_cache_;

        FileLock* db_lock_;

        // State below is protected by mutex_
        port::Mutex mutex_;
        std::atomic<bool> shutting_down_;
        // Notification thta the compression task is complete
        port::CondVar background_work_finished_signal_ GUARDED_BY(mutex_);
        MemTable* mem_;
        MemTable* imm_ GUARDED_BY(mutex_);  // The mem_ is full and being compacted
        std::atomic<bool> has_imm_;         // So bg thread can detect non-null imm_
        WritableFile* logfile_;             // WAL pointer
        uint64_t logfile_number_ GUARDED_BY(mutex_);
        log::Writer* log_;
        uint32_t seed_ GUARDED_BY(mutex_);  // For sampling

        // Queue of wirters
        std::deque<Writer*> writers_ GUARDED_BY(mutex_);
        WriteBatch* tmp_batch_ GUARDED_BY(mutex_);

        SnapshotList snapshots_ GUARDED_BY(mutex_);

        // Set of table files to protect from deletion because they are part of ongoing compactions
        std::set<uint64_t> pending_outputs_ GUARDED_BY(mutex_);

        // Has a background compaction been scheduled or is running?
        bool background_compaction_scheduled_ GUARDED_BY(mutex_);

        ManualCompaction* manual_compaction_ GUARDED_BY(mutex_);

        VersionSet* const versions_ GUARDED_BY(mutex_);

        // Have we encountered a background error in paranoid mode?
        Status bg_error_ GUARDED_BY(mutex_);

        CompactionStats stats_[config::kNumLevels] GUARDED_BY(mutex_);
};

// Sanitize db options, The caller should should delete result.info_log if it is not equal to src.info_log.
Options SanitizeOptions(const std::string& db, const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy, const Options& src);

}