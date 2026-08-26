#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cache.h"
#include "db.h"
#include "db/dbformat.h"
#include "env.h"
#include "filter_policy.h"
#include "options.h"
#include "status.h"

namespace {

namespace fs = std::filesystem;
using BenchmarkClock = std::chrono::steady_clock;
using NumericProperty = std::map<std::string, uint64_t>;

enum class MeasurementMode { kThroughput, kLatency };

[[noreturn]] void Fail(const std::string &message) {
  throw std::runtime_error(message);
}

void CheckOk(const rocketdb::Status &status, const std::string &context) {
  if (!status.ok())
    Fail(context + ": " + status.ToString());
}

const char *MeasurementModeName(MeasurementMode mode) {
  return mode == MeasurementMode::kThroughput ? "throughput" : "latency";
}

uint64_t ElapsedNanos(BenchmarkClock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          BenchmarkClock::now() - start)
          .count());
}

uint64_t ParseUint64(const std::string &text, const char *name) {
  if (text.empty() || text.front() == '-')
    Fail(std::string("invalid ") + name + ": " + text);
  size_t consumed = 0;
  try {
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size() ||
        value > std::numeric_limits<uint64_t>::max()) {
      Fail(std::string("invalid ") + name + ": " + text);
    }
    return static_cast<uint64_t>(value);
  } catch (const std::exception &) {
    Fail(std::string("invalid ") + name + ": " + text);
  }
}

size_t ParseSize(const std::string &text, const char *name) {
  const uint64_t value = ParseUint64(text, name);
  if (value > std::numeric_limits<size_t>::max()) {
    Fail(std::string("invalid ") + name + ": " + text);
  }
  return static_cast<size_t>(value);
}

int ParseInt(const std::string &text, const char *name) {
  const uint64_t value = ParseUint64(text, name);
  if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    Fail(std::string("invalid ") + name + ": " + text);
  }
  return static_cast<int>(value);
}

bool ReadOptionValue(const std::string &argument, const char *name,
                     std::string *value) {
  const std::string prefix = std::string(name) + "=";
  if (argument.rfind(prefix, 0) != 0)
    return false;
  *value = argument.substr(prefix.size());
  return true;
}

std::vector<int> ParseIntList(const std::string &text, const char *name) {
  std::vector<int> values;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (item.empty())
      Fail(std::string("empty item in ") + name);
    values.push_back(ParseInt(item, name));
  }
  if (values.empty())
    Fail(std::string(name) + " must not be empty");
  return values;
}

struct Config {
  std::vector<int> thread_counts{1, 2, 4, 8};
  std::vector<int> read_ratios{50, 95};
  uint64_t num = 1000000;
  uint64_t duration_seconds = 30;
  uint64_t seed = 301;
  uint64_t latency_sample = 100;
  uint64_t l0_sample_interval_ms = 1000;
  size_t key_size = 16;
  size_t value_size = 100;
  size_t write_buffer_size = 4 << 20;
  size_t max_file_size = 2 << 20;
  size_t block_cache_size = 8 << 20;
  int bloom_bits = -1;
  rocketdb::CompressionType compression = rocketdb::kNoCompression;
  MeasurementMode measurement_mode = MeasurementMode::kThroughput;
  bool sync = false;
  bool keep_db = false;
};

// Uncompressed mmap-backed blocks normally bypass the Block Cache because
// RandomAccessFile::Read returns a Slice into the mapped file instead of the
// caller's scratch buffer. This benchmark needs the data-block cache to be the
// measured read path, so normalize such reads into the supplied scratch buffer.
class CacheableRandomAccessFile : public rocketdb::RandomAccessFile {
public:
  explicit CacheableRandomAccessFile(
      std::unique_ptr<rocketdb::RandomAccessFile> target)
      : target_(std::move(target)) {}

  rocketdb::Status Read(uint64_t offset, size_t n, rocketdb::Slice *result,
                        char *scratch) const override {
    rocketdb::Slice target_result;
    const rocketdb::Status status =
        target_->Read(offset, n, &target_result, scratch);
    if (!status.ok()) {
      *result = target_result;
      return status;
    }
    if (target_result.data() != scratch) {
      std::memmove(scratch, target_result.data(), target_result.size());
      *result = rocketdb::Slice(scratch, target_result.size());
    } else {
      *result = target_result;
    }
    return status;
  }

private:
  std::unique_ptr<rocketdb::RandomAccessFile> target_;
};

class BlockCacheReadEnv : public rocketdb::EnvWrapper {
public:
  explicit BlockCacheReadEnv(rocketdb::Env *target)
      : rocketdb::EnvWrapper(target) {}

