# RocketDB

### Project Introduction

RocketDB is a local key-value database implementation based on the LSM-Tree approach, with an overall structure similar to RocketDB. It supports basic Put, Get, Delete, batch writes, iterators, snapshots, range compression, database repair, and destruction interfaces. The core write path consists of a WAL log and a MemTable. Data is ultimately flushed to SSTable files, and multi-level file versions and background compaction are managed through MANIFEST/VersionSet.

### Main Modules

- Public API: include/db.h:35, include/options.h:22

- Database Core: src/db/db_impl.cc:124, includes open, restore, read/write, MemTable flush, background compaction

- Version Management: src/db/version_set.cc:728, manages MANIFEST, SST file levels, and compaction selection

- Table Files/SSTable: src/table/table_builder.cc:1, src/table/table.cc:1

- WAL Log: src/wal/log_writer.cc:1, src/wal/log_reader.cc:1

- Utility Layer: Caching, encoding, CRC, Bloom filter, Env POSIX, etc., are in src/util

### Build and Examples

CMakeLists.txt will compile the static library `rocketdb`. If the repository also contains `main.cc`, it will build the interactive `db_test` example. The supported commands are:

put k1 v1
get k1
exit

### Regression Tests

The regression suite is in `test/db_regression_test.cc`. It uses isolated temporary database directories and covers CRUD, WriteBatch/WAL recovery, snapshots, iterators, Bloom filters, flush/compaction/SST reopening, concurrent writers, and the C API.

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To run the same suite under AddressSanitizer:

```bash
cmake -S . -B build-asan -DBUILD_TESTING=ON -DROCKETDB_ENABLE_ASAN=ON
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

### Basic Benchmarks

The repository includes a standalone, single-threaded benchmark for basic
write and read workloads. Benchmark targets are opt-in and are not part of
CTest. It provides separate `throughput` and `latency` measurement modes.
Latency mode is the default and includes the distribution histogram; select
`--mode=throughput` for a clock-free per-operation throughput measurement.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DROCKETDB_BUILD_BENCHMARKS=ON
cmake --build build-release -j
./build-release/rocketdb_bench \
  --benchmarks=fillseq,fillrandom,readseq,readrandom \
  --num=1000000 --reads=1000000 --key_size=16 --value_size=100 \
  --compression=none --mode=throughput
```

Throughput mode times only the complete workload. It does not read a clock for
each operation. Latency mode stores each sampled operation's raw `uint64_t`
nanosecond duration, sorts those samples, and reports nearest-rank p50/p95/p99
from the real data rather than from histogram buckets:

```bash
./build-release/rocketdb_bench --benchmarks=readrandom --num=1000000 \
  --reads=1000000 --mode=latency --latency_sample=1
```

Use a larger `--latency_sample` (for example `100`) to reduce timing overhead
while collecting a representative sample. The distribution histogram is only a
visualization; it does not participate in percentile calculations. Each run
overwrites `bench/benchmark_results.txt` with the complete output.

Available workloads are `fillseq`, `fillrandom`, `readseq`, and `readrandom`.
`readseq` scans with an iterator; `readrandom` issues point reads.
Read workloads prefill and compact a temporary database outside the timed
section. Each `fillseq` or `fillrandom` workload resets that temporary
database first, so write workloads do not share data and each starts empty.
The same temporary directory path is recreated between workloads and removed
after the run. To run against an existing database, pass both
`--db=/path/to/db` and `--use_existing_db`; the benchmark never removes that
directory, so workloads intentionally share that user-selected state.
Existing-database read workloads expect the benchmark's fixed-width numeric
keys in `[0, num)` and values of `--value_size` bytes. Run
`rocketdb_bench --help` for all options.
