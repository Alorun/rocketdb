#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "c.h"
#include "cache.h"
#include "db.h"
#include "db/builder.h"
#include "db/db_impl.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/perf_counters.h"
#include "db/table_cache.h"
#include "env.h"
#include "util/env_posix_test_helper.h"
#include "util/random.h"
#include "filter_policy.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"
#include "write_batch.h"

namespace rocketdb {

class EnvPosixTest {
 public:
  static void DisableMMapForTests() { EnvPosixTestHelper::SetReadOnlyMMapLimit(0); }
};

}  // namespace rocketdb

namespace {

namespace fs = std::filesystem;

class TestFailure : public std::runtime_error {
 public:
  explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void Fail(const char* expression, const char* file, int line) {
  throw TestFailure(std::string(file) + ":" + std::to_string(line) + ": " + expression);
}

#define CHECK(condition) \
  do { \
    if (!(condition)) Fail(#condition, __FILE__, __LINE__); \
  } while (false)

void CheckOk(const rocketdb::Status& status, const std::string& context) {
  if (!status.ok()) {
    throw TestFailure(context + ": " + status.ToString());
  }
}

std::map<std::string, uint64_t> ParseNumericProperty(const std::string& property) {
  std::map<std::string, uint64_t> result;
  size_t position = 0;
  while (position < property.size()) {
    const size_t line_end = property.find('\n', position);
    CHECK(line_end != std::string::npos);
    const std::string line = property.substr(position, line_end - position);
    const size_t separator = line.find('=');
    CHECK(separator != std::string::npos);
    CHECK(separator > 0);
    CHECK(line.find('=', separator + 1) == std::string::npos);
    const std::string key = line.substr(0, separator);
    const std::string encoded_value = line.substr(separator + 1);
    CHECK(!encoded_value.empty());
    size_t parsed = 0;
    const uint64_t value = std::stoull(encoded_value, &parsed);
    CHECK(parsed == encoded_value.size());
    CHECK(result.emplace(key, value).second);
    position = line_end + 1;
  }
  return result;
}

std::map<std::string, uint64_t> GetNumericProperty(rocketdb::DB* db,
                                                   const std::string& name) {
  std::string property;
  CHECK(db->GetProperty(name, &property));
  return ParseNumericProperty(property);
}

std::string NewDatabasePath(const std::string& test_name) {
  static std::atomic<uint64_t> sequence{0};
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
  return (fs::temp_directory_path() /
          ("rocketdb-regression-" + test_name + "-" + std::to_string(now) + "-" +
           std::to_string(id)))
      .string();
}

rocketdb::Options TestOptions(const rocketdb::FilterPolicy* filter_policy) {
  rocketdb::Options options;
  options.create_if_missing = true;
  options.paranoid_checks = true;
  // The implementation clips this to 64 KiB.  Keeping it small lets the
  // integration tests exercise MemTable flushes without using much disk.
  options.write_buffer_size = 64 << 10;
  options.max_file_size = 1 << 20;
  options.compression = rocketdb::kNoCompression;
  options.filter_policy = filter_policy;
  return options;
}

class DBFixture {
 public:
  explicit DBFixture(const std::string& test_name)
      : filter_policy_(rocketdb::NewBloomFilterPolicy(10)),
        options_(TestOptions(filter_policy_)),
        dbname_(NewDatabasePath(test_name)),
        db_(nullptr) {}

  DBFixture(const DBFixture&) = delete;
  DBFixture& operator=(const DBFixture&) = delete;

  ~DBFixture() {
    Close();
    (void)rocketdb::DestroyDB(dbname_, options_);
    std::error_code error;
    fs::remove_all(dbname_, error);
    delete filter_policy_;
  }

  void Open() {
    CHECK(db_ == nullptr);
    CheckOk(rocketdb::DB::Open(options_, dbname_, &db_), "open database");
  }

  void Reopen() {
    Close();
    Open();
  }

  void Close() {
    delete db_;
    db_ = nullptr;
  }

  rocketdb::DB* db() const {
    CHECK(db_ != nullptr);
    return db_;
  }

  const std::string& dbname() const { return dbname_; }

  rocketdb::Options* mutable_options() {
    CHECK(db_ == nullptr);
    return &options_;
  }

 private:
  const rocketdb::FilterPolicy* filter_policy_;
  rocketdb::Options options_;
  std::string dbname_;
  rocketdb::DB* db_;
};

class CountingEnv : public rocketdb::EnvWrapper {
 public:
  CountingEnv() : rocketdb::EnvWrapper(rocketdb::Env::Default()), now_micros_calls_(0) {}

  uint64_t NowMicros() override {
    now_micros_calls_.fetch_add(1, std::memory_order_relaxed);
    return target()->NowMicros();
  }

  uint64_t now_micros_calls() const {
    return now_micros_calls_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> now_micros_calls_;
};

class FlushTestEnv : public rocketdb::EnvWrapper {
 public:
  FlushTestEnv() : rocketdb::EnvWrapper(rocketdb::Env::Default()) {}

  void OnNextTable(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

  rocketdb::Status NewWritableFile(const std::string& name,
                                   rocketdb::WritableFile** file) override {
    const std::string extension = fs::path(name).extension().string();
    const bool is_table = extension == ".ldb" || extension == ".sst";
    if (is_table) {
      std::function<void()> callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = std::move(callback_);
        callback_ = nullptr;
      }
      // BuildTable calls this with the DB mutex released, after the snapshot
      // bound has been captured. Exercise snapshot changes at that exact point.
      if (callback) callback();
    }
    auto status = target()->NewWritableFile(name, file);
    if (status.ok() && is_table && fail_table_sync.load()) {
      *file = new SyncErrorFile(*file);
    }
    return status;
  }

  std::atomic<bool> fail_table_sync{false};

 private:
  class SyncErrorFile : public rocketdb::WritableFile {
   public:
    explicit SyncErrorFile(rocketdb::WritableFile* file) : file_(file) {}
    rocketdb::Status Append(const rocketdb::Slice& data) override {
      return file_->Append(data);
    }
    rocketdb::Status Flush() override { return file_->Flush(); }
    rocketdb::Status Close() override { return file_->Close(); }
    rocketdb::Status Sync() override {
      return rocketdb::Status::IOError("injected SST sync failure");
    }

   private:
    std::unique_ptr<rocketdb::WritableFile> file_;
  };

  std::mutex mutex_;
  std::function<void()> callback_;
};

using SnapshotPtr =
    std::unique_ptr<const rocketdb::Snapshot,
                    std::function<void(const rocketdb::Snapshot*)>>;

SnapshotPtr TakeSnapshot(rocketdb::DB* db) {
  return SnapshotPtr(db->GetSnapshot(),
                     [db](const rocketdb::Snapshot* snapshot) {
                       db->ReleaseSnapshot(snapshot);
                     });
}

struct InternalEntry {
  std::string key;
  std::string value;
  rocketdb::SequenceNumber sequence;
  rocketdb::ValueType type;
};

std::vector<InternalEntry> ReadInternalEntries(rocketdb::Iterator* iterator) {
  std::vector<InternalEntry> entries;
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    rocketdb::ParsedInternalKey key;
    CHECK(rocketdb::ParseInternalKey(iterator->key(), &key));
    entries.push_back({key.user_key.ToString(), iterator->value().ToString(),
                       key.sequence, key.type});
  }
  CheckOk(iterator->status(), "read internal records");
  return entries;
}

std::vector<InternalEntry> ReadInternalEntries(rocketdb::DB* db) {
  std::unique_ptr<rocketdb::Iterator> iterator(
      static_cast<rocketdb::DBImpl*>(db)->TEST_NewInternalIterator());
  return ReadInternalEntries(iterator.get());
}

struct IteratorOperationCounts {
  size_t next = 0;
  size_t prev = 0;
  std::vector<std::string> seek_targets;
  bool fail_seek = false;
  bool ignore_seek = false;
};

class CountingIterator : public rocketdb::Iterator {
 public:
  CountingIterator(rocketdb::Iterator* child, IteratorOperationCounts* counts)
      : child_(child), counts_(counts) {}

  bool Valid() const override { return !seek_error_ && child_->Valid(); }
  void SeekToFirst() override { child_->SeekToFirst(); }
  void SeekToLast() override { child_->SeekToLast(); }
  void Seek(const rocketdb::Slice& target) override {
    counts_->seek_targets.push_back(target.ToString());
    if (counts_->fail_seek) {
      seek_error_ = true;
    } else if (!(counts_->ignore_seek && Valid())) {
      child_->Seek(target);
    }
  }
  void Next() override {
    ++counts_->next;
    child_->Next();
  }
  void Prev() override {
    ++counts_->prev;
    child_->Prev();
  }
  rocketdb::Slice key() const override { return child_->key(); }
  rocketdb::Slice value() const override { return child_->value(); }
  rocketdb::Status status() const override {
    return seek_error_ ? rocketdb::Status::IOError("injected reseek failure")
                       : child_->status();
  }

 private:
  std::unique_ptr<rocketdb::Iterator> child_;
  IteratorOperationCounts* counts_;
  bool seek_error_ = false;
};

std::unique_ptr<rocketdb::Iterator> NewCountedDBIterator(
    rocketdb::DB* db, const std::vector<InternalEntry>& entries,
    rocketdb::SequenceNumber snapshot, size_t threshold,
    IteratorOperationCounts* counts,
    const rocketdb::Comparator* comparator = rocketdb::BytewiseComparator()) {
  auto unref = [](rocketdb::MemTable* mem) { mem->Unref(); };
  std::unique_ptr<rocketdb::MemTable, decltype(unref)> mem(
      new rocketdb::MemTable(rocketdb::InternalKeyComparator(comparator)),
      unref);
  mem->Ref();
  for (const auto& entry : entries) {
    mem->Add(entry.sequence, entry.type, entry.key, entry.value);
  }
  std::unique_ptr<rocketdb::Iterator> internal(mem->NewIterator());
  internal->RegisterCleanup(
      [](void* arg, void*) { static_cast<rocketdb::MemTable*>(arg)->Unref(); },
      mem.release(), nullptr);
  return std::unique_ptr<rocketdb::Iterator>(
      rocketdb::NewDBIterator(static_cast<rocketdb::DBImpl*>(db), comparator,
                              new CountingIterator(internal.release(), counts),
                              snapshot, 123, threshold));
}

using UserEntries = std::vector<std::pair<std::string, std::string>>;

UserEntries CollectEntries(rocketdb::Iterator* iterator, bool reverse) {
  UserEntries entries;
  if (reverse)
    iterator->SeekToLast();
  else
    iterator->SeekToFirst();
  while (iterator->Valid()) {
    entries.emplace_back(iterator->key().ToString(),
                         iterator->value().ToString());
    if (reverse)
      iterator->Prev();
    else
      iterator->Next();
  }
  CheckOk(iterator->status(), "collect iterator entries");
  return entries;
}

void CheckIteratorPosition(rocketdb::Iterator* reference,
                           rocketdb::Iterator* actual) {
  CHECK(reference->Valid() == actual->Valid());
  CHECK(reference->status().ToString() == actual->status().ToString());
  if (reference->Valid()) {
    CHECK(reference->key() == actual->key());
    CHECK(reference->value() == actual->value());
  }
}

void CheckIteratorTrace(rocketdb::Iterator* reference,
                        rocketdb::Iterator* actual) {
  CHECK(CollectEntries(reference, false) == CollectEntries(actual, false));
  CHECK(CollectEntries(reference, true) == CollectEntries(actual, true));
  rocketdb::Random random(2026);
  const std::vector<std::string> targets = {
      "", "a", "A", "b", "c", "missing", std::string("k\0x", 3), "z", "zz"};
  for (int step = 0; step < 500; ++step) {
    const auto operation = random.Uniform(5);
    if (operation == 0) {
      const auto& target = targets[random.Uniform(targets.size())];
      reference->Seek(target);
      actual->Seek(target);
    } else if (operation == 1 || (operation == 3 && !reference->Valid())) {
      reference->SeekToFirst();
      actual->SeekToFirst();
    } else if (operation == 2 || (operation == 4 && !reference->Valid())) {
      reference->SeekToLast();
      actual->SeekToLast();
    } else if (operation == 3) {
      reference->Next();
      actual->Next();
    } else {
      reference->Prev();
      actual->Prev();
    }
    CheckIteratorPosition(reference, actual);
  }
}

void ExpectSnapshotValue(rocketdb::DB* db, const rocketdb::Snapshot* snapshot,
                         const std::string& key, const std::string& expected) {
  rocketdb::ReadOptions options;
  options.snapshot = snapshot;
  std::string value;
  CheckOk(db->Get(options, key, &value), "read flush snapshot");
  CHECK(value == expected);
}

std::string Key(int index) {
  std::string number = std::to_string(index);
  return "key-" + std::string(4 - number.size(), '0') + number;
}

void ExpectValue(rocketdb::DB* db, const std::string& key, const std::string& expected) {
  std::string actual;
  CheckOk(db->Get(rocketdb::ReadOptions(), key, &actual), "get key " + key);
  if (actual != expected) {
    throw TestFailure("unexpected value for key " + key);
  }
}

void ExpectMissing(rocketdb::DB* db, const std::string& key) {
  std::string value;
  const rocketdb::Status status = db->Get(rocketdb::ReadOptions(), key, &value);
  if (!status.IsNotFound()) {
    throw TestFailure("expected missing key " + key + ", got " + status.ToString());
  }
}

std::vector<std::string> CollectKeys(rocketdb::Iterator* iterator, bool reverse) {
  std::vector<std::string> keys;
  if (reverse) {
    iterator->SeekToLast();
  } else {
    iterator->SeekToFirst();
  }
  while (iterator->Valid()) {
    keys.push_back(iterator->key().ToString());
    if (reverse) {
      iterator->Prev();
    } else {
      iterator->Next();
    }
  }
  CheckOk(iterator->status(), "iterate database");
  return keys;
}

bool HasTableFile(const std::string& dbname) {
  for (const fs::directory_entry& entry : fs::directory_iterator(dbname)) {
    if (entry.path().extension() == ".ldb" || entry.path().extension() == ".sst") {
      return true;
    }
  }
  return false;
}

void WriteFlushRound(rocketdb::DB* db, int round) {
  for (int i = 0; i < 8; ++i) {
    const std::string value = "round-" + std::to_string(round) + "-" +
                              std::string(4 * 1024, 'a' + (round % 26));
    CheckOk(db->Put(rocketdb::WriteOptions(), Key(i), value), "put flush round");
  }
  db->CompactRange(nullptr, nullptr);
}

void DeleteCacheTestValue(const rocketdb::Slice&, void* value) {
  delete reinterpret_cast<int*>(value);
}

void TestBasicCrudAndBinaryValues() {
  DBFixture fixture("basic");
  fixture.Open();

  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "alpha", "one"), "put alpha");
  ExpectValue(fixture.db(), "alpha", "one");

  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "alpha", "two"), "overwrite alpha");
  ExpectValue(fixture.db(), "alpha", "two");

  const std::string binary_key("key\0with\0nul", 12);
  const std::string binary_value("value\0with\0nul", 14);
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), binary_key, binary_value), "put binary value");
  ExpectValue(fixture.db(), binary_key, binary_value);

  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "empty", ""), "put empty value");
  ExpectValue(fixture.db(), "empty", "");

  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), "alpha"), "delete alpha");
  ExpectMissing(fixture.db(), "alpha");
}

