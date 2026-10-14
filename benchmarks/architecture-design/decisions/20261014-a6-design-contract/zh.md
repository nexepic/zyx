# A6 资源所有权与单文件落盘执行设计合同

日期：2026-10-14。范围：S1.6 完整设计决策；S2/S5/O7 的运行时接入、查询 spill 与性能仍待实现。本合同不把 bulk scratch 或独立组件当作全数据库资源治理已经完成。

## 当前事实与接入位置

LogicalPlanReadRuntime::ExecutionSharedState 持有每次执行独立的 MaterializedByteBudget、cache/final-result reservation、PMR entity cache；默认 materialized 上限为 64 MiB。它不是线程安全的进程总预算。BudgetMemoryResource 计入分配请求，手工 reservation 仍有逻辑载荷/源编码估计；不能把其和当成 RSS 或完整 allocator 容量。Database 的现有 cacheSize 默认也是 64 MiB，但不是所有数据库实例的合计限额。FixedWidthColumn、selection、绑定表与 PrimitiveNumericColumns 已有局部准入；动态 PropertyValue、普通容器、完整 source decoder、固定父 owner 和维护任务尚未全部接入。

NativeBulkRunStore 已在同一数据库文件内分配/封存/退役 SGK_BULK_SCRATCH，location 使用 SegmentId/generation/transaction owner。流式 writer 限定 fragment/manifest 深度与扇出；分配和释放使用短 metadata publication，checkpoint floor 不能倒退。reclaimUnpublishedOnOpen 会扫描 sealed bulk scratch 并回收。它没有查询 active-owner 注册、查询行 codec、sort/hash spill 算子或独立查询 pin；不得把 query scratch 放进该清理类别。RelocatableMetadataStore::beginWrite 明确拒绝物理只读发布/回收。

## 预算层级、配置与计费单位

采用显式进程 resource owner → database domain → resource account → allocation leases。多个 Database 实例共享同一进程 owner；以独立默认 owner 绕过进程总额不允许。root/database/account 的准入和退款在一个 root mutex 中同时检查/发布，禁止先分配后补账、先退款再跨 owner 重申请。任务/算子共享 query account，不复制其完整限额；split/absorb 只转移同 account 的租约，不增大 owner 额度。

目标默认策略：进程可计费内存 512 MiB、每 database domain 256 MiB、每 query 延用请求上限（默认 64 MiB）并受父级约束，cache 延用 cacheSize 并受父级约束。query/cache/write/maintenance/code 五个类别的 cap 都是上限而非预分配：默认 query 192 MiB、cache 64 MiB、write 128 MiB、maintenance 32 MiB、code 0（物理代码缓存首版关闭）；类别 cap 总和可超过总额，任一申请仍须满足总额。进程各类默认 cap 为进程总额，code 为 0。配置必须在工作启动前验证，数值使用 checked bounds；可显式提高有限上限，不接受隐式无限额度。现有构造/ABI 尚未暴露这些选项，此默认为实施合同，不声称已经生效；兼容选项与 SDK 文档须在 S5.1 接入时同步。

保护进展：root 总额中的 16 MiB 设为 maintenance/abort/spill-control 的保护余量，普通 query/cache/write/code 的增长不能消耗尚未使用的保护部分。这是 total 内的余量，不是另外增加 16 MiB，也不重复计为 live allocations。至少一个受协调的维护任务可使用此余量；单任务 working set 必须有界。默认同时存活的 database budget domains 和每数据库 active queries 各限 64（关闭但结果仍持有的 domain 也计入）；超限明确失败。foundation 尚未实现保护 floor 或 admission slot 管理。

磁盘计费另设 uint64 counters：进程 query scratch 目标总上限 2 GiB、数据库 1 GiB、单 query 128 MiB，受文件总体/剩余空间限制，按实际物理分配 extent 向上对齐计入。当前 segment 为 128 KiB，不仅统计有效 run bytes。reserved/writing/sealed/retired-but-unreclaimed 均占额；只有安全释放给 allocator 后才退款。用户数据文件容量、retirement backlog、scratch bytes 单独报告，不把已分配用户数据混为内存配额。磁盘 policy/counters 尚未实现。

