# A2 字段暂存及接口细化

日期：2026-10-14。状态：原生显式事务字段暂存完成；S3.1 验收见同目录 `acceptance-zh.md`。本记录细化 20261012-a2-design-contract 第 2.1 节的载体选择，保留其全部字段语义与迁移验收目标。

## 1. 为什么细化载体

原设计拟在 `LogicalGraphMutationBatch` 尾部增加 raw patch vector 与 entry bound，并在所有消费者迁移后启用全局能力。实际原生消费者已采用独立的组合 patch 入口，而旧 batch 消费者仍代表完整最终态。向旧 batch 增加可被忽略的字段会要求每个旧调用点支持或拒绝它，扩大静默丢操作的风险。

现在保持完整 batch 的类型与语义，采用独立拥有数据的 `stageMutationBatchWithPropertyPatches(companion, patches, bound, control)` 暂存入口。能力为 `LogicalGraphTransaction::supportsPropertyPatchStaging()`，默认 false；默认 raw 入口明确抛错，不能忽略操作。原生事务在迁移 preview/overlay、shape、创建引用、最后操作合并、完整替换交互、提交与失败清理后启用。旧事务与测试 backend 保留经过验证的完整集合适配路径。没有并存第二套 Cypher 语义，也没有新增 codec 或 Driver ABI 函数。

此接口细化替代原设计的两个 batch 字段和 `supportsLogicalGraphPropertyPatchBatch()` 名称，不减少任何源 fence、正 entry 上限、引用身份、取消、原子派生状态或旧 backend 回归要求。未经支持的消费者不会接收到隐藏在完整 batch 中的 raw 字段。

## 2. 语句可见性与持久暂存

原适配器的归一化抽为 `normalizeLogicalGraphPropertyPatchBatch`。它沿用先验证所有请求、再用来源 snapshot/fence 与 shaped admission 读源的协议，返回完整可见 batch、实际 entry 工作量及有变化的持久引用补丁。同值 SET 和不存在字段的 REMOVE 不进入持久补丁；混合修改中同值载荷从 source 移入可见集合，避免额外复制该输入值。创建 ordinal 的修改直接折叠进创建属性。

覆盖层使用同一完整可见 batch 执行 preview 和 apply，保证每条语句的 read-your-writes；对支持能力的事务，只把 companion 与拥有键/值的变更字段传给 raw 暂存入口。此前的按 owner `StatementUndo` 覆盖所有修改与失败恢复。语句异常不会递增 revision；preview/stage 的创建 ID 必须一致。

这是当前布局下的可见性适配：覆盖层仍拥有完整实体，读取和 undo 仍可能处理目标 owner 的全部字段。本实现消除的是 raw 属性在持久累计状态中的完整集合副本和提交前的 base/final 差量重建，不宣称消除了所有全集合读取、所有候选副本或单字段物理写放大。

## 3. 事务最终状态

原生暂存先校验全引用、键唯一性、非 NULL SET、删除/完整替换冲突、正上限与溢出，再构造候选累计状态。持久实体的同名字段仅保留最后一个 SET 或 REMOVE；Node/Edge 同数值 ID 分开。随后完整 map 替换清除该目标旧 raw 操作；完整替换之后的 patch 更新该完整候选集合，保留未涉及的显式存储 NULL。创建 ordinal 与后续 provisional ID 均重映射并折叠进创建载荷。

取消检查进入验证、字段移动/合并与候选发布前。候选修改失败不会改变原累计字段、创建 ID 列表、租约范围或 entry bound；覆盖层同步恢复语句前状态。完整/字段混合请求仍由同一事务最终发布。只有 raw 字段的事务也被视为已有写入，禁止与有界 ingest 模式混用；read-only 在暂存前拒绝。

提交时完整替换仍沿用其 base/final 适配，raw 累计字段直接加入唯一 native patch publication。显式删除与后续 DETACH 淘汰较早属性修改。当前图 fence 改变时冲突并要求重算；恢复原值也不能绕过 fence。NativeGraphMutationExecutor 保持 token 合并、无变化 root/索引跳过、约束、向量、统计和单一 COW 发布协议。添加后删除的临时字段不会创建 catalog token。最终 sync 可能已发布的错误仍报告 INDETERMINATE，关闭/重开后判断完整旧/新状态。

## 4. 工作上限与后续资源边界

每条归一化语句记录 source 字段及原始操作的实际 entry 工作量。事务累加已准入的 statement bound，作为最终原生合并的保守上限；原生读取再次检查真实 source 和最终操作。直接 raw API 必须提供正上限，创建/完整候选 source 在折叠前准入，计数溢出拒绝新语句。历史工作许可可以大于最终存活字段数；它不是内存字节、schema 上限或性能结果。

源的 encoded-byte admission 与拥有数据的同步读控制继续生效。整个查询的容器容量、覆盖层/候选/写事务共享 allocator 预算尚归 A6；S3.5 的真实内存、空间和性能验收尚未完成。本次不把 entry 数等同于这些资源，也不把 Linux 模块验证等同于跨平台构建。

## 5. 验证与证据

141 项原生属性/事务/索引测试、72 项覆盖层测试、293 项 mutation/Cypher transaction/旧属性布局测试，以及单独运行的 1 项 raw/ingest/read-only 混用测试通过，共 507 项不同测试。另对宽属性测试增加重开 integrity 验证后重建并单独复验通过。未运行全量测试或 benchmark。

新增验证包括 32 条语句、同数值 Node/Edge ID、16 KiB 不变字段，累计完整 property target 最大值 0、raw target 最大值 2、raw entry 最大值 2；每条语句及旧 pin 的值正确，提交重开后通过完整性检查。该数据只证明累计字段范围，不是吞吐或 RSS 收益。SET→REMOVE→SET/type 变化、完整替换重置、存储 NULL、创建 ordinal/provisional ID、无效请求/源上限/溢出失败、并发冲突及恢复原值无 root 变化均通过。

对三种真实事务状态（先前 raw 字段、先前完整 map、事务创建实体），测量一次实际检查点集合，然后逐个注入取消，验证 overlay fence/值及 prior transaction 状态不变，提交只保存前一语句的值。既有实际 Cypher、scalar/vector/constraint、最终 sync、旧 LargeValue/pin、删除/重开回归同时通过。

最初新增九项测试误用了 fixture 默认 read-only open mode，因 writer 入口拒绝而失败。检查真实 options 默认值后，仅修正这些新测试的打开方式，未改变生产 read-only 规则；上述最终回归已重建通过。

原始构建/测试日志、各模块链接命令、源码/二进制 hash 和验收记录位于 `benchmarks/architecture-design/results/20261014-a2-field-staging/`。正式 optimization.mdx 按既有约定不改，其历史表格不是最新验收状态。
