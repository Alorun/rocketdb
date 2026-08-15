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
