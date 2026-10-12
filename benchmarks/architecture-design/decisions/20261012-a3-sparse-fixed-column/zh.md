# A3 稀疏定长列实验合同

日期：2026-10-12。实验编码，未生产准入，不变更数据库格式。

候选支持至多 64 行、同质 int64/double/bool 及任意缺失/NULL；混合非 NULL 类型与其他变长类型明确拒绝，不能隐式转换。presence 表示字段存在，nulls 是 presence 子集；有效行集合为 presence & ~nulls。按有效行顺序只编码有效值，以 popcount 有效位的前缀定位，不另存每行 offset。全无有效值使用 NULL_ONLY kind，无载荷。

实验 header 为 48 字节，little endian：0 magic u32；4 version u16；6 kind u16；8 rows u32；12 payload_bytes u32；16 presence u64；24 nulls u64；32 reserved u64；40 CRC u32；44 reserved u32。CRC 覆盖完整 header/payload，计算时 CRC 字段置零。kind 使用带前缀的整数枚举，int64/double 按 8 字节位模式，bool 为规范 0/1 单字节。长度精确等于 48+popcount(valid)*stride；NULL_ONLY 长度为 48。

读取须校验版本、kind、行数、reserved、有效位范围、NULL 子集、完整长度、CRC、bool 规范值及 NULL_ONLY 无有效位；再返回 missing/null/value 三态。container 的 token/owner/group/snapshot 身份仍由外层目录绑定，单列 codec 不提供这种身份认证。旧 TypedPropertyBlock 不自动读取此格式，也不宣布其生命周期已完成。

编码单位最多 48+64×8 字节；组大小与页 COW 单位尚未定案，不能将列编码长度当成真实页面写入量。下一步需接入候选列目录与更新/删除/回收协议，并与密集编码按实际字节成本选择。未运行 benchmark，不预设性能收益。

## 本轮实现与验证

稀疏定长列 encode/read/validate 已实现，与上述实验合同一致。64 行仅两个整数有效、另有一个存在 NULL 的用例编码 64 字节；读取全部行核验 missing/null/value 及 row 63 的 rank。bool/double 位模式、负零、全 NULL/全缺失和空列覆盖；混合类型、字符串、超行数拒绝。校验和重算后仍错误的版本/kind/rows/presence/nulls/reserved/长度/bool 值均拒绝，另有真实 checksum 损坏及截断测试。

严格构建与 3 项新稀疏列、7 项字段目录、1 项真实快照及 2 项旧编码测试共 13 项通过。证据位于 results/20261012-a3-sparse-fixed-column，含日志/JSON 与源码 hash。64 字节是此独立列候选的编码大小，不能直接与旧原型不含 header 的列载荷比较，也不包含外层 descriptor、owner slots、字段目录、真实页与 allocator。

生产接入、密集/稀疏成本选择、变长/混合回退、页 COW、持久身份及完整生命周期尚未完成。未运行 benchmark、全量测试，未提交或推送，未更新正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
