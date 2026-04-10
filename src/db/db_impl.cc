#include "db_impl.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "builder.h"
#include "db_iter.h"
#include "dbformat.h"
#include "filename.h"
#include "../wal/log_reader.h"
#include "../wal/log_writer.h"
#include "memtable.h"
#include "table_cache.h"
#include "version_edit.h"
#include "version_set.h"
#include "write_batch_internal.h"
#include "../../include/db.h"
#include "../../include/env.h"
#include "../../include/env.h"
#include "../../include/table.h"
#include "../../include/table_builder.h"
#include "../port/port.h"
#include "../table/block.h"
#include "../table/merger.h"
#include "../table/two_level_iterator.h"
#include "../util/coding.h"
#include "../util/logging.h"
#include "../util/mutexlock.h"

namespace rocketdb {

// The size of file for other uses (log,list...)
const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
struct DBImpl::Writer {
    explicit Writer(port::Mutex* mu) : batch(nullptr), sync(false), cv(mu) {}

    Status status;
    WriteBatch* batch;
    bool sync;
    bool done;
    port::CondVar cv;
};

struct DBImpl::CompactionState {
    // File produced by compaction
    struct OutPut {
        uint64_t number;
        uint64_t file_size;
        InternalKey smallest, largest;
    };

    OutPut* current_output() { return &outputs[outputs.size() - 1]; }

    explicit CompactionState(Compaction* c)
        : compaction(c),
          smallest_snapshot(0),
          outfile(nullptr),
          builder(nullptr),
          total_bytes(0) {}

    Compaction* const compaction;

    // Sequence number <  smallest_snapshot will never to service a snapshot below smallest_snapshot.
    // We can drop all entries for the same key with sequence numbers < S
    SequenceNumber smallest_snapshot;

    std::vector<OutPut> outputs;

    // State kept for output beging generated
    WritbaleFile* outfile;
    TableBuilder* builder;

