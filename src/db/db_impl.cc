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

Status DBImpl::RecoverLogFile(uint64_t log_nubmer, bool last_log, bool* save_manifest,
                              VersionEdit* edit, SequenceNumber* max_sequence) {
    struct LogReporter : public log::Reader::Reporter {
        Env* env;
        Logger* info_log;
        const char* fname;
        Status* status;
        void Corruption(size_t bytes, const Status& s) override {
            Log(info_log, "%s%s : dropping %d bytes; %s", (this->status == nullptr ? "(ingoring error) " : ""),
                fname, static_cast<int>(bytes), s.ToString().c_str());
            if (this->status != nullptr && this->status->ok()) *this->status = s;
        }
    };

    mutex_.AssertHeld();

    // Open the log file
    std::string fname = LogFileName(dbname_, log_nubmer);
    SequentialFile* file;
    Status status = env_->NewSequentialFile(fname, &file);
    if (!status.ok()) {
        MaybeIgnoreError(&status);
        return status;
    }

    // Create the log reader
    LogReporter reporter;
    reporter.env = env_;
    reporter.info_log = options_.info_log;
    reporter.fname = fname.c_str();
    reporter.status = (options_.paranoid_checks ? &status : nullptr);

    log::Reader reader(file, &reporter, true, 0);
    Log(options_.info_log, "Recovering log #%llu", (unsigned long long)log_nubmer);

    // Read all the records and add to a memtable
    std::string scratch;
    Slice record;
    WriteBatch batch;
    int compactions = 0;
    MemTable* mem = nullptr;
    while (reader.ReaderRecord(&record, &scratch) && status.ok()) {
        if (record.size() < 12) {
            reporter.Corruption(record.size(), Status::Corruption("log record too small"));
            continue;
        }
        WriteBatchInternal::SetContents(&batch, record);

        if (mem == nullptr) {
            mem = new MemTable(internal_comparator_);
            mem->Ref();
        }
        status = WriteBatchInternal::InsertInto(&batch, mem);
        MaybeIgnoreError(&status);
        if (!status.ok()) {
            break;
        }
        const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
        if (last_log > *max_sequence) {
            *max_sequence = last_seq;
        }

        if (mem->ApproximateMemoeyUsage() > options_.write_buffer_size) {
            compactions++;
            *save_manifest = true;
            status = WriteLevel0Table(mem, edit, nullptr);
            mem->Unref();
            mem = nullptr;
            if (!status.ok()) {
                // Reflect errors immediately so that condition like full file-systems cause the DB::Open() to fail
                break;   
            }
        }
    }

    delete file;

    // See if we should keep reusing the last log file
    if (status.ok() && options_.reuse_logs && last_log && compactions == 0)  {
        assert(logfile_ == nullptr);
        assert(log_ == nullptr);
        assert(mem_ == nullptr);
        uint64_t lfile_size;
        if (env_->GetFileSize(fname, &lfile_size).ok() && env_->NewAppendableFile(fname, &logfile_).ok()) {
            Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
            log_ = new log::Writer(logfile_, lfile_size);
            logfile_number_ = log_nubmer;
            if (mem != nullptr) {
                // When mem is reused and is not empty.
                // mem is directly converted to the currently used mem_.
                mem_ = mem;
                mem = nullptr;
            } else {
                // mem can be nullptr if log exists but was empty
                mem_ = new MemTable(internal_comparator_);
                mem_->Ref();
            }
        }
    }

    if (mem != nullptr) {
        // mem did not get reused; compact it
        if (status.ok()) {
            *save_manifest = true;
            status = WriteLevel0Table(mem, edit, nullptr);
        }
        mem->Unref();
    }

    return status;
}
    
Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base) {
    mutex_.AssertHeld();
    const uint64_t start_micros = env_->NowMicros();
    FileMetaData meta;
    meta.number = versions_->NewFileNumber();
    pending_outputs_.insert(meta.number);
    Iterator* iter = mem->NewIterator();
    Log(options_.info_log, "Level-0 table #%llu: started", (unsigned long long)meta.number);

    Status s;
    {
        mutex_.Unlock();
        s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
        mutex_.Lock();
    }

    Log(options_.info_log, "Level-0 table #%llu: %llu bytes %s", (unsigned long long)meta.number, 
        (unsigned long long)meta.file_size, s.ToString().c_str());
    delete iter;
    pending_outputs_.erase(meta.number);

    // Note that if file_size is zero, the file has been deleted and should not be added to the manifest
    int level = 0;
    if (s.ok() && meta.file_size > 0) {
        const Slice min_user_key = meta.smallest.user_key();
        const Slice max_user_key = meta.largest.user_key();
        if (base != nullptr) {
            level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
        }
        edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
    }

    CompactionStats stats;
    stats.micros = env_->NowMicros() - start_micros;
    stats.bytes_written = meta.file_size;
    stats_[level].Add(stats);
    return s;
}

