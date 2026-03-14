1. WAL 模块 (Write-Ahead Logging)
在数据写入 MemTable 之前，必须先写日志以防宕机丢失。你需要实现基于固定大小 Block（如 32KB）的日志格式。
    新建文件：
        src/wal/log_format.h (定义 Record Type，如 Full, First, Middle, Last)
        src/wal/log_writer.h / .cc (负责将 WriteBatch 序列化并追加写入物理文件)
        src/wal/log_reader.h / .cc (负责在数据库重启时解析日志，恢复 MemTable)

2. Table 模块 (SSTable 的写入端)
MemTable 写满后（比如达到 4MB），需要冻结（Immutable MemTable）并转换为 SSTable 写入磁盘。你需要先实现写盘能力。
    新建文件：
        src/table/format.h / .cc (定义 Footer 结构和 BlockHandle)
        src/table/block_builder.h / .cc (核心难点：实现键值对的前缀压缩/Prefix Compression，将数据打包成 Block)
        src/table/table_builder.h / .cc (高层封装，依次写入 Data Block，最后生成 Index Block 和 Footer 闭合文件)

3. DB 模块 (顶层枢纽与批量写入)
你需要一个对外的门面类，以及能够把多条 Put/Delete 指令打包成一个原子的写入批次。
    新建文件：
        src/db/write_batch.h / .cc (用户 API 和底层引擎的中间层，将多条记录编码为连续的字节流)
        src/db/db_impl.h / .cc (实现核心的 Put 和 Get：Put 负责先写 WAL 再写 MemTable；Get 负责先查 MemTable，没有再查磁盘)

4. Cache 与 Table 读取端 (稍后阶段)
写入能力通了之后，再做磁盘文件的读取和缓存。
    新建文件：
        src/cache/lru_cache.h / .cc (实现基于哈希表和双向链表的 LRU 缓存)
        src/table/block.h / .cc (解析 Block 并在其中进行二分查找)
        src/table/table.h / .cc (解析 Footer 和 Index，打通 SSTable 的读取)