    uint64_t total_bytes;
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
    if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
    if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname, const InternalKeyComparator* icmp, 
                        const InternalFilterPolicy* ipolicy, const Options& src) {
    Options result = src;
    result.comparator = icmp;
    result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
    ClipToRange(&result.max_file_size, 64 + kNumNonTableCacheFiles, 50000);
    ClipToRange(&result.write_buffer_size, 64 << 10, 1 << 30);
    ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
    ClipToRange(&result.block_size, 1 << 10, 4 << 20);
    if (result.info_log == nullptr) {
        src.env->CreateDir(dbname);
        src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
        Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
        if (s.ok()) {
            result.info_log = nullptr;
        }
    }
    if (result.block_cache == nullptr) {
        result.block_cache = NewLRUCache(8 << 20);
    }
    return result;
}

static int TableCacheSize(const Options& sanitized_options) {
    return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
        : env_(raw_options.env),
          internal_comparator_(raw_options.comparator),
          internal_filter_policy_(raw_options.filter_policy),
          options_(SanitizeOptions(dbname, &internal_comparator_, &internal_filter_policy_, raw_options)),
          owns_info_log_(options_.info_log != raw_options.info_log),
          owns_cache_(options_.block_cache != raw_options.block_cache),
          dbname_(dbname),
          table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
          db_lock_(nullptr),
          shutting_down_(false),
          background_work_finished_signal_(&mutex_),
          mem_(nullptr),
          imm_(nullptr),
          has_imm_(false),
          logfile_(nullptr),
          logfile_number_(0),
          seed_(0),
          tmp_batch_(new WriteBatch),
          background_compaction_scheduled_(false),
          manual_compaction_(nullptr),
          versions_(new VersionSet(dbname_, &options_, table_cache_, &internal_comparator_)) {}

DBImpl::~DBImpl() {
    // Waiting for background work to finish
    mutex_.Lock();
    shutting_down_.store(true, std::memory_order_release);
    while (background_compaction_scheduled_) {
        background_work_finished_signal_.Wait();
    }
    mutex_.Unlock();

    if (db_lock_ != nullptr) {
        env_->UnlockFile(db_lock_);
    }

    delete versions_;
    if (mem_ != nullptr) mem_->Unref();
    if (imm_ != nullptr) imm_->Unref();
    delete tmp_batch_;
    delete log_;
    delete logfile_;
    delete table_cache_;

    if (owns_info_log_) {
        delete options_.info_log;
    }
    if (owns_cache_) {
        delete options_.block_cache;
    }
}

Status DBImpl::NewDB() {
    VersionEdit new_db;
    new_db.SetComparatorName(user_comparator()->Name());
    new_db.SetLogNumber(0);
    new_db.SetNextFile(2);
    new_db.SetLastSequence(0);

    const std::string manifest = DescriptorFileName(dbname_, 1);
    WritableFile* file;
    Status s = env_->NewWriteFile(manifest, &file);
    if (!s.ok()) {
        return s;
    }
    {
        log::Writer log(file);
        std::string record;
        new_db.EncodeTo(&record);
        s = log.AddRecord(record);
        if (s.ok()) {
            s = file->Sync();
        }
        if (s.ok()) {
            s = file->Close();
        }
    }

    delete file;
    if (s.ok()) {
        // Make "CURRENT" file that points to the new manifest file
        s = SetCurrentFile(env_, dbname_, 1);
    } else {
        env_->RemoveFile(manifest);
    }
    return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
    if (s->ok() || options_.paranoid_checks) {

    } else {
        Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
        *s = Status::OK();
    }
}

void DBImpl::RemoveObsoleteFiles() {
    mutex_.AssertHeld();

    if (!bg_error_.ok()) {
        // After a background error, we don't know whether a new version may or may not have been commited.
        // So we cannot safely garbage collect.
        return;
    }

    // Make a set of all of the live files
    // live is the newly created file.
    std::set<uint64_t> live = pending_outputs_;
    versions_->AddLiveFiles(&live);

    std::vector<std::string> filenames;
    env_->GetChildren(dbname_, &filenames);
    uint64_t number;
    FileType type;
    std::vector<std::string> files_to_delete;
    for (std::string& filename : filenames) {
        if (ParseFileName(filename, &number, &type)) {
            bool keep = true;
            switch (type) {
                case kLogFile:
                    keep = ((number >= versions_->LogNumber()) || number == versions_->PrevLogNumber());
                    break;
                case kDescriptorFile:
                    keep = (number >= versions_->ManifestFileNumber());
                    break;
                case kTableFile:
                    keep = (live.find(number) != live.end());
                    break;
                case kTempFile:
                    keep = (live.find(number) != live.end());
                    break;
                case kCurrentFile:
                case kDBLockFile:
                case kInfoLogFile:
                    keep = true;
                    break;
            }

            if (!keep) {
                files_to_delete.push_back(std::move(filename));
                if (type == kTableFile) {
                    table_cache_->Evict(number);
                }
                Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
                    static_cast<unsigned long long>(number));
            }
        }
    }

    // While deleting all files unblock other threads, all files being deleted have unique names,
    // which will not collide with newly created files.
    // there are therefore safe to delete while allowing other threads to proceed.
    mutex_.Unlock();
    for (const std::string& filename : files_to_delete) {
        env_->RemoveFile(dbname_ + "/" + filename);
    }
    mutex_.Lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool *save_manifest) {
    mutex_.AssertHeld();

    // Ingore err from CreateDir since the creation of the DB is committed only when the descriptor is created.
    // This directory may already exist from a prexious failed creation attempt.
    env_->CreateDir(dbname_);
    assert(db_lock_ == nullptr);
    // Lock the current database file to prevent other processes from accessing it.
    Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
    if (!s.ok()) {
        return s;
    }

    // Check if there is a CURRENTFILE, if not, create it.
    if (!env_->FileExists(CurrentFileName(dbname_))) {
        if (options_.create_if_missing) {
            Log(options_.info_log, "Creating DB %s since it was missing.", dbname_.c_str());
            s = NewDB();
            if (!s.ok()) {
                return s;
            }
        } else {
            return Status::InvalidArgument(dbname_, "dose not exist (create_if_missing is false)");
        }
    } else {
        if (options_.error_if_exits) {
            return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
        }
    }

    s = versions_->Recover(save_manifest);
    if (!s.ok()) {
        return s;
    }
    SequenceNumber max_sequence(0);

    // Recover from all newer log files than the ones named in the descriptor(manifest file).
    // New log files may have been added by the previous incarnation without registering them in the descriptor(manifest file).
    const uint64_t min_log = versions_->LogNumber();
    const uint64_t prev_log = versions_->PrevLogNumber();
    std::vector<std::string> filenames;
    s = env_->GetChildren(dbname_, &filenames);
    if (!s.ok()) {
        return s;
    }
    std::set<uint64_t> expected;
    versions_->AddLiveFiles(&expected);
    uint64_t number;
    FileType type;
    std::vector<uint64_t> logs;
    for (size_t i = 0; i < filenames.size(); i++) {
        if (ParseFileName(filenames[i], &number, &type)) {
            expected.erase(number);
            if (type == kLogFile && ((number >= min_log) || (number == prev_log))) {
                logs.push_back(number);
            }
        }
    }
    // Files are missing, startup refused
    if (!expected.empty()) {
        char buf[50];
        std::snprintf(buf, sizeof(buf), "%d missing files; e.g.", static_cast<int>(expected.size()));
        return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
    }

    // Recover in the order in which the logs were generated
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i < logs.size(); i++) {
        s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit, &max_sequence);
        if (!s.ok()) {
            return s;
        }

        // The previous incarnation may not have written any MANIFEST records after allocating this log nubmer.
        // So wen manually update the file number allocation counter in VersionSet.
        versions_->MarkFileNumberUsed(logs[i]);
    }

    if (versions_->LastSequence() < max_sequence) {
        versions_->SetLastSequence(max_sequence);
    }

    return Status::OK();
}
    

}