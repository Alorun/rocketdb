# RocketDB Bug 修复记录

LevelDB → RocketDB 迁移过程中引入的缺陷汇总。绝大多数是**确定性的迁移笔误**(运算符优先级、参数写错、变长/定长编码混用等),而非并发数据竞争——并发只是把潜伏 bug 压满、使其必现。

排查手段:AddressSanitizer(内存错误) + 纯 C++ 隔离复现(校验读取正确性,不走 cgo、不碰上层代码)。

---

## 一、SST 读路径(数据丢失 / 崩溃,影响最严重)

### 1. block 块 restart 偏移算错 —— `src/table/block.cc:31`
```cpp
// 错:* sizeof 只作用在 NumRestarts() 上
restart_offset_ = size_ - (1 + NumRestarts() * sizeof(uint32_t));
// 对:(1 + N) 整体乘 4
restart_offset_ = size_ - (1 + NumRestarts()) * sizeof(uint32_t);
```
- **现象**:所有落到 SST 的数据读不出来,Get 报 `Corruption: bad entry in block` 或 NotFound。
- **根因**:block 尾部是 `N 个 restart(4B) + 1 个计数(4B)`。少算 3 字节使 data/restart 边界右移,`GetRestartPoint` 读到垃圾偏移 → 解码垃圾。
- **教训**:早期只检查"崩不崩溃"的复现,从不校验"读出来对不对",这个静默错读长期未被发现。

### 2. `iter_seek` 空指针解引用 —— `src/table/two_level_iterator.cc:105`
```cpp
// 错:对传入的 nullptr 参数做虚调用
if (data_iter_.iter() != nullptr) SaveError(data_iter->status());
// 对:读旧 wrapper 的状态(替换前保留已有错误)
if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
```
- **现象**:`SIGSEGV addr=0x0`(从地址 0 读 vtable),崩在 `rocketdb_iter_seek`。
- **根因**:`InitDataBlock()` 在 index 越界时合法调用 `SetDataIterator(nullptr)`,原代码对该 `nullptr` 解引用。
- **触发**:seek 到某 data block 末尾、需前进到下一个不存在的 block 时。

### 3. block 迭代器 restart 定位越界读 —— `src/table/block.cc:194`
```cpp
// 错:命中 Slice(const char*) 构造 → 对非 NUL 结尾的 block 数据做 strlen
value_ = Slice(data_ + offset);
// 对:长度为 0 的 slice 定位到 restart 偏移
value_ = Slice(data_ + offset, 0);
```
- **现象**:ASAN 撞红区 SEGV;release 下扫到随机 0 字节返回垃圾长度 → 静默迭代错误 / 偶发崩溃。
- **根因**:`Slice(const char*)`(slice.h)会 `strlen`,但 block 不是 C 字符串。`SeekToFirst/Seek` 均经此路径。

---

## 二、恢复路径(重启后丢写)

### 4. 恢复时 LastSequence 设过低 —— `src/db/db_impl.cc:432` (`RecoverLogFile`)
```cpp
// 错:比较的是 bool 参数 last_log,不是本条记录的序号
if (last_log > *max_sequence) { *max_sequence = last_seq; }
// 对:
if (last_seq > *max_sequence) { *max_sequence = last_seq; }
```
- **现象**:重启恢复后,已提交的写"回滚"到旧值(例:某 key 写过 19367,重启 Get 读回 5)。
- **根因**:`last_log` 是 bool,几乎永远只在第一条记录把 `max_sequence` 抬到 1,之后不再更新 → `LastSequence` 被设得过低 → WAL 回放进来的高序号新写在读取时被当作"未来数据"过滤,Get 落回旧值。

---

## 三、内存安全 / 资源泄漏(ASAN 发现)

### 5. TableBuilder 写尾部越界 —— TableBuilder(块尾压缩类型 + 校验码)
- **错**:用变长编码写压缩类型/CRC,超出预分配的 5 字节缓冲。**对**:改回定长编码。
- **现象**:访问越界内存。

### 6. Version 析构漏减引用 —— Version 析构函数
- 忘记对 `FileMetaData` 做 `ref--`,引用计数残留 → SST 元数据泄漏。

### 7. DB 基类缺虚析构 —— `DB` 基类
- 缺 `virtual ~DB()`,`delete DB*` 只调基类析构,`DBImpl` 资源未释放。

### 8. Logger 初始化配置判断错误 —— Logger 初始化
- 配置处理有误,条件恒为 false,`delete` 不执行 → 泄漏。

### 9. Compaction 析构漏减引用 —— `src/db/version_set.cc` (`~Compaction`)
```cpp
Compaction::~Compaction() {
    if (input_version_ != nullptr) input_version_->Unref();  // 原为空体,缺此行
}
```
- **现象**:trivial-move 路径每次泄漏一个 `Version`(内存 + SST 文件)。已补回。

---

## 验证

- **SST 正确性**(进程内 flush→读):小值 / 1KB 多字节 varint 值均 `PASS`(修复前全部 `bad entry in block`)。
- **恢复正确性**(写→重启→读):返回最新值,不再回滚(修复前 NotFound / 旧值)。
- **并发无内存安全回归**(`zz_repro.cc`,12 线程共享 DB + 快照/迭代器,ASAN):`asan_errors=0`,跑完 exit=0。

产物:`build-release/librocketdb.a`(cgo 链接对象,干净 `-O2`)。上层重新编译/重链 cgo binding 后生效。

> 隔离原则:以上全部仅基于引擎自身代码定位,未查看上层数据库实现。