## 唯一计费归属与生命周期

| 资源 | 唯一全局归属 | 同时保留/转移规则 |
|---|---|---|
| query 编译/绑定、输入 source scratch、batch/selection/arena、operator/task、表达式临时值 | query account | 实际容量、扩容旧新重叠、嵌套对象、解码与转码缓冲计入；不同实际拷贝分别计费 |
| cache/page cache 的拥有缓冲 | cache account | 共享 borrower 不再次计载荷；query 只计其新增 handle/容器，pin 数/滞留页另限 |
| write private COW pages、overlay/staging/undo、约束和索引候选 | write account | 发布前候选仍计费；交给 cache 的同一个实际 buffer 原子转移归属，不 refund/readmit |
| 前台/后台回收、整理、恢复与完整性 walker 工作区 | maintenance account | 分配有界；保留前台与后台 task cap，保护 reserve 仅用于进展所需工作 |
| 可选物理程序代码缓存 | code account | 首版 cap=0，关闭；未来开启计代码、索引、eviction 元数据，不仅代码正文 |
| QueryResult/Driver ABI 的拥有输出 | originating query/result account | 在 query operator 结束后仍持有 charge；最后一个输出 owner 销毁后归还 |
| spill 内存/物理 extent | query 内存 account / 独立 scratch disk account | 读写双缓冲/merge cursor 是真实内存；磁盘 extent 不退款到内存预算 |

输出可以活过 Database::close；因此未来 runtime domain/输出 account 必须有 strong keepalive，不能保留已经析构的 Database 或 stack budget 指针。关闭 storage 不要求用户先销毁结果；仍存活的 domain/account 继续计费并限制再次打开实例。process control 本身只有一个有界 bootstrap owner；domain/query/control-block 元数据在创建前准入，并受上述 slot 限额限制，不允许不计费无限增加 budget wrappers。foundation 当前采用明确的 process > database > account > leases 生命周期引用；接入输出时必须提供 keepalive wrapper，尚未实现。

全局实际 allocator bytes 与现有 local logical/encoded estimates 分列：协调 allocator 向全局层只收一次实际请求；既有 MaterializedByteBudget 可以继续进行保守语义/源工作限制，不能再次作为全局物理分配收款。RSS、malloc 控制块、线程栈、mmap residency、外部运行库的不可控开销独立报告；不承诺 RSS 绝对硬限。所有新增内部可计费分配必须在请求前准入，尚未迁移的 owner 会阻止 S5.1 验收。

## 压力、取消和锁序

每次申请依次：检查 query cancellation/work limits → 原子尝试所有内存层级 → 在没有资源 mutex/metadata gate 时释放本 query 可选转换/缓存 → 有界驱逐本 database 未 pinned cache → 若算子/权限支持且已保留最小 IO/merge 工作区，开始 spill → 重试准入一次 → 明确 quota/admission 错误。不能吞 bad_alloc、corrupt source、schema/binding conflict 或用户求值错误。不得重放已消费 aggregate row 或公开回调。

首版不因资源等待阻塞：不持有 query pin/写门等待另一个 query 释放内存，也不建立依赖环。mutex 只保护计数；其中不执行分配、析构、回调、日志、eviction、IO、取消检查或 metadata publication。先释放 resource lock，再取得唯一 metadata write/publication gate；资源 counter 和 metadata gate 不嵌套。独立 allocation leases 可并发，单 lease/PMR resource 只归一个 task 或由该 owner 显式串行化；同一 ResourceAccount 的总量仍统一。现有非线程安全 local budget 不得直接进入 O7 多任务共享。

取消在每个 run/partition/merge IO 和有界 codec chunk 前后检查。先停止 producer/task，再销毁其拥有值和缓冲，最后退款和释放 pin；一个失败不能导致重复 refund。spill cleanup 是幂等、有界工作，已持有 cleanup 余量；IO 清理失败不能假装已释放 extent，durable orphan 保留并计 backlog，query 原始错误保留并附 cleanup 状态。进程崩溃不需要恢复查询输出，资源内存额度随进程消失；磁盘 orphan 必须经过恢复判定后回收。