void DBImpl::CompactMemTable() {
    mutex_.AssertHeld();
    assert(imm_ != nullptr);

    VersionEdit edit;
    Version* base = versions_->current();
    base->Ref();
    Status s = WriteLevel0Table(imm_, &edit, base);
    base->Unref();

    if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
        s = Status::IOError("Deleting DB during memtable compaction");
    }

    if (s.ok()) {
        edit.SetPrevLogNumber(0);
        edit.SetLogNumber(logfile_number_);
        s = versions_->LogAndApply(&edit, &mutex_);
    }

    if (s.ok()) {
        imm_->Unref();
        imm_ = nullptr;
        has_imm_.store(false, std::memory_order_release);
        RemoveObsoleteFiles();
    } else {
        RecordBackgroundError(s);
    }
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
    int max_level_with_files = 1;
    {
        MutexLock l(&mutex_);
        Version* base = versions_->current();
        for (int level = 1; level < config::kNumLevels; level++) {
            if (base->OverlapInLevel(level, begin, end)) {
                max_level_with_files = level;
            }
        }
    }
    TEST_CompactMemTable();
    for (int level = 0; level < max_level_with_files; level++) {
        TEST_CompactRange(level, begin, end);
    }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin, const Slice* end) {
    assert(level >= 0);
    assert(level + 1 < config::kNumLevels);

    // Extend life cycle
    InternalKey begin_storage, end_storage;

    ManualCompaction manual;
    manual.level = level;
    manual.done = false;
    if (begin == nullptr) {
        manual.begin = nullptr;
    } else {
        // To cover a wider area
        begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
        manual.begin = &begin_storage;
    }
    if (end == nullptr) {
        manual.end = nullptr;
    } else {
        // To cover a wider area
        end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
        manual.end = &end_storage;
    }

    MutexLock l(&mutex_);
    while (!manual.done && !shutting_down_.load(std::memory_order_acquire) && bg_error_.ok()) {
        if (manual_compaction_ == nullptr) {
            manual_compaction_ = &manual;
            MaybeScheduleCompaction();
        } else {
            // Running either my compaction or another compaction
            background_work_finished_signal_.Wait();
        }
    }


    // Regardless of whether the execution is successful or not.
    // The following protective measures must be taken.

    // Finish current background compaction in the case 
    // where 'background_work_finished_signal' was signalled due to an error.
    while (background_compaction_scheduled_) {
        background_work_finished_signal_.Wait();
    }
    if (manual_compaction_ == &manual) {
        // Cancel my manual compaction since we aborted early for some reason.
        manual_compaction_ = nullptr;
    }
}

Status DBImpl::TEST_CompactMemTable() {
    // nullptr batch means just wait for earlier writes to be done
    Status s = Write(WriteOptions(), nullptr);
    if (s.ok()) {
        MutexLock l(&mutex_);
        while (imm_ != nullptr && bg_error_.ok() && !shutting_down_.load(std::memory_order_acquire)) {
            background_work_finished_signal_.Wait();
        }
        if (imm_ != nullptr) {
            s = bg_error_;
        }
    }
    return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
    mutex_.AssertHeld();
    if (bg_error_.ok()) {
        bg_error_ = s;
        background_work_finished_signal_.SignalAll();
    }
}

void DBImpl::MaybeScheduleCompaction() {
    mutex_.AssertHeld();
    if (background_compaction_scheduled_) {

    } else if (shutting_down_.load(std::memory_order_acquire)) {

    } else if (!bg_error_.ok()) {

    } else if (imm_ != nullptr && manual_compaction_ == nullptr && !versions_->NeedsCompaction()) {

    } else {
        background_compaction_scheduled_ = true;
        env_->Schedule(&DBImpl::BGWork, this);
    }
}

}