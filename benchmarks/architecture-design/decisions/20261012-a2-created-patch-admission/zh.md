# A2 创建补丁的前置工作准入

日期：2026-10-12。上一轮创建补丁在 companion batch 被复制后才检查创建源字段数量；本轮将已知的创建源字段数计入请求验证阶段，在复制 companion 和源载荷之前拒绝工作量超限。归一化不再重复计入创建源，持久源仍在读取后按现有路径检查。

新增两项测试覆盖：创建源两个字段加一个覆盖操作在工作限额 2 下拒绝、限额 3 下成功；Node/Edge 创建 reference kind 与 entity kind 不匹配、重复 created target 均整批拒绝，overlay fence 不变。既有创建/持久混合和重叠 replacement 测试继续验证原子边界。

严格构建成功，属性补丁定向十五项通过，显式选定模块共 710 项、33 套件全部通过。证据含源码 SHA-256、最终构建/测试日志与 JSON、选定模块配置。仅 macOS 验证，未运行 benchmark、全量单测、提交或推送，未更新正式文档或英文记录。

字段工作量准入并不限制整个 companion batch 的动态载荷容量，尚不能称为 A6 的真实 allocator 计费；无补丁的 companion 请求继续由既有 backend 验证。CREATE/MERGE normalizer 直接产出 patch、原生按键消费、属性局部布局和恢复回收仍需实施。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
