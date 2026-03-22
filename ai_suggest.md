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


### 阶段一：打通 SSTable 读取链路 (Table Reader)
你已经能生成 SSTable，现在需要把它们读出来。这一步是后续一切的基础。

* **1. `src/table/block.cc` & `block.h`**
    * **目标：** 实现 `Block` 类。解析你之前用 `BlockBuilder` 刷入的 Data Block 和 Index Block。
    * **核心逻辑：** 解析 Restart Points（重启点），实现一个内部的 `Block::Iter`，让它可以根据二分查找在 Block 内部快速定位 Key。
* **2. `src/table/two_level_iterator.cc`** (如果你倾向于将其独立)
    * **目标：** 实现两级迭代器。
    * **核心逻辑：** 顶层迭代器遍历 Index Block，根据 Index Block 里的 offset 和 size 读取对应的 Data Block，然后底层迭代器遍历这个 Data Block。
* **3. `src/table/table.cc`** (对应 `include/table.h`)
    * **目标：** SSTable 文件的统一入口。
    * **核心逻辑：** 读取文件末尾的 Footer，解析出 Index Block 和 Meta Block (布隆过滤器) 的位置。提供 `InternalGet` 接口供上层点查。

### 阶段二：引入缓存机制 (Cache)
数据库的高性能离不开缓存，这在接下来的多版本管理和高频读取中极其重要。

* **4. `src/util/cache.cc`** (对应 `include/cache.h`)
    * **目标：** 实现一个基于哈希表和双向链表的 Sharded LRU Cache（分片 LRU 缓存，降低锁冲突）。
    * **核心逻辑：** 需要支持自定义的删除回调函数（Deleter）。这个组件将被实例化为 `TableCache`（缓存打开的文件句柄和 Index Block）和 `BlockCache`（缓存解压后的 Data Block 数据）。

### 阶段三：多路归并与基础设施 (Merge)
在进入复杂的 Compaction 之前，需要准备好合并多个数据源的工具。

* **5. `src/table/merger.cc` & `merger.h`**
    * **目标：** 实现多路归并迭代器。
    * **核心逻辑：** 接收一组子 Iterator（比如来自 MemTable, L0 SSTable, L1 SSTable），内部维护一个小顶堆或简单的遍历比较机制，对外提供一个全局有序的 Iterator 视图。

### 阶段四：多版本与元数据管理 (The Brains)
这是 LevelDB 最精妙、也是 C++ 内存管理和指针操作最复杂的部分。

* **6. `src/db/filename.cc` & `filename.h`**
    * **目标：** 文件命名规范与解析。
    * **核心逻辑：** 生成和解析 `*.sst`, `*.log`, `MANIFEST-*`, `CURRENT` 等文件的名称和 FileNumber。
* **7. `src/db/version_edit.cc` & `version_edit.h`**
    * **目标：** 记录状态的增量变化。
    * **核心逻辑：** 记录“删除了哪些层的哪些文件”、“新增了哪些层的哪些文件”、“LogNumber 前进了多少”。提供序列化和反序列化方法，用于写入 MANIFEST。
* **8. `src/db/version_set.cc` & `version_set.h`**
    * **目标：** 系统的“大脑”，管理整个 MVCC 生命周期。
    * **核心逻辑：**
        * 维护一个 `Version` 双向链表。
        * 实现 `LogAndApply`：将 `VersionEdit` 应用到当前 `Version` 产生新 `Version`，并持久化到 MANIFEST 文件。
        * 提供 `PickCompaction`：计算每一层的分数，决定下一次该对哪些文件进行 Compaction。

### 阶段五：压缩与持久化桥梁 (Compaction & Builder)
将内存数据刷盘，以及后台的文件合并。

* **9. `src/db/builder.cc` & `builder.h`**
    * **目标：** MemTable 到 SSTable 的转换。
    * **核心逻辑：** `BuildTable` 函数。遍历传入的 MemTable Iterator，调用你现有的 `TableBuilder` 将数据写入 `.sst` 文件，完成后将新文件的元信息放入 `VersionEdit`。
* **10. `src/compaction/compaction.cc` & `compaction.h`**
    * **目标：** 封装一次 Compaction 任务的上下文。
    * **核心逻辑：** 记录输入的层级（Level）、输入文件列表（inputs_[2]）、以及在合并过程中用来判断是否可以提前 Drop 某个 Tombstone（删除标记）的逻辑。

### 阶段六：引擎顶层组装 (DB Engine)
最后，将所有零件组合在一起，处理并发、读写队列和对外 API。

* **11. `src/db/snapshot.h`**
    * **目标：** 管理读快照。
    * **核心逻辑：** 维护一个由 `SequenceNumber` 组成的双向链表。
