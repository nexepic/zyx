# A2 创建补丁折叠到创建载荷

日期：2026-10-12。

核对原生 validateShape 时发现：它拒绝对已携带初始属性的 created Node/Edge 再传完整替换。而补丁适配器原先统一追加 property_replacements，测试 overlay 的宽松实现掩盖了该不兼容。

测试 fixture 新增按需启用的原生创建覆盖拒绝规则；混合持久引用与创建 ordinal 用例启用它，修复前确实失败，原始失败日志已保存。修复将 created Node/Edge 的合并最终属性移动回复制后的 companion 创建请求，仅持久目标追加完整替换。原始调用方 companion 不变，仍一次发布；entry/source 准入、创建 ordinal 和补丁/完整替换冲突检查保留。用例同时覆盖携带初始属性的创建 Node 与 Edge，以及同批持久 Node 补丁。

严格构建成功，补丁适配器、Cypher mutation 与原生 executor 三个模块共 249 项通过。证据位于 results/20261012-a2-created-patch-fold，含修复前失败、修复后日志/JSON 与源码 hash。该测试模拟实际 native 的明确 shape 规则，但没有将 fixture 结果冒充真实 backend 的端到端 patch 测试。

只验证 macOS；未运行 benchmark、全量单元测试、提交或推送，未修改正式文档/英文记录。原生字段消费、全查询预算与物理局部更新尚未完成。进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
