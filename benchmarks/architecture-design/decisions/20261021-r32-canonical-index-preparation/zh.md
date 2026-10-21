### 2026-10-21 R3.2／R6.1：canonical 索引差分的只读准备与所有权

证据：`benchmarks/architecture-design/decisions/20261021-r32-canonical-index-preparation/`。为后续原子索引协调抽出只读borrowed-key差分visitor，现有事务`stagePropertyDelta`直接复用同一实现，保留scalar／composite、部分索引成员、canonical key无变化跳过及错误传播。visitor即使没有definition／没有差分也要求当前metadata域的注册pin和有效callback；相同DatabaseId／generation的外域pin不能通过缓存绕过。callback只在调用期间借用key，不获得发布权限。

单值／复合key准备不再复制整棵`PropertyValue`载荷：最多64个指针的栈上视图复用同一编码循环，编码字节、正负零规范化、键大小／组件数边界不变；仅超长键返回无加速项，非有限值、空引用、非法EntityId等错误仍抛出。根存在性缓存的node／bucket改为保留owner的PMR分配，离开构造scope仍计费，重复命中不重复增长，销毁退款。

新增6个准备／codec／账户回归，并连同现有物理索引生命周期验证，两种格式各96项、5组通过；重建47个选定模块的二进制，没有运行全库单测，上一轮44模块回归证据独立保留。本轮没有开放原生property/composite索引发布，相关保护仍在，下一步仍需真实索引页／根与字段的共同准备和原子发布。没有性能基准或生产准入结论。**总计13/24（54.2%），验收增长0，不提交、不推送。**
