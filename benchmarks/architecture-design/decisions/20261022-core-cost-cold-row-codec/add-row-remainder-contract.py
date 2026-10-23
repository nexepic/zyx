from pathlib import Path
base=Path('/workspace/zyx/docs/apps/docs/content/docs')
zh='''

### 下一核心候选：组内热列与冷 ROW 块（13／39，未实现／未采用）

依据 `20261022-core-cost-direct-locators/input-field-shapes.json`，两套真实输入的全部节点均有长 ID。下一候选只处理该混合布局问题：保留热字段列，将非列字段合并为至多 64-owner 的冷 ROW 块，复用现有 canonical ROW／整根 LargeValue 引用及 LZ4／RAW 算法；不为每个冷字段建立 OwnerMap／ExtensionDirectory，不新建 backend、索引或缓存。该候选需要独立 profile 13／Superblock 39，拒绝旧 11／37 等实验文件；当前代码仍为 11／37，默认 5／31 不变。以下是实施合同，不是完成声明。

**唯一权威与位置。** 新组模式 `HPGM_COLUMN_ROW_REMAINDER` 使用成员记录版本 3。热字段仍按 token→列位置认证；冷 owner 位图及冷块位置也在成员记录中，冷字段不进入普通 EntityPropertyDirectory。原“真实 Entity 存在且无竞争普通 ROW 根”的访问检查保留；不能简单跳过。若热成员位为真，只读该列；否则读取该 owner 的冷块，并由 canonical ROW 目录判定存在／缺失／NULL。完整检查验证热／冷 token 不重叠、所有 owner／字段计数、全部正反引用、无主记录和不存在竞争逐字段树。未访问冷字段的损坏在首次访问或完整 integrity 时发现。

成员版本 3 保留既有 token／owner 位图／热列位置；header@40 保存冷 owner mask u64，@48 保存冷字段总数 u32，@52 保存行块模式／位置编码，@53 为零，@54 保存分组位置条目数 u16，@56..63 为零。只要是新模式，允许热列数为零；全部降级后仍可保存冷 ROW 和原生 owner 生命周期。冷位置的解码形态固定为 64 个 SlotReference；线性编码在“按冷 owner 依次存 16 字节位置”和“每个不同块存 mask u64＋16 字节位置”之间选择更短者，同长选前者。分组条目按首个 owner 槽排序、mask 非空且互斥、并集精确等于冷 mask；相同块不能重复成多条。未使用位置为零。最坏编码至多增加 1024 字节，成员记录最大 3812 字节；成员与 group 仍须同帧，原页／1 MiB／CACHE 边界不变。

**冷块字节与资源。** 冷块是既有 POK_HYBRID_PROPERTY 页内记录，magic=`0x5243595A`、版本 1、64 字节头。未压缩体最多 4096 字节，最多 64 行；超过上限就拆块，不提高额度。RAW 为通用回退；有既有 LZ4 支持且实际缩小时才编码 LZ4。行内容复用完整 canonical ROW root 字节；大根使用既有 owner-keyed ROW LargeValue 引用，实际引用／media／manifest／目录仍必须认证，不能跨数据库借用引用。压缩块不是 LargeValue 新 backend，也不分配独立 Manifest 池。

| 冷块头偏移 | 字段 |
| --- | --- |
| 0／4／6／7 | magic u32／version u16／codec u8（RAW=0、LZ4=1）／owner kind u8 |
| 8／16／24 | group ID u64／semantic generation u64／owner mask u64 |
| 32／36 | 未压缩体字节 u32／实际存储体字节 u32 |
| 40／42／44 | 行数 u16／目录字节 u16／字段总数 u32 |
| 48／52／56..63 | 未压缩体 CRC32／存储体 CRC32／零 |

体以每行 12 字节目录开头：owner 槽 u8、根种类 u8（inline ROW=0、既有 ROW LargeValue 引用=1）、零 u16、字段数 u32、该行结束 offset u32；行按 owner 槽严格递增，mask／计数／连续边界精确匹配。再连接既有 ROW root／引用字节。分配解压缓冲前先认证声明上限、存储长度和 CRC，解压必须恰好得到声明长度，再验证未压缩 CRC、所有行边界和 owner／类型／目录身份。只有已认证块内的选中字段可以同步借用；后续块失败不得 seal 或重放先前输出。最多复用原游标的一个 4 KiB ROW 缓冲表示来保留解码块，不能额外叠加第二个 ROW 缓存；类型化批次的临时缓冲仍走原共享准入。

**发布、变更与迁移。** 转换必须从实际源 ROW 构造冷根，按组打包并实际退役已脱离的源页／整池；热列、冷块、成员位置、group、Page Directory、LargeValue、普通 ROW 映射移除及派生索引在一次 A/B 发布中共同生效。兼容热更新不重写冷内容；冷更新或热列降级只重建受影响的有界块并同步计数／位置，删除处理完整 owner，空冷根移除位置；旧 pin 始终保留旧图。先完成 cold ROOT／LargeValue 读写与跨表示、取消、故障／退款、逆转换和全部物理反向检查，才允许真实输入转换。多组混合转换仍最多 8 组／512 owner／4096 member 每组／1 MiB，总成本与最终空间门禁不放松。若有界前缀不能通过全库门禁，使用尚需定义的私有目标分段构建，不能发布不合格前缀或临时取消准入。所有步骤完成后才进入 P7，当前进度不增加。
'''
en='''

### Next core candidate: group-owned hot columns and cold ROW blocks (13/39, unimplemented/unadopted)

`20261022-core-cost-direct-locators/input-field-shapes.json` shows that every node in both actual datasets retains a long ID. The next candidate addresses this mixed layout alone: keep hot columns and combine noncolumn fields into cold ROW blocks for at most 64 owners, reusing canonical ROW/whole-root LargeValue references and existing LZ4/RAW algorithms. Build no per-cold-field OwnerMap/ExtensionDirectory and add no backend, index or cache. This requires isolated profile 13/Superblock 39 rejecting old 11/37 and other experimental files; current code remains 11/37 and default 5/31 is unchanged. This is an implementation contract, not a completion claim.

**Unique authority and locations.** New group mode `HPGM_COLUMN_ROW_REMAINDER` uses membership version 3. Hot fields retain token-aligned locators; cold-owner masks/block locations also belong to membership, with no cold fields in ordinary EntityPropertyDirectory. Keep actual Entity presence and absence of a competing ordinary ROW root; do not merely skip that check. A true hot-membership bit selects its column; otherwise the owner's cold block and canonical ROW directory decide present/missing/NULL. Full integrity checks disjoint hot/cold tokens, all owner/field counts, forward/reverse references, orphan records and absence of competing per-field trees. Unvisited cold-field corruption is discovered at access or full integrity.

Membership v3 retains tokens/owner masks/hot locators. Header@40 holds cold-owner mask u64, @48 cold-field total u32, @52 row-block mode/location encoding, @53 zero, @54 grouped-location entry count u16, @56..63 zero. New-mode hot-column count may be zero so complete demotion retains cold ROW/native owner lifecycle. Decoded cold locations are 64 SlotReferences. Encode the shorter of 16-byte locations in cold-owner order and distinct-block entries of owner mask u64 plus 16-byte location; ties use the former. Grouped entries sort by first owner slot, with nonempty disjoint masks exactly covering the cold mask and no repeated block entries. Unused locations are zero. At most 1024 extra bytes give a 3812-byte maximum membership record; membership/group still share one frame, without changing page/1 MiB/CACHE bounds.

**Cold-block bytes and resources.** A cold block is a record in existing POK_HYBRID_PROPERTY pages, magic `0x5243595A`, version 1, 64-byte header. Its uncompressed body is at most 4096 bytes and 64 rows; split larger sets, never raise allowances. RAW is the universal fallback; use existing LZ4 only when available and actually smaller. Rows reuse complete canonical ROW roots; oversized roots use existing owner-keyed ROW LargeValue references, with actual reference/media/manifest/directory authentication and no cross-database reference borrowing. Compressed blocks add no LargeValue backend or separate Manifest pool.

| Cold-block header offset | Field |
| --- | --- |
| 0/4/6/7 | magic u32/version u16/codec u8 (RAW=0, LZ4=1)/owner kind u8 |
| 8/16/24 | group ID u64/semantic generation u64/owner mask u64 |
| 32/36 | uncompressed body bytes u32/stored body bytes u32 |
| 40/42/44 | row count u16/directory bytes u16/field total u32 |
| 48/52/56..63 | uncompressed CRC32/stored CRC32/zero |

The body starts with 12 bytes per row: owner slot u8, root kind u8 (inline ROW=0, existing ROW LargeValue reference=1), zero u16, field count u32, row-end offset u32, followed by concatenated existing ROW roots/references. Rows strictly increase by owner slot, with exact masks/counts/contiguous bounds. Before allocating decompression storage authenticate declared bounds, stored length and CRC; decompression must return the exact declared size, then validate uncompressed CRC, every row bound and owner/type/directory identity. Only selected fields in authenticated blocks may be borrowed synchronously; later block failure cannot seal or replay prior output. Reuse at most the cursor's existing single 4 KiB ROW-buffer representation for decoded blocks, without a second ROW cache. Typed transient buffers retain original shared admission.

**Publication, changes and migration.** Build cold roots from actual source ROW, pack by group and actually retire detached source pages/pools. Hot columns, cold blocks, membership locators, group, Page Directory, LargeValues, ordinary ROW mapping removal and derived indexes share one A/B publication. Compatible hot updates do not rewrite cold contents. Cold updates/hot demotion rebuild only affected bounded blocks and their counts/locations; deletion handles the whole owner, and empty cold roots drop their location. Old pins retain prior graphs. Complete cold ROOT/LargeValue reads/writes/transitions, cancellation/fault/refund, inverse conversion and all physical reverse checks before converting real input. Mixed cohorts remain at most eight groups/512 owners/4096 members per group/1 MiB; whole-cost and final space gates are unchanged. If bounded prefixes cannot pass whole-database admission, use private staged destination construction whose contract is still required, never publish ineligible prefixes or disable admission temporarily. Only then enter P7; current progress does not increase.
'''
for lang,body in [('zh',zh),('en',en)]:
 p=base/lang/'zyx/architecture/native-single-file-storage-plan.mdx';p.write_text(p.read_text()+body)
