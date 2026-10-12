# S0 工作负载、资源与归因合同

日期：2026-10-11。任务：S0.2、S0.4。状态：合同已预登记；完整执行器和语义观察仍待补齐，未验收。进度仅由 optimization.mdx 维护。

## 决策

同目录 contract.json 为机器可读合同，涵盖 13 类工作负载，逐项规定输入、操作、独立预期结果、计时边界和指标。`existing` 表示已有部分 benchmark 入口，不表示所列全部变体或语义观察已实现。`pending` 表示尚需新增执行入口。合同检查成功不证明性能、正确性或阶段验收通过。

正式对照保持 Docker Linux ARM64、容器 6 GiB、1 CPU、引擎 1 线程、查询物化预算 64 MiB。Colima 测量时 8 GiB/2 核，完成后恢复原配置。专门预算压力实验使用 4 MiB 查询预算；并发实验明确列出 1/4 客户端，不与单查询结果混合。保持同步持久化保证；写入从相同逻辑状态恢复，恢复耗时单列，计时到持久化确认。不同格式按相同数据重建，记录实际格式版本，不假定旧文件可被新格式读取。

训练数据使用现有 medium、seed 42；留出使用 seed 314159、73000 用户/87000 帖子/3000 标签、度数 7 及未用于调优的选择率。新增留出生成、异构属性、宽行大值、长快照和稳态执行器尚未实现。不得将现有环形五邻居数据宣称为超级节点或高异构覆盖。

主性能实验预登记为扫描、聚合和排序；保护点读、图遍历、宽行大值、小写及混合负载。单项性能门槛沿用至少 10% 可重复端到端改善，保护场景稳定退化不超过 5%，不提高内存上限。结构性实验额外报告 2 倍/10 倍里程碑与剩余 Kuzu 差距，作为观测标尺，不承诺达到、不替代逐项准入。

## 统计和结果校验

三独立进程、各三次预热/十次测量；control/current/Kuzu 交错。汇总独立进程 P50 的中位数，保留进程范围。十样本 P95 只作描述。接近门槛或控制波动大时最多补两进程、每进程一百样本；尾延迟敏感稳态实验至少每进程一千观察。单场景最多三十分钟，仍不确定则记录不确定，禁止测到通过为止。

校验实际 ID、类型、NULL/缺失、重复次数及必要顺序；聚合比较每组键和值，Top-K 比较排序值和合法边界。未规定的同分顺序不强加要求。计时内完整消费输出，独立结果比较在计时外执行。当前 read baseline 只有标量/行数观察，不能替代以上完整校验。Kuzu 不支持的语义记 N/A，不采用弱化语义的替代操作。

## 归因与反证

分别采集计划/缓存、快照建立、目录定位、页访问/校验、属性解码、表达式、行与容器分配、聚合/排序、输出拥有化成本。配对计数包括页/字节、实体数、边回查次数、输入输出行数、分配与内存；CPU 样本与正式无 instrumentation 计时分开，不累加嵌套 timer。

先对照存储单字段读取与完整查询，再在同一存储输入上比较通用/类型化执行，分离 A1 与 A3 收益。Top-K 与完整排序维持相同排序语义。邻接候选必须在一致性协议获准后比较，不能通过跳过当前必需校验制造收益。机制指标改善但端到端收益不确定时不宣布提速。

## 验证和剩余交付

仓库根执行：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/runner/optimization_contract.py benchmarks/architecture-design/decisions/20261011-s0-workload-contract/contract.json
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/tests/test_optimization_contract.py
```

合同结构检查通过；9 项模块测试通过，覆盖缺失工作负载族、重复 ID、非法资源上限、缩减采样、缺失 oracle/计时、缺失执行入口与 malformed 容器。不安装宿主依赖，不创建数据库，不启动 Docker，不执行全量测试或提交推送。

S0.2 剩余：补齐各变体的数据/操作执行与完整语义观察；完成合同预执行，冻结实际查询、参数、索引、数据 manifest 和格式身份。S0.3 剩余：按完整合同补测 control/current/Kuzu。S0.4 剩余：运行分层计数/CPU 采样及反证对照，报告时间归属和不确定性。清理按合同仅处理本轮归属产物，保留有界报告和控制文件。

## 完整结果校验接入进展

`runner.semantic_oracle` 已实现真实 group-by 的键/整数计数校验与 Top-K ID/排序校验。Top-K 从独立 CSV 分数判断合法结果，允许同分任意顺序与边界同分任选，拒绝重复 ID、未知 ID、错误选取和排序；本版只适用于唯一 ID、有限分数输入，不宣称覆盖 NULL 排序或异构值语义。

`runner.baseline --require-full-results` 要求每个测量迭代包含完整 rows，生成 `full-result-correctness.json`；缺失或错误输出使运行返回失败。默认模式仍保持原读取计时合同，不把旧样本补标为完整校验。原生 ZYX/Kuzu 的 rows 输出已通过显式观察模式接入，仍需新 Docker 二进制进行真实严格预检；不能用计时外另一次查询替代测量迭代的输出。保留实际输出所增加的计时/内存成本须在新的合同中披露，并对双方一致处理。

11 项 oracle 测试和 3 项基线接入测试通过；后者验证行数正确但值错误仍失败、每轮证据导出、临时输入清理，以及非法采样参数在创建输出/引擎前失败。测试仅使用标准库和小型生成数据，无宿主依赖安装或 Docker 资源。

```bash
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/tests/test_semantic_oracle.py
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/tests/test_baseline_semantics.py
```


## 双端实际行数据输出

原生基准新增 `--emit-result-rows`，对真实 group-by 和 Top-K 在结果消费时保留实际值，停止计时后序列化。Kuzu 在相同显式模式捕获实际游标行。`runner.baseline --require-full-results` 通过 runner 同时开启双端观察；默认维持行数模式。每次测量清理预热/上一轮输出；原生序列化保留字符串/整数类型，非法值在写出部分 JSON 前失败。runner 记录观察合同，对照来源门禁拒绝将完整行观察与旧行数模式混用。

验证：C++20 编译检查通过；使用当前 benchmark 源码、当前 PerfTrace.cpp 与已有 GoogleTest 缓存依赖编译独立模块执行器，运行 3 项 `CompareBenchmarkObserverTest.*` 测试。6 项适配/对照来源、11 项 oracle、3 项基线接入测试通过，共 23 项定向测试。本次未执行实际数据库查询路径或 Kuzu 库，游标测试使用确定性替身；下一步构建新 Docker 二进制并进行真实双端严格 smoke 预检。未全量测试、提交或推送；临时检查二进制/源码已清理。日志：`benchmarks/architecture-design/results/20261011-result-observers/tests.json`。


## 最新执行顺序

用户要求优化实现完成后再运行性能 benchmark。2026-10-11 严格预检镜像构建已在运行性能测试之前取消；Docker 资源为零，Colima 恢复 2 核/2 GiB，VM 临时 DNS/代理已移除。当前继续设计、实现及模块正确性测试；双端计时预检、归因和性能准入对照留在最终窗口，不阻塞代码工作，也不提前计为性能验收。合同检查仍通过 9 项测试。取消证据：`benchmarks/architecture-design/results/20261011-strict-preflight/cancellation.json`。
