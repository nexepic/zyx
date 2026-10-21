
### 2026-10-21 R3.2：私有准备与最终元数据发布分离

证据：`benchmarks/architecture-design/decisions/20261021-r32-shared-batch-preparation/`。原生字段入口继续在取写锁前创建读取组件并持有已认证source pin；私有prepare函数现在接受现有MetadataWriteBatch和placement，核对同一源及尚未替换的descriptor，在该批次中准备字段、目录、索引、统计和退役，不执行最终publish。外层保留source pin及placement到单次publish结果确定后，再记录已验证的复用提示。no-op仍不发布。

这是后续VECTOR同批协调所需的内部结构，不是已经接通向量增量／bootstrap，也未放开普通native拓扑writer。拆分后的字段／索引／删除／map／大值／journal及同执行器相关验证：默认32通过、候选128通过；此前57模块与向量缺陷回归证据独立保留，没有重复声称全跑。无全库单测或性能基准，**总计14/24（58.3%），验收增长0，不提交、不推送。**