void TestWriteBatchAndWalRecovery() {
  DBFixture fixture("batch-wal");
  fixture.Open();

  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "removed", "old"), "seed delete key");
  rocketdb::WriteBatch batch;
  batch.Put("first", "one");
  batch.Put("second", "two");
  batch.Delete("removed");

  rocketdb::WriteOptions write_options;
  write_options.sync = true;
  CheckOk(fixture.db()->Write(write_options, &batch), "write batch");
  fixture.Reopen();

  ExpectValue(fixture.db(), "first", "one");
  ExpectValue(fixture.db(), "second", "two");
  ExpectMissing(fixture.db(), "removed");
}

void TestSnapshots() {
  DBFixture fixture("snapshot");
  fixture.Open();

  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "account", "v1"), "put snapshot seed");
  const rocketdb::Snapshot* snapshot = fixture.db()->GetSnapshot();
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "account", "v2"), "update after snapshot");
  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), "account"), "delete after snapshot");

  rocketdb::ReadOptions snapshot_options;
  snapshot_options.snapshot = snapshot;
  std::string value;
  CheckOk(fixture.db()->Get(snapshot_options, "account", &value), "read snapshot");
  CHECK(value == "v1");
  ExpectMissing(fixture.db(), "account");
  fixture.db()->ReleaseSnapshot(snapshot);
}

