### 2026-10-21 R3.2：属性无关的 SystemCatalog 与真实字段修改

证据：`benchmarks/architecture-design/decisions/20261021-r32-property-independent-catalog/`。字段修改不再仅因SystemCatalog非空而拒绝：在相同writer／pin下认证有界完整目录控制census，token、配置、图／token计数及label/type索引可保持原样。property／composite／vector索引定义、约束及任何带目录的owner删除仍拒绝，未知控制命名空间、孤立／非规范索引方向、非法配置／统计标记及不完整census不能绕过协调。census暂限4096目录记录及原1MiB账户，未宣称任意规模schema或完整schema写支持。

同一LogicalPlanReadRuntime新增真实修改对照：公开backend建立64节点／64多重边但不建property索引，保留目录配置；转换后修改再恢复age字段，逐项验证范围过滤、count／sum、字符串／缺失NULL、OPTIONAL、多重边和TopK，SC根／图计数／旧pin字段值保持。另有实际property索引、NOT NULL约束、owner删除和合法BTree校验的未知控制键拒绝用例。拒绝前的首次可写重开可能执行必要pool恢复；夹具先用已验证的只改高水位租约完成恢复，再断言字段拒绝没有私有IO。初版混淆恢复IO与字段写IO的失败记录保留，未放宽保护来通过断言。

最终44个相关模块，排除已独立冻结且未受schema变更影响的成熟多组空间用例：默认586通过，候选875通过／1条件跳过；本轮查询对照7项均通过。未跑全量单测或中间性能基准。完整属性索引／约束／拓扑协调及R5／R7继续待办。**总计13/24（54.2%），验收增长0，不提交、不推送。**
