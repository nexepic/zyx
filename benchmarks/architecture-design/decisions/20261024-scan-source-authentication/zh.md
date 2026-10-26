# S2 源数据认证与初始环境校准（未验收）

本记录不增加进度，S2 仍未验收，总进度 15%。本轮没有 commit/push。完整阶段状态仍以文档站 optimization.mdx 为准。

BUILDING 复制现在先验证源 envelope 及其声明载荷的 CRC，再改写格式头；校验失败不改写输入段，也不复制损坏段到目标。既有完整 integrity 和最终准入仍不可省略。

新增内部 streamPrimaryMembership，从同一存储的真实 Entity Map/记录和完整 LabelSet 生成 64-ID 成员组；无属性与无标签节点不会遗漏。记录绑定来源 database/generation，标签按 token 排序。单实体或跨实体标签并集超过 64 时整组拒绝，不截断；取消或消费者异常不重放前缀。源工作区最多 1 MiB，须绑定调用者账户；consumer 的目标状态须使用独立目标账户。该函数只提供构建材料，不颁发扫描证明，不发布目录或清除 BUILDING。

必要测试共 17 个不同用例通过：扫描格式 codec/真实文件复制 8、默认格式拒绝 1、真实 Entity/LabelSet 来源 8。覆盖载荷损坏、源文件保留、取消、预算归还、无属性/无标签、跨组边界、旧快照、空输入、单实体/跨实体标签超限及消费者异常。未运行全量单元测试。

## 初始性能校准

Linux / Intel Xeon Platinum 8370C，4 CPU 配额、16 GiB 环境内存；单线程，6 GiB 进程地址空间上限，64 MiB 原缓存预算，Kuzu 0.11.3。两个数据集各 3 组交错独立引擎进程，每进程每场景 3 次预热、10 个正式样本，共 480 个样本；完整类型化结果和重复次数通过 oracle，多键分组也检查顺序。P50 合并样本，逐进程 P50/P95 见 initial-summary.json；P95 为线性插值，十个样本不能确立精确尾延迟。

历史 AMD 产物缺失，本机重建 CSV 与历史 12 个文件 hash 完全匹配（medium seed 42 / holdout seed 314159）；新建 ROW 数据库的物理身份不同。本结果仅作当前 work 初始校准，不取代登记的历史选型基线，不是候选窗口或最终验收。基准读取入口冻结于 9d65a397；后续私有构建源/header 改动未进入该查询二进制。计时 archive 的两个 I/O/metadata 翻译单元去除测试 hook 分派，保留 O3/NDEBUG，无 profile/coverage；库/二进制 hash、命令和原始数据索引均保留。

下表比值大于 1 表示 ZYX 更慢，所有场景均尚未超过 Kuzu。

| 场景 | 规模 | ZYX P50/ms | Kuzu P50/ms | ZYX/Kuzu |
|---|---|---:|---:|---:|
| 标签过滤 | medium | 341.934 | 5.278 | 64.78 |
| 全节点属性过滤 | medium | 490.376 | 1.734 | 282.78 |
| 单键分组 | medium | 339.292 | 9.884 | 34.33 |
| 多键分组 | medium | 409.214 | 14.478 | 28.26 |
| 标签过滤 | holdout | 228.068 | 2.991 | 76.25 |
| 全节点属性过滤 | holdout | 363.803 | 1.245 | 292.32 |
| 单键分组 | holdout | 230.327 | 4.762 | 48.37 |
| 多键分组 | holdout | 317.227 | 8.578 | 36.98 |

证据：receipt.json、initial-summary.json、三个模块测试 JSON，以及 initial-benchmark.py 与两个编译/链接 recipe。原始样本在 benchmarks/architecture-design/results/20261024-work-scan/，逐文件 SHA-256 见 receipt；不会改写旧证据。

剩余：BUILDING 中成员目录/计数/字段根的原子发布、完整且有界的 integrity、全库空间准入、认证持久扫描消费、更新后读取与重开、其余生命周期和 S2–S5 验收。当前实现不支持公开打开候选库，不能宣称 5 倍收益或 100%。
