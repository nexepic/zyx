# R4/R6 有界 COW 与查询观测验收

起点 89f1254928947c8b6fd32cbd8fc2917d2b7f55e9。测量前登记五项：R4.1、R4.2、R6.1、R6.2、R6.3；性质为实现/正确性/资源/观测基础交付，不是延迟准入。现有 COW 与 R1/R2 接口已就绪，主文档第 5 节要求公共正确性和资源合同随阶段落实。没有以这些工作替代 R3 格式原型或 R5 邻接候选，也没有新增格式/backend/cache/spill 算法、改变耐久性或资源额度。

## R4.1 实际变化与修改集合

复用已有按 owner 暂存和同源页 COW 合并：

- 旧批规划在任何 root 处理前都会枚举属性页池，已有 owner 的 SET/no-op/REMOVE 也付出全目录工作。现改为按需枚举；已有 owner 只认证所需源页并提前返回。新增 owner 确需放置时，PropertyCollectionRootStore 的既有批局部池列表至多初始化一次；其他分配/规划回退的枚举仍独立计数。没有新增长期缓存。
- 拥有数据的 root plan 移入暂存数组，未合并/溢出/搬迁 plan 移入最终 owner；成功合并成员时移动大值 mutation 缓冲。保留首成员合并所需所有权和全部认证源检查，不修改已发布页，不提前移动需要失败重试的事务状态。
- 计数 owner、累计临时 owning payload 字节、合并成员、最终 replacement request/payload 字节和实际属性池 catalog 枚举。payload 不含页 owner header；空源页退休 request 不一定写数据镜像。这些不是设备写入量或变化字段字节。

| 对象 | 已有 owner 字段更新的实际 COW 行为 |
|---|---|
| Entity record/map | 属性变化不重写无变化的固定实体记录 |
| Property | 沿用当前 owner canonical 行/引用编码，同页变化合成一份 replacement；大集合仍可能整体编码，这是 R3 未完成问题 |
| owner/Page 路由 | 更新实际 slot/generation 与稳定 PageId 的物理映射，保留其他 owner |
| 派生索引 | 保留现有按值变化识别路径；no-op、唯一性、类型、REMOVE 等测试保护 |
| catalog/allocator/control | 变化分配/页池状态及必要共享路径；新增/增长页池仍可能触发配置和放置工作 |
| 元数据路由/fence | 保留不可变分支复用、图语义 fence 和保守冲突校验 |
| 退休 | 通过既有 pin 感知协议退休精确旧页，不以文件长度推导可复用空间 |

剩余全局工作明确登记：新增 owner 的 best-fit 仍可枚举当前属性池和候选页，保留容量受既有 WRITE 账户及有限存储/ID 配置约束，但不声称工作量只随新字段增长。元数据可达性遍历在分配/空闲阈值或显式恢复/清理时仍会发生，必须认证当前与 pinned roots。PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES=32×31 帧，约 4 MiB；另有少于八池的延迟条件及空闲帧底线，不能混为一个阈值。大值 owner/manifest 扫描、整理/搬迁沿用已有最大页/位置/退休/周期预算及 continuation，不新增无界延迟链。成熟库延迟、持续写、完整维护积压/最老 pin/空闲/尾裁剪观测及空间准入仍属 R4.3/R7。

精确 fixture：8 owner 在同一属性页更新，8 合并成员、1 份 4032 字节 replacement payload、0 次属性池 catalog 枚举；旧 pin 仍读 rank=0，重开后 rank=1 且 body 不变。no-op 不产生属性 replacement，也不枚举属性池。这是机制/正确性证据，不是提速倍数。

## R4.2 发布与维护保护

发布次序不变：候选数据/元数据 → DATA_FENCE → 非当前 A/B → SUPERBLOCK_FENCE → 确认。候选/control 短写或失败、sync 失败、不确定的 root 后确认继续按既有重开/恢复语义处理。ID 发布后的诊断增量不会抛错把已耐久确认变成失败。选有效根不替代完整 integrity。