void TestFlushPruningBuildTable() {
  DBFixture fixture("flush-pruning-table");
  CHECK(fs::create_directory(fixture.dbname()));
  rocketdb::InternalKeyComparator comparator(rocketdb::BytewiseComparator());
  auto options = *fixture.mutable_options();
  options.comparator = &comparator;
  options.filter_policy = nullptr;
  rocketdb::TableCache cache(fixture.dbname(), options, 16);
  auto unref = [](rocketdb::MemTable* mem) { mem->Unref(); };
  std::unique_ptr<rocketdb::MemTable, decltype(unref)> mem(
      new rocketdb::MemTable(comparator), unref);
  mem->Ref();
  const std::string value(4096, 'v');
  for (auto sequence : {10, 20, 30}) {
    mem->Add(sequence, rocketdb::kTypeValue, "a", value);
  }
  mem->Add(rocketdb::kMaxSequenceNumber - 1, rocketdb::kTypeValue, "m", value);
  mem->Add(rocketdb::kMaxSequenceNumber, rocketdb::kTypeValue, "m", value);
  mem->Add(40, rocketdb::kTypeValue, "z", value);
  mem->Add(50, rocketdb::kTypeDeletion, "z", "");
  mem->Add(60, rocketdb::kTypeValue, "z", value);
  std::unique_ptr<rocketdb::Iterator> input(mem->NewIterator());
  auto build = [&](uint64_t number,
                   const rocketdb::FlushPruningOptions* pruning) {
    rocketdb::FileMetaData meta;
    meta.number = number;
    CheckOk(rocketdb::BuildTable(fixture.dbname(), options.env, options, &cache,
                                 input.get(), &meta, pruning),
            "build flush SST");
    CHECK(fs::file_size(rocketdb::TableFileName(fixture.dbname(), number)) ==
          meta.file_size);
    return meta;
  };
  auto read = [&](const rocketdb::FileMetaData& meta) {
    std::unique_ptr<rocketdb::Iterator> iterator(cache.NewIterator(
        rocketdb::ReadOptions(), meta.number, meta.file_size));
    return ReadInternalEntries(iterator.get());
  };
  const auto raw = build(1, nullptr);
  CHECK(read(raw).size() == 8);
  uint64_t number = 2;
  const std::vector<std::pair<rocketdb::SequenceNumber, size_t>> cases = {
      {0, 8}, {20, 7}, {25, 7}, {55, 5}, {rocketdb::kMaxSequenceNumber, 3}};
  for (const auto& test : cases) {
    const rocketdb::FlushPruningOptions pruning = {
        rocketdb::BytewiseComparator(), test.first};
    const auto pruned = build(number++, &pruning);
    const auto entries = read(pruned);
    CHECK(entries.size() == test.second);
    CHECK(pruned.smallest.Encode() == raw.smallest.Encode());
    rocketdb::ParsedInternalKey largest;
    CHECK(rocketdb::ParseInternalKey(pruned.largest.Encode(), &largest));
    CHECK(largest.user_key == "z");
    CHECK(largest.sequence == entries.back().sequence);
    if (test.first == 55) {
      CHECK(entries.back().sequence == 50);
      CHECK(entries.back().type == rocketdb::kTypeDeletion);
    }
    if (test.first == rocketdb::kMaxSequenceNumber) {
      CHECK(entries[1].sequence == rocketdb::kMaxSequenceNumber);
      CHECK(entries.back().sequence == 60);
      CHECK(pruned.file_size < raw.file_size);
      std::cout << "[INFO] flush pruning: 8 -> 3 records, " << raw.file_size
                << " -> " << pruned.file_size
                << " SST bytes (block compression disabled)\n";
    }
  }

  const rocketdb::FlushPruningOptions pruning = {rocketdb::BytewiseComparator(),
                                                 0};
  rocketdb::FileMetaData empty;
  empty.number = number++;
  std::unique_ptr<rocketdb::Iterator> empty_input(rocketdb::NewEmptyIterator());
  CheckOk(rocketdb::BuildTable(fixture.dbname(), options.env, options, &cache,
                               empty_input.get(), &empty, &pruning),
          "build empty flush");
  CHECK(empty.file_size == 0);
  CHECK(!fs::exists(rocketdb::TableFileName(fixture.dbname(), empty.number)));
  std::unique_ptr<rocketdb::Iterator> error_input(rocketdb::NewErrorIterator(
      rocketdb::Status::IOError("injected iterator error")));
  CHECK(rocketdb::BuildTable(fixture.dbname(), options.env, options, &cache,
                             error_input.get(), &empty, &pruning)
            .IsIOError());
  CHECK(!fs::exists(rocketdb::TableFileName(fixture.dbname(), empty.number)));
}

void TestFlushPruningWithoutSnapshots() {
  DBFixture fixture("flush-pruning-no-snapshots");
  fixture.Open();
  const std::string binary_key("k\0x", 3);
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "", "old-empty"),
          "put empty key");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), binary_key, "old-binary"),
          "put binary key");
  std::unique_ptr<rocketdb::Iterator> old_iterator(
      fixture.db()->NewIterator(rocketdb::ReadOptions()));
  for (int version = 0; version < 8; ++version) {
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "", "new-empty"),
            "overwrite empty key");
    CheckOk(
        fixture.db()->Put(rocketdb::WriteOptions(), binary_key, "new-binary"),
        "overwrite binary key");
  }
  CHECK(ReadInternalEntries(fixture.db()).size() == 18);
  CheckOk(static_cast<rocketdb::DBImpl*>(fixture.db())->TEST_CompactMemTable(),
          "flush versions");
  const auto entries = ReadInternalEntries(fixture.db());
  CHECK(entries.size() == 2);
  CHECK(entries[0].key.empty() && entries[0].value == "new-empty");
  CHECK(entries[1].key == binary_key && entries[1].value == "new-binary");
  old_iterator->SeekToFirst();
  CHECK(old_iterator->Valid() && old_iterator->value() == "old-empty");
  old_iterator->Next();
  CHECK(old_iterator->Valid() && old_iterator->value() == "old-binary");
  old_iterator->SeekToLast();
  CHECK(old_iterator->Valid() && old_iterator->value() == "old-binary");
  old_iterator->Prev();
  CHECK(old_iterator->Valid() && old_iterator->value() == "old-empty");
  CheckOk(old_iterator->status(), "read iterator pinned before flush");
  old_iterator.reset();
  fixture.Reopen();
  ExpectValue(fixture.db(), "", "new-empty");
  ExpectValue(fixture.db(), binary_key, "new-binary");
  CHECK(ReadInternalEntries(fixture.db()).size() == 2);
}

void TestFlushPruningSnapshots() {
  DBFixture fixture("flush-pruning-snapshots");
  fixture.Open();
  for (int version = 1; version <= 3; ++version) {
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "k",
                              "v" + std::to_string(version)),
            "put snapshot version");
  }
  auto oldest = TakeSnapshot(fixture.db());
  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), "k"),
          "delete between snapshots");
  auto deleted = TakeSnapshot(fixture.db());
  for (int version = 5; version <= 7; ++version) {
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "k",
                              "v" + std::to_string(version)),
            "put newer version");
  }
  CheckOk(static_cast<rocketdb::DBImpl*>(fixture.db())->TEST_CompactMemTable(),
          "flush snapshot versions");
  const auto entries = ReadInternalEntries(fixture.db());
  CHECK(entries.size() == 5);
  CHECK(entries.front().sequence == 7 && entries.back().sequence == 3);
  CHECK(entries[3].type == rocketdb::kTypeDeletion);
  ExpectSnapshotValue(fixture.db(), oldest.get(), "k", "v3");
  rocketdb::ReadOptions options;
  options.snapshot = deleted.get();
  std::string value;
  CHECK(fixture.db()->Get(options, "k", &value).IsNotFound());
  ExpectValue(fixture.db(), "k", "v7");
  std::unique_ptr<rocketdb::Iterator> iterator(
      fixture.db()->NewIterator(options));
  iterator->SeekToFirst();
  CHECK(!iterator->Valid());
  CheckOk(iterator->status(), "iterate deletion snapshot");
  iterator.reset();
  oldest.reset();
  deleted.reset();
  fixture.Reopen();
  ExpectValue(fixture.db(), "k", "v7");
  CHECK(ReadInternalEntries(fixture.db()).size() == 5);
}

void TestFlushPruningKeepsTombstones() {
  DBFixture fixture("flush-pruning-tombstone");
  fixture.Open();
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "removed", "disk-old"),
          "put disk value");
  auto* impl = static_cast<rocketdb::DBImpl*>(fixture.db());
  CheckOk(impl->TEST_CompactMemTable(), "flush disk value");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "removed", "memory-old"),
          "overwrite disk value");
  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), "removed"),
          "delete disk value");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "new", "temporary"),
          "put temporary value");
  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), "new"),
          "delete temporary value");
  CheckOk(impl->TEST_CompactMemTable(), "flush tombstones");
  const auto entries = ReadInternalEntries(fixture.db());
  CHECK(entries.size() == 3);
  CHECK(entries[0].key == "new" && entries[0].type == rocketdb::kTypeDeletion);
  CHECK(entries[1].key == "removed" &&
        entries[1].type == rocketdb::kTypeDeletion);
  CHECK(entries[2].value == "disk-old");
  ExpectMissing(fixture.db(), "removed");
  ExpectMissing(fixture.db(), "new");
  fixture.Reopen();
  ExpectMissing(fixture.db(), "removed");
  ExpectMissing(fixture.db(), "new");
}

