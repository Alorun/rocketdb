histogram -> bloom -> snapshot (扫清外围障碍，全是简单的独立数据结构)

builder (打通内存到磁盘的最后一步)

 db_iter (搞定多版本和删除标记的清洗逻辑)

db_impl (核心调度，先写骨架，先跑通最简单的 Put/Get，再加入后台 Compaction)

 dumpfile & leveldbutil (写 db_impl 抓狂时，写这个来帮自己 debug)

repair (等一切都稳定运行后，最后挑战这个异常恢复逻辑)