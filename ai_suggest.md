 你要实现         → 对应源码位置
-----------------------------------------
基础组件         → util/ + include/leveldb/
SkipList         → db/skiplist.h
MemTable         → db/memtable.*
InternalKey      → db/dbformat.*
WAL              → db/log_*
DB 写路径        → db/db_impl.cc
SSTable 写       → table/table_builder.cc
SSTable 读       → table/table.cc
Version 管理     → db/version_set.*
Compaction       → db/version_set.cc