  rocketdb::Status
  NewRandomAccessFile(const std::string &filename,
                      rocketdb::RandomAccessFile **result) override {
    *result = nullptr;
    rocketdb::RandomAccessFile *raw_file = nullptr;
    rocketdb::Status status =
        target()->NewRandomAccessFile(filename, &raw_file);
    if (!status.ok())
      return status;
    std::unique_ptr<rocketdb::RandomAccessFile> target_file(raw_file);
    *result = new CacheableRandomAccessFile(std::move(target_file));
    return status;
  }
};

void PrintUsage(const char *program) {
  std::cout
      << "Usage: " << program << " [options]\n\n"
      << "Runs an isolated temporary DB for every read-ratio/thread-count "
         "scenario.\n\n"
      << "Options:\n"
      << "  --threads=LIST             Thread counts (default: 1,2,4,8)\n"
      << "  --read_ratios=LIST         Read percentages; writes use the "
         "remainder (default: 50,95)\n"
      << "  --duration=N               Measured seconds per scenario (default: "
         "30)\n"
      << "  --num=N                    Records prefilled per scenario "
         "(default: 1000000)\n"
      << "  --key_size=N               Fixed key width in bytes (default: 16)\n"
      << "  --value_size=N             Value size in bytes (default: 100)\n"
      << "  --write_buffer_size=N      MemTable size in bytes (default: "
         "4194304)\n"
      << "  --max_file_size=N          SSTable target size in bytes (default: "
         "2097152)\n"
      << "  --block_cache_size=N       Statistics-enabled data cache bytes "
         "(default: 8388608)\n"
      << "  --compression=TYPE         none, snappy, or zstd (default: none)\n"
      << "  --bloom_bits=N             Enable Bloom filter with N bits/key\n"
      << "  --sync                     Enable WAL sync for each write\n"
      << "  --mode=MODE                throughput or latency (default: "
         "throughput)\n"
      << "  --latency_sample=N         Time every Nth op in latency mode "
         "(default: 100)\n"
      << "  --l0_sample_interval_ms=N  L0 polling interval (default: 1000)\n"
      << "  --seed=N                   Base random seed (default: 301)\n"
      << "  --keep_db                  Keep all scenario databases for "
         "inspection\n";
}

Config ParseArgs(int argc, char **argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    std::string value;
    if (argument == "--help" || argument == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (argument == "--sync") {
      config.sync = true;
    } else if (argument == "--keep_db") {
      config.keep_db = true;
    } else if (ReadOptionValue(argument, "--threads", &value)) {
      config.thread_counts = ParseIntList(value, "--threads");
    } else if (ReadOptionValue(argument, "--read_ratios", &value)) {
      config.read_ratios = ParseIntList(value, "--read_ratios");
    } else if (ReadOptionValue(argument, "--duration", &value)) {
      config.duration_seconds = ParseUint64(value, "--duration");
    } else if (ReadOptionValue(argument, "--num", &value)) {
      config.num = ParseUint64(value, "--num");
    } else if (ReadOptionValue(argument, "--key_size", &value)) {
      config.key_size = ParseSize(value, "--key_size");
    } else if (ReadOptionValue(argument, "--value_size", &value)) {
      config.value_size = ParseSize(value, "--value_size");
    } else if (ReadOptionValue(argument, "--write_buffer_size", &value)) {
      config.write_buffer_size = ParseSize(value, "--write_buffer_size");
    } else if (ReadOptionValue(argument, "--max_file_size", &value)) {
      config.max_file_size = ParseSize(value, "--max_file_size");
    } else if (ReadOptionValue(argument, "--block_cache_size", &value)) {
      config.block_cache_size = ParseSize(value, "--block_cache_size");
    } else if (ReadOptionValue(argument, "--latency_sample", &value)) {
      config.latency_sample = ParseUint64(value, "--latency_sample");
    } else if (ReadOptionValue(argument, "--l0_sample_interval_ms", &value)) {
      config.l0_sample_interval_ms =
          ParseUint64(value, "--l0_sample_interval_ms");
    } else if (ReadOptionValue(argument, "--seed", &value)) {
      config.seed = ParseUint64(value, "--seed");
    } else if (ReadOptionValue(argument, "--bloom_bits", &value)) {
      config.bloom_bits = ParseInt(value, "--bloom_bits");
    } else if (ReadOptionValue(argument, "--compression", &value)) {
      if (value == "none") {
        config.compression = rocketdb::kNoCompression;
      } else if (value == "snappy") {
        config.compression = rocketdb::kSnappyCompression;
      } else if (value == "zstd") {
        config.compression = rocketdb::kZstdCompression;
      } else {
        Fail("invalid --compression: " + value);
      }
    } else if (ReadOptionValue(argument, "--mode", &value)) {
      if (value == "throughput") {
        config.measurement_mode = MeasurementMode::kThroughput;
      } else if (value == "latency") {
        config.measurement_mode = MeasurementMode::kLatency;
      } else {
        Fail("invalid --mode: " + value);
      }
    } else {
      Fail("unknown option: " + argument + " (use --help for usage)");
    }
  }

  if (config.num == 0 || config.duration_seconds == 0 ||
      config.latency_sample == 0 || config.l0_sample_interval_ms == 0) {
    Fail("--num, --duration, --latency_sample, and --l0_sample_interval_ms "
         "must be positive");
  }
  if (config.key_size == 0 || config.value_size == 0 ||
      config.write_buffer_size == 0 || config.max_file_size == 0 ||
      config.block_cache_size == 0) {
    Fail("all configured sizes must be positive");
  }
  for (const int threads : config.thread_counts) {
    if (threads <= 0)
      Fail("--threads values must be positive");
  }
  for (const int read_ratio : config.read_ratios) {
    if (read_ratio <= 0 || read_ratio >= 100) {
      Fail("--read_ratios values must be between 1 and 99");
    }
  }
  if (config.bloom_bits == 0 || config.bloom_bits < -1) {
    Fail("--bloom_bits must be positive when provided");
  }
  return config;
}

