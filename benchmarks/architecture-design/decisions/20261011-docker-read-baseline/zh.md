# Docker 读取性能基线（2026-10-11）

状态：三轮读取评估已完成，S0.1 来源冻结已验收。优化进度统一维护在 optimization.mdx。

用户指定 Docker/Colima 隔离评估。实际环境为 Apple M4 Pro 宿主机、Linux ARM64 容器，GCC 13.3.0、C++20 Release，Kuzu 0.11.3。正式测量采用 Colima 8 GiB、容器 6 GiB/单核、引擎单线程、seed 42、medium 225000 节点与 860000 边、warm 模式、3 次预热及 10 次测量，共 3 个独立进程轮次。奇数轮 ZYX 先运行，偶数轮 Kuzu 先运行。关闭 ZYX profile；数据库导入、复制、打开关闭不混入查询计时。

原 2 GiB Colima/1800 MiB 容器条件触发内核 global OOM，发生于 ZYX 导入阶段。Python runner 曾额外持有整份生成图，已在写出 CSV 后释放，并由弱引用测试核验。正式轮次使用独立资源合同，不能将扩容解释为数据库优化收益。查询物化预算保持源码默认的 64 MiB。

实施前修复 EntityPage.hpp 缺失 functional 标准头文件，保持数据格式及查询行为不变。容器内 567 个编译源码 hash 与工作树一致。基准二进制 SHA-256 为 41b5c9f3f26d94a579a421feeffea34c9112c217efd7674e1216be1b8593cf08。源码、CMake/Ninja 配置、依赖版本、运行库及数据文件 hash 保存在原始证据目录。

18 个场景覆盖扫描过滤、关系过滤、一跳/两跳、DISTINCT、真实 group-by、Top-K、点查、二级索引及投影读取。Kuzu 不支持标量二级索引的两个场景明确记 N/A。标量和投影行数与 CSV 独立计算结果核对；不以该检查替代投影值、类型、顺序、重复行及完整语义验收。

统计采用各独立进程 P50 的中位数，并保留进程间范围及每进程原始样本。每进程 10 个样本的 P95 仅为描述性数据，不能视为准确尾延迟估计。资源高水位包含导入、benchmark 输入、缓存及校验；不是查询专属内存。历史 macOS 汇总位于 optimization.mdx，但本工作树没有历史原始样本，因此不作严格前后回归判断。

原始证据：benchmarks/architecture-design/results/20261011-docker-baseline/。复现入口：runner.baseline 与 runner.baseline_report。已完成 1020 个有效计时样本及对应结果观察，正式轮次标量/行数核验通过。


| Scale | Workload | ZYX P50 | Kuzu P50 | ZYX/Kuzu |
|---|---|---:|---:|---:|
| medium | aggregation_count_by_group | 301.455000 | 2.165042 | 139.237484 |
| medium | aggregation_group_by | 299.879000 | 1.990833 | 150.629912 |
| medium | all_nodes_property_filter | 355.153000 | 0.430791 | 824.420659 |
| medium | batch_node_fetch_100 | 1.746420 | 0.240416 | 7.264159 |
| medium | label_multi_property_filter | 233.057000 | 1.359917 | 171.375900 |
| medium | label_scan_filter | 208.899000 | 1.293667 | 161.478186 |
| medium | one_hop_expand | 1.458960 | 0.228875 | 6.374484 |
| medium | one_hop_fetch_neighbor_ids | 1.649420 | 0.304583 | 5.415338 |
| medium | one_hop_fetch_neighbor_records | 2.106960 | 0.373375 | 5.643013 |
| medium | point_edge_fetch_by_endpoints | 1.953920 | 0.164208 | 11.899055 |
| medium | point_lookup_indexed | 1.084670 | 0.101667 | 10.668850 |
| medium | point_node_fetch_by_id | 1.751210 | 0.089208 | 19.630638 |
| medium | property_equality_indexed | 133.227000 | N/A | N/A |
| medium | property_range_indexed | 828.054000 | N/A | N/A |
| medium | relationship_property_filter | 630.077000 | 5.273500 | 119.479852 |
| medium | relationship_type_scan | 206.960000 | 2.988792 | 69.245367 |
| medium | topk_property_sort | 495.441000 | 1.955875 | 253.309133 |
| medium | two_hop_expand | 1.702670 | 0.477834 | 3.563309 |

89 项相关模块测试通过；未执行全量单元测试，未提交或推送。Docker 镜像、容器、卷和构建缓存均为 0；生成数据及数据库已删除。Colima 已恢复 2 核/2 GiB，临时 DNS/代理配置已移除。保留约 11.4 MiB 报告及压缩控制文件。TRIM 报告 7.6 GiB，不等于实测宿主磁盘释放量。扫描、聚合、排序为后续优化候选；ZYX C++ 与 Kuzu Python/Cypher 入口存在差异。复现使用 runner.baseline --help 和 runner.baseline_report，在新目录按记录的资源合同执行三轮。

- `baseline-summary.json` SHA-256: `b7020734a2a9aaa0839e222868268f2f87cf82777edfe051dee571aa341437c0`

- `control/source.tar.gz` SHA-256: `391fec9aca8f36c667ea23f02265b357fb2eb5f64ec12b6f9eaaa75e95282a41`

- `control/zyx-compare-bench.gz` SHA-256: `2c6defa925fd99f0853f9a31281cbb36866243cf7b58de8d0081d4df27cf3954`
