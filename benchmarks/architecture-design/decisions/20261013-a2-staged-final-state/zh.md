# A2 暂存属性与标签的有界最终态折叠

日期：2026-10-13。状态：事务累计列表按完整实体引用保留最后一次属性/标签替换，127 项限定模块回归通过。S3.1 仍未整体验收。

此前 `append` 保存每条语句的完整 property/label replacement，到 canonicalization 才取最后一份。同一实体反复修改宽属性时，暂存载荷按语句次数累积，下一条语句的失败还原副本还会复制这份历史列表。

`mergeFinalReplacements` 现在以 entity kind/reference kind/value 定位目标，在语句的候选累计状态内覆盖旧替换。失败时原 accumulated 状态不变，成功后才移动候选；overlay 继续拥有每条语句的中间可见状态。创建引用仍由既有 fold/rebase 处理，Node/Edge 同数值 ID 不混淆。没有合并独立事务或减少 durability 屏障。

诊断在候选发布前记录累计 property/label target 数；诊断分配若失败，不会在已修改事务后再抛错。新增回归连续 32 次替换一个 16 KiB owner 的属性与标签，每次读取均核验当时的 rank/label，累计列表两个 target 指标最大值均为 1；提交后保留 payload、rank=32 和最后标签。该数字只证明累计列表有界，不是全查询内存/RSS、分配计数或延迟测量。

production core 与受影响模块构建后，127 项测试通过、0 失败。相关 Node/Edge 最终态、创建引用、失败语句保留、索引/约束、旧快照、实际 Cypher 事务、最终 sync indeterminate 和 DETACH 回归一起通过。无全量测试/benchmark。证据：`benchmarks/architecture-design/results/20261013-a2-staged-final-state/`。

仍保留完整集合暂存及候选状态副本；原生字段级暂存/资源计费与替换条件继续推进。此项不关闭 S3.1 或性能验收。当前新增验收来自独立 S1.7 格式设计，固定进度为 S0 25%、S1 28.6%、S2–S6 0%，总体 3/34（8.8%）。