void WriteResultFile(const std::string &result) {
  std::ofstream output(ROCKETDB_MIXED_BENCH_RESULT_PATH,
                       std::ios::out | std::ios::trunc);
  if (!output) {
    Fail(std::string("cannot open benchmark result file: ") +
         ROCKETDB_MIXED_BENCH_RESULT_PATH);
  }
  output << result;
  if (!output) {
    Fail(std::string("cannot write benchmark result file: ") +
         ROCKETDB_MIXED_BENCH_RESULT_PATH);
  }
}

NumericProperty ParseNumericProperty(const std::string &text,
                                     const char *property_name) {
  NumericProperty result;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty())
      continue;
    const size_t separator = line.find('=');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == line.size()) {
      Fail(std::string("invalid ") + property_name + " line: " + line);
    }
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (!result.emplace(key, ParseUint64(value, property_name)).second) {
      Fail(std::string("duplicate ") + property_name + " key: " + key);
    }
  }
  if (result.find("version") == result.end()) {
    Fail(std::string(property_name) + " is missing version");
  }
  return result;
}

NumericProperty ReadNumericProperty(rocketdb::DB *db,
                                    const char *property_name) {
  std::string text;
  if (!db->GetProperty(property_name, &text)) {
    Fail(std::string("property is not supported: ") + property_name);
  }
  return ParseNumericProperty(text, property_name);
}

uint64_t PropertyValue(const NumericProperty &property,
                       const std::string &key) {
  const auto found = property.find(key);
  if (found == property.end())
    Fail("property key is missing: " + key);
  return found->second;
}

uint64_t PropertyDelta(const NumericProperty &begin, const NumericProperty &end,
                       const std::string &key) {
  const uint64_t begin_value = PropertyValue(begin, key);
  const uint64_t end_value = PropertyValue(end, key);
  if (end_value < begin_value)
    Fail("property counter decreased: " + key);
  return end_value - begin_value;
}

uint64_t ReadL0Files(rocketdb::DB *db) {
  std::string value;
  if (!db->GetProperty("rocketdb.num-files-at-level0", &value)) {
    Fail("rocketdb.num-files-at-level0 is not supported");
  }
  return ParseUint64(value, "rocketdb.num-files-at-level0");
}

class StartGate {
public:
  explicit StartGate(int participants) : participants_(participants) {}

  void WorkerReadyAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++ready_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return started_; });
  }

  void WaitUntilReady() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return ready_ == participants_; });
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    condition_.notify_all();
  }

private:
  const int participants_;
  int ready_ = 0;
  bool started_ = false;
  std::mutex mutex_;
  std::condition_variable condition_;
};

struct ThreadStats {
  uint64_t read_operations = 0;
  uint64_t write_operations = 0;
  uint64_t user_bytes_written = 0;
  std::vector<uint64_t> read_latency_ns;
  std::vector<uint64_t> write_latency_ns;
  std::string error;
};

struct LatencySummary {
  size_t samples = 0;
  double average_us = 0;
  double p50_us = 0;
  double p95_us = 0;
  double p99_us = 0;
  double p999_us = 0;
  double maximum_us = 0;
};

uint64_t NearestRank(const std::vector<uint64_t> &values, uint64_t permille) {
  const size_t rank =
      static_cast<size_t>((values.size() * permille + 999) / 1000);
  return values[rank - 1];
}

