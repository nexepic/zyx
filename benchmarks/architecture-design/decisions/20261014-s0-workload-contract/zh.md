# S0.2 可执行工作负载与资源合同

日期：2026-10-14。替代 2026-10-11 尚待交付的 S0.2 合同，保留历史证据。本项验收工作负载/资源合同，不验收 S0.3 基线、S0.4 归因、正式规模运行或性能结果。

## 冻结输入及执行入口

contract.json 保留 13 类负载、资源上限、计时边界、统计、清理及独立 oracle，补精确 smoke/contract 规模、已实现变体及能力分档。`runner.workload_preflight` 通过独立无计时 C++ worker 驱动真实原生引擎，通过 Python API 驱动真实 Kuzu。完整类型结果对输入推导模型验证，不能只靠行数。map 区分缺失键和显式 NULL，整数/布尔/字符串不同，保留重复数，要求排序时比较序列。原生 NULL 属性赋值沿用删除语义，同时检查标量 NULL 及缺失投影。大 JSON 输入字符串按认证 UTF-8 blob 单次保留，完整值先校验再保存证据。

medium-manifest.json/holdout-manifest.json 来自真实生成 CSV，冻结每文件字节/SHA-256、schema、seed42/314159；训练 225000 节点/860000 边，holdout 163000 节点/772000 边（73000 users、87000 posts、3000 tags、degree7）。明确原输入为 ring，不称其为 hub；另有可执行拓扑 fixture 含共享 hub、平行边、自环和两跳。dataset.generate 已实现 holdout profile 及独立 CLI 默认 seed，未用 holdout 结果调优。

预检 registration 在查询前冻结源码/二进制 hash、规模、资源及实际 envelope/Superblock 身份。source-format-freeze.json 另记录源码局部 codec 常量，不假定不同 control 格式一致。trace 保存实际查询/参数、预期值、结果 hash、顺序及预期错误。数据库只在自有临时目录创建，关闭后清理，证据单独保留；不使用用户文件，不以当前源码替换或重建历史冻结 control。

## 可执行覆盖

| 类别 | 已实现检查及变体 |
|---|---|
| 点/扫描 | 确定性随机 ID、标量/完整 owner 路径、.003/.37/1 选择率、索引范围与 DDL 转换 |
| 遍历 | 一/两跳、邻居完整标量投影、hub、平行重复边、自环及边属性 |
| 聚合/排序 | 八组、90% 倾斜、独有键分组、DISTINCT、完整排序及 K=1/100/10000、tie-breaker、NULL/缺失分数 |
| 参数 | 100 组变化 ID/country/range，缓存/清空逻辑计划、创建/删除索引 |
| 动态 | 交错 1/8/64 键、singleton 键、整数/字符串变化、remove/NULL 删除、完整 map/缺失查找及旧快照 |
| 宽/大值 | 8/128 字段 ×32 KiB/1 MiB body、单字段更新保留全部无关字段/body、点/完整读取 |
| 写入 | 1/100/1000 批次、create/set/remove/不变 set/delete、完整后状态及重开；遍历/删除另覆盖边 |
| 混合 | 90% 读/10% 写、独立状态模型、四个真实原生线程/独立引擎、有界全新事务重试；另验证确定性四写者冲突 |
| 快照 | 旧视图跨更新及 detach-delete，释放/重开/完整性 |
| 维护 | 十个 churn/回收窗口、有限原生扫描/退役上限、file/RSS/read/write 观察、重开/完整性及精确空活跃状态 |
| 预算 | sort/hash 物化失败、取消、逻辑/物理只读超预算失败、重开/完整性、不返回部分成功 |

smoke 限制总体数据，仍实际覆盖 128 字段/1 MiB body 及 1000 写批次。contract 模式已实现 10000 动态 owner、100000 独有聚合键、每变体 1000 宽实体、100000 单客户端操作、四客户端合计 100000 操作、10000 固定快照更新及十个 10000-owner churn 窗口。正式压力为 4 MiB，smoke 为独立 4 KiB 探针。正式规模扫测仍在最终窗口执行，smoke 不证明其耗时、资源平台或性能。

原生 worker 是 storage-neutral QueryEngine/事务及显式原生管理的正确性入口，不是公开 Driver ABI 性能入口。四客户端共享一个后端，但全局资源接入仍属 S5；字节/RSS 观察不证明进程治理。回收上限是有限 harness 参数，不代表 A6 生产默认已验收。保留文件平台观察，不据此宣称稳态准入。

Kuzu 使用固定 schema、主键 ID 索引、每连接一执行线程及 64 MiB raw buffer pool。等价点/扫描/遍历/聚合/排序/参数/宽实体/写/混合/快照/churn 消费真实完整 cursor；写后真实关闭/重开校验，另验证四个真实 Kuzu 客户端线程及独立旧读事务。原生二级索引 DDL、整图 fence 错误、owner/generation 回收和查询计费预算属分开能力案例；任意动态缺失键身份及原生预算协议对 Kuzu 标记 N/A，不弱化后冒充等价。Linux x86 smoke 不产生 ZYX/Kuzu 倍率或 ARM64 基线结论。

## 资源与最终计量合同

正式对照仍为 Docker Linux ARM64、容器 6 GiB/无额外 swap、一 CPU/一引擎线程、原生查询物化 64 MiB；一/四客户端场景分开。保留同步持久化，计时至 durable acknowledgment。每个写入计时样本前恢复相同不可变逻辑状态，恢复耗时另记。不兼容格式从相同输入重建。不宣称 Kuzu 内部查询预算等于原生计费预算，统一外部容器上限并明确分类不兼容预算案例。

三独立进程、三预热、十样本，轮换 control/current/Kuzu；聚合进程 P50 中位数并保留范围，十样本 P95 仅描述。接近门槛/控制波动时最多加两个百样本进程，长尾稳态需每进程千观察。每 case30 分钟停止，保留不确定结果。仍要求 10% 可复现收益、5% 稳定退化上限、不提升内存上限；机制计数不能替代端到端收益。S0.4 单独归因规划/缓存、快照、目录/页/校验/解码、表达式、分配/行及拥有输出。双引擎统一披露 observer 成本，之后重新跑无计时查询不能验证被计时输出。

## 验证与限制

预检使用真实最新构建原生代码及 Kuzu0.11.3，不是模拟 cursor。对应本地 results 保留原始构建/测试/预检日志、registration、trace/input blob、二进制/源码 hash 和 report。十五项 Python 合同/oracle 测试通过；真实预检发现的转义属性键修复有 292 项选定原生/解析测试。最终预检数量/身份见验收记录。未跑全量或性能；S0.3/S0.4、完整正式规模矩阵、全局运行时资源接入及最终性能门禁均未验收。

仓库根目录执行：

```bash
cmake --build buildDir --target zyx_workload_contract_worker
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.workload_preflight --native-worker buildDir/zyx-workload-contract-worker --output /tmp/zyx-private-preflight --kuzu
python3 benchmarks/graphdb-comparison/runner/optimization_contract.py benchmarks/architecture-design/decisions/20261014-s0-workload-contract/contract.json
```

`--kuzu` 需要真实库，每个输出目录必须新建。`--profile contract` 提供正式规模模式，按当前用户顺序留到最终执行窗口。
