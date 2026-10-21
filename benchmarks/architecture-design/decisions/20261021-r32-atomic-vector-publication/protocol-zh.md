# 显式原生字段／向量联合发布

范围：R3.2／R6.1正确性原型，仅实验格式32；默认格式31保留ROW。没有生产或性能准入。

## 权限与顺序

1. 先暂存有界字段、map或owner删除；读取组件与source pin在取得元数据写锁前创建。
2. `coordinateHybridPropertyVectors`校验同域Vector组件及有限参数；准备开始前设置pending栅栏。此后失败阻断commit，移动保留栅栏，需rollback释放。
3. 每次仅解码一个owner的选定旧值，不解码无关字段。先应用ROW相等规则，再做BFloat16／metric差分，保留有符号零no-op及严格属性语义。
4. 独立临时native事务按同一源代数调用既有bootstrap／delta规划器，仅转移派生工作。原字段事务的暂存保护在回调期间也保持。
5. commit在自己的栈上创建`NativeHybridVectorPublication`。构造函数仅供事务／元数据实现使用，绑定metadata实例、实际derived对象、已计费不可变字段请求及完整语义差分；不持久化，也不交给外部调用方。
6. 元数据层拒绝借此证明附加普通Entity／ROW／LargeValue请求、私有artifact及deferred／early planner；派生tree必须属于本次Vector bootstrap／delta。既有typed Vector writer继续校验映射、Manifest及分配身份。
7. 字段prepare认证schema／alias、真实owner／标签范围、旧／最终选定值、约束及全部Vector语义，每项语义都必须被消费。scalar／支持树／统计仍由该阶段准备。
8. 字段、目录、scalar、约束、统计、独立LargeValue与Vector正向／反向／delta映射、Raw、Manifest、退役共用一个MetadataWriteBatch，保留槽位和分配身份相互避让。只有外层最终publish，结果确定后才记录复用提示。

传入Vector组件须存活到commit结束，与普通Vector暂存一致。可写metadata上的只读adapter用于避免规划期间allocator reconcile，不授予物理只读backend写权限。

## 故障与生命周期

| 边界 | 结果 |
| --- | --- |
| 准备前参数／域错误 | 不暂存、不发布 |
| 准备中值／操作数／额度错误 | pending阻断commit；rollback释放缓冲及计费 |
| commit准备前确定取消 | 完整active计划保留，取消解除后可重试 |
| 后续commit准备失败 | 不能重试可能已被消费的部分计划 |
| 约束违反 | generation／IO epoch不变，字段及映射保持 |
| inactive Superblock半写 | 重开选择旧的完整字段／Vector版本 |
| inactive Superblock完整写 | 重开选择新的完整字段／Vector版本 |
| 移动 | 账户、pending、语义计划及typed组件绑定跟随事务 |
| 旧pin | 旧字段及原始Raw记录继续可读 |
| ROW有符号零no-op | generation、VectorId及私有IO均不变 |

## 限制与剩余准入

字段仍限64变化单元／owner及原1MiB账户；联合语义最多128项、schema定义4096、Raw页单元128、配置overlay4096项、delta页单元128。它们约束单次工作／控制状态，不是全图规模上限；账户可在达到上限前先拒绝。

PMR容器直接计费；复用普通Vector规划器前保留其非托管前沿的保守额度：每个受影响index64KiB、owner／维度缓冲及每个既有delta项512B。真实64-owner批次和512维案例在原1MiB账户内完成并退款；不据此证明进程RSS。已积累的overlay可超出该workspace而需要维护，成熟稳态／生产资源门槛仍待办。

当前定向证据：候选13项、默认／普通Vector／查询51项通过。此前58模块候选收据仍保留LargeValueStore读取夹具失败；修正绑定、重复键公开异常合同和有符号零实现后只跑受影响组，不伪造新的宽回归全绿计数。成功联合路径另执行完整integrity。

普通native拓扑／公有writer路由仍受保护，完整空间／资源／维护准入、剩余R5与R7未完成。没有性能计时或全库单测。验收仍14/24（58.3%），不提交、不推送。
