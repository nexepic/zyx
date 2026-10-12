# A3 共享小列页实验格式

日期：2026-10-12。

新增 PackedPropertyColumnPage，24 字节页头包含 magic、version、count、总长度、CRC、reserved；每列目录 12 字节包含 token、offset、length。连续存放完整 AdaptivePropertyColumn 镜像，最多 4096 列，并以调用方明确的总字节上限限制头、目录和载荷。writer 验证输入列镜像和严格 token 顺序。

reader 验证页头、CRC、完整目录范围、严格 token 顺序、连续不重叠子块和精确结束后，返回借用原始页的列 span。reader 此层只验证容器，不验证子镜像语义，调用方必须通过 AdaptivePropertyColumn::decodeImage 验证选中列。span 生命周期不得超过输入页。

新增混合固定列与 canonical 列同页往返、全部字节计数、精确预算和有效 CRC 下重叠目录拒绝测试。五个限定模块共 80 项通过，同名 results 目录保留日志、JSON 和源码 hash。

这是共享页载体原型，不是已接入生产的页类型。仍需真实页身份、descriptor 子块引用、快照绑定、原子更新与回收，及统一预算、取消、页填充/拆分策略和完整成本验证。不能以本 codec 证明全数据库空间或性能收益。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