LatencySummary SummarizeLatency(std::vector<uint64_t> values) {
  LatencySummary summary;
  summary.samples = values.size();
  if (values.empty())
    return summary;
  std::sort(values.begin(), values.end());
  long double total_ns = 0;
  for (const uint64_t value : values)
    total_ns += value;
  const auto micros = [](uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1000.0;
  };
  summary.average_us = static_cast<double>(total_ns / values.size() / 1000.0L);
  summary.p50_us = micros(NearestRank(values, 500));
  summary.p95_us = micros(NearestRank(values, 950));
  summary.p99_us = micros(NearestRank(values, 990));
  summary.p999_us = micros(NearestRank(values, 999));
  summary.maximum_us = micros(values.back());
  return summary;
}

struct ScenarioResult {
  int threads = 0;
  int read_ratio = 0;
  double elapsed_seconds = 0;
  uint64_t read_operations = 0;
  uint64_t write_operations = 0;
  uint64_t user_bytes_written = 0;
  LatencySummary read_latency;
  LatencySummary write_latency;
  NumericProperty perf_begin;
  NumericProperty perf_end;
  NumericProperty cache_begin;
  NumericProperty cache_end;
  uint64_t current_l0_files = 0;
  uint64_t sampled_peak_l0_files = 0;
  std::string database_path;
};

class ScopedDatabase {
public:
  ScopedDatabase(std::string path, rocketdb::Options options, bool keep)
      : path_(std::move(path)), options_(options), keep_(keep) {}

  ~ScopedDatabase() {
    if (keep_)
      return;
    (void)rocketdb::DestroyDB(path_, options_);
    std::error_code error;
    fs::remove_all(path_, error);
  }

private:
  std::string path_;
  rocketdb::Options options_;
  bool keep_;
};

class BenchmarkRunner {
public:
  explicit BenchmarkRunner(Config config)
      : config_(std::move(config)),
        block_cache_read_env_(
            std::make_unique<BlockCacheReadEnv>(rocketdb::Env::Default())) {
    if (config_.bloom_bits > 0) {
      filter_policy_.reset(rocketdb::NewBloomFilterPolicy(config_.bloom_bits));
    }
    (void)KeyFor(config_.num - 1);
  }

  void Run() {
    WriteHeader();
    uint64_t scenario_index = 0;
    for (const int read_ratio : config_.read_ratios) {
      for (const int threads : config_.thread_counts) {
        std::cerr << "Running mixed scenario: " << read_ratio << "% reads, "
                  << threads << " thread(s), "
                  << MeasurementModeName(config_.measurement_mode) << "\n";
        const ScenarioResult result =
            RunScenario(read_ratio, threads, scenario_index++);
        WriteScenario(result);
      }
    }

    const std::string report = output_.str();
    WriteResultFile(report);
    std::cout << report
              << "Results written to: " << ROCKETDB_MIXED_BENCH_RESULT_PATH
              << '\n';
  }

private:
  rocketdb::Options MakeOptions(bool create_if_missing,
                                rocketdb::Cache *cache) const {
    rocketdb::Options options;
    options.create_if_missing = create_if_missing;
    options.write_buffer_size = config_.write_buffer_size;
    options.max_file_size = config_.max_file_size;
    options.block_cache = cache;
    options.env = block_cache_read_env_.get();
    options.compression = config_.compression;
    options.filter_policy = filter_policy_.get();
    return options;
  }

  std::string NewTemporaryDatabasePath(uint64_t scenario_index) const {
    std::error_code error;
    const fs::path temp_directory = fs::temp_directory_path(error);
    if (error)
      Fail("cannot find a temporary directory: " + error.message());
    const uint64_t timestamp = rocketdb::Env::Default()->NowMicros();
    for (uint64_t suffix = 0; suffix != 1000; ++suffix) {
      const fs::path candidate =
          temp_directory /
          ("rocketdb-mixed-bench-" + std::to_string(timestamp) + "-" +
           std::to_string(scenario_index) + "-" + std::to_string(suffix));
      if (!fs::exists(candidate, error) && !error)
        return candidate.string();
      if (error)
        Fail("cannot inspect temporary database path: " + error.message());
    }
    Fail("could not allocate a unique temporary database path");
  }

  std::unique_ptr<rocketdb::DB> OpenDatabase(const rocketdb::Options &options,
                                             const std::string &path) const {
    rocketdb::DB *database = nullptr;
    CheckOk(rocketdb::DB::Open(options, path, &database), "open database");
    return std::unique_ptr<rocketdb::DB>(database);
  }

  std::string KeyFor(uint64_t index) const {
    const std::string number = std::to_string(index);
    if (number.size() > config_.key_size)
      Fail("--key_size is too small for --num");
    return std::string(config_.key_size - number.size(), '0') + number;
  }

  std::string Value() const {
    std::string value(config_.value_size, 'a');
    for (size_t index = 0; index < value.size(); ++index) {
      value[index] = static_cast<char>('a' + index % 26);
    }
    return value;
  }

