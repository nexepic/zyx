# A1 动态值 PMR arena 基础

日期：2026-10-14。状态：任务局部动态值 owner 的独立组件已通过验证，生产 source/consumer、拥有输出 adapter 与完整批次 envelope 尚未接入。总验收保持 6/34（17.6%）；本轮没有新增完整验收项，不自动提交推送。

DynamicValueArena 用共享 local MaterializedByteBudget 的 BudgetMemoryResource 持有六类 PMR 缓冲：value descriptor、连续 byte、list child 索引、map entry（key byte range/value index）、TemporalDuration 三分量数组及 forest root 索引。固定 bool/int64/double/date/datetime 用 uint64 bits 与 C++20 bit_cast，duration 保留三个 int64；string 与 bytes 的 type tag 分开，missing/NULL/value 是独立 cell state。子范围/位置均为 uint64 描述，实际容器索引受当前平台 size_t 和 allocator 容量限制；不借用输入 PropertyValue 的容器或字符串。

DynamicValueId 携带 uint64 index/owner/incarnation。每次取得 type/state/scalar/text/binary/child/map-entry 都先检查 owner/incarnation 和 descriptor 界限，外来、同地址重建、复用旧版本及越界 descriptor 失败。append 尝试（包括 preflight/分配/guard 失败）、appendMissing、clear/release 终止旧 handle 与 raw view 借用；发布值的 index 不变，可显式取得新 incarnation 的 handle。返回的 string_view/span 仍是有生命周期的 C++ 借用，不能跨 mutation 保存使用，不能把旧裸 pointer 自动认证。

append 的输入 adapter 做有 guard 的预扫描，按整个 arena 的 value/byte/list-child/map-entry 限额和最大深度（1–256）检查，再分别 reserve 实际 PMR 缓冲，计入旧新重叠；随后以有界 4096-byte chunk 拷贝字符串/二进制/键，并复制嵌套描述。根只在完整复制及最后 guard 成功后返回。任何复制/分配/取消失败恢复六种缓冲的已发布长度；已成功 reserve 的容量仍活着并计费，不假装退款。clear 重用容量，release/析构退还。树遍历与值解释不是新 evaluator，不执行比较/函数，也不改变已有 arithmetic/NULL/顺序合同。

list 顺序和重复位置保留；map entry 顺序按输入 map 的当前迭代顺序存储，不声称已验证 keys()/map 输出等所有公开函数；重新构造拥有 PropertyValue/QueryResult 的输出接口仍待实施。空 string/bytes/list/map 合法，byte/child/map-entry quota 可以为零；NULL 与 missing 可单独存在。UNKNOWN/COMPOSITE 不是当前 PropertyValue 普通 query variant，未强行支持。源 input 的 heap 仍由其调用者拥有/计费，本 adapter 的预扫描+复制是明确迁移成本，不宣称原生 decoder 已零拷贝或全 source 准入完成。

验证新增 14 项组件测试：所有固定/时间类型、int64 两端、负零/NaN payload/infinity，含 NUL 字符串和 binary、输入销毁后的拥有性、嵌套 map/list/NULL/duration、missing/NULL、clear/release、同地址重建/外来/越界、五类 work limit 的分配前拒绝、每个观察到的 upstream 分配失败、每个 preflight/copy/byte-chunk/final guard 失败的已发布前缀回滚与容量计费。还把独立 local budgets 的两个 arena 上游接到已提交的 CoordinatedMemoryResource：共同 process cap 拒绝第二个 arena，原始 ResourceAdmissionDenied 传播，释放第一个后重试成功，所有层最终退款为零。这只证明组件叠加，不代表 Database 已全局接入。

动态 arena + FixedWidthBatch/Column 必要模块 **40 项/3 suites 全部通过**（14 新项、26 已有项），新增编译无警告。Linux/C++20；未运行全量、性能基准或 Kuzu 对比。当前生产行为未变，Native source 列仍是既有拥有 PropertyValue；此组件不计作 S2.1–S2.5 完成。基线为已推送 47f886ad。下一步需要 query/program/layout/snapshot envelope 与矩形多列批次，以及有分配前准入的 native producer/动态消费者和最后拥有化输出，不能为了“接入”无条件多拷贝一份现有列。

证据：同名 results 目录保存构建/链接命令/原始测试日志、source SHA-256 和 verification.json。

后续补强：appendMany 以一个事务发布多个 root，DynamicForestId 记录 uint64 first/count/owner/incarnation，root 索引独立于嵌套 descriptor 数量；重复值和 root 次序保持。预扫描与每个 reserve 前有 guard，所有值复制及最后 guard 成功后才一起更新 published value/root 计数。guard 中只能看到原有完整前缀，新 root 与 descriptor 在发布前无法取得 handle；失败跨多个 root 回滚。新增三项测试覆盖 nested root 顺序/空 forest/旧 forest 失效、每个 forest guard 点的无半行观察和重试、appendMissing 在 root-index 分配失败时不发布孤立值。guard 不得修改 owner 或输入；owner 是 task-confined。原始日志与 source hash 已按最后一次构建刷新。