## 查询 scratch 的权限、身份与发布协议

逻辑只读和物理只读分开。只有 file writable + query scratch policy enabled + backend 内部 QueryScratchPermit 才能写 scratch；permit 不给用户 mutation/graph root 修改权限。该能力只进入专用 scratch facade，不把一般 MetadataWriteGuard 暴露给只读 query。物理只读文件不分配、发布或回收 scratch；超内存时选择已支持的纯内存路径，否则明确失败，禁止外部临时文件绕过单文件约束。

采用独立 target namespace SGK_QUERY_SCRATCH（既有枚举前缀规则），不复用 SGK_BULK_SCRATCH 的 reopen cleanup。QueryScratchOwnerId 是 database domain 内持久分配、非复用 uint64 lease；其 high-water reservation 与 allocator 状态通过同一 metadata publication 原子维护。不能用地址、线程 ID 或重启后归零的进程 counter 作为 durable owner。登记包含 owner、状态、manifest 根/extent identity、字节额度；每页/manifest 校验 kind、owner、SegmentId/PageId、generation、长度/版本/checksum。具体 enum 数值与 query-row codec 版本必须在 S5.2 前经 A7 envelope/格式矩阵记录，当前不更改文件格式。

状态机固定为 registered → reserved/writing → sealed → retiring → reclaimed；失败可由前四态进入 retiring，只有 reclaimed 才归还 disk charge。先 durable reserve/register，再写/认证完整 run，最后发布 sealed manifest；发布失败保留 registered owner 可供幂等清理，不向消费者暴露半个 run。用户提交、scratch 分配/释放和维护通过同一个短 publication gate，在最新 metadata/allocator 状态上重新生成更新；不能以旧候选覆盖另一方 allocator 更新。spill 不增加用户图计数，不放宽已有写 fence；metadata 变动引起的保守写冲突由 A5 后续协议处理，不能在 A6 偷改。

活动 query owner/pin 注册与 durable ownership 分开：query payload 只在该 owner manifest 和显式 scratch read capability 下可达，不进入用户 graph roots。pin 以 owner+extent generation 为键；integrity/recovery/后台 worker 要读 payload 也必须取得同 domain pin。只有不再有授权 scratch pin 才能回收该 payload；不让无关 graph 长快照无谓保留 query payload。共享 catalog/allocator/metadata 页仍遵守原 MetadataSnapshotRegistry/Retirement，不能因 query payload 可释放而提前回收共用元数据。未经此特殊 query-kind walker/退役规则测试前，保持现有保守回收，不能把缺少 pin 当安全证明。

恢复在 exclusive writable startup gate、开放新查询前运行；多个共享句柄必须归同一个 backend owner/pin registry。不能在另一个活句柄有 active scratch owner 时执行 orphan 全扫。恢复先校验登记/manifest 和 allocator 一致性，只回收认证为本 database 的 query scratch 且本 startup 没有 active owner 的 extent；错误 owner/generation、指向用户数据、torn/损坏登记均报错或隔离，绝不猜测释放。read-only startup 只诊断，不 cleanup。用户页不能因 orphan 列表污染被释放。

## spill 算法、支持范围与错误合同

首先落地 sort run：按既有比较器预计算一次有序 key 与拥有的行，追加 uint64 checked input ordinal 保持 tie 稳定顺序；已 bounded buffer 排序后生成不可变 run。merge fan-in 由剩余工作内存、cursor/decoder 实际容量和 configured cap 决定，至少两个输入加一个输出才开始；不够则失败。多级 merge 有明确 run 数/深度/磁盘工作限制。持久行只写值/实体逻辑 ID/列布局身份及必要 ordinal，不写 C++ 指针/span。deferred EntityRef 重读仍使用活 query 原快照/pin；crash 后直接丢弃查询 scratch，不恢复旧 C++ query。