  void Prefill(const std::string &path) const {
    const rocketdb::Options options = MakeOptions(true, nullptr);
    std::unique_ptr<rocketdb::DB> database = OpenDatabase(options, path);
    const rocketdb::WriteOptions write_options;
    const std::string value = Value();
    for (uint64_t index = 0; index < config_.num; ++index) {
      CheckOk(database->Put(write_options, KeyFor(index), value),
              "prefill record");
    }
    database->CompactRange(nullptr, nullptr);
  }

  void VerifyBlockCache(rocketdb::DB *database,
                        rocketdb::Cache *block_cache) const {
    block_cache->Prune();
    const NumericProperty before =
        ReadNumericProperty(database, "rocketdb.block-cache-stats");
    const rocketdb::ReadOptions read_options;
    std::string value;
    CheckOk(database->Get(read_options, KeyFor(0), &value),
            "block cache verification first read");
    if (value.size() != config_.value_size)
      Fail("block cache verification returned an unexpected value size");
    const NumericProperty after_miss =
        ReadNumericProperty(database, "rocketdb.block-cache-stats");
    if (PropertyValue(after_miss, "block_cache_miss") <=
        PropertyValue(before, "block_cache_miss")) {
      Fail("block cache verification did not observe the first-read miss");
    }

    value.clear();
    CheckOk(database->Get(read_options, KeyFor(0), &value),
            "block cache verification second read");
    const NumericProperty after_hit =
        ReadNumericProperty(database, "rocketdb.block-cache-stats");
    if (PropertyValue(after_hit, "block_cache_hit") <=
        PropertyValue(after_miss, "block_cache_hit")) {
      Fail("block cache verification did not observe the second-read hit");
    }
    // Keep verification traffic and its warmed block outside the measured
    // interval. The counters are cumulative, so the begin snapshot is taken
    // only after this prune.
    block_cache->Prune();
  }

  void Worker(rocketdb::DB *database, int thread_index, int read_ratio,
              StartGate *gate, std::atomic<bool> *stop,
              std::condition_variable *stop_condition,
              ThreadStats *stats) const {
    std::mt19937_64 random(config_.seed +
                           static_cast<uint64_t>(thread_index) * 104729);
    std::uniform_int_distribution<uint64_t> key_distribution(0,
                                                             config_.num - 1);
    std::uniform_int_distribution<int> operation_distribution(0, 99);
    const rocketdb::ReadOptions read_options;
    rocketdb::WriteOptions write_options;
    write_options.sync = config_.sync;
    const std::string write_value = Value();
    std::string read_value;
    uint64_t operation_index = 0;

    gate->WorkerReadyAndWait();
    while (!stop->load(std::memory_order_relaxed)) {
      const bool is_read = operation_distribution(random) < read_ratio;
      const std::string key = KeyFor(key_distribution(random));
      const bool record_latency =
          config_.measurement_mode == MeasurementMode::kLatency &&
          operation_index % config_.latency_sample == 0;
      BenchmarkClock::time_point operation_start;
      if (record_latency)
        operation_start = BenchmarkClock::now();

      rocketdb::Status status;
      if (is_read) {
        read_value.clear();
        status = database->Get(read_options, key, &read_value);
        if (record_latency) {
          stats->read_latency_ns.push_back(ElapsedNanos(operation_start));
        }
        if (status.ok() && read_value.size() != config_.value_size) {
          status = rocketdb::Status::Corruption(
              "mixed read returned an unexpected value size");
        }
        if (status.ok())
          ++stats->read_operations;
      } else {
        status = database->Put(write_options, key, write_value);
        if (record_latency) {
          stats->write_latency_ns.push_back(ElapsedNanos(operation_start));
        }
        if (status.ok()) {
          ++stats->write_operations;
          stats->user_bytes_written += key.size() + write_value.size();
        }
      }
      ++operation_index;

      if (!status.ok()) {
        stats->error = status.ToString();
        stop->store(true, std::memory_order_relaxed);
        stop_condition->notify_all();
        return;
      }
    }
  }

