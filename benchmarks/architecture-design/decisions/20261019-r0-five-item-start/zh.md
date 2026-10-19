# R0 起点、基线与五项范围

计划 `cow-performance-20261019`。本轮只交付 R0.1/R0.2/R0.3/R1.1/R1.2；不接入混合属性格式、邻接候选、WAL、多写者或新 spill 算法。

## R0.1：源码与消费者审计

远程 work 与本地起点一致。文档整理独立提交为 `02380c0d`；生产源码与冻结 `c28794a7` 无差异，720 个实际源文件的 hash 见 source-manifest.json。保留全部既有接口与格式。

| 当前路径 | 本轮替换边界 | 仍保留的适配器 |
|---|---|---|
| PropertyCollectionRootScanCursor 选列解码 → owner×token entries → optional 值列 → named 值列 | 新增同步 selected-cell sink；同一认证/页排序逻辑可直接交付已解码单元，不建立整个矩阵 | 原有返回矩阵接口及其缓存行为保持 |
| BoundNodeScanBatch → PrimitiveNumericColumns | 为新生产者保留一个预算内的直接列 owner，标量进入对应数组，字符串进入 offsets/bytes | 不支持新接口的 snapshot 仍走显式旧矩阵路径 |
| NodeColumnReceiver → 无分组、非 DISTINCT 数值聚合 | 增加直接单/多键分组和 DISTINCT 消费，不构造 LogicalPlanReadRow/property_cache | 复杂表达式、关联、副作用、未支持算子保持既有有界行路径 |
| sort/DISTINCT scratch、QUERY/CACHE/WRITE/MAINT、结果保留 | 仅作为保护合同 | 不重复实现、不默认开启 scratch |

现有普通事务图根 fence、受限 blind rebase、key-only 邻接和 COW/A-B 不变。首次属性认证、旧快照及错误传播不被优化绕过。

## R0.2：基线与最终采样预登记

按照主文允许的同条件复用规则，复用上一轮完成的 c287 读/写/资源基线，不重新跑长矩阵。proof.json 实际验证：生产及 harness 源码无差异、Linux/platform/机器架构/允许 CPU 集合完全相同、冻结 benchmark/worker 二进制 hash、medium/holdout 输入文件 hash。既有 Release 构建选项、数据 manifest/schema、1 线程、3 独立进程、3 warmup/10 measurements、计时/结果消费/缓存/durability 边界沿用原登记，不能把报告中的 harness Git 元数据误当二进制生产源码身份。

主要目标是数值/字符串过滤计数、真实单/多键聚合、COUNT DISTINCT。点查/gather、完整实体、复杂类型与 NULL、排序/DISTINCT spill、小写、快照及资源是保护合同。主要 P50 改善 10%、保护退化 5%、资源增长超过 5% 须明确准入的门槛不变。保留全部独立进程分布、对照失败/N/A、已接纳的 7–11% 准备阶段 RSS 成本和尾延迟不确定性。正式性能集中到 R7，届时使用保留的 before 二进制，不能混用旧 source 的 after 样本。

R1.1/R1.2 在实现前归类为基础接口/能力交付，按语义、完整生命周期和必要模块验收；其性能收益由 R1.3/R7 判断。当前无新性能样本，无超越 Kuzu 声明。

## R0.3：机制与生产前决策

已有短诊断揭示：User.country 过滤处理 100000 owner，2917 次元数据页读取及 17019 次文件大小调用；真实分组产生 100000 逻辑行，累计物化字节 42700000。全节点数值过滤已使用 430 个类型化批次，不能称没有向量化。单次计数仅定位机制，不能推导 CPU 份额、峰值或设备读取量。原记录在 `/workspace/zyx-architecture-analysis-20261019/summary.json`；本轮只保留精简计数，完整正式准入仍引用已提交旧证据。

| 决策 | 本轮确定的合同 | 所属任务 |
|---|---|---|
| native producer | 同步、可选能力；输入严格递增非零 ID，token/name 允许独立绑定；sink 接收原始 owner ordinal 和列 ordinal，不依赖物理页顺序 | R1.1 |
| gather | 排序/去重仅用于源定位，公开 gather 必须保存重复及原 ordinal；缺失单元不变成有效 NULL；扫描仍拒绝重复实体 ID | R1.1 |
| payload | native cursor 完成原 codec/owner/generation 认证后逐 owner 交付；临时 scalar PropertyValue 解码不等于整个 PropertyValue 矩阵；直接 owner 不再经矩阵转类型列 | R1.1 |
| lifetime | 数据归查询批 owner；所有数组/字符串容量计费，借用受 batch owner/incarnation 约束；seal 后才可消费，失败销毁不发布部分聚合/输出 | R1.1 |
| dynamic | 混合类型保留逐 cell 类型；复杂值走明确计费的 owner，不能推断永久 schema | R1.1 |
| filter | 复用已绑定谓词及选择向量；字符串直接比较借用字节，不构建每行属性表；NULL/缺失均按现有过滤语义处理 | R1.2 |
| aggregation | 完整 tuple 编码沿用现有类型敏感 DISTINCT key；首次出现顺序、物理输入浮点顺序、计数/NULL/空输入不变；状态/输出复用现有预算与 accumulator | R1.2 |
| failure | 仅交付前的资源压力可拆批/回退；损坏、取消、I/O 不重放；消费者异常不回到 source 重试 | R1.1/R1.2 |
| deferred | 混合物理格式/组参数、邻接可信覆盖、通用 group/hash spill、并发冲突放宽不在这五项中定案或实施 | R3–R6 |

必要测试覆盖缺失/NULL/混型、字符串/布尔/数值、重复与乱序 gather、别名、单/多键分组、DISTINCT、过滤、空输入、错误、不确定额度、取消、旧快照和借用失效。只运行受影响模块，未跑全量单测。
