# A2 连续补丁描述符与原生接入边界

日期：2026-10-12。

## 已实现变更

finalPropertyPatch 将原属性查找从 PMR 哈希表改为 PMR 连续描述符数组，每项借用原属性键及值地址。数组排序后通过二分查找比较最终属性；未改变字段不进入 sets，最终缺失字段进入 removals。SET/REMOVE、MERGE 与 CREATE/FOREACH 的持久实体路径共用该入口。输入、排序比较和查找均检查查询取消。描述符生命周期限于当前调用，早于 resource 和 budget 销毁。

数组实际分配仍由 BudgetMemoryResource 准入，不复制借用的原字段载荷。此变更取消逐字段哈希节点和桶分配，但增加排序与二分比较成本；未运行性能测试，不能宣称速度提升。局部预算仍不覆盖输出补丁、实体缓存和结果，也不是全查询或进程总预算。

## 验证与证据

严格模块构建成功。LogicalPlanMutationRuntimeTest 202 项、BudgetMemoryResourceTest 7 项，共 209 项通过。新增乱序原属性与增加、修改、删除混合操作测试，检查保留字段、最终值、单次批次发布及 shaped source 读取。证据位于 results/20261012-a2-patch-flat-lookup，包含构建日志、测试日志/JSON、模块构建配置及生产/测试源码 hash。仅验证 macOS；未运行全量单元测试、benchmark、提交或推送，未修改正式文档或英文记录。

## 原生增量尚未闭环

当前调用链：mutation runtime 生成最终差量 → LogicalGraphWriteBackend::executeLogicalGraphMutationBatchWithPropertyPatches → 读取源完整属性 → 合并为 property_replacements → executeLogicalGraphMutationBatchWithReadControl → NativeGraphMutationExecutor。适配器已避免未改变字段的第二份值复制，但仍处理完整集合。NativeGraphMutationExecutor 已对标量索引使用 stagePropertyDelta，不能因此宣称属性物理局部修改已完成。

下一步需使原生 planner 消费字段操作并与 companion creates/deletes/labels 在同一发布点处理。不得单独发布补丁或旁路 overlay。源身份仍由显式 planning fence 绑定；冲突后整个语句重新读取并计算，不复用旧差量。真实布局成本受 PropertyCollectionRootStore 的页内集合及 LargeValue 编码限制：接口增量无法自动消除整集合编码/页 COW。必须明确旧布局的不可避免成本，再由 A3 生命周期和准入决定新布局，不能借本轮调整格式。

新增原生入口前须覆盖：同值、删除后重设、NULL/缺失、类型变化、创建引用 ordinal、删除目标、索引/约束失败、旧快照、故障发布与回收。输出与暂存统一 owner 也仍待实现。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。本轮不新增整项验收。
