### 补充

table_build format

---

### 第一阶段：搭建骨架与纯内存引擎 (In-Memory Engine)
*目标：让引擎能跑起来，支持在内存中写入和读取，不考虑持久化和磁盘。*

1.  **定义基础成员与构造/析构**
    * `DBImpl(const Options& options, const std::string& dbname)`: 初始化各种基础组件。
    * `~DBImpl()`: 确保能干净地释放资源。
    * **核心变量**：先引入 `MemTable* mem_` 和 `port::Mutex mutex_`。暂时忽略 `imm_` (Immutable MemTable) 和 `VersionSet`。

2.  **实现最基础的写入 (Write Path)**
    * `Write(const WriteOptions& options, WriteBatch* updates)`: 这是所有写入（Put/Delete）的最终入口。先写一个极简版：遍历 `WriteBatch`，直接把数据插入到 `mem_` 中。
    * `Put()` 和 `Delete()`: 它们只是 `Write()` 的便捷包装器，构造一个单条记录的 `WriteBatch` 传给 `Write` 即可。

3.  **实现最基础的读取 (Read Path)**
    * `Get(const ReadOptions& options, const Slice& key, std::string* value)`: 在这个阶段，`Get` 只需要去 `mem_` 里查数据。查到了返回 OK，查不到返回 NotFound。

*💡 测试点：写一个简单的单测，执行 Put, Delete, Get，确保内存态的逻辑完全正确。*

---

### 第二阶段：引入预写日志 (WAL) 与崩溃恢复基础
*目标：哪怕程序突然崩溃，内存里的数据也不会丢。*

1.  **接入 Log Writer**
    * 修改 `Write()` 函数。在将数据写入 `mem_` **之前**，先将其序列化并追加到 `log::Writer` 中，并根据配置决定是否执行 `Sync()`（刷盘）。

2.  **实现极简版恢复引擎**
    * `Recover()`: 在 `DBImpl` 构造时调用。目前只需要让它去读取刚才写的 `.log` 文件，把里面的数据重放（Replay）回 `mem_` 中。

---

### 第三阶段：打通第一层磁盘落盘 (Minor Compaction)
*目标：MemTable 不能无限增长，满了必须写到磁盘变成 SSTable。*

1.  **空间管理调度器**
    * `MakeRoomForWrite(bool force)`: 这是写入路径上的“红绿灯”。在这里实现逻辑：如果 `mem_` 满了，把它变成 `imm_`，然后创建一个新的 `mem_` 和新的 `.log` 文件。

2.  **MemTable 变身 SSTable**
    * `CompactMemTable()`: 读取 `imm_` 中的数据。
    * `WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base)`: 将 `imm_` 里的数据真正写成一个 Level 0 的 SSTable 文件，并把文件元信息记录到 `VersionEdit` 中。

3.  **升级读取路径**
    * 修改 `Get()`: 现在的查找顺序变成了：先查 `mem_` -> 再查 `imm_` -> **（暂不查 SSTable）**。

---

### 第四阶段：完整的版本控制与读取路径 (VersionSet & Iterators)
*目标：管理多个 SSTable，并支持全量数据的读取与遍历。*

1.  **接入 VersionSet**
    * 在构造函数和 `Recover()` 中初始化 `VersionSet`。
    * 在 `WriteLevel0Table` 成功后，调用 `versions_->LogAndApply(edit, &mutex_)` 将新生成的 SSTable 正式挂载到版本树上。

2.  **补全磁盘读取**
    * 彻底完善 `Get()`: 现在的顺序是 `mem_` -> `imm_` -> `current_version->Get()`（去磁盘上的 SSTable 里找）。

3.  **实现迭代器核心**
    * `NewIterator(const ReadOptions& options)`: 将 `MemTableIterator`、`SSTableIterator` 等通过 `MergingIterator` 组装起来，最后套上你之前了解过的 `DBIter` 返回给用户。

---

### 第五阶段：后台大合并 (Major Compaction)
*目标：解决 L0 文件过多导致的读放大问题，实现层级合并。*

1.  **触发与调度**
    * `MaybeScheduleCompaction()`: 检查当前状态，如果满足合并条件，向后台线程池丢一个任务。
    * `BackgroundCall()` -> `BackgroundCompaction()`: 后台线程执行的入口。

2.  **执行合并**
    * `DoCompactionWork(Compaction* c)`: 这是 `DBImpl` 中最难、最复杂的函数之一。你需要打开多个参与合并的 SSTable，使用 `MergingIterator` 遍历它们，丢弃过期和被删除的数据，然后写出新的 Level N+1 的 SSTable。
    * 完成合并后，再次调用 `versions_->LogAndApply()` 更新版本。

---

### 第六阶段：高级特性与边角料处理 (Advanced Features)
*目标：完善工业级功能。*

1.  **快照机制 (Snapshots)**
    * `GetSnapshot()` / `ReleaseSnapshot()`: 维护一个存活序列号的链表（SnapshotList）。修改 `Get` 和 `NewIterator` 使得它们能够读取历史视图。
2.  **并发与锁优化**
    * 优化 `Write()` 中的并发写入：实现 `BuildBatchGroup()`，将多个并发线程的 WriteBatch 合并成一个，由一个 Leader 线程统一写入 WAL，极大地提升吞吐量。
3.  **统计与监控**
    * 完善内部的 `CompactionStats` 和读采样逻辑（就是你之前看到的 `Seek Compaction` 相关的计数器）。

---

### 给你的建议

面对这么多模块，**千万不要在没有跑通前置步骤时，去写后面的代码**。例如，如果你连单 MemTable 的 `Put` 和 `Get` 都没测通，千万不要去写 `DoCompactionWork`，那会陷入无尽的 Debug 地狱。

你目前是打算从头手写一个简化版的 LevelDB/RocksDB 作为学习项目，还是在某个现有的代码库上做魔改或者功能追加呢？
