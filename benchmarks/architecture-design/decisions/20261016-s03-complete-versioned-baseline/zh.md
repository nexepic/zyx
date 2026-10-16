# S0.3：同条件、分版本基线与完整场景结果

本交付完成基线资料及已登记完整场景的执行结果记录；**不等于所有场景成功、不等于 S6.2 综合性能通过，也不证明超过 Kuzu**。Native 的“current”标签严格指冻结源 `6a735df8`；生产修复源 `03436b7a` 只有独立的正确性复验及取消的 O6 预热记录，不能把旧源数字标成最新修复版。

## 来源、资源与计时范围

原登记、冻结 worker/harness hash、准备失败和续跑协议都随本目录保存。控制版本保持不可变；只恢复因外部暂停而中断的聚合案例，不重跑已成功的读样本。实际格式与每案例 worker/runner hash 在压缩证据中。复制准备输入的 prefix/suffix 来源如实保留，未声称从空库完成最终版本导入，导入耗时也不混入读延迟。

同机单 CPU affinity、单查询线程、每进程 6 GiB 地址空间与 2 GiB 单文件上限；Native driver/worker 各自受限，不能冒称合并 6 GiB cgroup。分别报告其 RSS，不把不同时刻峰值之和当作实测并发峰值；Kuzu 64 MiB buffer pool / 2 GiB mapping，Native QUERY 64 MiB、总域和类别上限按冻结实现记录，内部私有写工作区变化在先前修复证据披露。该环境不是 6 GiB RSS/cgroup 等价实验；实际主机 cgroup、CPU、工具链和依赖见 registration。Native 默认语句时限为 30 秒，Kuzu 合同 helper 使用剩余家族时限（至多 30 分钟）；该差异明确保留，不把超时/失败差异当成同截止时间的性能胜负。默认同步及确认协议不降低。缓存为热态，不宣称清空 OS cache 或冷态结果。

正式读部分：20 个可比较工作负载，24 个独立 profile 进程、120 个进程×工作负载分布，1,200 个有效样本；每查询每进程 3 次预热、10 次拥有输出测量。不能把 120 个分布冒称 120 个独立进程。P50 汇总为独立进程 P50 的中位数；保留进程区间，10 样本 P95 只作描述，不通过尾延迟门禁。

其余完整合同场景按预先登记的独立家族策略执行，逐值/类型/NULL/缺失/顺序/重复核对，保留原子性及故障结果。除维护的预热/测量窗口外，它们是完整规模的语义与操作成本观察，不是重复热态查询延迟或保护场景通过证明；不补造 3 次预热/10 次重复数据。Native IPC 计时含序列化、解析、校验与 trace 保留，Kuzu Python cursor 定时含 execute/drain/close 和校验、在 trace 保留之前结束；这些观察开销并不完全相同；C++ 读入口和 Kuzu Cypher 的绑定/计划成本不同，不能把这些不同层的 timer 相加或混用。

## 完整合同覆盖与失败

117 个登记槽位覆盖 13 家族、三方、三轮；实际执行进程数与未执行/N/A 分开。只把完整 oracle 成功的案例写作 completed。不可测的原版准备、确定性失败和时限结果保留为 N/A/不确定，不填零，不据此推导提速。后续同源同场景的失败轮次依预登记策略引用原失败，而不冒称独立执行。

| 家族 | control | 冻结 current6a | Kuzu |
|---|---|---|---|
| aggregate | failed=1, unmeasurable_previous_failure=2 | completed=3 | completed=3 |
| budget | completed=3 | failed=1, unmeasurable_previous_failure=2 | unsupported_NA=3 |
| dynamic | inconclusive_timeout=1, unmeasurable_previous_failure=2 | inconclusive_timeout=1, unmeasurable_previous_failure=2 | unsupported_NA=3 |
| maintenance | failed=1, unmeasurable_previous_failure=2 | failed=1, unmeasurable_previous_failure=2 | completed=3 |
| mixed | failed=1, unmeasurable_previous_failure=2 | completed=3 | failed=1, unmeasurable_previous_failure=2 |
| parameters | completed=3 | completed=3 | completed=3 |
| point | completed=3 | completed=3 | completed=3 |
| scan | completed=3 | completed=3 | completed=3 |
| snapshot | completed=3 | completed=3 | failed=1, unmeasurable_previous_failure=2 |
| sort | completed=3 | completed=3 | completed=3 |
| traversal | completed=3 | completed=3 | completed=3 |
| wide_large | inconclusive_timeout=1, unmeasurable_previous_failure=2 | failed=1, unmeasurable_previous_failure=2 | failed=1, unmeasurable_previous_failure=2 |
| write | completed=3 | completed=3 | completed=3 |

实际执行独立合同进程：87. 各状态计数：{"failed": 9, "completed": 75, "unmeasurable_previous_failure": 24, "inconclusive_timeout": 3, "unsupported_NA": 6}.

