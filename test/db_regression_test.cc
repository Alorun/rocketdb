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
#include "db.h"
#include "filter_policy.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"
#include "write_batch.h"

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

 private:
  const rocketdb::FilterPolicy* filter_policy_;
  rocketdb::Options options_;
  std::string dbname_;
  rocketdb::DB* db_;
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
  const std::vector<TestCase> tests = {
      {"basic_crud_and_binary_values", &TestBasicCrudAndBinaryValues},
      {"write_batch_and_wal_recovery", &TestWriteBatchAndWalRecovery},
      {"snapshots", &TestSnapshots},
      {"iterator_directions_and_seek", &TestIteratorDirectionsAndSeek},
      {"bloom_filter_has_no_false_negatives", &TestBloomFilterHasNoFalseNegatives},
      {"flush_compaction_and_sst_reopen", &TestFlushCompactionAndSstReopen},
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
