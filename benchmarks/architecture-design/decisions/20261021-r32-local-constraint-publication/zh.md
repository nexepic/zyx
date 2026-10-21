### 2026-10-21 R3.2／R6.1：非空与类型约束的最终字段验证

证据：`benchmarks/architecture-design/decisions/20261021-r32-local-constraint-publication/`。字段schema证明现在认证有界NOT NULL／PROPERTY TYPE定义及其规范name／semantic方向，不再拒绝所有约束。Node标签和Edge类型分别匹配约束作用域，按最终字段值调用现有验证器共享的borrowed-value规则，不复制整棵属性树。违规在物理写入前拒绝；合法更新与标量／复合索引共同发布。UNIQUE／NODE KEY支持树、VECTOR及带schema的owner删除仍保持保护，继续待办。

新增非空缺失／显式NULL拒绝、整数与浮点严格区分、类型约束允许缺失、Edge类型作用域及旧pin／索引一致性用例。抽出本地规则时曾误要求已分配的持久约束ID，导致普通创建的草稿定义被拒；现保留原先草稿语义，并加直接回归，初始失败日志保留。扩大到50个相关模块，成熟多组空间用例仍使用独立冻结证据；没有全库单测或性能基准。**总计13/24（54.2%），验收增长0，不提交、不推送。**
