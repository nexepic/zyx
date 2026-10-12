# A3 从真实原生快照构造字段目录

日期：2026-10-12。

候选新增 buildFromNativeSnapshot：从 slots 实际 owner 列表按 ID 顺序读取，显式限定 Node/Edge 类型，先验证实体记录/拓扑存在，再在同一 MetadataSnapshotPin 和 NativeReadSession 中读属性。Node 使用已有 nodePropertyCount 在载荷读取前检查剩余字段限额；Edge 缺少对称 count 入口，读属性后检查累计字段工作，源载荷仍经强制提供的 SelectedPropertyReadAdmission。最终目录身份由真实 pin 取得，不再依赖调用方任意 roots 与 snapshot 的配对。

Node/Edge 新测试通过真实 native writer 构造不同字段，检查目录结果和 admission 调用；源预算拒绝、缺失 admission 和不存在 Node 拒绝。严格构建与四个相关模块 22 项通过。证据位于 results/20261012-a3-native-field-source，含日志/JSON 与源码 hash。fixture 清理临时文件。

该入口临时保留完整属性值，尚非 token-only reader；entry 限额不是字节预算，admission 回调的计费合同沿用 native reader，不声称整组/查询所有 allocator 容量已统一受控。Edge 前置字段计数与取消仍需完善。候选纯数据构造接口保留用于 codec 单测，不能将其当作权威 source 路径。

持久 descriptor/原子可达性/回收及完整生命周期仍未完成。未运行 benchmark、全量测试，未提交或推送，未修改正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
