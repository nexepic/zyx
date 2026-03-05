# 查询引擎

## 解析器

基于 ANTLR4 的 Cypher 解析器，提供完整的语言支持。

## 规划器

将解析的 Cypher 转换为逻辑计划，并进行基于规则的优化。

## 执行器

使用各种运算符执行物理计划：
- 扫描运算符: NodeScan, EdgeScan
- 修改运算符: CreateNode, MergeNode, Delete
- 查询运算符: Filter, Sort, Project
- 特殊运算符: VectorSearch, TrainVectorIndex