  ScenarioResult RunScenario(int read_ratio, int threads,
                             uint64_t scenario_index) const {
    ScenarioResult result;
    result.threads = threads;
    result.read_ratio = read_ratio;
    result.database_path = NewTemporaryDatabasePath(scenario_index);

    const rocketdb::Options cleanup_options = MakeOptions(false, nullptr);
    ScopedDatabase cleanup(result.database_path, cleanup_options,
                           config_.keep_db);
    Prefill(result.database_path);

    std::unique_ptr<rocketdb::Cache> block_cache(
        rocketdb::NewLRUCacheWithStatistics(config_.block_cache_size));
    const rocketdb::Options measured_options =
        MakeOptions(false, block_cache.get());
    std::unique_ptr<rocketdb::DB> database =
        OpenDatabase(measured_options, result.database_path);

    VerifyBlockCache(database.get(), block_cache.get());
    result.perf_begin =
        ReadNumericProperty(database.get(), "rocketdb.perf-stats");
    result.cache_begin =
        ReadNumericProperty(database.get(), "rocketdb.block-cache-stats");
    if (PropertyValue(result.cache_begin, "supported") != 1) {
      Fail("statistics-enabled block cache did not expose counters");
    }
    result.sampled_peak_l0_files = ReadL0Files(database.get());

    StartGate gate(threads);
    std::atomic<bool> stop(false);
    std::mutex stop_mutex;
    std::condition_variable stop_condition;
    std::vector<ThreadStats> thread_stats(static_cast<size_t>(threads));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));
    for (int index = 0; index < threads; ++index) {
      workers.emplace_back(&BenchmarkRunner::Worker, this, database.get(),
                           index, read_ratio, &gate, &stop, &stop_condition,
                           &thread_stats[index]);
    }

    gate.WaitUntilReady();
    uint64_t sampled_peak_l0_files = result.sampled_peak_l0_files;
    std::string sampler_error;
    std::thread sampler([&] {
      try {
        while (!stop.load(std::memory_order_relaxed)) {
          sampled_peak_l0_files =
              std::max(sampled_peak_l0_files, ReadL0Files(database.get()));
          std::unique_lock<std::mutex> lock(stop_mutex);
          stop_condition.wait_for(
              lock, std::chrono::milliseconds(config_.l0_sample_interval_ms),
              [&] { return stop.load(std::memory_order_relaxed); });
        }
        sampled_peak_l0_files =
            std::max(sampled_peak_l0_files, ReadL0Files(database.get()));
      } catch (const std::exception &error) {
        sampler_error = error.what();
        stop.store(true, std::memory_order_relaxed);
        stop_condition.notify_all();
      }
    });

    const BenchmarkClock::time_point start = BenchmarkClock::now();
    gate.Start();
    {
      std::unique_lock<std::mutex> lock(stop_mutex);
      stop_condition.wait_for(
          lock, std::chrono::seconds(config_.duration_seconds),
          [&] { return stop.load(std::memory_order_relaxed); });
    }
    stop.store(true, std::memory_order_relaxed);
    stop_condition.notify_all();
    for (std::thread &worker : workers)
      worker.join();
    sampler.join();
    result.elapsed_seconds =
        static_cast<double>(ElapsedNanos(start)) / 1000000000.0;

    if (!sampler_error.empty())
      Fail("L0 sampler: " + sampler_error);
    std::vector<uint64_t> read_latencies;
    std::vector<uint64_t> write_latencies;
    for (ThreadStats &stats : thread_stats) {
      if (!stats.error.empty())
        Fail("mixed worker: " + stats.error);
      result.read_operations += stats.read_operations;
      result.write_operations += stats.write_operations;
      result.user_bytes_written += stats.user_bytes_written;
      read_latencies.insert(read_latencies.end(), stats.read_latency_ns.begin(),
                            stats.read_latency_ns.end());
      write_latencies.insert(write_latencies.end(),
                             stats.write_latency_ns.begin(),
                             stats.write_latency_ns.end());
    }

    result.read_latency = SummarizeLatency(std::move(read_latencies));
    result.write_latency = SummarizeLatency(std::move(write_latencies));
    result.current_l0_files = ReadL0Files(database.get());
    result.sampled_peak_l0_files =
        std::max(sampled_peak_l0_files, result.current_l0_files);
    result.perf_end =
        ReadNumericProperty(database.get(), "rocketdb.perf-stats");
    result.cache_end =
        ReadNumericProperty(database.get(), "rocketdb.block-cache-stats");
    return result;
  }

  static double ToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / 1048576.0;
  }

  void WriteHeader() {
    const long double dataset_bytes = static_cast<long double>(config_.num) *
                                      (config_.key_size + config_.value_size);
    output_
        << "# RocketDB Mixed Read/Write Benchmark\n\n"
        << "## Configuration\n\n"
        << "| Setting | Value |\n"
        << "|---|---:|\n"
        << "| Records per scenario | " << config_.num << " |\n"
        << "| Approx. user dataset (MiB) | " << std::fixed
        << std::setprecision(2)
        << static_cast<double>(dataset_bytes / 1048576.0L) << " |\n"
        << "| Block cache (MiB) | " << ToMiB(config_.block_cache_size) << " |\n"
        << "| Duration per scenario (s) | " << config_.duration_seconds
        << " |\n"
        << "| Mode | " << MeasurementModeName(config_.measurement_mode)
        << " |\n"
        << "| SST read path | Block Cache compatible (mmap reads copied) |\n"
        << "| L0 sampling interval (ms) | " << config_.l0_sample_interval_ms
        << " |\n";
    if (config_.measurement_mode == MeasurementMode::kLatency) {
      output_ << "| Latency sampling | every " << config_.latency_sample
              << " operation |\n";
    }
    output_ << '\n';
    if (dataset_bytes <= config_.block_cache_size) {
      output_ << "> Warning: the configured user dataset is not larger than "
                 "the block cache.\n\n";
    }
  }

  void WriteLatencyRow(const char *operation, const LatencySummary &latency) {
    output_ << "| " << operation << " | " << latency.samples;
    if (latency.samples == 0) {
      output_ << " | n/a | n/a | n/a | n/a | n/a | n/a |\n";
      return;
    }
    output_ << std::fixed << std::setprecision(3) << " | " << latency.average_us
            << " | " << latency.p50_us << " | " << latency.p95_us << " | "
            << latency.p99_us << " | " << latency.p999_us << " | "
            << latency.maximum_us << " |\n";
  }

  void WriteScenario(const ScenarioResult &result) {
    const double read_ops_per_second =
        result.elapsed_seconds == 0
            ? 0
            : result.read_operations / result.elapsed_seconds;
    const double write_ops_per_second =
        result.elapsed_seconds == 0
            ? 0
            : result.write_operations / result.elapsed_seconds;
    const double total_ops_per_second =
        read_ops_per_second + write_ops_per_second;

    output_ << "## " << result.read_ratio << "% read / "
            << (100 - result.read_ratio) << "% write — " << result.threads
            << " thread(s)\n\n"
            << "### Foreground\n\n"
            << "| Metric | Value |\n"
            << "|---|---:|\n"
            << "| Actual duration (s) | " << std::fixed << std::setprecision(3)
            << result.elapsed_seconds << " |\n"
            << "| Read operations | " << result.read_operations << " |\n"
            << "| Read ops/s | " << std::setprecision(2) << read_ops_per_second
            << " |\n"
            << "| Write operations | " << result.write_operations << " |\n"
            << "| Write ops/s | " << write_ops_per_second << " |\n"
            << "| Total ops/s | " << total_ops_per_second << " |\n";

    if (config_.measurement_mode == MeasurementMode::kLatency) {
      output_ << "\nLatency values use raw nanosecond samples and nearest-rank "
                 "percentiles.\n\n"
              << "| Operation | Samples | Average (us) | P50 (us) | P95 (us) | "
                 "P99 (us) | P99.9 (us) | Max (us) |\n"
              << "|---|---:|---:|---:|---:|---:|---:|---:|\n";
      WriteLatencyRow("Read", result.read_latency);
      WriteLatencyRow("Write", result.write_latency);
    }

    uint64_t major_count = 0;
    uint64_t major_total_micros = 0;
    uint64_t major_bytes_read = 0;
    uint64_t major_bytes_written = 0;
    uint64_t trivial_moves = 0;
    for (int level = 0; level < rocketdb::config::kNumLevels; ++level) {
      const std::string suffix = "." + std::to_string(level);
      major_count += PropertyDelta(result.perf_begin, result.perf_end,
                                   "major_compaction_count" + suffix);
      major_total_micros +=
          PropertyDelta(result.perf_begin, result.perf_end,
                        "major_compaction_total_micros" + suffix);
      major_bytes_read += PropertyDelta(result.perf_begin, result.perf_end,
                                        "major_compaction_bytes_read" + suffix);
      major_bytes_written +=
          PropertyDelta(result.perf_begin, result.perf_end,
                        "major_compaction_bytes_written" + suffix);
      trivial_moves += PropertyDelta(result.perf_begin, result.perf_end,
                                     "trivial_move_count" + suffix);
    }

    const uint64_t flush_count =
        PropertyDelta(result.perf_begin, result.perf_end, "flush_count");
    const uint64_t flush_total_micros =
        PropertyDelta(result.perf_begin, result.perf_end, "flush_total_micros");
    const uint64_t flush_bytes = PropertyDelta(
        result.perf_begin, result.perf_end, "flush_bytes_written");
    output_ << "\n### LSM and cache\n\n"
            << "| Metric | Value |\n"
            << "|---|---:|\n"
            << "| Flush count | " << flush_count << " |\n"
            << "| Flush total (ms) | " << std::fixed << std::setprecision(3)
            << flush_total_micros / 1000.0 << " |\n"
            << "| Flush max since DB open (us) | "
            << PropertyValue(result.perf_end, "flush_max_micros") << " |\n"
            << "| Flush bytes (MiB) | " << ToMiB(flush_bytes) << " |\n"
            << "| Major compaction count | " << major_count << " |\n"
            << "| Major compaction total (ms) | " << major_total_micros / 1000.0
            << " |\n"
            << "| Major bytes read (MiB) | " << ToMiB(major_bytes_read)
            << " |\n"
            << "| Major bytes written (MiB) | " << ToMiB(major_bytes_written)
            << " |\n"
            << "| Trivial moves | " << trivial_moves << " |\n"
            << "| Current L0 files | " << result.current_l0_files << " |\n"
            << "| Sampled peak L0 files | " << result.sampled_peak_l0_files
            << " |\n";
    if (result.user_bytes_written == 0) {
      output_ << "| SST write amplification | n/a |\n";
    } else {
      const double sst_write_amplification =
          static_cast<double>(flush_bytes + major_bytes_written) /
          static_cast<double>(result.user_bytes_written);
      output_ << "| SST write amplification | " << std::setprecision(3)
              << sst_write_amplification << " |\n";
    }

    const uint64_t cache_hit =
        PropertyDelta(result.cache_begin, result.cache_end, "block_cache_hit");
    const uint64_t cache_miss =
        PropertyDelta(result.cache_begin, result.cache_end, "block_cache_miss");
    const uint64_t cache_lookups = cache_hit + cache_miss;
    output_ << "| Block cache hits | " << cache_hit << " |\n"
            << "| Block cache misses | " << cache_miss << " |\n";
    if (cache_lookups == 0) {
      output_ << "| Block cache hit rate | n/a |\n";
    } else {
      output_ << "| Block cache hit rate | " << std::setprecision(2)
              << 100.0 * cache_hit / cache_lookups << "% |\n";
    }
    output_ << "\nBlock Cache counts include foreground and background data-block "
               "lookups; TableCache is excluded.\n";

    output_ << "\n### Write stalls\n\n"
            << "| Reason | Count | Total (ms) | Max since DB open (us) |\n"
            << "|---|---:|---:|---:|\n";
    WriteStallRow(result, "Immutable MemTable", "immutable_memtable_wait");
    WriteStallRow(result, "L0 slowdown", "l0_slowdown");
    WriteStallRow(result, "L0 stop", "l0_stop");

    bool wrote_level_header = false;
    for (int level = 0; level < rocketdb::config::kNumLevels; ++level) {
      const std::string suffix = "." + std::to_string(level);
      const uint64_t level_flushes =
          PropertyDelta(result.perf_begin, result.perf_end,
                        "flush_count_by_output_level" + suffix);
      const uint64_t level_major =
          PropertyDelta(result.perf_begin, result.perf_end,
                        "major_compaction_count" + suffix);
      const uint64_t level_trivial = PropertyDelta(
          result.perf_begin, result.perf_end, "trivial_move_count" + suffix);
      if (level_flushes == 0 && level_major == 0 && level_trivial == 0)
        continue;
      if (!wrote_level_header) {
        output_ << "\n### Per-output-level activity\n\n"
                << "| Level | Flushes | Major | Major read (MiB) | Major write "
                   "(MiB) | Major max since DB open (us) | Trivial moves |\n"
                << "|---:|---:|---:|---:|---:|---:|---:|\n";
        wrote_level_header = true;
      }
      output_ << "| " << level << " | " << level_flushes << " | " << level_major
              << " | " << std::fixed << std::setprecision(3)
              << ToMiB(PropertyDelta(result.perf_begin, result.perf_end,
                                     "major_compaction_bytes_read" + suffix))
              << " | "
              << ToMiB(PropertyDelta(result.perf_begin, result.perf_end,
                                     "major_compaction_bytes_written" + suffix))
              << " | "
              << PropertyValue(result.perf_end,
                               "major_compaction_max_micros" + suffix)
              << " | " << level_trivial << " |\n";
    }
    if (config_.keep_db)
      output_ << "\nDatabase kept at: `" << result.database_path << "`\n";
    output_ << '\n';
  }

  void WriteStallRow(const ScenarioResult &result, const char *label,
                     const std::string &prefix) {
    output_ << "| " << label << " | "
            << PropertyDelta(result.perf_begin, result.perf_end,
                             prefix + "_count")
            << " | " << std::fixed << std::setprecision(3)
            << PropertyDelta(result.perf_begin, result.perf_end,
                             prefix + "_total_micros") /
                   1000.0
            << " | " << PropertyValue(result.perf_end, prefix + "_max_micros")
            << " |\n";
  }

  Config config_;
  std::unique_ptr<BlockCacheReadEnv> block_cache_read_env_;
  std::unique_ptr<const rocketdb::FilterPolicy> filter_policy_;
  std::ostringstream output_;
};

} // namespace

int main(int argc, char **argv) {
  try {
    BenchmarkRunner(ParseArgs(argc, argv)).Run();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "rocketdb_mixed_bench: " << error.what() << '\n';
    return 1;
  }
}
