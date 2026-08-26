#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "c.h"
#include "cache.h"
#include "db.h"
#include "db/db_impl.h"
#include "db/perf_counters.h"
#include "db/table_cache.h"
#include "env.h"
#include "util/env_posix_test_helper.h"
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
      {"iterator_directions_and_seek", &TestIteratorDirectionsAndSeek},
      {"bloom_filter_has_no_false_negatives", &TestBloomFilterHasNoFalseNegatives},
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
