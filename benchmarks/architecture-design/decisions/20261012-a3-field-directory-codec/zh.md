# A3 字段枚举目录实验编码合同

日期：2026-10-12。独立实验编码，不是生产格式准入。

little endian header 48 字节：magic u32@0，version u16@4，slot capacity u16@6，database_id u64@8，snapshot generation u64@16，block u64@24，owner type u32@32，member count u32@36，token count u32@40，CRC u32@44。成员按 slot 排序，每项 32 字节：owner u64、slot generation u64、slot u32、token begin u32、token count u32、reserved u32。尾部为连续 token u32。CRC 覆盖全部编码，计算时 CRC 字段置零。

解码要求显式期望 snapshot、当前 slots、实际字段上限和编码字节上限。先验证长度/上限/identity，再验证 CRC、成员唯一顺序、完整 route、每成员 token 顺序以及无重叠/遗漏的连续数组覆盖。零字段成员允许空 slice，slot 身份仍检查。编码字节为 48+32×实际成员数+4×实际字段数；不包含列 descriptor、owner slots、页/allocator。后续需共同定义原子可达性、回收及真实页布局。

## 实现与测试

encode/decode 已实现上述合同；encode 在分配前检查字节上限和 wire token 宽度，decode 在复制 CRC 工作缓冲/分配对象前检查长度、字段及字节上限。成员 slot 顺序与 token slice 顺序可以不同，slice 排序后须无重叠/遗漏地覆盖全部 token。空成员仍验证 route 与 reserved。

三成员三字段（含空成员、逆序 slot）编码为 156 字节，往返编码完全一致。新增两项测试覆盖 wire/字段上限、重算 CRC 后的错误版本、快照、slot generation、重叠 slice、reserved、零 token、成员上限，以及真实 CRC 损坏。严格构建与相关四个模块 20 项通过。证据位于 results/20261012-a3-field-directory-codec，含日志/JSON、RecordProperty 和源码 hash。

该编码尚未写入真实 group page 或目录根，也没有原子发布/回收协议。当前 directory codec 可重建内存对象，不意味着已经实现数据库重开恢复。slice/checksum 认证不替代实体目录、列目录与 LargeValue 的共同完整性验证。未生产准入。

未运行 benchmark、全量测试，未提交或推送，未更新正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