class CaseInsensitiveComparator : public rocketdb::Comparator {
 public:
  int Compare(const rocketdb::Slice& a,
              const rocketdb::Slice& b) const override {
    auto fold = [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const int left = fold(static_cast<unsigned char>(a[i]));
      const int right = fold(static_cast<unsigned char>(b[i]));
      if (left != right) return left < right ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }
  const char* Name() const override { return "test.CaseInsensitiveComparator"; }
  void FindShortestSeparator(std::string*,
                             const rocketdb::Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};

void TestFlushPruningCustomComparator() {
  CaseInsensitiveComparator comparator;
  DBFixture fixture("flush-pruning-comparator");
  fixture.mutable_options()->comparator = &comparator;
  // The bytewise Bloom policy cannot be used with case-insensitive equality.
  fixture.mutable_options()->filter_policy = nullptr;
  fixture.Open();
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "Key", "v1"),
          "put equivalent key");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "KEY", "v2"),
          "overwrite equivalent key");
  auto snapshot = TakeSnapshot(fixture.db());
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "keY", "v3"),
          "overwrite after snapshot");
  CheckOk(static_cast<rocketdb::DBImpl*>(fixture.db())->TEST_CompactMemTable(),
          "flush equivalent keys");
  const auto entries = ReadInternalEntries(fixture.db());
  CHECK(entries.size() == 2);
  CHECK(entries[0].key == "keY" && entries[1].key == "KEY");
  ExpectSnapshotValue(fixture.db(), snapshot.get(), "key", "v2");
  ExpectValue(fixture.db(), "key", "v3");
  snapshot.reset();
  fixture.Reopen();
  ExpectValue(fixture.db(), "key", "v3");
}

void TestFlushPruningSnapshotChangesDuringBuild() {
  for (bool with_old_snapshot : {false, true}) {
    FlushTestEnv env;
    DBFixture fixture("flush-pruning-snapshot-changes");
    fixture.mutable_options()->env = &env;
    fixture.Open();
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "k", "v1"),
            "put initial version");
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "k", "v2"),
            "overwrite initial version");
    auto old_snapshot = TakeSnapshot(fixture.db());
    if (!with_old_snapshot) old_snapshot.reset();
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "k", "v3"),
            "put newer version");
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "k", "v4"),
            "put latest flush version");
    SnapshotPtr new_snapshot;
    rocketdb::Status write_status;
    bool called = false;
    env.OnNextTable([&] {
      new_snapshot = TakeSnapshot(fixture.db());
      old_snapshot.reset();
      write_status = fixture.db()->Put(rocketdb::WriteOptions(), "k", "v5");
      called = true;
    });
    CheckOk(
        static_cast<rocketdb::DBImpl*>(fixture.db())->TEST_CompactMemTable(),
        "flush during snapshot changes");
    CHECK(called);
    CheckOk(write_status, "write while flush builds SST");
    CHECK(new_snapshot != nullptr);
    ExpectSnapshotValue(fixture.db(), new_snapshot.get(), "k", "v4");
    ExpectValue(fixture.db(), "k", "v5");
    const auto entries = ReadInternalEntries(fixture.db());
    CHECK(entries.size() == (with_old_snapshot ? 4 : 2));
    CHECK(entries.back().sequence == (with_old_snapshot ? 2 : 4));
  }
}

void TestFlushPruningSyncFailureAndRecovery() {
  FlushTestEnv env;
  DBFixture fixture("flush-pruning-sync-failure");
  fixture.mutable_options()->env = &env;
  fixture.Open();
  rocketdb::WriteOptions options;
  options.sync = true;
  CheckOk(fixture.db()->Put(options, "k", "v1"), "put durable old value");
  CheckOk(fixture.db()->Put(options, "k", "v2"), "put durable new value");
  env.fail_table_sync.store(true);
  CHECK(static_cast<rocketdb::DBImpl*>(fixture.db())
            ->TEST_CompactMemTable()
            .IsIOError());
  ExpectValue(fixture.db(), "k", "v2");
  CHECK(ReadInternalEntries(fixture.db()).size() == 2);
  env.fail_table_sync.store(false);
  fixture.Reopen();
  ExpectValue(fixture.db(), "k", "v2");
  // Recovery uses the unfiltered builder and replays both durable versions.
  CHECK(ReadInternalEntries(fixture.db()).size() == 2);
}

void TestIteratorDirectionsAndSeek() {
  DBFixture fixture("iterator");
  fixture.Open();

  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "a", "1"), "put a");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "b", "old"), "put b old");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "b", "new"), "put b new");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "c", "3"), "put c");
  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), "c"), "delete c");
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "d", "4"), "put d");

  rocketdb::Iterator* iterator = fixture.db()->NewIterator(rocketdb::ReadOptions());
  CHECK(CollectKeys(iterator, false) == std::vector<std::string>({"a", "b", "d"}));
  CHECK(CollectKeys(iterator, true) == std::vector<std::string>({"d", "b", "a"}));

  iterator->Seek("b");
  CHECK(iterator->Valid());
  CHECK(iterator->key().ToString() == "b");
  CHECK(iterator->value().ToString() == "new");
  iterator->Next();
  CHECK(iterator->Valid());
  CHECK(iterator->key().ToString() == "d");
  iterator->Seek("bb");
  CHECK(iterator->Valid());
  CHECK(iterator->key().ToString() == "d");
  iterator->Seek("z");
  CHECK(!iterator->Valid());
  CheckOk(iterator->status(), "iterator seek");
  delete iterator;
}

void TestIteratorReseekForward() {
  DBFixture fixture("reseek-forward");
  fixture.Open();
  for (size_t hidden : {0, 7, 8, 9, 127}) {
    std::vector<InternalEntry> entries;
    for (size_t sequence = 0; sequence <= hidden; ++sequence) {
      entries.push_back(
          {"a", std::to_string(sequence), sequence, rocketdb::kTypeValue});
    }
    entries.push_back({"b", "next", 0, rocketdb::kTypeValue});
    for (size_t threshold : {size_t{0}, size_t{1}, size_t{8},
                             std::numeric_limits<size_t>::max()}) {
      IteratorOperationCounts counts;
      auto iterator = NewCountedDBIterator(fixture.db(), entries,
                                           rocketdb::kMaxSequenceNumber,
                                           threshold, &counts);
      iterator->SeekToFirst();
      CHECK(iterator->Valid() && iterator->key() == "a");
      CHECK(iterator->value().ToString() == std::to_string(hidden));
      iterator->Next();
      CHECK(iterator->Valid() && iterator->key() == "b");
      CHECK(iterator->value() == "next");
      const bool triggered = threshold != 0 && hidden >= threshold;
      CHECK(counts.seek_targets.size() == (triggered ? 1 : 0));
      CHECK(counts.next == (triggered ? threshold : hidden + 1));
      if (triggered) {
        rocketdb::ParsedInternalKey target;
        CHECK(rocketdb::ParseInternalKey(counts.seek_targets.front(), &target));
        CHECK(target.user_key == "a" && target.sequence == 0);
        CHECK(target.type == rocketdb::kTypeDeletion);
      }
      if (hidden == 127 && threshold == 8) {
        std::cout << "[INFO] iterator reseek: 128 -> " << counts.next
                  << " internal Next calls, " << counts.seek_targets.size()
                  << " Seek\n";
      }
      iterator->Next();
      CHECK(!iterator->Valid());
      CheckOk(iterator->status(), "finish forward reseek");
    }
    // A seek that unexpectedly makes no progress must not cause a reseek loop.
    IteratorOperationCounts counts;
    counts.ignore_seek = true;
    auto iterator = NewCountedDBIterator(
        fixture.db(), entries, rocketdb::kMaxSequenceNumber, 1, &counts);
    iterator->SeekToFirst();
    iterator->Next();
    CHECK(iterator->Valid() && iterator->key() == "b");
    CHECK(counts.seek_targets.size() <= 1);
  }
}