必要模块通过：原生耐久性、共享页池替换/搬迁故障、row-chunk 故障矩阵、有界元数据回收、大值可达性/journal、长 pin 文件压力/复用、尾部裁剪。尾部测试区分逻辑释放、物理搬迁和缩文件，包括裁剪前/后 sync 失败。pin 仍保留旧代可达性；配额/文件压力拒绝保留旧 root，释放后可继续复用。未放宽额度或 fence。maintenanceMetrics、metadata-pool stats、reclaim/truncate 结果仍由现有组件持有；查询 PROFILE 不是完整数据库维护状态 API。

## R6.1 所有权和资源

DirectPropertyColumns/BatchEnvelope 保持查询拥有且先准入；借用解码页保留分配 owner，lookup 失效不释放存活视图。既有测试保护陈旧身份、重复 ordinal、容量压力、取消、pin、结果逃逸、计费/退款。

新增观测使用固定指标表/样本数组；执行前通过 MaterializedByteBudget 预约对象及保守的 owning report/map/string 暂存。动态列名视图也从同预算分配。QueryResult 在观测暂存析构前准入自己的持久行/列/map，不提前退款再申请。部分暂存预约是保守上界，不提高 cap。原生父 QUERY 账户同时计费执行/观测/输出；无父账户的 standalone backend 仍显式限制本地观测元数据。

原生测试证明结果及指标 map 可活过 backend.close，结果释放后同一调用方账户回到 0。执行前观测元数据拒绝不得发布 CREATE。观测已激活后的注入式内存 guard 中止及语义异常均恢复线程状态、退款并允许重试。耐久修改之后的输出准入错误仍按既有“提交已发生/结果不确定”处理，不自动重放副作用。

sort、DISTINCT/UNION 延用文件内 spill；group-by/hash join 超额清楚失败，不宣称通用外存执行。逻辑只读 scratch 仍需内部权限和可写文件；物理只读 PROFILE 零写入、文件/fence 不变。现有权限、cleanup 和原生 spill 检查通过。

## R6.2 公开观测合同

PerfTrace::ScopedObserver 为同步、线程局部、栈作用域，不切换/清空全局采集。嵌套 inclusive，排除其他线程。receiver noexcept 且不分配。全局 page-pin 集合归属不变，PROFILE 不启用该全局唯一镜像集合。诊断不替代权威 I/O 或恢复检查。

PROFILE 内部语句只执行一次，保留原数据值与已有末尾 summary 行；原列顺序保留，补齐尚未出现的 summary 列，因此现有 Driver ABI map getter 可发现 physical_work。普通数据行按列访问新增字段为 NULL。没有 ABI 函数/值类型变化；新增 ABI 测试验证标量数据、末尾 map/行数、游标结束和拥有的值。

summary 字段：phase、total_time_ms、calls、actual_output_rows、access_path、physical_work、physical_work_scope、estimates_available、estimated_root_rows、direct_selected_fraction。末行是元数据，actual_output_rows 不含末行。用户返回同名 alias 的原数据行仍保留原值，summary 行对应字段描述元数据。

