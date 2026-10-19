# R3.2 extension 信封检查点

代码 `ce0fee49`，父提交 `dcac8dcf`。本次是有界 payload／计划准备，不是图生命周期接通或 R3.2 完成。固定的 R3.2/R3.3/R5.1/R5.2/R5.3 均尚未完成；验收进度从活动双语优化表派生，仍为 13/24（54.2%）。

`HybridPropertyExtensionView` 实现了 `r3-layout-zh.md` 新明确的 extension 信封：准确的 owner/token/语义身份、inline/reference 模式、单个现有规范值及保留字节／长度检查。inline 检查借用不可变且持有 pin 的源字节。编码输出和规范 map 工作区使用调用方 PMR 账户；原生 slotted 记录至多 3952 字节，实际目标可能要求更低上限，不能据此认定最大记录能放入 packed B+Tree key。未改动 SegmentKind、PageOwnerKind 或格式版本。

大值引用复用现有校验和／身份 codec 及 Property payload media，额外要求 bytes kind、可见代数和恰好一个匹配的目录／字段 token。多字段整行在目录按 count 分配前拒绝。LargeValueStore 必须先认证引用／manifest／chunk／内容 hash 才能提供载荷；信封 decoder 本身不执行这部分存储读或内容 hash 认证。内容共享及精确退役仍须由后续原生协调器接通。测试中的引用是构造的 codec 输入，不证明原生 LargeValue 发布已完成。

view 只允许移动，防止复制时暗中分配另一个 media 字符串。引用解码须在有界 media 分配前通过调用方持有准入，再报告观察到的容量。规范值解码按既有 inspection 的堆请求上界先准入，目录容量单独准入。预约属于调用方，须覆盖返回值生命周期；helper 不自行持有 query budget。`ReservedGroupConversion` 只输出实际 inline-extension 成员，拒绝 COLUMN 来源、缺失成员和超限值，不改变源 ROW 或持有计划额度。真实大值创建不能拆成此前一次独立发布。

四个必要模块在默认格式 31 和隔离格式 32 构建中分别 36/36 通过，是同一批用例的两种配置。保留原始范围／结果、源码 hash／补丁和二进制身份。新 header 在必要测试和实验转换计划中实例化，生产字段路由尚未安装。仅执行 Linux x86_64 Debug／coverage；未跑全量单测、其他平台、性能窗口或 Kuzu。

一次中间运行 34/35 通过：边界测试仅扣除了规范值的六字节信封，漏掉现有字符串编码的四字节长度字段，生产 codec 正确拒绝了这个超限测试值。核对现有大小／编码实现后，测试改为由 `encodedSize` 获取空字符串开销，再验证真实容量和超一字节；原始失败保留。最终运行另加入真实账户超限／退款／重试用例。首次编译还发现默认值误用了抽象 CancellationCheckpoint，最终改为现有规范 codec 使用的函数回调签名。

加载既有工具链后用 `python3 run-necessary-modules.py BUILD_DIRECTORY OUTPUT_DIRECTORY` 复现；`verify-evidence.py` 校验源码身份、原始计数、定向范围及文档派生进度。后续仍需 variable map 前缀唯一性／最终变更合并、原生字段池／定位、同次发布中元数据持有的早期 data ID 与最终描述符、全部读写／索引／直接批路由、精确大值归属、完整生命周期及原生成本／ROW 对照。非空描述符保护继续保留，不宣称生产准入或性能收益。
