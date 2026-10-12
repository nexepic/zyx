# A3 紧凑字段枚举与稀疏列反证

日期：2026-10-12。

旧候选固定持有 64 个 route optional/字段 vector，即使仅有两个实体、三个字段，对象加容器容量也为 4140 字节。现改为只持有实际成员 descriptor vector 与一份连续 token vector。descriptor 记录完整 route、begin/count，按 slot 排序二分定位；token 数组按实体实际字段构造，保留 snapshot/slot 身份和完整 presence 关系校验。没有复制属性值。

同一稀疏用例新对象与容器容量为 188 字节。该数字按 sizeof 和实际 vector capacity 计算，排除 allocator 开销、缓存、真实页、持久 codec 和 RSS；不是数据库空间或性能收益。

新增满槽位、逆序插入、每实体单个不同 token 用例，核验实际枚举及列 presence。64 个实际字段时：目录对象/容量 3408 字节，旧 TypedPropertyBlock 载荷 32768 字节，列元数据 3104 字节，owner slots 编码 1056 字节。内存目录与 wire 数字不应混成精确持久总空间。旧列载荷仍为各字段按全部 64 行分配定长数组，实际仅 64 个整数却形成 64×64 容量。这是稀疏/一次性字段的具体反证，不能把旧 typed 原型直接准入生产。

下一步须设计稀疏 presence/rank 或有序 slot/value 编码，并共同核验字段枚举元数据、块寻址、小更新与页 COW。目录连续化仍不解决全目录重编码/持久回收，不能宣称已满足 A3 局部更新合同。

严格模块构建及 10 项字段目录、真实快照、旧编码测试通过。证据位于 results/20261012-a3-compact-membership，含日志/JSON、RecordProperty 与源码 hash。没有性能计时实验；未运行 benchmark、全量测试，未提交/推送，未修改正式文档或英文记录。

进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
