# A3 自适应列版本化实验格式

日期：2026-10-12。

新增 encodeImage/decodeImage，实验格式使用 32 字节小端头：magic、version=1、layout、rows、payload length、presence、CRC、reserved。固定布局嵌入完整稀疏列；canonical 布局顺序保存每个存在槽位的 uint32 长度和完整属性根，缺失不占根。完整 image 上限包括头和长度字段，与原 encodedBytes 的载荷计数明确区分。

解码检查头、版本、布局、保留字段、精确长度、最多 64 行、presence 范围、CRC、固定列行数、每个根的长度与槽位身份、token、尾随字节。恢复值后重新构造封装和类型计数，拒绝不符合当前准入规则的布局 tag，不采用损坏后尝试另一 codec 的回退。

新增固定与 canonical 往返、恢复后的布局转换、精确 image 上限、CRC 损坏、有效 CRC 下超行数测试。五个限定模块共 75 项通过，同名 results 目录保留日志、JSON、源码 hash。该格式仅为实验格式，不修改生产文件含义。

尚未绑定真实数据库/快照/owner 路由；CRC 临时副本、值解码与重新编码均未纳入统一内存预算或取消。生产 descriptor、共享页和原子迁移回收仍待完成。本轮不能据此宣布 A3 或 S1.3 完成。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