| 指标 | 单位、累计/峰值及范围 |
|---|---|
| direct_property rows/batches/rejected | 源批行数/批数及源选择阶段拒绝的累计量 |
| aggregate.direct_filter_rejected/direct_input_rows | 聚合附带字面量过滤拒绝/真实聚合输入，和源选择不同层 |
| direct_selected_fraction | (直接源行−源拒绝−附带聚合过滤拒绝)/直接源行，无直接输入为 NULL；不是任意整查询选择率 |
| consumer_adapter.*.calls | 实际有界 dispatch 次数，不是行数或去重节点数，相关/嵌套可重复 |
| batch.materialized_bytes | 累计物化量，不是 live peak |
| materialized_budget_peak_bytes | 所观测各本地预算峰值的最大值，含观测元数据；不是全部 QUERY 账户之和、CACHE 或 RSS |
| 元数据/缓存/解码/复制 | 既有累计访问、命中、owner 解码、固定结构字节；访问次数不是唯一页 |
| checksum.bytes | 累计校验输入字节 |
| buffer_pool.physical_read_bytes | buffer pool positioned-read 字节，不是设备流量 |
| file_size_calls、completed_write_bytes/calls | 既有实际 I/O 范围，和逻辑工作分列 |
| property_plan_*、pool_catalog_scans | 临时 payload、合并和 replacement request 工作，见 R4.1 |
| id_reservation_calls/id_lease_cache_hits | 预留调用尝试/既有租约命中，不是耐久提交 |
| id_reservation_publications | 成功确认的耐久 ID 租约发布，含直接接口和 acquisition miss |
| graph_commit_calls/maintenance_reclaim_calls | 普通图 commit 尝试/显式维护回收调用，不含其他后台线程 |
| sync.data_fence/superblock_fence | 本查询线程全部内部发布的已完成 fence 次数 |
| *.ns | 部分 fence/发布 inclusive 累计 ns；不能把嵌套 timer 相加冒充 wall time |

缺项可能是没有事件或路径不支持，不能普遍按 0 推导。access_path 按实际观测给直接单元/索引候选/属性批转行/其他，细计数显示混合与嵌套。PROFILE 对支持的 root 复用已有有界规划统计，不支持的聚合 root、overlay 等以 NULL 明确未知。估计可早于执行快照，不是并发结果的正确性保证。EXPLAIN 仍不打开快照，保留 operator/details 两列，在 details 附上真实的候选/适配器条件；不冒充已知实际访问，也不全图扫描求估计。

fixture：64 直接源行、源拒绝 0、附带过滤拒绝 32、聚合输入 32，直接选择率 0.5。CREATE 观察到一次 graph commit 尝试、一次 ID 发布、两次 data 和两次 superblock fence，不把内部发布隐去。原始 timer 是正确性诊断，不是性能准入；R7 正式计时使用普通查询并关闭 instrumentation。

## R6.3 责任、验证和限制

固定指标/报告/解释由 src/query/api/LogicalProfileObservation.hpp 持有；PerfTrace 只负责作用域递送和已有全局汇总；MaterializedByteBudget 持有本地峰值；PropertyCollectionRootStore 持有批放置/合并。不并行复制执行器、存储或资源协调实现。

首轮 166 项，163 通过、3 失败。源选择断言混淆了下游过滤，新增真实聚合过滤拒绝计数并分别验证；ID 计数最初漏 acquisition，在实际路径补齐；一个 fixture 使用只读 QueryEngine 构造器，改为显式提供读写接口，保留原构造器合同。没有删断言掩盖生产故障，原始失败与中间复验保留。最终必要模块 245/245；最终 PROFILE 列/报告/ABI 改动后 99/99 受影响检查通过。最新状态合计 263 个不同用例通过，不是一次 263 项运行。

实际执行 Linux x86_64/Clang 19。C++20 可移植，无新 OS 持久化原语；macOS/Windows/WASM 执行和完整语言绑定矩阵未做，不声称通过。只跑必要模块，没有全量单测或新增正式评测。未覆盖用户 DB 或冻结 before binary；本轮私有构建/日志留在 /workspace/zyx-r46-five-20261019，测试使用各自临时文件。未来 R3/R5 接入仍必须落实同样的所有权、格式、故障和资源合同并补相应模块验证；本次不是未实现路径的证据。

五项已登记基础交付全部验收，当前总计 13/24（54.2%）。R1.3/R2.3、R4.3/最终性能门槛仍待准入；R3.2/R3.3/R5 未被替代或提前计数。

total_time_ms 计内部执行，不含新增的有界统计读取和 summary 构造；非 PROFILE 查询的列不改变。

PROFILE 只启用局部数值观测，不启用 duration timer 构造，避免额外启用未纳入本次预约的 timer 标签字符串分配。已有全局计时开启或显式递送 duration 时才出现所选 *.ns，否则省略；内部 total_time_ms 和完成 fence 次数仍可用。全局诊断保持原有归属策略。