void TestIteratorReseekSnapshotsAndZeroSequence() {
  DBFixture fixture("reseek-snapshot-zero");
  fixture.Open();
  std::vector<InternalEntry> entries = {{"b", "", 0, rocketdb::kTypeDeletion},
                                        {"d", "zero", 0, rocketdb::kTypeValue}};
  for (int sequence = 1; sequence <= 100; ++sequence) {
    entries.push_back({"a", "v" + std::to_string(sequence),
                       static_cast<uint64_t>(sequence), rocketdb::kTypeValue});
    if (sequence > 4) {
      entries.push_back({"b", "future", static_cast<uint64_t>(sequence),
                         rocketdb::kTypeValue});
      entries.push_back({"c", "future", static_cast<uint64_t>(sequence),
                         rocketdb::kTypeValue});
    }
  }
  const UserEntries expected = {{"a", "v4"}, {"d", "zero"}};
  for (size_t threshold : {0, 1, 8}) {
    IteratorOperationCounts counts;
    auto iterator =
        NewCountedDBIterator(fixture.db(), entries, 4, threshold, &counts);
    CHECK(CollectEntries(iterator.get(), false) == expected);
    if (threshold != 0) {
      rocketdb::ParsedInternalKey target;
      CHECK(!counts.seek_targets.empty());
      CHECK(rocketdb::ParseInternalKey(counts.seek_targets.front(), &target));
      CHECK(target.user_key == "a" && target.sequence == 4);
      CHECK(target.type == rocketdb::kValueTypeForSeek);
      CHECK(counts.next < 30);
    }
    auto reverse_expected = expected;
    std::reverse(reverse_expected.begin(), reverse_expected.end());
    CHECK(CollectEntries(iterator.get(), true) == reverse_expected);
    iterator->Seek("b");
    CHECK(iterator->Valid() && iterator->key() == "d");
    iterator->Seek("zz");
    CHECK(!iterator->Valid());
    CheckOk(iterator->status(), "seek beyond snapshot range");
  }
}

void TestIteratorReseekReverseAndDirections() {
  DBFixture fixture("reseek-reverse-directions");
  fixture.Open();
  std::vector<InternalEntry> entries = {
      {"", "empty", 0, rocketdb::kTypeValue},
      {"a", "first", 0, rocketdb::kTypeValue},
      {"d", "last-visible", 0, rocketdb::kTypeValue}};
  for (int sequence = 1; sequence <= 128; ++sequence) {
    entries.push_back({"b", "v" + std::to_string(sequence),
                       static_cast<uint64_t>(sequence), rocketdb::kTypeValue});
    entries.push_back(
        {"c", "v" + std::to_string(sequence), static_cast<uint64_t>(sequence),
         sequence == 128 ? rocketdb::kTypeDeletion : rocketdb::kTypeValue});
    entries.push_back({"z", "future", static_cast<uint64_t>(sequence + 128),
                       rocketdb::kTypeValue});
  }
  for (rocketdb::SequenceNumber snapshot :
       {uint64_t{0}, uint64_t{4}, uint64_t{128},
        rocketdb::kMaxSequenceNumber}) {
    for (size_t threshold : {1, 8, 64}) {
      IteratorOperationCounts original_counts;
      IteratorOperationCounts optimized_counts;
      auto original = NewCountedDBIterator(fixture.db(), entries, snapshot, 0,
                                           &original_counts);
      auto optimized = NewCountedDBIterator(fixture.db(), entries, snapshot,
                                            threshold, &optimized_counts);
      const auto expected = CollectEntries(original.get(), true);
      if (snapshot == 4) {
        CHECK(expected == UserEntries({{"d", "last-visible"},
                                       {"c", "v4"},
                                       {"b", "v4"},
                                       {"a", "first"},
                                       {"", "empty"}}));
      } else if (snapshot == 128) {
        CHECK(expected == UserEntries({{"d", "last-visible"},
                                       {"b", "v128"},
                                       {"a", "first"},
                                       {"", "empty"}}));
      }
      CHECK(CollectEntries(optimized.get(), true) == expected);
      CHECK(optimized_counts.prev < original_counts.prev);
      if (threshold <= 8)
        CHECK(optimized_counts.prev < original_counts.prev / 2);
      CheckIteratorTrace(original.get(), optimized.get());
    }
  }
  CaseInsensitiveComparator comparator;
  entries.clear();
  for (int sequence = 0; sequence < 64; ++sequence) {
    entries.push_back({sequence % 2 == 0 ? "a" : "A", std::to_string(sequence),
                       static_cast<uint64_t>(sequence), rocketdb::kTypeValue});
    entries.push_back({std::string("k\0x", 3), std::to_string(sequence),
                       static_cast<uint64_t>(sequence), rocketdb::kTypeValue});
  }
  for (size_t threshold : {1, 8}) {
    IteratorOperationCounts original_counts;
    IteratorOperationCounts optimized_counts;
    auto original = NewCountedDBIterator(fixture.db(), entries, 31, 0,
                                         &original_counts, &comparator);
    auto optimized = NewCountedDBIterator(fixture.db(), entries, 31, threshold,
                                          &optimized_counts, &comparator);
    CheckIteratorTrace(original.get(), optimized.get());
  }
}

void TestIteratorReseekAcrossTables() {
  DBFixture fixture("reseek-memtable-sst");
  fixture.mutable_options()->write_buffer_size = 1 << 20;
  fixture.Open();
  // Keep every historical version through the flush-pruning pass.
  auto oldest = TakeSnapshot(fixture.db());
  SnapshotPtr middle;
  const std::vector<std::string> keys = {
      "", "a", "b", "c", std::string("k\0x", 3), "z"};
  for (int round = 0; round < 40; ++round) {
    for (size_t index = 0; index < keys.size(); ++index) {
      if ((round + index) % 7 == 0) {
        CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), keys[index]),
                "delete layered key");
      } else {
        CheckOk(fixture.db()->Put(
                    rocketdb::WriteOptions(), keys[index],
                    std::to_string(round) + ":" + std::to_string(index)),
                "write layered history");
      }
    }
    if (round == 15) middle = TakeSnapshot(fixture.db());
    if (round < 32 && round % 8 == 7) {
      CheckOk(
          static_cast<rocketdb::DBImpl*>(fixture.db())->TEST_CompactMemTable(),
          "flush layered history");
    }
  }
  CHECK(HasTableFile(fixture.dbname()));
  CHECK(ReadInternalEntries(fixture.db()).size() == keys.size() * 40);
  for (const auto* snapshot : {static_cast<const rocketdb::Snapshot*>(nullptr),
                               oldest.get(), middle.get()}) {
    for (size_t threshold : {1, 8, 64}) {
      rocketdb::ReadOptions baseline;
      baseline.snapshot = snapshot;
      auto options = baseline;
      options.max_sequential_skip_in_iterations = threshold;
      std::unique_ptr<rocketdb::Iterator> original(
          fixture.db()->NewIterator(baseline));
      std::unique_ptr<rocketdb::Iterator> optimized(
          fixture.db()->NewIterator(options));
      CheckIteratorTrace(original.get(), optimized.get());
    }
  }
}

class CountingComparator : public rocketdb::Comparator {
 public:
  int Compare(const rocketdb::Slice& a,
              const rocketdb::Slice& b) const override {
    comparisons.fetch_add(1, std::memory_order_relaxed);
    return rocketdb::BytewiseComparator()->Compare(a, b);
  }
  const char* Name() const override { return "test.CountingComparator"; }
  void FindShortestSeparator(std::string* start,
                             const rocketdb::Slice& limit) const override {
    rocketdb::BytewiseComparator()->FindShortestSeparator(start, limit);
  }
  void FindShortSuccessor(std::string* key) const override {
    rocketdb::BytewiseComparator()->FindShortSuccessor(key);
  }
  mutable std::atomic<size_t> comparisons{0};
};

void TestIteratorReseekReadOptionsAndTiming() {
  CountingComparator comparator;
  DBFixture fixture("reseek-read-options");
  fixture.mutable_options()->comparator = &comparator;
  fixture.Open();
  for (int version = 0; version < 256; ++version) {
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "a", "value"),
            "overwrite measured key");
  }
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "b", "next"),
          "put next measured key");
  rocketdb::ReadOptions options;
  CHECK(options.max_sequential_skip_in_iterations == 0);
  std::unique_ptr<rocketdb::Iterator> original(
      fixture.db()->NewIterator(options));
  options.max_sequential_skip_in_iterations = 8;
  std::unique_ptr<rocketdb::Iterator> optimized(
      fixture.db()->NewIterator(options));
  options.max_sequential_skip_in_iterations = 0;
  original->SeekToFirst();
  optimized->SeekToFirst();
  comparator.comparisons.store(0);
  original->Next();
  const size_t baseline_comparisons = comparator.comparisons.exchange(0);
  optimized->Next();
  const size_t optimized_comparisons = comparator.comparisons.load();
  CheckIteratorPosition(original.get(), optimized.get());
  CHECK(optimized_comparisons < baseline_comparisons / 2);
  auto measure = [](rocketdb::Iterator* iterator) {
    const auto start = std::chrono::steady_clock::now();
    for (int run = 0; run < 1000; ++run) {
      for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
      }
    }
    CheckOk(iterator->status(), "measure reseek scan");
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  };
  const auto baseline_micros = measure(original.get());
  const auto optimized_micros = measure(optimized.get());
  std::cout << "[INFO] 1000 overwritten-key scans: disabled=" << baseline_micros
            << " us, threshold=8=" << optimized_micros
            << " us; Next comparisons " << baseline_comparisons << " -> "
            << optimized_comparisons << '\n';

  // These iterators have implicit snapshots. Later writes and a flush must
  // not change their visible values, even when a reseek starts at the root.
  for (int version = 0; version < 64; ++version) {
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "a", "later"),
            "write after iterator creation");
  }
  const UserEntries expected = {{"a", "value"}, {"b", "next"}};
  CHECK(CollectEntries(optimized.get(), false) == expected);
  CheckIteratorTrace(original.get(), optimized.get());
  CheckOk(static_cast<rocketdb::DBImpl*>(fixture.db())->TEST_CompactMemTable(),
          "flush with pinned iterators");
  CHECK(CollectEntries(optimized.get(), false) == expected);
  CheckIteratorTrace(original.get(), optimized.get());
}