宽行／大值的冻结 current 在 offset 764542976 写入时报告 Input/output error，原因未确定，不能擅自归为文件上限或引擎格式错误；Kuzu 在 64 MiB pool 下报告无法分配缓冲页；control 到达 30 分钟案例时限。原始错误均保留，不重跑到成功。动态属性两种 Native 版本也均达到案例时限，不当作完整正确性或性能通过。

100,000 组 Native 拥有输出达到固定额度时，完整查询仍记 N/A；分区逐值核对不能改写成该查询成功。Kuzu 二级标量索引不支持的真实能力记录保留；原无限 mapping probe 错误在受影响案例尚无有效样本时修正，失败与唯一重试记录均保留。动态字段缺失语义和 Native fence/回收接口不伪造 Kuzu 等价实现。

原 source6a 维护的 Native 隐式完整删除结果与 Kuzu 无返回语义不同，且 Native 在可选缓存处失败；两者不构成可比较的稳态延迟。修复版对齐显式标量返回的 O6 窗口在两个预热后因本轮范围收敛主动取消，没有测量样本，没有稳态通过。冻结 current 的预算家族在重开后的 INTEGRITY 步骤发生 hierarchical resource admission denied，整个家族保持失败，不因之前拒绝/取消检查符合预期而算通过。冻结 current 的混合读写三轮均完整完成；control 四客户端阶段的发布认证错误及 Kuzu 的 pool-full 错误保留，不能推导严格速度比例。多客户端保守 fence 与 Kuzu 事务行为差异单独记录。维护全局积压、旧案例的 Native peak RSS、设备物理 I/O 等未提供的量不填 0；旧 elapsed 没有 rss_scope 时仅能解释为 driver RSS。

## 正式读比较

以下均为进程 P50 中位数（ms）；倍数为 Native/Kuzu，越大表示 Native 越慢。完整进程 P50 区间、P95、样本和能力/失败状态见 [read-comparison.json](read-comparison.json)。原控制两种输入准备均失败，因此所有 control 提速列保持空，不能声称主要目标降低 10% 或保护门禁通过。

| 规模 | 工作负载 | Native ms | Kuzu ms | Native/Kuzu |
|---|---|---:|---:|---:|
| holdout | point_node_fetch_by_id | 63.756 | 0.230 | 277.672 |
| holdout | aggregation_count_by_group | 2316.490 | 2.622 | 883.401 |
| holdout | label_scan_filter | 2210.235 | 1.668 | 1325.086 |
| holdout | topk_property_sort | 2383.020 | 3.314 | 719.174 |
| holdout | two_hop_expand | 32.913 | 0.826 | 39.863 |
| medium | point_lookup_indexed | N/A | N/A | N/A |
| medium | property_equality_indexed | N/A | N/A | N/A |
| medium | property_range_indexed | N/A | N/A | N/A |
| medium | batch_node_fetch_100 | 41.792 | 0.485 | 86.226 |
| medium | one_hop_fetch_neighbor_ids | 36.685 | 0.621 | 59.088 |
| medium | one_hop_fetch_neighbor_records | 57.733 | 0.753 | 76.693 |
| medium | point_edge_fetch_by_endpoints | 42.844 | 0.449 | 95.317 |
| medium | point_node_fetch_by_id | 65.210 | 0.245 | 266.267 |
| medium | aggregation_count_by_group | 3203.310 | 3.629 | 882.585 |
| medium | aggregation_group_by | 3216.990 | 3.344 | 962.032 |
| medium | all_nodes_property_filter | 5961.120 | 0.900 | 6624.954 |
| medium | label_multi_property_filter | 3106.350 | 2.355 | 1319.273 |
| medium | label_scan_filter | 3055.525 | 2.142 | 1426.383 |
| medium | one_hop_expand | 21.136 | 0.389 | 54.317 |
| medium | relationship_property_filter | 7039.810 | 11.241 | 626.264 |
| medium | relationship_type_scan | 659.060 | 6.249 | 105.460 |
| medium | topk_property_sort | 3303.220 | 4.016 | 822.560 |
| medium | two_hop_expand | 27.739 | 0.874 | 31.746 |

Kuzu 目标尚未达到。S2.5、S3.5、S4.4、S5.5、S6.2 的生产/资源/端到端性能准入保持独立，不能以本基线资料交付替代。最终修复源尚未完成同版本全矩阵比较。CPU 时钟偏差与有界堆窗口见 [S0.4](../20261016-s04-hotspot-and-falsification/proof.json)；计数累计字节不等于 RSS 峰值或设备 I/O。

证据：[全家族状态](family-outcomes.json)、[压缩证据与逐文件 hash](full-family-files.json)、[正式读原始资料](../20261016-final-read-results/proof.json)、[修复源正确性](../20261016-cache-decline-delete-budget/proof.json)、[可选决策/文档](../20261016-s64-optional-decisions-and-docs/proof.json)。归档解压后的每个文件 hash 均已逐项验证。源码、输入和结果版本保持分离；本交付不新增算法、不调整预算或持久化承诺。

S0.3 基线资料交付已验收：**29/34（85.3%），本轮 5/5**。仅按本报告的版本、失败和计时范围验收，不改变尚未通过的性能/资源交付及 Kuzu 目标。
