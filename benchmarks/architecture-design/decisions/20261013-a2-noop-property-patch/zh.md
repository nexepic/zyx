# A2 无变化属性 patch 消除

日期：2026-10-13。状态：局部实现与定向测试通过；S3.1、S3.5 仍未验收。

`LogicalGraphWriteBackend` 在将 SET/REMOVE patch 归一化为完整属性集合前，现在先与同一快照中的已排序属性比较。所有 SET 值均相等且所有 REMOVE 键均缺失时，跳过 replacement。上层 overlay 对空 batch 不增加 logical revision，因此不触发属性根重编码、派生索引重算或发布。比较使用现有 `PropertyValue` 相等语义；NaN 等不相等值继续走原路径。遇到任何实际变化时保留完整集合编码与原有原子发布协议。

新增 `UnchangedValuesAndMissingRemovalsDoNotStageAReplacement`，验证同值 SET 加缺失 REMOVE 后 revision 和属性集合不变。属性 patch 与原生创建、重开、预算重试及唯一约束回滚共 20 项定向测试通过。构建时还修复了 native reservation 的显式聚合初始化，以及 BTree 页测试漏掉 `<algorithm>` 的直接依赖。

没有运行性能 benchmark；因此只记录实现路径减少了无效 replacement 工作，不声称端到端收益。尚需在最终统一对照中验证收益与保护场景。证据、命令和源码 hash 位于 `benchmarks/architecture-design/results/20261013-a2-noop-property-patch/`。本轮不改变 34 项进度，也不完成 S3.1/S3.5。