void TestIteratorReseekErrorsAndUniqueKeys() {
  DBFixture fixture("reseek-errors-unique");
  fixture.Open();
  std::vector<InternalEntry> entries;
  for (int sequence = 0; sequence < 128; ++sequence) {
    entries.push_back(
        {"a", "value", static_cast<uint64_t>(sequence), rocketdb::kTypeValue});
  }
  entries.push_back({"b", "next", 0, rocketdb::kTypeValue});
  for (bool reverse : {false, true}) {
    IteratorOperationCounts counts;
    auto iterator =
        NewCountedDBIterator(fixture.db(), entries, 100, 8, &counts);
    counts.fail_seek = true;
    if (reverse) {
      iterator->SeekToLast();
      CHECK(iterator->Valid() && iterator->key() == "b");
      iterator->Prev();
    } else {
      iterator->SeekToFirst();
    }
    CHECK(!iterator->Valid());
    CHECK(iterator->status().IsIOError());
  }
  entries.clear();
  for (int index = 0; index < 128; ++index) {
    entries.push_back({Key(index), "unique", static_cast<uint64_t>(index),
                       rocketdb::kTypeValue});
  }
  IteratorOperationCounts counts;
  auto iterator = NewCountedDBIterator(
      fixture.db(), entries, rocketdb::kMaxSequenceNumber, 8, &counts);
  CHECK(CollectEntries(iterator.get(), false).size() == entries.size());
  CHECK(CollectEntries(iterator.get(), true).size() == entries.size());
  CHECK(counts.seek_targets.empty());
}

void TestBloomFilterHasNoFalseNegatives() {
  const rocketdb::FilterPolicy* policy = rocketdb::NewBloomFilterPolicy(10);
  std::vector<std::string> owned_keys;
  std::vector<rocketdb::Slice> keys;
  owned_keys.reserve(128);
  keys.reserve(128);
  for (int i = 0; i < 128; ++i) {
    owned_keys.push_back("bloom-key-" + std::to_string(i));
  }
  for (const std::string& key : owned_keys) {
    keys.emplace_back(key);
  }

  std::string filter;
  policy->CreateFilter(keys.data(), static_cast<int>(keys.size()), &filter);
  for (const std::string& key : owned_keys) {
    CHECK(policy->KeyMayMatch(key, filter));
  }
  delete policy;
}

void TestFlushCompactionAndSstReopen() {
  DBFixture fixture("flush-compaction");
  fixture.Open();

  std::map<std::string, std::string> expected;
  for (int i = 0; i < 40; ++i) {
    const std::string key = Key(i);
    const std::string value = "value-" + std::to_string(i) + "-" + std::string(24 * 1024, 'a' + (i % 26));
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), key, value), "put flush input");
    expected.emplace(key, value);
  }
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), Key(3), "latest"), "overwrite compacted key");
  expected[Key(3)] = "latest";
  CheckOk(fixture.db()->Delete(rocketdb::WriteOptions(), Key(4)), "delete compacted key");
  expected.erase(Key(4));

  fixture.db()->CompactRange(nullptr, nullptr);
  CHECK(HasTableFile(fixture.dbname()));
  for (const auto& entry : expected) {
    ExpectValue(fixture.db(), entry.first, entry.second);
  }
  ExpectMissing(fixture.db(), Key(4));

  fixture.Reopen();
  for (const auto& entry : expected) {
    ExpectValue(fixture.db(), entry.first, entry.second);
  }
  ExpectMissing(fixture.db(), Key(4));
}

void TestPerfPropertyAndFlushMetrics() {
  DBFixture fixture("perf-flush");
  fixture.Open();

  const auto initial = GetNumericProperty(fixture.db(), "rocketdb.perf-stats");
  CHECK(initial.size() == 63);
  CHECK(initial.at("version") == 1);
  CHECK(initial.at("flush_count") == 0);
  CHECK(initial.at("immutable_memtable_wait_count") == 0);
  CHECK(initial.at("l0_slowdown_count") == 0);
  CHECK(initial.at("l0_stop_count") == 0);

  std::string legacy_stats;
  CHECK(fixture.db()->GetProperty("rocketdb.stats", &legacy_stats));
  std::string l0_files;
  CHECK(fixture.db()->GetProperty("rocketdb.num-files-at-level0", &l0_files));

  WriteFlushRound(fixture.db(), 0);
  const auto stats = GetNumericProperty(fixture.db(), "rocketdb.perf-stats");
  CHECK(stats.at("version") == 1);
  CHECK(stats.at("flush_count") > 0);
  CHECK(stats.at("flush_total_micros") >= stats.at("flush_max_micros"));
  CHECK(stats.at("flush_bytes_written") > 0);

  uint64_t flushes_by_level = 0;
  for (int level = 0; level < rocketdb::config::kNumLevels; ++level) {
    flushes_by_level +=
        stats.at("flush_count_by_output_level." + std::to_string(level));
  }
  CHECK(flushes_by_level == stats.at("flush_count"));
}

void TestRecoveryFlushIsExcludedFromPerfMetrics() {
  DBFixture fixture("perf-recovery");
  fixture.Open();
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "wal-key", std::string(4096, 'r')),
          "put recovery input");
  fixture.Reopen();

  const auto stats = GetNumericProperty(fixture.db(), "rocketdb.perf-stats");
  CHECK(stats.at("flush_count") == 0);
  CHECK(stats.at("flush_bytes_written") == 0);
  ExpectValue(fixture.db(), "wal-key", std::string(4096, 'r'));
}

void TestMajorCompactionMetrics() {
  DBFixture fixture("perf-major");
  fixture.Open();

  // The first non-overlapping flush can be promoted to L2. The second flush
  // overlaps those keys and is placed lower, after which CompactRange performs
  // a real merge/rewrite into L2.
  WriteFlushRound(fixture.db(), 0);
  const auto before = GetNumericProperty(fixture.db(), "rocketdb.perf-stats");
  WriteFlushRound(fixture.db(), 1);
  const auto after = GetNumericProperty(fixture.db(), "rocketdb.perf-stats");

  bool found_major = false;
  for (int level = 0; level < rocketdb::config::kNumLevels; ++level) {
    const std::string suffix = "." + std::to_string(level);
    const uint64_t count = after.at("major_compaction_count" + suffix);
    if (count > before.at("major_compaction_count" + suffix)) {
      found_major = true;
      CHECK(after.at("major_compaction_total_micros" + suffix) >=
            after.at("major_compaction_max_micros" + suffix));
      CHECK(after.at("major_compaction_bytes_read" + suffix) > 0);
      CHECK(after.at("major_compaction_bytes_written" + suffix) > 0);
    }
  }
  CHECK(found_major);
}