* **12. `src/db/db_impl.cc` & `db_impl.h`**
    * **目标：** 实现 `include/db.h` 中的接口，协调全局。
    * **核心逻辑：**
        * **并发控制：** 维护一个全局的 `mutex_`。
        * **写入队列：** 实现 `Write` 方法，利用 `Condition Variable` 将多个线程的 `WriteBatch` 合并（Group Commit）写入 WAL 和 MemTable。
        * **后台线程：** 实现 `MaybeScheduleCompaction` 和 `BackgroundCompaction`，在后台执行 Minor Compaction (MemTable 刷盘) 和 Major Compaction (SSTable 合并)。
        * **读取逻辑：** 实现 `Get`，按顺序查找：MemTable -> Immutable MemTable -> Version (SSTables)。

---

### 步骤 1：补齐缺失的对外公共接口
在写引擎内部逻辑前，你的 `include/` 目录还缺几个关键的对外暴露头文件。这些头文件处于依赖链的最底层。
* **1. `include/cache.h`**: 定义 `Cache` 纯虚接口（`Insert`, `Lookup`, `Release` 等）。它只依赖标准库 `<stdint.h>` 和 `<string>`，以及你已有的 `slice.h`。
* **2. `include/table.h`**: 定义 `Table` 类的读取接口。它依赖你已有的 `include/iterator.h`。
* *(注：此时你可以顺手在已有的 `include/options.h` 里加上 `Cache* block_cache` 成员，为后续做准备。)*

### 步骤 2：Cache 模块闭环
Cache 几乎不依赖引擎内的其他复杂组件，最适合最先写完。
* **3. `src/util/cache.cc`**: 实现 `LRUCache` 和 `ShardedLRUCache`。只需 `#include "cache.h"` 和你已有的 `src/port/port.h`、`src/util/mutex.h`。

### 步骤 3：SSTable 读取链路 (自底向上)
这里要严格遵守先内部头文件，再写 `.cc` 实现。
* **4. `src/table/block.cc`**: 你已经有 `block.h` 了，直接写实现。它需要包含 `block.h`, `src/table/format.h`, `include/comparator.h`（这三个你都有了）。
* **5. `src/table/two_level_iterator.h` & `.cc`**: 依赖 `include/iterator.h` 和 `src/table/format.h`。
* **6. `src/table/merger.h` & `.cc`**: 多路归并迭代器。只依赖 `include/iterator.h`。
* **7. `src/table/table.cc`**: 核心组装。它 `#include` 你在前面准备好的 `include/table.h`, `src/table/block.h`, `src/table/filter_block.h`, `src/table/two_level_iterator.h`。到这里，Table 模块的编译链完美闭环。

### 步骤 4：元数据管理基础 (Version 控制前置)
* **8. `src/db/filename.h` & `.cc`**: 解析和生成文件名（如 `MANIFEST-0001`）。依赖 `<string>`, `<stdint.h>`, `include/status.h`。
* **9. `src/db/version_edit.h` & `.cc`**: 记录状态变更。依赖 `<set>`, `<vector>`, `include/slice.h`。

### 步骤 5：TableCache 封装 (关键依赖桥梁)
在写复杂的 `VersionSet` 之前，必须先有 `TableCache`，因为 `Version` 需要通过它来打开和读取 SSTable 文件。
* **10. `src/db/table_cache.h` & `.cc`**: 封装你在步骤 2 写的 `Cache`。依赖 `include/cache.h`, `include/table.h`, `src/db/filename.h`。

### 步骤 6：版本控制核心 (The Brain)
此时所有底层依赖齐备，可以安全实现状态机大脑。
* **11. `src/db/version_set.h` & `.cc`**: 依赖 `src/db/version_edit.h`, `src/db/table_cache.h`, `src/db/filename.h`, `src/table/merger.h`。没有任何头文件缺失的风险。

### 步骤 7：后台作业设施 (Compaction & Builder)
* **12. `src/db/builder.h` & `.cc`**: 负责把 Memtable 变成 SSTable。依赖 `src/db/filename.h`, `include/table_builder.h`, `src/db/version_edit.h`。
* **13. `src/compaction/compaction.h` & `.cc`**: 依赖 `src/db/version_set.h`。

### 步骤 8：顶层组装 (DB API)
* **14. `include/db.h`**: 对外的主接口（`Put`, `Get`, `Delete`）。依赖 `include/options.h`, `include/iterator.h`, `include/status.h`。
* **15. `src/db/snapshot.h`**: 内部快照的双向链表定义。
* **16. `src/db/db_impl.h` & `.cc`**: 终极组件。可以直接 `#include` 以上所有准备好的头文件。