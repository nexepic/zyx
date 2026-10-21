### 2026-10-20 R3.2：未知 State 发布保护与同执行器正确性对照

证据：`benchmarks/architecture-design/decisions/20261020-r32-state-query-parity/`。新回归真实复现未知State存在时字段／map修改仍能发布；原来只有owner删除检查。现将同一有界State census移到全部字段修改的共同发布入口，保留规范maintenance journal、拒绝未知派生State／未来代际／不完整census；拒绝前不写私有帧，不更改根／属性／State，原额度退款。未知State并未获得通用写入支持，schema／派生索引保护未移除。

增加同一LogicalPlanReadRuntime执行器的端到端正确性对照：通过公开backend建立schema／属性索引、64节点与64条多重边，用相同查询依次核对ROW、Node候选、Node＋Edge候选、两次逆转换后的结果。覆盖范围过滤、count／sum、字符串与缺失NULL、OPTIONAL与聚合、多重边遍历、ORDER／LIMIT，基线64节点及sum2016有独立断言。每次只读重开后执行，不执行性能采样。初版夹具漏填分页边界，修正为显式正值，失败日志保留。

最终仅运行39个相关模块：默认531通过；候选802通过、1个条件跳过。未跑全量单测或中间性能基准，仅验证Linux。这是小规模语义对照，不代替完整形状／空间矩阵、同条件性能结论及生产准入。schema／统计／查询索引的写入协调、通用拓扑与剩余R5／R7继续待办。**总计13/24（54.2%），没有验收增长，不提交、不推送。**