void TestTrivialMoveIsSeparateFromMajorCompaction() {
  DBFixture fixture("perf-trivial-move");
  fixture.Open();
  auto* impl = dynamic_cast<rocketdb::DBImpl*>(fixture.db());
  CHECK(impl != nullptr);

  const std::string value(1 << 20, 't');
  // Each pair first creates an L2 file and then an overlapping L1 file. Nine
  // L1 files stay below RocketDB's existing 10 MiB L1 compaction threshold.
  for (int file = 0; file < 9; ++file) {
    const std::string key = "trivial-" + std::to_string(file);
    CheckOk(impl->Put(rocketdb::WriteOptions(), key, value), "put L2 trivial input");
    CheckOk(impl->TEST_CompactMemTable(), "flush L2 trivial input");
    CheckOk(impl->Put(rocketdb::WriteOptions(), key, value), "put L1 trivial input");
    CheckOk(impl->TEST_CompactMemTable(), "flush L1 trivial input");
  }

  // Move the overlapping L2 inputs out of the way via a controlled manual
  // rewrite. The L1 files remain and can subsequently take the automatic
  // trivial-move path.
  impl->TEST_CompactRange(2, nullptr, nullptr);
  const auto before = GetNumericProperty(impl, "rocketdb.perf-stats");

  CheckOk(impl->Put(rocketdb::WriteOptions(), "trivial-z", value),
          "put final L2 trivial input");
  CheckOk(impl->TEST_CompactMemTable(), "flush final L2 trivial input");
  CheckOk(impl->Put(rocketdb::WriteOptions(), "trivial-z", value),
          "put final L1 trivial input");
  CheckOk(impl->TEST_CompactMemTable(), "flush final L1 trivial input");

  // Joining a no-op manual range waits for any already scheduled automatic
  // compaction without relying on a timing delay.
  impl->TEST_CompactRange(5, nullptr, nullptr);
  const auto after = GetNumericProperty(impl, "rocketdb.perf-stats");

  uint64_t trivial_before = 0;
  uint64_t trivial_after = 0;
  for (int level = 0; level < rocketdb::config::kNumLevels; ++level) {
    const std::string suffix = "." + std::to_string(level);
    trivial_before += before.at("trivial_move_count" + suffix);
    trivial_after += after.at("trivial_move_count" + suffix);
    CHECK(after.at("major_compaction_count" + suffix) ==
          before.at("major_compaction_count" + suffix));
  }
  CHECK(trivial_after > trivial_before);
}

void TestPerfCounterClassificationAndConcurrency() {
  rocketdb::DBPerfCounters counters;
  counters.RecordTrivialMove(2);
  CHECK(counters.TrivialMoveCount(2) == 1);
  CHECK(counters.MajorCompactionSnapshot(2).count == 0);
  counters.RecordMajorCompaction(2, 7, 11, 13);
  CHECK(counters.TrivialMoveCount(2) == 1);
  CHECK(counters.MajorCompactionSnapshot(2).count == 1);

  counters.RecordWriteStall(rocketdb::WriteStallType::kImmutableMemtableWait, 3);
  counters.RecordWriteStall(rocketdb::WriteStallType::kL0Slowdown, 5);
  counters.RecordWriteStall(rocketdb::WriteStallType::kL0Stop, 7);
  CHECK(counters.WriteStallSnapshot(
                     rocketdb::WriteStallType::kImmutableMemtableWait)
            .count == 1);
  CHECK(counters.WriteStallSnapshot(rocketdb::WriteStallType::kL0Slowdown).count == 1);
  CHECK(counters.WriteStallSnapshot(rocketdb::WriteStallType::kL0Stop).count == 1);

  constexpr int kThreadCount = 8;
  constexpr int kRecordsPerThread = 10000;
  std::vector<std::thread> threads;
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&counters] {
      for (int i = 0; i < kRecordsPerThread; ++i) {
        counters.RecordWriteStall(rocketdb::WriteStallType::kImmutableMemtableWait, 2);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  const rocketdb::TimedEventSnapshot snapshot =
      counters.WriteStallSnapshot(rocketdb::WriteStallType::kImmutableMemtableWait);
  CHECK(snapshot.count == 1 + kThreadCount * kRecordsPerThread);
  CHECK(snapshot.total_micros == 3 + 2 * kThreadCount * kRecordsPerThread);
  CHECK(snapshot.max_micros == 3);
}

void TestBlockCacheStatistics() {
  rocketdb::Cache* plain_cache = rocketdb::NewLRUCache(1024);
  rocketdb::CacheStats unsupported_stats;
  CHECK(!plain_cache->GetStats(&unsupported_stats));
  CHECK(plain_cache->Lookup("not-present") == nullptr);
  CHECK(!plain_cache->GetStats(&unsupported_stats));
  delete plain_cache;

  rocketdb::Cache* cache = rocketdb::NewLRUCacheWithStatistics(1 << 20);
  {
    rocketdb::CacheStats direct_stats;
    CHECK(cache->GetStats(&direct_stats));
    CHECK(direct_stats.hit == 0);
    CHECK(direct_stats.miss == 0);

    rocketdb::Cache::Handle* inserted =
        cache->Insert("present", new int(1), 1, &DeleteCacheTestValue);
    cache->Release(inserted);
    constexpr int kCacheThreadCount = 4;
    constexpr int kCacheLookupsPerThread = 5000;
    std::atomic<bool> lookup_failed{false};
    std::vector<std::thread> lookup_threads;
    for (int thread_id = 0; thread_id < kCacheThreadCount; ++thread_id) {
      lookup_threads.emplace_back([&] {
        for (int i = 0; i < kCacheLookupsPerThread; ++i) {
          rocketdb::Cache::Handle* handle = cache->Lookup("present");
          if (handle == nullptr) {
            lookup_failed.store(true, std::memory_order_relaxed);
          } else {
            cache->Release(handle);
          }
          if (cache->Lookup("not-present") != nullptr) {
            lookup_failed.store(true, std::memory_order_relaxed);
          }
        }
      });
    }
    for (std::thread& thread : lookup_threads) {
      thread.join();
    }
    CHECK(!lookup_failed.load(std::memory_order_relaxed));
    CHECK(cache->GetStats(&direct_stats));
    CHECK(direct_stats.hit == kCacheThreadCount * kCacheLookupsPerThread);
    CHECK(direct_stats.miss == kCacheThreadCount * kCacheLookupsPerThread);

    rocketdb::Options table_options;
    table_options.block_cache = cache;
    const std::string missing_db = NewDatabasePath("table-cache-isolation");
    rocketdb::TableCache table_cache(missing_db, table_options, 16);
    rocketdb::Iterator* missing =
        table_cache.NewIterator(rocketdb::ReadOptions(), 123, 456);
    CHECK(!missing->status().ok());
    delete missing;
    rocketdb::CacheStats after_table_cache;
    CHECK(cache->GetStats(&after_table_cache));
    CHECK(after_table_cache.hit == direct_stats.hit);
    CHECK(after_table_cache.miss == direct_stats.miss);
  }

  {
    DBFixture fixture("block-cache-stats");
    fixture.mutable_options()->block_cache = cache;
    fixture.mutable_options()->block_size = 1024;
    fixture.Open();
    const std::string expected(8 * 1024, 'c');
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "cached-key", expected),
            "put cache input");
    fixture.db()->CompactRange(nullptr, nullptr);
    cache->Prune();

    const auto before =
        GetNumericProperty(fixture.db(), "rocketdb.block-cache-stats");
    CHECK(before.size() == 4);
    CHECK(before.at("version") == 1);
    CHECK(before.at("supported") == 1);

    ExpectValue(fixture.db(), "cached-key", expected);
    const auto after_miss =
        GetNumericProperty(fixture.db(), "rocketdb.block-cache-stats");
    CHECK(after_miss.at("block_cache_miss") > before.at("block_cache_miss"));

    ExpectValue(fixture.db(), "cached-key", expected);
    const auto after_hit =
        GetNumericProperty(fixture.db(), "rocketdb.block-cache-stats");
    CHECK(after_hit.at("block_cache_hit") > after_miss.at("block_cache_hit"));
  }

  {
    // Mirror the benchmark lifecycle: create an uncompressed SST with the
    // ordinary cache, close the DB, then reopen it with the statistics cache.
    DBFixture fixture("block-cache-reopen-stats");
    fixture.mutable_options()->block_size = 1024;
    fixture.Open();
    const std::string expected(8 * 1024, 'r');
    CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "reopened-cache-key", expected),
            "put reopened cache input");
    fixture.db()->CompactRange(nullptr, nullptr);
    fixture.Close();
    fixture.mutable_options()->block_cache = cache;
    fixture.Open();
    cache->Prune();

    const auto before =
        GetNumericProperty(fixture.db(), "rocketdb.block-cache-stats");
    ExpectValue(fixture.db(), "reopened-cache-key", expected);
    const auto after_miss =
        GetNumericProperty(fixture.db(), "rocketdb.block-cache-stats");
    CHECK(after_miss.at("block_cache_miss") > before.at("block_cache_miss"));

    ExpectValue(fixture.db(), "reopened-cache-key", expected);
    const auto after_hit =
        GetNumericProperty(fixture.db(), "rocketdb.block-cache-stats");
    CHECK(after_hit.at("block_cache_hit") > after_miss.at("block_cache_hit"));
  }
  delete cache;

  DBFixture default_fixture("block-cache-default-off");
  default_fixture.Open();
  const auto disabled =
      GetNumericProperty(default_fixture.db(), "rocketdb.block-cache-stats");
  CHECK(disabled.size() == 2);
  CHECK(disabled.at("version") == 1);
  CHECK(disabled.at("supported") == 0);
}

void TestNormalPutDoesNotReadClockForStallMetrics() {
  CountingEnv env;
  DBFixture fixture("no-normal-write-clock");
  fixture.mutable_options()->env = &env;
  fixture.Open();
  const uint64_t before = env.now_micros_calls();
  CheckOk(fixture.db()->Put(rocketdb::WriteOptions(), "small", "value"),
          "put without stall");
  CHECK(env.now_micros_calls() == before);
}

