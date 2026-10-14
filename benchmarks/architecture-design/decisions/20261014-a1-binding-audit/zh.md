# A1 绑定身份与物理接口核对

日期：2026-10-14。状态：当前源码核对及 116 项必要组件测试通过，S1.1 仍进行中。本记录不新增验收项，也不把现有列快路径当作完整物理计划。

后续接口基础见 `../20261014-a1-schema-observation/zh.md` 与 `../20261014-a1-property-descriptors/zh.md`。下表保留核对时状态；原生 System Catalog 实际通过专用 B+Tree 和两级目录解析，不能以 Superblock.catalog_root 或逻辑根 PageId/lifetime generation 直接充当 schema 内容版本。

## 已核对的实际代码

| 边界 | 当前实现与限制 |
|---|---|
| 前端/缓存 | QueryEngine::buildPlan 按 query 字符串查询现有逻辑 PlanCache，parse/optimize 后缓存；mutatesSchema 清空缓存。不存在独立的不可变物理计划缓存，不能直接把 snapshot token 放入这里 |
| 变量绑定 | BoundSlotRegistry 的 uint32 slot/scope 与 uint64 plan identity 拒绝跨程序描述符，seal 后只读；BoundProjectionReferences 按行借用输入，在作用域退出清空引用。它仍是 query-local 旧行适配器，不是完整 AST/算子绑定器 |
| 定长列 | int64/double/bool 连续值；bool 为 uint8。缺失、NULL、有效值分开；容量与扩容重叠由 BudgetMemoryResource 计费，clear 保留计费容量，release 返还；矩形批次追加失败只回退未发布行 |
| 选择 | SelectionVector 使用 uint64 bitmap，而不是逐行索引数组；保持物理位置，重复行由不同输入位置保留。此组件的标准 vector 容量不自动纳入 PMR，完整物理 owner 必须显式准入其持有容量 |
| 动态回退 | PrimitiveNumericColumns 先验证矩阵与类型，混合类型/可选缓冲预算不足在聚合状态修改前退回同一动态累加器；取消、allocator 错误及坏 shape 不当作可选预算退回 |
| 执行状态 | ExecutionSharedState 借用 snapshot/bounds/context，持有预算、PMR cache 与结果保留；ExecutionTaskState 持有 stats/injected row/property columns。当前单线程，shared cache/预算不能直接并发访问 |
| 输出 | QueryResult 拥有结果行，公开行回调与可延后 EntityRef 的最终结果管线有不同借用/read-ahead 边界；不能让列/页 span 逃逸到 ABI |
| schema 身份 | 当前 mutation fence 包含 backend/format、7 个图根的 21 个 opaque token 及 overlay revision，不是独立 schema epoch。Superblock 有 database_id/catalog_root，但查询层尚无经过认证的 schema observation 接口 |

组件测试包括槽位/作用域/旧 plan ID、投影借用、定长列与矩形回滚、选择、allocator 准入、动态数值回退、NULL/NaN/溢出、列过滤/聚合以及嵌套执行/预算/输入恢复。当前 Linux C++20 核心及这 10 个限定 suite 链接后，116 项通过；未运行全量测试或 benchmark。

## 下一实现前必须关闭的接口

1. 完整的 query-local 不可变物理描述符：slot/layout、类型合同、访问路径/算法、流水线/阻塞点与语义版本。当前已有组件不能代替对所有 LogicalOperator/Expression 的绑定遍历。现有逻辑 IR/缓存保留，不能先把可写 task 状态塞进 cached plan。
2. snapshot-owned token resolution 与 schema observation 身份：数据库身份、backend incarnation、认证 schema/catalog stamp、能力合同及 overlay revision 的校验。不能未经定义把完整 graph fence 当独立 schema epoch。首版物理缓存应关闭；未来只有这项接口及跨数据库/reopen/DDL/overlay 的失效测试闭环后才启用。
3. 统一批次 envelope：定长列/三态、EntityRef、动态值 owner、选择、来源身份、容量准入和批次代次。动态/变长拥有化以及 source decoder 的实际容量仍需明确；标准 vector 或 encoded bytes 不能冒充整个分配 owner。
4. 平坦首版与分组扩展的确切字段/消费者合同：父绑定、offset、相关组、重复计数、顺序、NULL 扩展与溢出；DISTINCT/group-by/path/subquery 的展开边界必须定案。代表性分支/多跳实验留到统一最终窗口，不能以没有连接的扫描差距推导因子化收益。
5. 状态/生命周期与移除清单：不可变计划、query shared、task cursor/batch/accumulator、blocking merge owner 分开；单线程先闭环，O7 再接调度。旧行适配必须被计划明确列出、计费及测试，不能静默回到未计费路径。

这些是后续具体接口的工作清单，尚未宣布接口已实施或 S1.1 已验收。A1 与 A6 的容量/生命周期依赖要在相应生产接入前关闭；当前源码没有被替换为另一套未经验证的执行器。

证据：`benchmarks/architecture-design/results/20261014-a1-binding-audit/` 的日志、模块链接命令与源码 hash。源码基底为已推送 `646298ae`；本记录暂留工作树，待后续验收增长时提交。当前验收为 S0 25%、S1 28.6%、S2 0%、S3 20%、S4–S6 0%，总体 4/34（11.8%）。
