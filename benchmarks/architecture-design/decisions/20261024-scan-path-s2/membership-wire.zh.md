# S2 持久扫描成员记录：编码原型

此记录仅固定成员记录本身的字节合同。它不是完整候选文件协议，不授权写入新格式数据库。完整的根寻址、格式版本、构建状态、原子发布、更新、恢复及全库准入尚未实现；在这些内容补齐前，禁止将此记录用于跳过 Entity 验证。当前 S2 未验收，进度仍为 15%。

复用既有物理组的 64-ID 区间，未提高 S1 的 512 行批上限。group_id = 1 + (entity_id - 1) / 64；位 i 对应 (group_id - 1) * 64 + 1 + i。最终组不得设置导致 uint64 溢出的第 63 位。成员记录覆盖组内全部 live 节点，包括没有属性和没有标签的节点；不是仅覆盖已列化字段的 owner。组标签并集超过 64 时整个组使用一般读取路径，禁止截断或把未记录标签当成不存在。

所有整数为 little endian。头 64 字节，标签条目每个 16 字节，最多 64 个；总长度必须精确为 64 + 16 * label_count，最大 1088 字节。

| 偏移 | 字段 |
|---|---|
| 0 / 4 / 6 | magic u32 = 0x4d53595a / version u16 = 1 / header_bytes u16 = 64 |
| 8 / 16 / 24 | database_id u64 / semantic_generation u64 / group_id u64 |
| 32 / 40 / 42 | live bitmap u64 / label_count u16 / reserved u16 = 0 |
| 44 / 48 / 52 | total_bytes u32 / body_crc u32 / header_crc u32 |
| 56 | reserved u64 = 0 |
| 64 + 16*i | label token u64，随后为 label membership bitmap u64 |

CRC 使用既有 calculateCrc；body_crc 覆盖标签条目全部字节；header_crc 覆盖 64 字节头，计算时自身字段置零，包含 body_crc。标签 token 非零且严格递增；每个标签位图非空且是 live 的子集。解码验证全部目录后才允许筛选。空标签请求返回 live；多个标签取成员位图交集；未知 token 返回空。缺失/重复/乱序字段、非零保留位、错误长度、校验错误、数据库/group 不符或未来代际均拒绝。

CRC 和 semantic_generation 不构成可见性证明。生产接入必须从当前快照认证根解析到真实 owner 页，认证物理版本、槽代际、LSN、数据库/数据区域与 I/O epoch；借用期内持有 owner。成员记录和节点/标签/字段更新必须同批发布；旧 pin 继续读取旧记录。尚未实现这些不变量时，编码原型不能替代主节点存在性或标签检查。

验证：位图交集和无标签节点、每字节翻转、全部截断长度、错误数据库/group/代际、标签重复、非 live 位、标签数量上限和 uint64 ID 尾部。测试仅操作内存字节，没有创建候选格式文件。
