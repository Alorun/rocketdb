#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "db.h"
#include "env.h"
#include "filter_policy.h"
#include "options.h"
#include "status.h"
#include "util/histogram.h"

namespace {

namespace fs = std::filesystem;
using BenchmarkClock = std::chrono::steady_clock;

enum class MeasurementMode { kThroughput, kLatency };

uint64_t ElapsedNanos(BenchmarkClock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(BenchmarkClock::now() - start).count());
}

const char* MeasurementModeName(MeasurementMode mode) {
  return mode == MeasurementMode::kThroughput ? "throughput" : "latency";
}

[[noreturn]] void Fail(const std::string& message);

void WriteResultFile(const std::string& result) {
  std::ofstream output(ROCKETDB_BENCH_RESULT_PATH, std::ios::out | std::ios::trunc);
  if (!output) {
    Fail(std::string("cannot open benchmark result file: ") + ROCKETDB_BENCH_RESULT_PATH);
  }
  output << result;
  if (!output) {
    Fail(std::string("cannot write benchmark result file: ") + ROCKETDB_BENCH_RESULT_PATH);
  }
}

struct Config {
  std::string benchmarks = "fillseq,fillrandom,readseq,readrandom";
  std::string db;
  uint64_t num = 1000000;
  uint64_t reads = 1000000;
  uint64_t seed = 301;
  size_t key_size = 16;
  size_t value_size = 100;
  size_t write_buffer_size = 4 << 20;
  size_t max_file_size = 2 << 20;
  int bloom_bits = -1;
  int threads = 1;
  uint64_t latency_sample = 1;
  rocketdb::CompressionType compression = rocketdb::kNoCompression;
  MeasurementMode measurement_mode = MeasurementMode::kLatency;
  bool sync = false;
  bool use_existing_db = false;
  bool keep_db = false;
};

[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
}

void CheckOk(const rocketdb::Status& status, const std::string& context) {
  if (!status.ok()) {
    Fail(context + ": " + status.ToString());
  }
}

uint64_t ParseUint64(const std::string& text, const char* name) {
  if (text.empty() || text.front() == '-') {
    Fail(std::string("invalid ") + name + ": " + text);
  }
  size_t consumed = 0;
  try {
    const unsigned long long value = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
      Fail(std::string("invalid ") + name + ": " + text);
    }
    if (value > std::numeric_limits<uint64_t>::max()) {
      Fail(std::string("invalid ") + name + ": " + text);
    }
    return static_cast<uint64_t>(value);
  } catch (const std::exception&) {
    Fail(std::string("invalid ") + name + ": " + text);
  }
}

size_t ParseSize(const std::string& text, const char* name) {
  const uint64_t value = ParseUint64(text, name);
  if (value > std::numeric_limits<size_t>::max()) {
    Fail(std::string("invalid ") + name + ": " + text);
  }
  return static_cast<size_t>(value);
}

int ParseInt(const std::string& text, const char* name) {
  const uint64_t value = ParseUint64(text, name);
  if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    Fail(std::string("invalid ") + name + ": " + text);
  }
  return static_cast<int>(value);
}

bool ReadOptionValue(const std::string& argument, const char* name, std::string* value) {
  const std::string prefix = std::string(name) + "=";
  if (argument.rfind(prefix, 0) != 0) return false;
  *value = argument.substr(prefix.size());
  return true;
}

std::vector<std::string> SplitBenchmarks(const std::string& benchmarks) {
  std::vector<std::string> result;
  std::stringstream stream(benchmarks);
  std::string name;
  while (std::getline(stream, name, ',')) {
    if (name.empty()) Fail("empty benchmark name");
    result.push_back(name);
  }
  if (result.empty()) Fail("at least one benchmark is required");
  return result;
}

void PrintUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n\n"
      << "Basic workloads:\n"
      << "  fillseq, fillrandom, readseq, readrandom\n\n"
      << "Options:\n"
      << "  --benchmarks=LIST       Comma-separated workloads (default: fillseq,fillrandom,readseq,readrandom)\n"
      << "  --num=N                 Records to write or prefill (default: 1000000)\n"
      << "  --reads=N               Reads per read workload (default: 1000000)\n"
      << "  --key_size=N            Fixed key width in bytes (default: 16)\n"
      << "  --value_size=N          Value size in bytes (default: 100)\n"
      << "  --write_buffer_size=N   MemTable size in bytes (default: 4194304)\n"
      << "  --max_file_size=N       SSTable target size in bytes (default: 2097152)\n"
      << "  --compression=TYPE      none, snappy, or zstd (default: none)\n"
      << "  --bloom_bits=N          Enable Bloom filter with N bits/key\n"
      << "  --sync                  Enable WAL sync for each write\n"
      << "  --mode=MODE             throughput or latency (default: latency)\n"
      << "  --latency_sample=N      Record every Nth operation in latency mode (default: 1)\n"
      << "  --seed=N                Random workload seed (default: 301)\n"
      << "  --threads=1             Reserved for a later multi-threaded benchmark\n"
      << "  --db=PATH --use_existing_db\n"
      << "                           Run against an existing DB; this tool never removes it\n"
      << "  --keep_db               Keep the automatically-created temporary DB\n";
}

