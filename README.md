# rocketdb

### 项目简介

rocketdb 是一个基于 LSM-Tree 思路的本地键值数据库实现，整体结构参考 LevelDB。它支持基础的 Put、Get、
Delete、批量写入、迭代器、快照、范围压缩、数据库修复与销毁等接口。核心写入路径由 WAL 日志和 MemTable 组
成，数据最终刷写为 SSTable 文件，并通过 MANIFEST / VersionSet 管理多层文件版本与后台 Compaction。

### 主要模块

- 公共 API：include/db.h:35、include/options.h:22
- 数据库核心：src/db/db_impl.cc:124，包含打开、恢复、读写、MemTable flush、后台 compaction
- 版本管理：src/db/version_set.cc:728，管理 MANIFEST、SST 文件层级和 compaction 选择
- 表文件/SSTable：src/table/table_builder.cc:1、src/table/table.cc:1
- WAL 日志：src/wal/log_writer.cc:1、src/wal/log_reader.cc:1
- 工具层：缓存、编码、CRC、Bloom filter、Env POSIX 等在 src/util

### 构建与示例

CMakeLists.txt:1 会编译静态库 rocketdb，并构建一个简单交互程序 db_test。入口在 main.cc:1，支持命令：

put k1 v1
get k1
exit