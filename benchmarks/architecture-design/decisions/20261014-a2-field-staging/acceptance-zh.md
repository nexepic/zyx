# S3.1 A2 及旧布局回归验收

日期：2026-10-14。结论：S3.1 通过。此验收仅覆盖 A2 在当前布局上的实现与回归，不验收 A3、统一资源预算、后续完整故障矩阵或性能收益。

依据正式 optimization.mdx 的 A2 合同、S3.1 定义及 A2 设计合同。载体选择的具体细化见同目录 zh.md，所有原迁移目标保留。

| 验收要求 | 实现与直接证据 |
|---|---|
| 按键 SET/REMOVE 与完整替换分开 | native 组合 patch 与 transaction raw staging 有独立入口；完整 batch 不携带可被忽略的 raw 字段。默认能力关闭/raw 明确拒绝，旧 backend 完整适配通过 |
| 现有 Cypher 四种属性语义 | runtime 不引入第二条语义；mutation/atomicity/validation、FOREACH/map/alias 与真实 Cypher transaction 模块通过 |
| 全引用及 snapshot 身份 | entity kind/reference kind/ID，source fence 与 overlay revision 验证；同数值 Node/Edge、created ordinal/provisional ID、外部 writer 冲突通过 |
| 逐语句可见、最终字段折叠 | 连续 32 条真实 Node/Edge raw 语句值正确；累计 complete target 0、raw target 2、entry 2；最后 SET/REMOVE/type、恢复原值、full-map 重置与存储 NULL 通过 |
| 创建后修改/删除 | 创建字段折叠及后续 provisional 引用通过；既有创建/删除 canonicalization 和 DETACH 回归通过，较早 raw 属性不重新写入已删除目标 |
| 有界验证、取消及原子失败 | 正 entry 上限、源准入、无效/重复/冲突键、溢出、read-only、ingest 混用拒绝；三种真实暂存状态逐检查点取消后保留前一语句并成功提交 |
| 同一派生状态发布 | 原生 token、scalar/vector/constraint/statistics 沿用一个 native publication；唯一/NOT NULL、类型/碰撞、不变索引与 companion 标签原子失败回归通过 |
| pin 与旧布局生命周期 | owning 字段不保留页面借用；16 KiB source/旧 snapshot、LargeValue/property-root/store 回归通过；提交重开及 integrity 通过；没有改变 codec |
| 失败/恢复 | graph fence 冲突状态明确；实际显式 Cypher 最终 sync 失败为 INDETERMINATE，重开只见完整旧/新状态；既有 writer 失败/重开模块通过 |

验证：507 项不同限定模块测试通过、0 失败，另补验宽属性的重开 integrity。源码/日志及模块链接命令见同目录记录所列 results。未运行全量单元测试或 benchmark；不宣称 macOS/Windows 已构建。

A2 允许在旧布局实现，所以最终 root 的完整编码、可见性实体的完整字段以及显式完整替换的适配不是跳过 A3 的理由，也不影响本项对正确字段请求和最终状态的验收。A6 的全查询容量/预算与 S3.5 的资源/性能门槛仍未验收，不把工作 entry 数当作这些指标。S3.2–S3.5 均保持未完成。

固定 34 项新增 S3.1：S0 1/4（25%）；S1 2/7（28.6%）；S2 0/5；S3 1/5（20%）；S4 0/4；S5 0/5；S6 0/4。总体 4/34（11.8%），权威 JSON 为 `../current-progress.json`。正式 optimization.mdx 保留历史状态。验收增长后的代码、双语决策与证据按既有授权提交/推送；完整优化目标仍未完成。
