## 23. 框架级读取路径优化与实现差距

本节记录基准对比后发现的框架级问题和后续实现方案。它们不是针对某一个 benchmark 的特例，也不是重新引入
Segment Chain、每节点 adjacency root、Edge 双向链表、第二数据库文件或匹配具体 query text/workload 的 fast path。基于 logical operator
shape、可供所有等价查询复用的 storage-neutral fast path 属于执行框架，不在禁止范围内。所有优化必须继续服从本方案的
单文件、A/B Superblock、generation-aware Catalog/Page Directory、共享 Buffer Pool、全局 key-only adjacency Packed B+Tree 和无迁移约束。
当前 write path 已改善并不表示 read path 已完成；此前 Stage 2、4、5、6、8 中标记为 native core 的内容，应理解为 correctness core，
其生产级读取门禁仍需按本节重新验收。

性能差距的最新判断、执行优先级和禁止事项维护在[优化策略](optimization#1-当前判断与证据)中，具体实施顺序见[分阶段执行计划](optimization#4-分阶段执行计划)；本节继续作为单文件物理格式与存储不变式的权威来源。

### 23.1 基准证据和测量口径

在同一台 macOS arm64 机器、AppleClang 17、Conan 工具链、Release、相同 smoke 数据集和 seed 下，对当前架构和独立重建的旧架构进行
对照。旧架构来自历史 worktree，commit 与当前分支不是线性祖先关系，因此结果是参考基线而非严格 before/after 证明。`smoke` 数据集
包含 250 个 Node 和 660 个 Edge；无额外属性索引的加载从 85.244 ms 降至 60.228 ms，约快 29%，而包含属性索引构建的加载为
117.047 ms 对 116.409 ms，基本持平。读取路径明显退化：`label_scan_filter`、`all_nodes_property_filter`、`label_multi_property_filter`、
`relationship_type_scan`、`relationship_property_filter`、`one_hop_expand`、`two_hop_expand`、`shortest_path_chain`、`topk_property_sort`、
`point_lookup_indexed`、`property_equality_indexed` 和 `property_range_indexed` 分别约为旧架构的数十倍至数千倍耗时。精确数字、p50/p95/p99、
RSS、数据库大小和读写字节必须进入可复现 benchmark artifact，不能写成架构常量。

画像显示，节点属性扫描的主要成本集中在反复的 metadata directory lookup、page route lookup、Catalog/segment 校验、page read、
payload copy、slotted-page decode 和完整 property-root decode；标签过滤、全 Node 属性过滤、聚合和 Top-K 的 Property optional lookup
约占各自执行时间的 88% 至 98%。关系属性过滤还会从 Node scan 发起多次 adjacency range，再逐条 materialize 完整 Edge；最短路径会为
只需要拓扑的访问读取完整 Edge 和 Node。`small` 及更大规模尚无完整当前样本，不能以 smoke 的热缓存倍率替代规模结论。后续每个阶段
都必须同时记录正确性和这些结构性计数，避免只看 wall-clock。

### 23.2 分阶段实现

#### 阶段 0：重新冻结可观察契约

- 固定 scan、indexed、multihop、write 四类 workload，并覆盖 smoke、small、百万级、千万级规模，以及 opened/warm/cold-ish 模式。
- 记录 p50/p95/p99、RSS、文件大小、逻辑/物理读写字节；输出 deterministic structural counters。
- 增加 Page Directory lookup、Catalog lookup、envelope/control validation、typed payload-copy bytes、B+Tree root descent、完整实体
  materialization、decoded property count 和动态分配次数计数器。
- 先建立当前实现的 baseline，再逐阶段比较；任何优化必须在多类 workload 上有效且不改变公开结果。

#### 阶段 1：引入 `NativeReadSession`

创建由单个 query 拥有、绑定到单一 metadata snapshot、默认单线程无锁的只读 session，持有 `MetadataSnapshotPin`、有界的 PageId 到 PageLocation 缓存、有界的 SegmentId
到 CatalogEntry 缓存、已经验证的 envelope/control view、snapshot-local token dictionary 和 Directory/B+Tree cursor 状态。缓存按 generation
隔离，并按 entry 或 byte 数量有界；物理 offset 只能在 storage 层内部流动，不能泄露到 query、binding 或 public ABI。固定 256 槽、
`id % slots` 的 direct-mapped Entity cache 加共享 mutex 不是本阶段的完成形态：它在顺序扫描中冲突严重，并缓存了错误的粒度。跨 query/
thread 共享继续由 generation-aware Buffer Pool 负责。

#### 阶段 2：增加零拷贝 typed page view

区分 `PinnedTypedPageView`、`DecodedRecordView` 和 `MutableOwnedPage`。读路径保留 Buffer Pool pin，typed view 只引用已经完成 checksum、
owner、generation、slot-generation 校验的 bytes；写路径继续使用拥有所有权的 COW 表示。禁止 cache hit 再复制完整 payload，禁止绕过现有
完整性校验。

#### 阶段 3：统一 Directory/B+Tree 流式 API

为 Entity Map、Property Directory、Index 和 adjacency 提供统一的 `pointLookup(key)`、`multiLookup(sorted keys)`、
`openRangeCursor(lower, upper)`。range cursor 只下降一次 root，沿 leaf/sibling 流式读取，使用 callback 或 span 输出而不是一次性构造完整
vector，并允许有界 sibling prefetch。cursor 只保存逻辑 continuation，不能暴露物理 PageId。

#### 阶段 4：真正的 Entity page scan

在 Entity Page 中增加 page-local EntityId array，使全量扫描可以按 page/bitmap 顺序直接枚举 live slot；需要 ID 顺序时另用 Entity Map
range cursor。批量 point lookup 先按 PageId 排序分组，每个 distinct page 只 pin 一次，再恢复调用者顺序。必须验证 scan 不再为每个实体执行一次
独立 Entity Map tree lookup。

#### 阶段 5：选择性、分层 Property 存储

采用“无 property 无目录项；小集合使用 packed shared slotted record；大集合使用 shared overflow/per-key tree；单个超大 value 使用
`LargeValueRef`”的层级。Property token offset directory 必须排序并支持 binary search/selective decode；完整 decode 只允许出现在
`properties(n)`、`keys(n)` 或明确要求完整结果时。overflow key 使用 `(EntityKind, EntityId, PropertyToken)`，布局选择由空间/压力求解器和
带 hysteresis 的阈值决定，不得依赖固定 property 数量。小批量更新使用 sorted `multiLookup`，不得构造整个 Property Directory 镜像。

#### 阶段 6：投影感知和延迟 materialization

增加与 Cypher 文本无关的 `EntityReadShape`，携带实体身份、label/type token、请求的 Property token 和 `all Properties` 标志。内部 query
row 默认只保存 `EntityRef` 和选定字段 cache，不再强制创建完整 `LogicalGraphNode`/`LogicalGraphEdge`；公共结果仍可在边界处 materialize 完整
类型。执行阶段保留 token ID，只在 result/binding 边界解析名称。

#### 阶段 7：topology-first adjacency

实现三层 cursor：`AdjacencyKeyCursor` 只返回 EdgeId，`EdgeTopologyBatchCursor` 返回 endpoint/type token，`EdgeMaterializationCursor`
只在过滤通过且确有需要时读取 selected/full Properties。degree/count、关系类型过滤和 shortest path 不得触碰无关 Property page；关系属性
谓词只解码请求 token。EdgeId 应按 Entity Page 批量解析，每个 distinct page 只 pin 一次。除非基准证明必要，不增加 type-qualified adjacency
tree 或 per-Node persistent delta。

#### 阶段 8：索引候选与实体读取解耦

Index 层返回 `IndexCandidateBatch`，包含 EntityId、索引中已覆盖的值以及 root/snapshot identity。runtime 只验证候选存活状态，对 covered
predicate 直接求值，仅加载缺失的请求字段，避免 index hit 后重复读取完整 Node。全量跨 store integrity cross-check 移到 integrity scanner 或
paranoid mode，不放在每次正常查询路径。

#### 阶段 9：恢复批量执行框架

在 logical runtime 中引入 typed column batch、selection vector，以及可组合的 predicate、filter、project 和 aggregate pipeline。query 编译时
一次性将 label、relationship type 和 Property 名称解析为 token，执行期间不为每行构造字符串键 `unordered_map`。恢复基于 logical operator
shape 的 storage-neutral count、aggregate 和 traversal fast path；它们不得匹配具体 Cypher 文本，也不得引用旧物理格式。point lookup 与 scan
可以选择不同策略，但必须复用相同 typed page、Directory resolution、snapshot validation 和 Buffer Pool 原语。

#### 阶段 10：写入和维护扩展

保持现有格式语义，增加 target-page grouping、每页一个 COW candidate、sorted directory merge、增量 free-space index，以及 online/bulk
共用的 B+Tree WriteBatch。禁止为 adjacency 单独引入 special-case write buffer；只有在测得 COW write amplification 后，才讨论通用且有界的
write buffer。

### 23.3 结构性验收门禁

- 全量 Entity scan 不得对每个实体执行独立 Entity Map tree lookup。
- 同一 snapshot/segment 的 envelope/control 校验必须可复用，不能重复执行。
- Buffer Pool 命中不得复制完整 page payload。
- B+Tree range cursor 只下降一次 root；adjacency count 不得读取 Property page。
- shortest path 不得读取无关 Properties；index candidate 路径不得两次读取同一实体。
- 请求一个 Property 只解码一个 Property；批量 point lookup 对每个 distinct Entity Page 至多一次 pin。
- 全 Edge 属性过滤必须使用顺序 Edge scan 或等价批量路径，不得按每个 Node 重复下降 adjacency root。
- `count` 等仅需标量结果的操作不得构造逐行完整 result；名称到 token 的解析在 query/session 范围内至多执行一次。
- 所有 cache、cursor、batch 和 materialization 内存必须有界。
- relocation、旧 snapshot、split/merge、reopen、checksum/owner/generation corruption 必须保持结果一致或 fail closed。
- 每阶段补充 pin lifetime、cursor continuation、selective/full equivalence、topology/full Edge equivalence 和 deterministic counter tests。
- 在源集合稳定后才执行最终四维 coverage；覆盖率是最终门禁，不得以 mock、伪造损坏页、畸形 plan、TTY/I/O 故障或规模 workload 刷数。

阶段 2、4、5、6、8 的“生产读取完成”状态必须在上述门禁全部通过后重新关闭；在此之前只能标记为 correctness core complete。

### 23.4 当前实施快照（2026-09-24）

本快照用于明确 storage-core 台账与 production-read 优化台账之间的区别。
`Native core complete` 的含义是 native owner、格式、持久化/恢复行为和必要的正确性证据
已经存在；它不表示所有 logical-plan、procedure、GDS、overlay、索引校验或 maintenance
消费者都已使用优化后的 read shape 和 batch 契约。第二个问题的权威阶段状态仍以[优化策略](optimization)
为准。

上文编号为 Stage 0-10 的章节仍是规范性目标，并列出各阶段的最终发布门禁；其中带日期的
检查点属于审计历史。如果带日期的段落与下面的表格看起来冲突，应以本表判断当前实现边界，
以能力验收矩阵判断公开契约能否退休，并以[优化策略](optimization)判断 production-read
消费者迁移状态。

历史段落中出现的 format v24、v25 或其他中间 format 编号，只表示当时的清理检查点，
不是当前数据库仍接受的格式。当前源码常量是 format envelope v4 和独立的 Superblock
codec v30；当前格式拒绝旧版本，不提供 migration 或 compatibility fallback。同样，历史台账
中的完整测试、coverage 和 benchmark 结果不能验证当前未提交工作树。

当前读取优化证据也必须按时间边界解释：较早的 shape-aware projection/adapter 检查点为
`15/15`；命名 binary 上单次 projection staging 改动后的 `LogicalGraphProjectionTest.*` 为 `14/14`。
记录中的 runtime/executor `306/306` 和 direct-edge `77/77` 是历史定向证据；direct-edge 的
`78/78` 也早于当前 Vector 改动。上一次已验证的 `QueryEngineLogicalBackendTest.*` boundary
suite 在 Vector 改动前通过 `57/57`，其中 `VectorProcedure*` 为 `8/8`；这些数字在当前实现
重新编译和重跑前都不能作为当前工作树证据。独立的 `ShortestPath*` 边界组此前通过 `3/3`，
`gds.shortestPath.dijkstra.stream`
与 `algo.shortestPath` 共享排序后的 path-Node 物化 helper；它恢复逻辑路径顺序，并在 byte bound
中计算重复路径位置。这些数字都不是当前完整测试套件结果，也不能单独关闭 production-read 阶段。

本节之后保留的 dated audit 段落属于历史记录；其中出现的“当前 direct-edge `78/78`”均指
Vector lazy-`EntityRef` 改动之前的工作树。阅读当前状态时，以上快照和
`Vector procedure hit 物化` 的“待验证”状态优先，不能把历史段落中的“当前”解释为已验证的最新工作树。

| 范围 | 当前状态 | 证据边界与下一步 |
| --- | --- | --- |
| Typed Entity scan | 已实现 | Node/Edge typed Entity-directory cursor 复用已解码的 Entity page，并已有 relocation、reopen、corruption、early-stop 和旧 source 清理定向覆盖。后续增加 prefetch 或 continuation 时必须保持该边界。 |
| Shape-aware GDS projection scan | 部分实现 | `LogicalGraphReadSnapshot` 和 native page-backed adapter 新增可续接 Node scan，按 identity/label-only、selected-column 和 complete-property shape 读取，同时保持 continuation、ID 顺序和 metadata-page 上限不变。`LogicalGraphProjection::build()` 仅在存在 label filter 时请求 label，并在单次 scan 中逐页完成校验和过滤，只保留选中的逻辑 ID，不再保留第二份完整临时 Node vector。projection/adapter 定向运行通过 `15/15`，当前 binary 的 projection suite 通过 `14/14`。这是独立检查点，不属于已记录的 `306/306` 结果，也不能关闭 Stage 2/3。 |
| Native/topology 路径的 selected read | 部分实现 | source scan、topology target、Edge batch 和 native index candidate 已有 selected-before-complete 检查点；shape-aware GDS projection scan 是最新的 native source-scan 检查点；其他消费者仍需审计并补结构计数器证据。 |
| Logical runtime 批量执行 | 部分实现 | Map Projection、裸 Entity projection 的 alias-cache 转移、dynamic list literal、list comprehension、reduce、quantifier、`labels()`/`type()` 元数据、pattern comprehension 外层 row Property、pattern-local target/relationship binding，以及跨行 NamedPath payload 已使用共享有界批量收集。pattern-local 路径按候选 reference/topology adjacency page 批量读取 selected columns 并恢复原 adjacency 顺序。complete-Entity 需求（`properties(entity)`、`keys(entity)` 和 `entity{.*}`）现在在 Filter、Project、Aggregate 和 Sort 中使用一次有界预取，同时保留安全 selected-column sibling。通用 selector、complete-payload、Filter 直接契约以及直接 JOIN/OPTIONAL MATCH payload 测试已通过最近一次定向回归；复杂消费者、procedure 路径和其余生命周期审计仍开放。 |
| 跨行 deferred EntityRef 输出物化 | 部分实现 / 检查点已验证 | `executeToQueryResult(...)` 已按有界批次排序去重并批量物化 Node/Edge，再恢复原行顺序；QueryResult-only NamedPath 还会跨 UNION/JOIN/OPTIONAL MATCH 行预取 path reference。公共逐行 visitor 使用单行 flush 保持严格 early-stop。历史 306/306 组合定向回归与 Vector 改动前的 direct-edge `78/78` 都是 focused evidence；后者包含只读 correlated subquery complete-Entity 改动，并覆盖 deferred-EntityRef Sort-key 与较早的 imported-Property 回归。四个核心算子之外的完整 Entity payload 及其他 Stage 6 消费者仍需审计。 |
| Vector procedure hit 物化 | 部分实现 / 待验证 | 当前 `LogicalPlanProcedureRuntime` 在验证、过滤、投影、`SKIP` 和 `LIMIT` 阶段保留 `EntityRef`，区分 selected 与 complete Property cache，仅为最终仍需 Node 的存活输出按排序有界 batch 读取并恢复 search-score 顺序；scalar-only 和 Property-only 输出应避免完整 Node read。此前 `QueryEngineLogicalBackendBoundaryTest.VectorProcedure*` 的 `8/8` 早于该实现，必须在重新编译后重跑。procedure 跨行 Property expression batching 和更广泛的 procedure 生命周期审计仍未关闭。 |
| Shortest-path procedure Node 物化 | 部分实现 / 检查点已验证 | native `algo.shortestPath` 与 `gds.shortestPath.dijkstra.stream` 共享有界排序 Node helper。它校验 batch cardinality 和逻辑 ID，恢复算法路径顺序，并将重复路径位置分别计入 materialized-byte 上限。`ShortestPath*` 通过 `3/3`；通用 procedure Property-expression batching 以及更广泛的 procedure/GDS 生命周期审计仍未关闭。 |
| Snapshot/cache 与写入批处理 | 部分实现 | snapshot-local route/page/session cache 和部分 selected write planning 已存在，但生命周期、回收、故障和通用 write-batch 门禁仍开放。 |
| 最终评估 | 暂缓 | 最近记录的完整 macOS quick run（`native-20260923T211232-90355`）早于工作区最新未提交代码修改；历史 306/306 定向运行已包含 complete-Entity 预取、混合表达式 selector、跨行 NamedPath batch、deferred-EntityRef Sort-key 和较早的只读 correlated subquery imported-Property 回归；Vector 改动前的 direct-edge suite 在 complete-Entity subquery 改动后通过 `78/78`，但这些都不是完整测试套件。跨平台无诊断 Release 对比、规模矩阵和最终四维 coverage 仍属于发布门禁。 |

shape-aware projection scan 只是新的实现检查点，不改变任何完成百分比。通用 adapter 的默认 overload
仍保留原 complete scan 语义；native adapter 则不会打开调用方未请求的 label 或 Property root。
`15/15` 定向运行已经覆盖 selected/complete 等价性、label filter 语义、缺失 Property 行为、
continuation 和 metadata-page 计数；更广泛的 consumer、规模和生命周期门禁仍未关闭。

约 **34%** 的进度只属于 production-read 优化路线图，不是存储迁移、正确性、coverage 或
性能提升百分比。最近记录的诊断 coverage 为 Region 94.33%、Function 99.11%、Line 95.55%、
Branch 89.13%，不能作为 C7 关闭结果。Stage 6 pattern-local binding 的候选页路径已经
完成（历史组合 runtime/executor 定向回归为 306/306，通过时 direct-edge suite 为 77/77；当前 direct-edge suite 为 78/78。此前的 305/305、304/304、298/298、297/297、296/296、229/229、222/222、220/220、219/219 和改动前的 104/104 均保留为历史证据）。当前定向回归已经包含 complete-Entity 预取、通用混合表达式 selector、跨行 NamedPath path reference 批处理、deferred-EntityRef Sort-key 和只读 correlated subquery imported-Property 回归；当前工作树的完整回归
和 Stage 6 其余消费者审计仍然开放。本方案不得因此宣称目标设计已经全部实现，也不得宣称
native 架构相对于已退休旧架构已经整体更快。

最近一次已验证的工作树中的 `LogicalPlanReadRuntime.cpp` 已形成跨行 deferred
`EntityRef` 输出物化检查点。completion callback 在 `executeOnSnapshot()` 或
`executeInternal()` 仍持有 `ExecutionState` 时执行，Node/Edge、重复行和 early-stop
历史定向回归通过 306/306（当时 direct-edge suite 为 77/77；当前 direct-edge suite 为 78/78），其中包含接入 Filter、Project、Aggregate 和 Sort 的通用混合表达式 selector、complete-Entity 预取、完整表达式 fallback、deferred-EntityRef Sort-key 和较早的只读 correlated subquery imported-Property 回归
及跨行 NamedPath path reference 批处理。上述结果只能计入 Stage 6 的部分消费者边界，不能关闭剩余
logical-plan、JOIN/OPTIONAL MATCH/UNION、procedure、GDS、overlay、maintenance 或
完整回归门禁，也不改变约 34% 的路线图估计。
