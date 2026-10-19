# R3.2 原生类型化字段记录检查点

代码检查点：`b28cb0bf`；父提交：`bd5f906c`。本次只推进既定 R3.2 原型，没有完成固定的 R3.2/R3.3/R5.1/R5.2/R5.3 中任何完整交付；验收进度仍从优化主文档派生，为 13/24（54.2%）。

`HybridPropertyRecords.hpp` 按既定字节布局编码组头和标量列，不持久化 C++ 结构体，也不申请堆内存。独立 payload 标签为 ZYHG（`0x4748595A`）和 ZYHC（`0x4348595A`），版本 1；已搜索现有原生源码，未发现同标签。此处没有分配 SegmentKind/PageOwnerKind 或安装文件 writer，非空描述符保护继续保留。未来原生 reader 仍须认证数据库／页／槽位及校验和；payload 身份检查不能代替外层认证。

组头校验命名空间、模式、正代数、原生组 ID 范围、存活位图、计数和保留字节。原生实体 ID 上限为 2^63−1，最后一组只有 63 个合法 owner。列校验准确的 kind/type/group/token/generation、规范宽度和容量、presence/NULL/live 子集、48-slot 资格、缺失／NULL 的零载荷和布尔规范位。未改变的列允许保留旧语义代数／live，但 presence 必须仍属于当前组 live；校验 column <= group <= descriptor <= snapshot。整数和 IEEE 原始位完整保留，包括负零和 NaN payload；no-op 仍使用现有 PropertyValue 语义。

`ReservedGroupConversion` 可以从实际计费且有界的分类结果输出原生组头和列 payload，不分配 ID、不编码 extension、不切换权威数据、不发布元数据，也不代表成本准入。输出缓冲区由调用方持有并计费；原生临时列不超过已有的临时列预约。编码成功或拒绝均不改变源值和持有账户额度。

修正实验组头把 4096 个字段归属作为永久上限的问题。活动决策将它规定为转换工作上限，实际计数字段为 u32；普通动态属性增长不能凭空增加隐藏的 schema 限制。组头保留既定计数范围；显式正向规划仍拒绝 4097 个归属。未来 writer 必须在发布前检查计数运算溢出；有界反向规划超限也须原子拒绝。

四个必要模块在默认格式 31 和隔离格式 32 构建中分别 48/48 通过。这是同一批 48 个用例的两种配置，不是 96 个不同语义用例。原始结果见 `*-results.json`，编译／链接范围见 `*-scope.json`，另附源码清单／补丁及二进制身份。仅执行 Linux x86_64 Debug＋coverage，未跑全量单测、性能窗口、Kuzu 或其他平台。加载现有云工具链后，可用 `python3 run-necessary-modules.py BUILD_DIRECTORY OUTPUT_DIRECTORY` 复现。

R3.2 尚须接通原生字段池／定位和 variable extension 编码、同次发布中的早期 data-ID 分配与最终 map／描述符准备、全部属性读写／直接批和索引路由、精确大值归属、双向转换、字段降级／删除／搬迁／复用／完整性／失败准入、真实分配成本估计及同执行器 ROW 对照。R3.3 与 R5 的完整合同不变。本检查点不证明时延或实际分配空间收益。