其次可分区 aggregate/hash join：用既有 canonical equality/hash 规则分区，相同 key 保证同 partition，载荷在重读时保持 NULL、类型、重复和 relevant order。partition 太大时最多 configured depth 再分区；skew/key 不可再分时明确失败，不能无限递归。仅允许能在同一语义下归并的 accumulator；COLLECT/精确 DISTINCT/顺序敏感或无法保持既有浮点累计顺序的函数在专用算法通过之前不 spill，明确 quota error。不得以改变 SUM 顺序或近似去重换取过测。

第一版排除 streaming public callback read-ahead、相关子查询/路径 owner、mutation planning、optional/grouped 因子化布局与没有合格 codec 的动态值类型。普通 scan/filter/project 可用有界 streaming；不需要 spill 就不主动写 scratch。blocking operator 只有输入/最终结果完整认证后输出，IO/disk-full/codec/cancel 失败不能宣称 complete input 或静默截断。LOAD CSV 外部输入不是允许 host temp spill 的理由。

## 维护、观测与验收门

记录 per account/class/database/process 的 cap、实际 admitted bytes、peak、denial cause；local logical estimates 分开。另记录 active query/domain slots、pinned cache、query scratch reserved/sealed/retiring bytes、run/merge/depth/IO bytes、cleanup failures、用户 retirement backlog 与 oldest snapshot。默认每 database 的用户 retirement backlog 上限 1 GiB；前台维护工作内存 cap 16 MiB、后台 8 MiB，每轮最多 64 个页级 IO/work 步骤，checkpoint/codec 仍逐 chunk 检查取消。前台与后台各最多一个有界任务，并由 root 保护余量决定可获准运行者。长快照阻止回收时新维护/写扩展在有限重试后明确压力错误，不能无限文件增长、绕过 snapshot 或永久忙循环。

S5.1 接入门：覆盖编译、执行、输入 decoder、可变长/嵌套值、固定父 owner、存活结果、cache、write private pages 与维护的 allocator/ownership 审计；多 query/多 database 单独低于局部限额却共同触发 process cap；取消/失败/退款/转移与输出活过 close；slot/floor 与 deadlock/pressure 回归。S5.2/3 spill 门：排序/partition 的完整值、重复、NULL、排序与 guard oracle，物理只读/逻辑只读权限，磁盘不足、每个 publication/IO 边界 fault、活句柄/长 graph snapshot/query scratch pin、orphan/损坏恢复、不误回收用户页、allocator 与用户提交交错。S5.4/5 和 O7/O8 的资源/性能门仍待后续验证，Kuzu 比较仅进入所有实现完成后的最终窗口。

本轮 private ResourceCoordinator.hpp 是三层计数/类别 cap、同 account 租约转移及 PMR 请求准入的可运行基础。7 项新测试证明单 account 多 lease 不扩大 quota、跨 DB 类别/总额、原子拒绝、释放/移动、upstream bad_alloc 回滚、旧新容量重叠、size_t 边界，以及两个真实线程同时请求只允许一方占满进程总额。它没有 singleton/shared-DB registry、runtime 接入、slot/floor、keepalive、disk coordinator 或 query spill，不应开生产总额治理 feature flag。与本轮 A1 模块合计 278 项独立定向测试通过；无全量测试或性能结论。

## 性能假设与反证

一个 root mutex 的准入成本为每请求固定数量的 counter 检查，与数据库数量无关，但每次分配锁仍可能在 O7 下形成竞争；PMR 实际请求计费不保证吞吐收益。未来若实测需批量额度租约，未使用额度必须继续占总限额，不能按“当前 live payload”再次借出。默认不先做 thread-local 额度复制。spill 降低内存峰值但增加 IO、codec 与 merge 成本，不能据此宣称超过 Kuzu；大量小查询、保留输出、cache 高命中、多个数据库/写维护并发、长 snapshot 和 skewed partition 都是保护场景。最终窗口需同时报告吞吐/延迟、admitted memory/RSS、disk IO/峰值 scratch、取消清理与所有正确性门；资源/语义退化或 mutex/复制费用抵消收益时保留已认证的 in-memory/有限失败路径并记录修订。