Config ParseArgs(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    std::string value;
    if (argument == "--help" || argument == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (argument == "--sync") {
      config.sync = true;
    } else if (argument == "--use_existing_db") {
      config.use_existing_db = true;
    } else if (argument == "--keep_db") {
      config.keep_db = true;
    } else if (ReadOptionValue(argument, "--benchmarks", &value)) {
      config.benchmarks = value;
    } else if (ReadOptionValue(argument, "--db", &value)) {
      config.db = value;
    } else if (ReadOptionValue(argument, "--num", &value)) {
      config.num = ParseUint64(value, "--num");
    } else if (ReadOptionValue(argument, "--reads", &value)) {
      config.reads = ParseUint64(value, "--reads");
    } else if (ReadOptionValue(argument, "--seed", &value)) {
      config.seed = ParseUint64(value, "--seed");
    } else if (ReadOptionValue(argument, "--latency_sample", &value)) {
      config.latency_sample = ParseUint64(value, "--latency_sample");
    } else if (ReadOptionValue(argument, "--key_size", &value)) {
      config.key_size = ParseSize(value, "--key_size");
    } else if (ReadOptionValue(argument, "--value_size", &value)) {
      config.value_size = ParseSize(value, "--value_size");
    } else if (ReadOptionValue(argument, "--write_buffer_size", &value)) {
      config.write_buffer_size = ParseSize(value, "--write_buffer_size");
    } else if (ReadOptionValue(argument, "--max_file_size", &value)) {
      config.max_file_size = ParseSize(value, "--max_file_size");
    } else if (ReadOptionValue(argument, "--bloom_bits", &value)) {
      config.bloom_bits = ParseInt(value, "--bloom_bits");
    } else if (ReadOptionValue(argument, "--threads", &value)) {
      config.threads = ParseInt(value, "--threads");
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

  if (config.num == 0 || config.reads == 0 || config.latency_sample == 0) {
    Fail("--num, --reads, and --latency_sample must be greater than zero");
  }
  if (config.key_size == 0) Fail("--key_size must be greater than zero");
  if (config.threads != 1) Fail("only --threads=1 is supported in the first benchmark phase");
  if (config.bloom_bits == 0 || config.bloom_bits < -1) {
    Fail("--bloom_bits must be positive when provided");
  }
  if (!config.db.empty() && !config.use_existing_db) {
    Fail("--db requires --use_existing_db so the benchmark cannot remove a user directory");
  }
  if (config.db.empty() && config.use_existing_db) {
    Fail("--use_existing_db requires --db=PATH");
  }
  return config;
}

class Stats {
 public:
  Stats(MeasurementMode measurement_mode, uint64_t expected_operations, uint64_t latency_sample)
      : measurement_mode_(measurement_mode), latency_sample_(latency_sample) {
    if (measurement_mode_ == MeasurementMode::kLatency) {
      const uint64_t sample_count = expected_operations / latency_sample_ +
                                    (expected_operations % latency_sample_ == 0 ? 0 : 1);
      latency_ns_.reserve(static_cast<size_t>(sample_count));
    }
  }

  bool ShouldRecordLatency(uint64_t operation_index) const {
    return measurement_mode_ == MeasurementMode::kLatency &&
           operation_index % latency_sample_ == 0;
  }

  void FinishedOperation(uint64_t bytes) {
    ++operations_;
    bytes_ += bytes;
  }

  void RecordLatency(uint64_t latency_ns) {
    latency_ns_.push_back(latency_ns);
    // Histogram only visualizes the distribution. Percentiles are calculated
    // below from latency_ns_ after sorting the original nanosecond samples.
    latency_.Add(static_cast<double>(latency_ns) / 1000.0);
  }

  void Report(const std::string& name, uint64_t elapsed_ns, std::ostream& output) {
    const double seconds = static_cast<double>(elapsed_ns) / 1000000000.0;
    const double ops_per_second = seconds == 0.0 ? 0.0 : operations_ / seconds;
    const double megabytes_per_second =
        seconds == 0.0 ? 0.0 : (static_cast<double>(bytes_) / 1048576.0) / seconds;

    output << name << ": " << operations_ << " ops, " << std::fixed << std::setprecision(2)
           << seconds << " s, " << ops_per_second << " ops/s, " << megabytes_per_second
           << " MiB/s, mode: " << MeasurementModeName(measurement_mode_) << '\n';
    if (measurement_mode_ != MeasurementMode::kLatency) return;

    if (latency_ns_.empty()) Fail("latency mode did not record any samples");
    std::sort(latency_ns_.begin(), latency_ns_.end());
    long double total_ns = 0;
    for (const uint64_t latency_ns : latency_ns_) total_ns += latency_ns;

    output << "Latency samples: " << latency_ns_.size() << " (every " << latency_sample_
           << " operation, stored as raw nanoseconds)\n"
           << std::fixed << std::setprecision(3)
           << "  average: " << static_cast<double>(total_ns / latency_ns_.size() / 1000.0L)
           << " us  p50: " << ToMicros(NearestRank(50)) << " us  p95: "
           << ToMicros(NearestRank(95)) << " us  p99: " << ToMicros(NearestRank(99))
           << " us  min: " << ToMicros(latency_ns_.front()) << " us  max: "
           << ToMicros(latency_ns_.back()) << " us\n";
    output << "Latency histogram (microseconds; visualization only):\n" << latency_.ToString();
  }

 private:
  uint64_t NearestRank(uint64_t percentile) const {
    const size_t rank = (latency_ns_.size() * percentile + 99) / 100;
    return latency_ns_[rank - 1];
  }

  static double ToMicros(uint64_t latency_ns) {
    return static_cast<double>(latency_ns) / 1000.0;
  }

  uint64_t operations_ = 0;
  uint64_t bytes_ = 0;
  MeasurementMode measurement_mode_;
  uint64_t latency_sample_;
  std::vector<uint64_t> latency_ns_;
  rocketdb::Histogram latency_;
};

class BenchmarkRunner {
 public:
  explicit BenchmarkRunner(Config config) : config_(std::move(config)) {
    if (config_.bloom_bits > 0) {
      filter_policy_.reset(rocketdb::NewBloomFilterPolicy(config_.bloom_bits));
    }

    if (config_.db.empty()) {
      owns_database_ = true;
      dbname_ = NewTemporaryDatabasePath();
    } else {
      dbname_ = config_.db;
    }

    // Validate that the largest generated numeric key fits the requested fixed width.
    (void)KeyFor(config_.num - 1);
  }

  ~BenchmarkRunner() { Cleanup(); }

  void Run() {
    output_ << "RocketDB basic benchmark\n"
            << "  db: " << dbname_ << (owns_database_ ? " (temporary)" : " (existing)") << '\n'
            << "  num: " << config_.num << ", reads: " << config_.reads
            << ", key_size: " << config_.key_size << ", value_size: " << config_.value_size
            << ", threads: 1, mode: " << MeasurementModeName(config_.measurement_mode);
    if (config_.measurement_mode == MeasurementMode::kLatency) {
      output_ << ", latency sample: every " << config_.latency_sample << " operation";
    }
    output_ << '\n';

    for (const std::string& benchmark : SplitBenchmarks(config_.benchmarks)) {
      if (benchmark == "fillseq") {
        RunFill("fillseq", true);
      } else if (benchmark == "fillrandom") {
        RunFill("fillrandom", false);
      } else if (benchmark == "readseq") {
        RunRead("readseq", true);
      } else if (benchmark == "readrandom") {
        RunRead("readrandom", false);
      } else {
        Fail("unknown benchmark: " + benchmark);
      }
    }

    const std::string result = output_.str();
    WriteResultFile(result);
    std::cout << result << "Results written to: " << ROCKETDB_BENCH_RESULT_PATH << '\n';
  }

 private:
  rocketdb::Options MakeOptions() const {
    rocketdb::Options options;
    options.create_if_missing = !config_.use_existing_db;
    options.write_buffer_size = config_.write_buffer_size;
    options.max_file_size = config_.max_file_size;
    options.compression = config_.compression;
    options.filter_policy = filter_policy_.get();
    return options;
  }

  std::string NewTemporaryDatabasePath() const {
    std::error_code error;
    const fs::path temp_directory = fs::temp_directory_path(error);
    if (error) Fail("cannot find a temporary directory: " + error.message());

    const uint64_t timestamp = rocketdb::Env::Default()->NowMicros();
    for (uint64_t suffix = 0; suffix != 1000; ++suffix) {
      const fs::path candidate = temp_directory /
                                 ("rocketdb-bench-" + std::to_string(timestamp) + "-" +
                                  std::to_string(suffix));
      if (!fs::exists(candidate, error) && !error) return candidate.string();
      if (error) Fail("cannot inspect temporary database path: " + error.message());
    }
    Fail("could not allocate a unique temporary database path");
  }

  std::unique_ptr<rocketdb::DB> OpenDatabase() const {
    rocketdb::DB* database = nullptr;
    CheckOk(rocketdb::DB::Open(MakeOptions(), dbname_, &database), "open database");
    return std::unique_ptr<rocketdb::DB>(database);
  }

  void ResetOwnedDatabase() {
    if (!owns_database_) return;

    const rocketdb::Status status = rocketdb::DestroyDB(dbname_, MakeOptions());
    CheckOk(status, "remove temporary database");
    std::error_code error;
    fs::remove_all(dbname_, error);
    if (error) Fail("remove temporary database directory: " + error.message());
  }

  void PrepareReadDatabase() {
    if (config_.use_existing_db) return;

    ResetOwnedDatabase();
    std::unique_ptr<rocketdb::DB> database = OpenDatabase();
    const rocketdb::WriteOptions write_options;
    const std::string value = Value();
    for (uint64_t index = 0; index < config_.num; ++index) {
      CheckOk(database->Put(write_options, KeyFor(index), value), "prefill record");
    }

    // Move prefilled data out of the MemTable.  The subsequent reopen starts
    // the timed read benchmark without data left in process-local memory.
    database->CompactRange(nullptr, nullptr);
    database.reset();
  }

  std::string KeyFor(uint64_t index) const {
    const std::string number = std::to_string(index);
    if (number.size() > config_.key_size) {
      Fail("--key_size is too small for --num");
    }
    return std::string(config_.key_size - number.size(), '0') + number;
  }

  std::string Value() const {
    std::string value(config_.value_size, 'a');
    for (size_t index = 0; index < value.size(); ++index) {
      value[index] = static_cast<char>('a' + (index % 26));
    }
    return value;
  }

  void RunFill(const std::string& name, bool sequential) {
    // In the default mode this destroys the previous benchmark database, so
    // every fill workload starts with an empty database. With --use_existing_db
    // it intentionally does nothing, preserving the user-selected database.
    ResetOwnedDatabase();
    std::unique_ptr<rocketdb::DB> database = OpenDatabase();
    rocketdb::WriteOptions write_options;
    write_options.sync = config_.sync;
    const std::string value = Value();
    std::mt19937_64 random(config_.seed);
    std::uniform_int_distribution<uint64_t> distribution(0, config_.num - 1);
    Stats stats(config_.measurement_mode, config_.num, config_.latency_sample);

    const BenchmarkClock::time_point start = BenchmarkClock::now();
    for (uint64_t index = 0; index < config_.num; ++index) {
      const uint64_t key_index = sequential ? index : distribution(random);
      const std::string key = KeyFor(key_index);
      const bool record_latency = stats.ShouldRecordLatency(index);
      BenchmarkClock::time_point operation_start;
      if (record_latency) operation_start = BenchmarkClock::now();
      CheckOk(database->Put(write_options, key, value), name + " put");
      if (record_latency) stats.RecordLatency(ElapsedNanos(operation_start));
      stats.FinishedOperation(key.size() + value.size());
    }
    stats.Report(name, ElapsedNanos(start), output_);
  }

  void RunRead(const std::string& name, bool sequential) {
    PrepareReadDatabase();
    std::unique_ptr<rocketdb::DB> database = OpenDatabase();
    const rocketdb::ReadOptions read_options;
    if (sequential) {
      RunSequentialRead(name, database.get(), read_options);
      return;
    }

    std::mt19937_64 random(config_.seed);
    std::uniform_int_distribution<uint64_t> distribution(0, config_.num - 1);
    std::string value;
    Stats stats(config_.measurement_mode, config_.reads, config_.latency_sample);

    const BenchmarkClock::time_point start = BenchmarkClock::now();
    for (uint64_t index = 0; index < config_.reads; ++index) {
      const uint64_t key_index = sequential ? index % config_.num : distribution(random);
      const std::string key = KeyFor(key_index);
      value.clear();
      const bool record_latency = stats.ShouldRecordLatency(index);
      BenchmarkClock::time_point operation_start;
      if (record_latency) operation_start = BenchmarkClock::now();
      CheckOk(database->Get(read_options, key, &value), name + " get");
      if (value.size() != config_.value_size) {
        Fail(name + " returned an unexpected value size");
      }
      if (record_latency) stats.RecordLatency(ElapsedNanos(operation_start));
      stats.FinishedOperation(key.size() + value.size());
    }
    stats.Report(name, ElapsedNanos(start), output_);
  }

  void RunSequentialRead(const std::string& name, rocketdb::DB* database,
                         const rocketdb::ReadOptions& read_options) {
    std::unique_ptr<rocketdb::Iterator> iterator;
    const auto reset_iterator = [&]() {
      iterator.reset(database->NewIterator(read_options));
      iterator->SeekToFirst();
      CheckOk(iterator->status(), name + " iterator setup");
      if (!iterator->Valid()) Fail(name + " found no records");
    };

    Stats stats(config_.measurement_mode, config_.reads, config_.latency_sample);
    const BenchmarkClock::time_point start = BenchmarkClock::now();
    reset_iterator();
    for (uint64_t index = 0; index < config_.reads; ++index) {
      if (!iterator->Valid()) {
        CheckOk(iterator->status(), name + " iterator");
        reset_iterator();
      }

      const bool record_latency = stats.ShouldRecordLatency(index);
      BenchmarkClock::time_point operation_start;
      if (record_latency) operation_start = BenchmarkClock::now();
      const rocketdb::Slice key = iterator->key();
      const rocketdb::Slice value = iterator->value();
      if (value.size() != config_.value_size) {
        Fail(name + " returned an unexpected value size");
      }
      iterator->Next();
      if (record_latency) stats.RecordLatency(ElapsedNanos(operation_start));
      stats.FinishedOperation(key.size() + value.size());
    }
    CheckOk(iterator->status(), name + " iterator");
    stats.Report(name, ElapsedNanos(start), output_);
  }

  void Cleanup() noexcept {
    if (!owns_database_ || config_.keep_db || dbname_.empty()) return;

    (void)rocketdb::DestroyDB(dbname_, MakeOptions());
    std::error_code error;
    fs::remove_all(dbname_, error);
  }

  Config config_;
  std::string dbname_;
  bool owns_database_ = false;
  std::unique_ptr<const rocketdb::FilterPolicy> filter_policy_;
  std::ostringstream output_;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    BenchmarkRunner(ParseArgs(argc, argv)).Run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rocketdb_bench: " << error.what() << '\n';
    return 1;
  }
}
