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

CMakeLists.txt:1 will compile the static library rocketdb and build a simple interactive program db_test. The entry point is in main.cc:1, and the supported commands are:

put k1 v1
get k1
exit