void TestConcurrentWriters() {
  DBFixture fixture("concurrent-writers");
  fixture.Open();

  constexpr int kThreadCount = 6;
  constexpr int kWritesPerThread = 40;
  std::mutex error_mutex;
  std::string error;
  std::vector<std::thread> threads;
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&, thread_id] {
      for (int write_id = 0; write_id < kWritesPerThread; ++write_id) {
        const std::string key = "thread-" + std::to_string(thread_id) + "-" + std::to_string(write_id);
        const std::string value = "value-" + std::to_string(thread_id) + "-" + std::to_string(write_id);
        const rocketdb::Status status = fixture.db()->Put(rocketdb::WriteOptions(), key, value);
        if (!status.ok()) {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (error.empty()) {
            error = status.ToString();
          }
          return;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  CHECK(error.empty());

  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    for (int write_id = 0; write_id < kWritesPerThread; ++write_id) {
      const std::string key = "thread-" + std::to_string(thread_id) + "-" + std::to_string(write_id);
      const std::string value = "value-" + std::to_string(thread_id) + "-" + std::to_string(write_id);
      ExpectValue(fixture.db(), key, value);
    }
  }
}

void CheckNoCError(char*& error, const std::string& context) {
  if (error != nullptr) {
    const std::string message(error);
    rocketdb_free(error);
    error = nullptr;
    throw TestFailure(context + ": " + message);
  }
}

void TestCApi() {
  const std::string dbname = NewDatabasePath("c-api");
  rocketdb_options_t* options = rocketdb_options_create();
  rocketdb_options_set_create_if_missing(options, 1);
  rocketdb_options_set_write_buffer_size(options, 64 << 10);
  rocketdb_options_set_compression(options, rocketdb_no_compression);
  rocketdb_writeoptions_t* write_options = rocketdb_writeoptions_create();
  rocketdb_writeoptions_set_sync(write_options, 1);
  rocketdb_readoptions_t* read_options = rocketdb_readoptions_create();

  char* error = nullptr;
  rocketdb_t* db = rocketdb_open(options, dbname.c_str(), &error);
  CheckNoCError(error, "C API open");
  CHECK(db != nullptr);

  const char key[] = {'c', '\0', 'k'};
  const char value[] = {'v', '\0', '1'};
  rocketdb_put(db, write_options, key, sizeof(key), value, sizeof(value), &error);
  CheckNoCError(error, "C API put binary value");
  rocketdb_put(db, write_options, "empty", 5, "", 0, &error);
  CheckNoCError(error, "C API put empty value");
  for (int version = 0; version < 16; ++version) {
    rocketdb_put(db, write_options, key, sizeof(key), value, sizeof(value),
                 &error);
    CheckNoCError(error, "C API overwrite iterator key");
  }

  size_t value_length = 0;
  char* result = rocketdb_get(db, read_options, key, sizeof(key), &value_length, &error);
  CheckNoCError(error, "C API get binary value");
  CHECK(result != nullptr);
  CHECK(value_length == sizeof(value));
  CHECK(std::string(result, value_length) == std::string(value, sizeof(value)));
  rocketdb_free(result);

  result = rocketdb_get(db, read_options, "empty", 5, &value_length, &error);
  CheckNoCError(error, "C API get empty value");
  CHECK(result != nullptr);
  CHECK(value_length == 0);
  rocketdb_free(result);

  result = rocketdb_get(db, read_options, "missing", 7, &value_length, &error);
  CheckNoCError(error, "C API get missing value");
  CHECK(result == nullptr);
  CHECK(value_length == 0);

  rocketdb_readoptions_set_max_sequential_skip_in_iterations(read_options, 1);
  rocketdb_iterator_t* iterator = rocketdb_create_iterator(db, read_options);
  rocketdb_readoptions_set_max_sequential_skip_in_iterations(read_options, 0);
  rocketdb_iter_seek_to_first(iterator);
  CHECK(rocketdb_iter_valid(iterator));
  size_t key_length = 0;
  const char* found_key = rocketdb_iter_key(iterator, &key_length);
  CHECK(std::string(found_key, key_length) == std::string(key, sizeof(key)));
  rocketdb_iter_next(iterator);
  CHECK(rocketdb_iter_valid(iterator));
  found_key = rocketdb_iter_key(iterator, &key_length);
  CHECK(std::string(found_key, key_length) == "empty");
  rocketdb_iter_prev(iterator);
  CHECK(rocketdb_iter_valid(iterator));
  found_key = rocketdb_iter_key(iterator, &key_length);
  CHECK(std::string(found_key, key_length) == std::string(key, sizeof(key)));
  rocketdb_iter_seek_to_last(iterator);
  rocketdb_iter_prev(iterator);
  CHECK(rocketdb_iter_valid(iterator));
  found_key = rocketdb_iter_key(iterator, &key_length);
  CHECK(std::string(found_key, key_length) == std::string(key, sizeof(key)));
  rocketdb_iter_get_error(iterator, &error);
  CheckNoCError(error, "C API iterator reseek");
  rocketdb_iter_destroy(iterator);

  rocketdb_close(db);
  rocketdb_readoptions_destroy(read_options);
  rocketdb_writeoptions_destroy(write_options);
  rocketdb_options_destroy(options);

  rocketdb::Options cleanup_options;
  (void)rocketdb::DestroyDB(dbname, cleanup_options);
  std::error_code filesystem_error;
  fs::remove_all(dbname, filesystem_error);
}

struct TestCase {
  const char* name;
  void (*function)();
};

}  // namespace

int main() {
  // Heap-backed random reads make block insertion deterministic; mmap-backed
  // blocks intentionally bypass the block cache because their bytes are
  // already memory resident.
  rocketdb::EnvPosixTest::DisableMMapForTests();
  const std::vector<TestCase> tests = {
      {"basic_crud_and_binary_values", &TestBasicCrudAndBinaryValues},
      {"write_batch_and_wal_recovery", &TestWriteBatchAndWalRecovery},
      {"snapshots", &TestSnapshots},
      {"flush_pruning_build_table", &TestFlushPruningBuildTable},
      {"flush_pruning_without_snapshots", &TestFlushPruningWithoutSnapshots},
      {"flush_pruning_snapshots", &TestFlushPruningSnapshots},
      {"flush_pruning_keeps_tombstones", &TestFlushPruningKeepsTombstones},
      {"flush_pruning_custom_comparator", &TestFlushPruningCustomComparator},
      {"flush_pruning_snapshot_changes_during_build",
       &TestFlushPruningSnapshotChangesDuringBuild},
      {"flush_pruning_sync_failure_and_recovery",
       &TestFlushPruningSyncFailureAndRecovery},
      {"iterator_directions_and_seek", &TestIteratorDirectionsAndSeek},
      {"iterator_reseek_forward", &TestIteratorReseekForward},
      {"iterator_reseek_snapshots_and_zero_sequence",
       &TestIteratorReseekSnapshotsAndZeroSequence},
      {"iterator_reseek_reverse_and_directions",
       &TestIteratorReseekReverseAndDirections},
      {"iterator_reseek_across_tables", &TestIteratorReseekAcrossTables},
      {"iterator_reseek_read_options_and_timing",
       &TestIteratorReseekReadOptionsAndTiming},
      {"iterator_reseek_errors_and_unique_keys",
       &TestIteratorReseekErrorsAndUniqueKeys},
      {"bloom_filter_has_no_false_negatives",
       &TestBloomFilterHasNoFalseNegatives},
      {"flush_compaction_and_sst_reopen", &TestFlushCompactionAndSstReopen},
      {"perf_property_and_flush_metrics", &TestPerfPropertyAndFlushMetrics},
      {"recovery_flush_is_excluded_from_perf_metrics",
       &TestRecoveryFlushIsExcludedFromPerfMetrics},
      {"major_compaction_metrics", &TestMajorCompactionMetrics},
      {"trivial_move_is_separate_from_major_compaction",
       &TestTrivialMoveIsSeparateFromMajorCompaction},
      {"perf_counter_classification_and_concurrency",
       &TestPerfCounterClassificationAndConcurrency},
      {"block_cache_statistics", &TestBlockCacheStatistics},
      {"normal_put_does_not_read_clock_for_stall_metrics",
       &TestNormalPutDoesNotReadClockForStallMetrics},
      {"concurrent_writers", &TestConcurrentWriters},
      {"c_api", &TestCApi},
  };

  int failures = 0;
  for (const TestCase& test : tests) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    }
  }

  std::cout << tests.size() - failures << "/" << tests.size() << " regression tests passed\n";
  return failures == 0 ? 0 : 1;
}
