# A2 删除目标的补丁前置拒绝

日期：2026-10-12。

补丁适配器在请求验证后检查 companion 的显式 Node/Edge 删除目标，使用现有全引用 target 集合，无需额外删除索引分配。命中持久目标即拒绝，发生在源 shaped admission、companion 复制和 staging 前，统一 native validateShape 已有规则。创建 ordinal 不与持久 ID 混同；DETACH 隐式删除边仍由 backend 的拓扑验证负责，本轮不声称前置发现全部隐式关系。

新增测试分别覆盖 Node/Edge，验证拒绝发生在 source admission 前，fence、companion 标签及原边保持不变。严格构建及 16 项适配器、2 项真实 native backend 集成测试共 18 项通过。证据位于 results/20261012-a2-patch-deleted-target，含日志、JSON、源码 hash。

仅 macOS 验证。未运行 benchmark、全量测试，未提交或推送，未修改正式文档和英文记录。原生字段消费、统一预算及局部物理更新尚未完成。进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
