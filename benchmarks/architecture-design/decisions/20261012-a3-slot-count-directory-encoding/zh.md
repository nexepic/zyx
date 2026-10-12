# A3 按槽位字段数的编码候选

日期：2026-10-12。仅编码/空间实验，当前 descriptor reader 不接受此格式。

新增 encodeSlotCounts 与 validateOwnerRoutes，先验证完整 owner 数量、block/type、当前身份及所有 route generation。候选将 owner ID/slot generation 交给同一 descriptor 中已认证的 owner 槽位表，目录只存 64 个 U32 字段数与按物理槽位排序的 token。空槽计数为零。

实验 wire：48 字节 header，0 magic 0x43464D5A，4 version U16=1，6 capacity U16=64，8 database U64，16 descriptor generation U64，24 block U64，32 owner type U32，36 tokens U32，40 CRC U32（计算时为零），44 保留零；随后 64 个 U32 count 与 token U32。CRC 覆盖目录本身，不单独绑定 owner 表；未来解码必须使用同一已认证 descriptor 的 owner 表，并确定独立防混配方式，不能把任意外部 owner 表当权威。

结构测试在共享字段和单次字段场景中都得到 560 字节目录，原目录为 2352 字节，减少 1792 字节。固定计数开销为 256 字节，少于 8 个 owner 时旧 32 字节成员编码更小，8 个时相等；因此不能单一替换。仍保留旧 codec，下一步补解码、身份绑定及格式选择验证。

目录节省不消除每小列完整 LargeValueRef 的稀疏空间反证，也不代表完整生产存储已达目标。五个限定模块共 66 项通过，预算/身份拒绝和字段计数有定向断言；证据在同名 results 目录。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
