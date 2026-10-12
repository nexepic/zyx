# A2 补丁适配器的真实原生通路验证

日期：2026-10-12。

新增 RelocatablePropertyPatches 模块，使用真实单文件 RelocatableStorageBackend，而非测试 overlay 替身。创建用例在有初始属性的 Node/Edge 上执行补丁并一次发布，验证调用方 companion 未变化、真实返回 ID、关闭/重开后的字段值及自环端点。它确认上一轮的创建字段折叠适用于 native shape 规则和实际持久化。

持久用例创建 16 KiB 字符串属性及 rank。源 admission 低预算失败后 fence 保持原值；同 planning fence 重试成功；旧快照 rank 仍为 1，新快照为 2，两个快照均读取原大值。该测试验证快照可见性和载荷保留，不证明大值物理复用比例或实际写放大已优化。

严格模块构建成功，两项真实集成用例与 15 项适配器用例共 17 项通过。模块仅运行这些过滤器，不运行全量测试。新文件由仓库测试 GLOB 自动发现，并纳入本地 selected module 配置。fixture 在 teardown 删除临时数据库，测试中关闭前释放 snapshot，未保留生成数据库。

原始构建/测试日志、JSON、模块配置及新增测试 hash 位于 results/20261012-a2-native-patch-integration。仅 macOS 验证；未运行 benchmark、提交或推送，未更新正式文档/英文记录。

字段原生直接消费、统一预算和局部物理布局仍未完成，不把原生 adapter 集成测试当作这些能力已实现。进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
