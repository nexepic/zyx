
### 2026-10-21 R3.2／R6.1：字段与向量 bootstrap／delta 同批原子发布

协议及原始证据：`benchmarks/architecture-design/decisions/20261021-r32-atomic-vector-publication/`。显式字段／map／owner事务可调用`coordinateHybridPropertyVectors`，复用已有BFloat16／metric及Vector bootstrap／delta规划。向量暂存放在独立临时事务中，核对同一源后仅转移其派生工作；原字段事务的暂存保护始终保留，取消回调不能附加无关scalar操作。失败暂存阻断commit，移动保留状态及账户；确定在发布前的取消仍可重试。

commit内部生成仅能绑定当前metadata和实际derived对象的不可构造证明；元数据层限制其根命名空间及工作形状，并用真实旧／最终选定字段重新核对全部向量语义。字段、标量／约束、统计、独立LargeValue、Node／反向／delta映射、Raw／Manifest及退役共用一个MetadataWriteBatch和一次最终publish。没有分次提交或新持久格式。ROW相等规则先于向量字节比较；回归先复现+0／-0错误触发差分，修复后整个no-op不改变generation／VectorId／IO epoch。

当前13项定向用例通过：首次向量发布、真实替换、最后字段删除／重建、仅INSERT的最后delta退役并保留base、索引owner删除／统计、旧pin／原Raw、真正跨对象移动、非法值及64KiB拒绝／退款、取消重试、回调暂存保护、两种A/B中断重开、64-owner批次在原1MiB账户内完成、512维独立大值及UNIQUE共存／重复值写前拒绝；成功路径均做完整integrity。默认／普通向量及查询对照51项通过。此前58相关模块默认934通过，候选1276通过／1条件跳过／1读取夹具失败；补齐LargeValueStore绑定后只重跑受影响组，未冒称候选宽回归全绿。初始编译／夹具／异常合同错误和有符号零真实失败均保留。

1MiB包括已计费容器及保守的向量工作前沿，不是进程RSS准入；较大既有delta可因额度拒绝，仍需稳态维护／完整资源验证。普通native拓扑writer、R3完整生产空间门槛、R5生产／规划与R7尚未完成。无全库单测或中途性能基准。**总计14/24（58.3%），验收增长0，不提交、不推送。**
