# R3.1 有界属性子块候选定案

此处在 R3.2 编码前关闭一个候选，不是生产准入。生产继续保留现行 ROW 及可用回退。复用 20261014-a3-layout-contract 的归属和成本证据，不恢复其已拒绝的整容器 carrier；已有行 chunk、字段暂存、独立大值和计费不重复算成果。

## 固定参数和字节布局

记录体整数为 little endian；有序 B+Tree 键前缀使用无符号 big endian，保持既有字典序比较器的 owner/group/token 数值顺序；保留字节为零。未知模式/类型、溢出、长度矛盾、重复/无序键及归属矛盾均拒绝。磁盘编码不使用 C++ sizeof/填充。以下字节不含现有认证页/slot framing。R3.2 审计 registry 后，在写任何实验文件前一并分配 codec/page-kind/version 标识，不能悄悄塞进当前格式。

| 参数 | 固定候选及理由 |
|---|---|
| 逻辑组 | Node/Edge 分开；64 连续逻辑 ID，group=1+(id−1)/64，slot=(id−1)%64。标签不决定布局；只评此已有候选，不参数海选 |
| 物理/更新单元 | 现有 4096 字节认证 COW 页；共享 slotted pool 打包记录，不每列独占页/segment |
| 压缩块 | 基线不新增压缩；每字段最多 64 个原始值为独立子块，已有大值压缩独立 |
| 列准入 | 非 NULL 值同为 int64/double/bool，至少 48 个 live slot 有该字段。混型/稀疏走 extension/ROW；NULL 属于 present |
| 转换 | 显式、有界；类型/占用条件不满足时只降级该字段，不整组重排，不在后续写自动重新晋升 |
| 工作上限 | 一次转换最多 64 owner、4096 个实际 owner/token 成员、1 MiB 规划/暂存字节，且受实际 QUERY/WRITE 账户限制。单记录含 header ≤4096 字节且不超过扣除 framing 后的原生 slot 可用容量；超出用认证独立大值引用 |
| 稀疏回退 | 不建空列描述符；完整成本准入失败或超上限保持 ROW。删除不压缩/复用逻辑 ID |

列记录为 64 字节 header 加按 slot 索引的值。offset：0 magic u32；4 version u16；6 Node/Edge kind u8；7 scalar type u8；8 group ID u64；16 token u32；20 capacity u16 (=64)；22 payload bytes u16；24 semantic generation u64；32 presence u64；40 NULL bitmap u64；48 live bitmap u64；56 reserved-zero u64。presence⊆live，NULL⊆presence；缺失/NULL 的 payload slot 为零。int/double payload 512 字节，总计 576；bool 位 payload 8 字节，总计 72。沿用 canonical 标量语义，包括 int/double 区分和 NaN 规则；同时认证 group/token/kind、物理页与 slot generation。

组 header 64 字节：0..7 magic/version/kind/mode；8..15 group ID；16..23 semantic generation；24..31 live bitmap；32 actual membership count u32；36 admitted column count u32；40..63 reserved zero。不嵌入所有可能 token。

每个 Node/Edge 命名空间使用三张共享有序 map，不按 owner/字段各建树：

1. owner 枚举：键 owner u64+token u32，值 source u8+reserved-zero[3]，每实际字段 payload 16 字节，另计 B+Tree framing/填充。source=COLUMN/EXTENSION 是唯一语义归属选择器；ROW 继续已有实际字段枚举。
2. 列目录：键 group u64+token u32；值 stable PageId u64+slot u16+reserved u16+slot-generation u32，共 28 字节 payload。每获准字段/组一条 locator，owner 项通过 group/token 推导，不重复 locator。
3. extension：逻辑键 owner u64+token u32，物理目录值使用与列目录相同的 16 字节稳定 slot locator。所指共享字段页记录保存一份 canonical 字段或认证独立 LargeValue 引用；认证 body 含 owner/token，服从记录／原生可用载荷上限，复用既有大字段/media 合同，不保存完整第二份 ROW。

map 使用已有 B+Tree framing，R3.2 实测全部成本；16/28 字节只是 payload 下界。组 header 目录是每命名空间第四张共享 map，键 group u64，值使用列目录相同的 16 字节 slot locator，额外计 O(G) 成本。

明确选择新增 Superblock 物理元数据根 hybrid_property_descriptor_root，并作为额外图语义 fence 成员。它指向认证的 192 字节描述符 payload：0 magic u32；4 version u16；6 zero flags u16；8 semantic generation u64；16 database ID u64；24..63 reserved zero；八个 B+Tree 根引用均为 stable PageId u64+expected root generation u64，offset 64/80/96/112 为 Node owner/column/extension/group-header，128/144/160/176 为 Edge 对应根。空树两个字段均为零，部分零拒绝；描述符 generation 必须对 Superblock 可见。组 mode 只存在组 header。ROW 继续通过现有 entity_property_directory_root 路由，不在描述符重复 ROW 根。

R3.2 在任何文件编码前，同时分配新 Superblock 字段 offset/format version 和 metadata page-kind，配套所有 reader/writer/搬迁/integrity。首次转换和后续字段修改均替换描述符、推进额外图 fence，即使 B+Tree PageId 稳定且只改物理映射也如此。故意保留保守冲突；原七根 fence 不含该描述符；当前已追加第八个描述符成员，并使默认/实验格式具有不同 format token。

selector、列 presence、extension 存在性精确一致；无 selector 是缺失，存储 NULL 有 selector。extension 不留完整第二份行。点读验证所访问归属，完整 integrity 双向枚举发现孤儿/重复/错 owner/group/type；不在每次点读扫描全图。当前列类型不成为永久 schema。

R3.2 物理映射实现定案：复用既有 key-only B+Tree entry，以有序逻辑键前缀加 canonical 值后缀编码。owner selector 为 12+4 字节，列和 extension 定位均为 12+16，组定位为 8+16；这些仍是 packed framing/restart/内部分隔符/填充前的下界，值后缀参与压缩和成本计算。逻辑前缀唯一性独立于完整原生键唯一性：点读匹配项并前看一项，同前缀第二个值即拒绝，跨叶也不能漏检。类型化所有者须验证完整 integrity，并把事务内最终修改归并成每前缀一次 erase-old/insert-new；不能从同一旧快照多次未归并替换。内部 B+Tree child 仍为 U64；原生复合变更和 fresh rebuild 显式保留 EMPTY 叶布局。`FixedBTreeMap` 仅提供通用定宽编码/查找，不安装 hybrid 根、不解释定位目标、不定义图 fence。extension 变长 body 位于共享字段页，仍需独立类型化 codec 和页绑定，不能认为定宽 helper 已实现它。上述共享 map 测试使用通用 index 命名空间，不代表安装了 hybrid 字段路由或完成生产转换。

## 访问、修改和完整成本

F 为实际字段，C 获准列，G 分配组，X 例外字段，h 共享树高度。空间 O(F+C+G+X)，C≤F、X≤F，不为缺失字段分配 O(E×K) 成员项。

| 操作 | 读取对象 | COW/发布对象 |
|---|---|---|
| 单 owner/字段 | group/mode、selector、选中列目录/子块或 extension、页/catalog 路由及所需大值 | 无；树下降可能多于 ROW，点读延迟仍须保护 |
| 完整 owner | owner 前缀范围的实际键、选中源和请求的大值；ROW 用当前 root | 无；按实际字段/相关页工作，不扫组/全库所有 token |
| N owner、f 字段 | 批定位 group/membership，各访问组的 f 子块及匹配 extension，恢复原始/重复 ordinal | 无；ROW/candidate 使用同一 R1 执行器 |
| 已有标量 SET | 选中源/selector、相关索引、页映射 | 变化数据页和必要 O(h) 路径；逻辑 slot 身份不变时不重写无变化 selector/locator；仅物理映射变化仍推进语义 fence |
| 新增/REMOVE/变型 | 选中归属；降级时最多该字段 64 个值 | 受影响 selector、源/目录、组计数、索引、allocator、精确 retire；配额拒绝原子保留旧图 |
| 整 map 替换 | 实际新旧键与变化索引，沿用事务输入准入 | 变化字段/路径，不整组重写；不保证接受无上限用户输入 |
| 删除/回收 | owner 实际字段/关联图记录、成员和 bitmap | 变化路径/计数/索引，移除空列/组及精确退役，等 pin 后物理复用 |

no-op 按 canonical 类型/值判断，不产生属性/索引替换。NULL↔missing、int↔double、merge/replace 各自转换。无变化索引和无关大值复用。普通单字段操作最多降级一个字段；暂存不够则原子拒绝。ROW 回退保留现有限制，不冒充所有数据均已细粒度更新。

转换前保守估计 candidate/ROW 的完整原生分配：实际填充下向上取整数据页、map framing/路由、枚举、type/presence/live、描述符、catalog/allocator/Page Directory/metadata route、大值 manifest/content index、受影响查询索引、retire queue、转换期间旧版本。计入实际保留 128 KiB pool 空间和未填充帧，不能假设 31 帧全满。只有证明相同才抵消共享未变成本；转换 scratch 单独计费。

准入条件：候选稳态 live 分配至少比 ROW 小 10%，且旧+新峰值保留在原资源/文件额度内；无法计算则拒绝转换。R3.2 对照真实分配，低估任何组件使估计器失效并修正。这是字节资格条件，不是 R7 延迟准入。

64 owner 共享 int 字段下界：576 子块+64×16 selector+28 列 locator+64 group header+24 组 locator=1716 字节，尚未计 framing/整页/全部原生元数据。独有字段不建列。A3 的 2224/4744 和 2296/4016 是其他编码观察，不能当完整分配对比。共享成员树可能抵消列节省，必须照实记录，不能为基准强制列模式。

## 快照存活与字段转换合同

当前 owner 存活以组头为权威。未修改列保留其最后编码代际的 live 掩码；它与当前组 live 只能在缺失单元上不同。列 presence 必须同时属于编码时 live 和当前组 live，NULL 属于 presence。某 owner 在该字段中缺失时，其创建/删除不应改写无关列；删除有该字段的 owner 则必须原子删除 presence/selector，死 owner 仍有值即损坏。不能要求所有列的 live 或代际等于最新组头。

验证 `列语义代际 <= 组代际 <= 描述符语义代际 <= 快照可见代际`，身份非零且被选 Node/Edge 命名空间的 owner/column/group 根存在。另行认证期望 owner/group/token 及原生 PageId/slot-generation。未变列保留旧语义代际；任何实际 hybrid 字段/归属变化仍须替换描述符并推进额外图 fence。不扩展现有逻辑 ID 范围：当前 EntityPropertyKey 仅允许正数至 2^63−1；payload 实验的通用 uint64 运算不是扩大原生范围。

标量 SET/no-op/REMOVE 只转换目标字段。同类型变化且 present 至少 48 槽时只替换该列，NULL 属于 present 并把 payload 清零；低于 48 或非 NULL 类型变化则只把该字段降级为最多 64 个实际单元。不自动提升 extension/ROW。传入的 opaque extension 值由调用方持有，直到独立准入的原生 extension writer 处理；标记不是持久化大值引用。no-op 比较必须与既有 ROW 的 `PropertyValue::operator==` 一致：int/double 不混同，double +0 等于 -0，NaN 不等于自身；实际替换仍保留 IEEE 原始位。codec 字节 round-trip 相同不等于写入 no-op。进入前须从实际账户预约固定转换暂存，并持有到结果释放；输出不得超过原生 slotted 可用 payload 或 4096 字节上限。规划失败不改 source、索引或已发布根。纯转换内核不能替代原生反向转换、真实 extension 归属和全局 integrity。

## 生命周期与格式义务

复用事务内字段暂存，精确计算归属/索引/引用变化，预留 ID/容量，构建变化数据及描述符/路由/allocator/retire。保持 data fence → 非当前 A/B → superblock fence → 确认。校验语义属性 fence，保持 overlay/read-your-writes、当前冲突和 blind-rebase 范围，不重放副作用语句。

发布前失败仍以旧图为权威；不确定确认通过重开选根解决。旧 pin 保留完整旧 selector/route/大值引用。退休依据跨 owner 精确可达性，不以 candidate 引用差集证明全局无引用。反向转换、字段降级、删除、搬迁、物理复用、长 pin/背压和尾裁剪使用同一协议。

R3.2 同步更新全部 reader/writer/descriptor/map/page-kind/version、root 校验、integrity、搬迁、allocator 可达性和 retire。沿用新文件/旧版本拒绝策略，不加 sidecar/迁移。生命周期未贯通不能进 R3.3；保留同执行器下隔离 ROW 对照。

必测 R3.2：47/48/64 占用边界，稀疏/独有字段、随机交错形状、多标签、持续变型/降级、删除后稀疏、>4096 成员、宽行/大值、无序重复邻居 gather、no-op/NULL/merge/replace/remove、旧/新快照/重开、合法 checksum 身份错配、各发布 fence 故障、配额拒绝、搬迁/复用/退休。精确校验 ID/类型/值/重复和完整原生成本。失败可以完成比较项 R3.2，不能完成生产项 R3.3；R7 延迟/写入/资源/稳态门槛不变。

R3.2 原生描述符 frame 编码准备：`HybridPropertyDescriptorCodec` 使用 32 字节物理头（0 ZYXH magic u32，4 version u16=1，6 header bytes u16=32，8 physical generation u64，16 committed LSN u64，24 payload CRC32，28 header CRC32），其后为上述 192 字节 payload（magic ZYXD、version 1），其余 4096 字节页尾必须为零。payload CRC 覆盖 offset 32 至页尾，header CRC 覆盖 32 字节头且计算时自身字段归零。区分搬迁的物理代际与属性语义代际，要求 semantic<=physical<=snapshot、committed LSN 可见、database 与预期物理根代际精确匹配；物理代际还须能由 Superblock 的 59 位 packed reference 表示。八个非空命名空间根不能共用 PageId，部分零根拒绝。树根的预期 generation 是共享池物理复用代际，不能与描述符／帧／快照代数作大小比较。后续实际路由 reader 必须匹配所选命名空间 IndexId 和 pin 下的 Index Root Directory 项，再精确认证实际 B+Tree 根页的 PageId／物理代际；frame CRC 或完整的正数根引用本身不能认证所引用的树。内存 codec 检查点之后，已接入下述隔离的空描述符元数据生命周期；仍不能据此验收 R3.2 或发布非空 hybrid 属性布局。

隔离元数据原型定案：`ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT` 默认 OFF；默认 Superblock 31/header 336/profile 5 不变，非空描述符根不能编码或发布。ON 构建仅用于实验，使用 Superblock 32/header 352/profile 6，新增 physical root 位于 336..351（slot u64 + 既有 generation/frame packed u64），emergency reserve 从 352 开始，共 32 字节；其后保留字节仍为零。两种构建互相拒绝文件，不迁移。当前实验 writer/read/open/integrity 只允许八根全零的空描述符；任何非空根在分配 frame 前拒绝，合法校验和下的非空路由或错误身份也会在打开/读取时拒绝，不能静默回落 ROW。普通 Node/Edge/ROW 能力继续可用，但实际子块转换 API 尚未安装。

描述符是 metadata pool 中独立的物理 frame，不分配 routed MetadataPageId，不加入 12 根 logical bootstrap 列表，也不伪装为 State/radix。元数据池恢复、只读发现、可达性、旧 pin 保护和保守整树搬迁显式访问它；局部搬迁遇到额外物理 frame 时保留原来的保守 fallback。搬迁更换 physical generation/LSN 并保留 semantic generation。清除 root 后按现有 pin/reachability 协议回收，不另加退役 sidecar。第八根 fence 在两种构建均存在（默认文件根为零），format token 区分 31/32；该根变化不能被误认成 ID-only 预留或盲邻接 rebase。文件发布仍使用既有 data fence → inactive A/B → superblock fence。完整成本必须计算实际 4 KiB 描述符 frame、128 KiB 保留池及旧 pin 占用，不能仅计 192/224 字节。非空根保护只能与完整字段 reader/writer/integrity/retirement 一起替换。证据：`20261019-r3-native-root/`。

已打开句柄取得新 metadata snapshot 时也认证描述符；失败必须释放刚取得的 pin，不能只在首次 open 验证后让新快照静默沿 ROW 继续。描述符读取使用文件 view 构造时固定的 database ID，避免与 writer 对 mutable routing Superblock 的 rebind 竞争。PROFILE 固定计费 registry 记录 `native_graph.read.metadata_decodes.hybrid_descriptor`（当前线程累计解码尝试数）；未增加资源上限，registry/staging 上界按实际条目数派生。

4096 个字段归属仅限制一次显式转换计划，不是转换后组的永久容量上限。组头用既定 u32 字段记录实际数量；普通写入必须保留现有动态属性能力，并检查计数运算溢出。正向／反向转换超过计划工作上限时仍应拒绝，不能截断、静默拆分发布，也不能把 4096 解释成永久模式限制。
命名空间 IndexId 现由 `HybridPropertyIndexIds.hpp` 固定：ordinal 0..7 对应 `0x8000000000000030`..`0x8000000000000037`，与全局邻接、System Catalog、LargeValue、schema definition 及 vector mapping 前缀不重叠。普通图标量暂存的五个入口在两种构建中都拒绝该范围。描述符请求的非零 IndexId 必须在数据准备前与 ordinal 精确匹配，不能替换为普通索引或错置 Node／Edge／字段角色。此检查不认证树内容，不准许非空字段路由。存储规范已记录默认 5／31／336 与隔离 6／32／352 的 profile／Superblock／header 兼容矩阵及持续保留的拒绝保护。


## Extension 信封准备

尚未安装路由的 R3.2 extension codec 使用 48 字节 ZYHE（`0x4548595A`）信封，版本 1：magic u32 位于 0，version u16 位于 4，owner kind u8 位于 6，mode u8 位于 7（1=inline，2=LargeValue 引用），原生 owner ID u64 位于 8，token u32 位于 16，body 字节数 u32 位于 20，语义代数 u64 位于 24，32..47 保留为零。inline body 恰好为一个现有规范 PropertyValue；缺失时没有 extension／selector，NULL 则保存规范 NULL。认证外层 owner/token，要求 extension<=group<=descriptor<=snapshot，引用的逻辑代数不得超过 extension。

大值模式复用现有 LargeValueRefCodec 和 `application/vnd.zyx.entity-property-root` media，但无 owner 的内容必须恰好包含一个 token/value；目录 token 和字段 token 均须匹配外层。多字段、尾随数据、错误 kind/media、未来身份都拒绝。现有 LargeValueStore 必须先认证 manifest、chunk 和内容 hash，再提供数据；信封 decoder 本身不认证内容。不能再复制整行，也不能把任意同 media 的整行当作单字段。相同单字段内容允许沿用现有跨 owner 不可变内容去重及精确退役规则。

完整 extension 记录必须满足真实目标上限，且至多 3952 字节（4032 字节 owner payload 减去 64 字节 slotted header 和 16 字节槽位）。extension 目录使用固定的 12+16 字节有序前缀／locator key；变长信封留在共享字段页，不进入内部 separator。真实成本须计入 48 字节信封及额外目录／值编码开销。目标缓冲区和规范编码工作区使用调用方 PMR；引用 media 容量及字段／目录解码须分别持有预约。已计费转换计划只能输出实际 inline-extension 成员，拒绝 COLUMN 来源和超限值。此 codec 不安装 extension 目录／字段页路由、不分配 data ID、不发布大值，也不移除非空根保护。

## Extension 物理定位与私有字段页准备

extension 的逻辑键仍为 owner u64/token u32，物理目录与 column 一样使用固定 16 字节 stable PageId/slot/slot-generation 定位值，完整 key-only 记录为 12+16 字节；48 字节信封和变长字段内容放在共享 slotted 字段页。不能把变长字段直接放进 B+Tree key 后缀：原生内部 separator 保留整个 key，会复制大字段并降低扇出。新增定位值及所选字段页读取必须计入完整空间成本和点查保护指标，不据此预判性能准入。复用现有固定 map，不保留新增通用变长 B+Tree map。未采纳的私有变长 key 试验仅用于核查 separator／容量机制，不作为时延收益或完成项。

`HybridPropertyPageImage` 为页图像、记录复制和序列化缓冲区使用明确分配账户，准备中立的原生 SlottedPage 图像；group、column 和变长 extension 记录以 stable PageId/slot/slot-generation 引用共享打包。它尚未安装 pool/格式、不分配 ID，绝不能作为普通 ROW 页持久化。后续仍须在同一次发布中，由元数据先分配 data ID，再规划目录 key。

共享池物理 generation 与语义提交代数必须分开：data pool 的复用代数不会每次图提交都增长。列语义代数 7 可以合法存于物理代数 1 的 data pool，只要满足 group／descriptor／snapshot 的语义上界；描述符元数据帧的物理代数仍遵循其独立发布规则。私有 clone／overwrite 保持稳定槽位，erase／reuse 推进 slot generation。这些图像检查不代替原生图路由、最终权威／完整性、scratch 准入、分配／退役及 R3.2 完整生命周期验收。

## 普通图事务发布骨架

`NativeGraphHybridPropertyRootMutation` 是类型化的最终根请求，包含预期数据库／旧根、replace／clear 模式及八个命名空间 IndexId，不新增通用图回调或物理权限。writer 在准备图数据前验证数据库／旧根，并认证已存在的旧描述符；最终阶段从本次候选的最终树根或当前保留目录解析引用，拒绝缺失／已退役／重名命名空间，使用真实 batch generation。描述符和普通 Index 数据沿用同一次 data fence → inactive A/B → superblock fence → 确认；此请求不需要 bulk scratch，既有 bulk 早期 hook 的 scratch 合同不变。

隔离构建目前仍只允许空描述符发布／清除；非空描述符即使在普通 Index 页已准备后也会拒绝，旧图继续有效。尚无生产调用方暂存该请求。这是发布骨架，不是已安装字段池、转换或完整属性路由。locator key 规划前仍须接通元数据持有的字段协调器来分配 data ID，不能为了套用既有 early hook 而制造此前一次 ID 提交或 bulk scratch。

有界转换分类和反向投影现仅由存储模块 `HybridPropertyGroupConversion.hpp` 实现；输入是协调器认证、持续持有且不可变的 ROW 根。固定 workspace 只分配一次，必须通过持续持有的 PMR owner 计费，不回退默认资源；64 owner／4096 成员／85 列及 workspace ≤1 MiB 的上限不变。源根／堆字段、输出编码和原生发布由各自账户持有。取消、超限和输入错误释放 workspace，不修改 ROW 源。规划器不发布 ID／字段／根。benchmark 的 `ReservedGroupConversion` 现仅为复用此实现的实验视图适配器，额外视图存储单独计费；生产存储不包含 benchmark 或 query execution 头。元数据持有的早期字段协调器须在最终 locator／map 规划前使用该原生规划器；迁移本身不代表 R3.2 验收。

元数据模块现由 `HybridPropertyConversionPreparation` 持有私有早期 ID 准备；构造入口仅向原生 metadata store 和测试访问器开放，不向普通图调用方暴露。要求转换语义代数等于真实 batch generation，在同一个 `MetadataWriteBatch` 内按需取得逻辑 PageId，打包只追加的 group／column／inline-extension，并生成最终 owner／column／extension／group 槽位定位。它不分配物理池，不发布 map／字段／ID，不提供通用规划回调。中性物理代际 1 仅是私有图像占位，后续 owner 绑定必须安装实际池代际。取消或拒绝后必须丢弃整个 batch，局部消耗的 ID 不成为单独发布的预留。源根／规划器释放后，输出图像、定位缓冲和逃逸记录副本继续持有资源 owner。最终协调器须在既有 QUERY／WRITE 父账户下建立共享的 ≤1 MiB 转换子账户；私有 helper 无法推断任意 PMR 的额度。超大 inline 值明确拒绝，独立大值引用的创建／内容认证／退役仍待接通。普通图元数据入口现按下述类型化请求调用私有准备，随后在物理发布前拒绝；backend／公开事务层尚未暂存此类请求，普通池／非空描述符保护保持。

## 私有字段页 owner 绑定

隔离格式为 HYBRID_PROPERTY 保留 PageOwnerKind 16；13–15 仍为退役邻接编码，必须继续拒绝。复用 SGK_PROPERTY_PAGE_POOL，不增加 SegmentKind。`bindPrivateOwner` 只消耗校验后的私有图像：要求 SEALED 的 PROPERTY catalog 项、池 owner／物理代际精确匹配、页身份／预期 LSN 一致、flags 为零、完整 SlottedPage payload 宽度。使用持续持有的账户枚举全部 live slots，认证类型化 group／column／extension 形状与语义可见性，全部成功后才以无分配操作 rebind。失败后原图像不变，可重试。大值引用信封检查仍需调用方准入，不认证内容，也不证明全局归属。

这仍是私有绑定。普通池读取匹配和独立 allocate／append／replace／evacuate 入口即使在隔离构建中也拒绝 HYBRID_PROPERTY。元数据持有的类型化协调器、全部字段读写、integrity 和精确退役接通同一原子生命周期之前，不能允许字段发布，更不能以 ROW frame 冒充。pin 下的实际树认证与描述符 codec 各负其责；codec 已修正为允许独立正数池代际大于元数据／语义代数，同时保留元数据可见性校验。

## 普通图元数据转换源与早期准备

`NativeGraphHybridPropertyConversion` 携带 Node／Edge kind、一个组 ID、持续持有的资源、协作取消、允许的物理预留范围及正数字段池 owner，不提供已选物理帧或逻辑身份，作为描述符请求的可选类型化部分。初始模式仅允许单独组转换：REPLACE、调用方 replacement IndexId 全零、不混入 Entity／Property／LargeValue／derived／ID lease 修改，且必须提供精确的预期 metadata generation；最终 map／根绑定由后续协调器持有。入口要求 MaterializedByteBudget 的 used+available 总额度不超过1MiB，缺失／超额／未知资源在源读取前拒绝；实际 QUERY／WRITE 父账户额度继续生效。

只读 Entity／Property／LargeValue 适配器在取得 writer 前完成接附，实际读取仅使用其 pin 下的当前数据库／代际／描述符根。先认证最多64个主 Entity Map 项及精确类型化记录（最后原生组仅63个ID），统计已认证 ROW 字段，合计超过4096时在解码任何值前拒绝；包含无属性的存活 owner。之后只通过保留的 Property cursor 物化这些 owner，复用 PropertyDecodeWorkspace 准入，将不可变源持有至原生分类及私有准备结束，使用真实 batch generation／ID。调用方不得提供伪造权威载荷，不能另行发布 ID-only 预留。

当前代码在私有页与稠密固定 map 规划成功后故意抛出 UNSUPPORTED_VERSION，位于物理池／最终 map／描述符发布之前。失败保留原文件／根／high-water marks，返还本次缓冲／源堆额度，并保留调用方其他持有者的信用。大值 ROW 源经现有 store 读取认证；超大 native inline 字段拒绝，不截断，也不以整份多字段 ROW 引用冒充字段。独立字段大值生产仍未安装。只有物理放置、最终 map、完整图路由／索引／overlay／归属／integrity／恢复／退役及双向转换一起就绪，才能替换此显式拒绝；拒绝测试只证明早期协调器，不代表成功转换或 R3.2 验收。


普通协调器现复用既有 BulkBTreeBuilder 规划四个角色目录。所有角色保留命名空间均须缺席，包括本次结果为空的目录；前缀与最终定位符／来源后缀共同编码为 key-only 叶记录，空目录不分配树。可重复有序来源持有不可变字段准备对象与资源，不借用调用方 ROW 或分类器。Index 池 owner 使用命名空间 IndexId，输入的字段池 owner 与之区分；实际物理槽与最终树 PageId 仍由元数据后续分配。

Bulk 规划向量及嵌套分隔键／子节点缓冲改为持有准入的 OwnedStdVector；共享规划节点及控制块使用 makeSharedOwned；原生初始树规划的回调与池向量显式持有账户。语义 CRC 的新建 backing 先准入，字节释放后退款；原生 preparer 调用时恢复其持有的分配作用域。这只覆盖规划状态，后续物理 store／batch 缓冲和完整分配／退役成本仍须在发布前审计，不能据此宣称字段已持久化或完整发布已满足暂存上限。证据见 20261019-r3-directory-plans：六个必要模块默认60/60、隔离84/84。


## 准入前的私有物理准备

在同一普通 writer／batch 内，沿输入预留范围单调选择实际 allocator 空闲槽，跳过元数据与同 batch 预留，分配新逻辑 SegmentId；字段池每池最多31帧。以POK_HYBRID_PROPERTY=16及精确池owner／generation／LSN绑定字段页，保持稳定slot身份，持有图像持续计费。对每个非空目录分配新Index池／PageId，通过既有构建器第二遍及仅限metadata的私有图像sink编码。普通公开builder入口不接收sink参数，保持既有写入行为；私有sink将认证图像移入持有缓冲，给出真实帧位置与最终根。最后按batch数据库／代际／LSN验证描述符字节，不分配其元数据帧。

位置选择仅属私有准备，不发布allocation bitmap／catalog／page map／树根／描述符，未安装生命周期保护之前无写入。后续发布必须在任何元数据分配前向batch登记全部所选槽；本次准备不能在writer释放后继续保护这些位置。分配表backing含受支持STL的proxy／对齐开销，先准入，向量销毁后退款。私有数据池字节计数包含完整128KiB预留，与已提交字节／完整转换成本区分；最终元数据／根／退役及old+new峰值仍须在准入前计入。不能由记录载荷推导收益，也不能把未填满池尾当作免费。稠密机制夹具当前的候选数据池容量高于来源ROW数据池，仅是部分成本观察，不据此改参数／门槛或授予生产准入。定向证据20261020-r3-physical-preparation：60/60、86/86。

物理字段读取现复用原生共享页池的快照、Catalog、分配位图及 Page Directory 认证；专用 reader 只接受 PROPERTY 池中的字段 owner 16，普通 ROW 入口继续拒绝该类型。返回对象持有 Buffer Pool pin 或同账户的复制缓冲，搬移后借用 view 仍有效；配额拒绝可释放后重试。读取不将物理池 epoch 与语义代数比较，字段语义仍受 group／descriptor／snapshot 约束。退休标记本身不使仍由有效旧 pin 路由的帧不可读，也不授权复用该帧。按 owner 枚举已校验有序 token、唯一归属及来源；列读取直接返回 presence／NULL／原始标量位。共享树读取现可绑定数据库身份、精确快照代数、固定命名空间及目录根，沿原有单次根解析继续认证实际页；延迟创建的扫描也保留绑定。逻辑 PageId／池 epoch 在 COW 后可不变，不能据此跨快照复用。读取上下文已串联实际空描述符读取、目录绑定、实际字段枚举与列／扩展定位，并核对 Entity Map 存活信息及排除竞争 ROW 路由；可直接附着已有 pin，在持有写入锁时不再次获取快照。候选非空描述符目前仅由测试专用入口提供，以验证真实文件中的目录与字段页；普通入口仍保留非空描述符拒绝。单字段 owning 点读已接入 Property Store 及 cursor，并保留 cursor 取消检查；兼容层只为需要 PropertyValue 的调用方物化值，类型化批消费者必须直接使用原始标量位。独立大字段复用 LargeValueStore 的 manifest／内容认证，拒绝合法哈希的多字段载体；返回值计费由调用方持有，临时引用／目录／读缓冲独立归还。Store 的单实体计数／token 及 Store／cursor 存在性已接入：计数／token 验证实际成员元数据，不读取未访问的字段体；存在性不跟随大值引用，非 NULL 检查才认证大值并检查 canonical 头，均不物化 PropertyValue。B+Tree 内部读取临时字节／slot 数组使用当前或来源原生账户，离开作用域的解码条目保留来源账户，不借用属性输出额度。完整实体 owning 读取已接入 Store／cursor：按实际 owner 成员获取 token，先准入全部编码字节，再分配返回槽位和解码；无字段返回空 optional，临时 token／目录额度独立归还。排序 selected／count／selected+count 和 cursor sink 已接入同一快照上下文；仅填充 hybrid 的原始 owner／token 位置，混合 ROW 继续原有目录路径，全 hybrid 可跳过 ROW merge。原始标量通路已接入 LogicalGraphRead → 原生 adapter／cursor → 执行器 DirectPropertyColumns：已认证列的整数／浮点原始位、布尔和 NULL 可同步写入计费的类型化数组，避免 PropertyValue 中转；缺失不等于 NULL，输入顺序／重复 ID 及重复 key 的输出位置须保留。扩展字段及 ROW／overlay／其他 reader 保留 owning 回退。15 个受影响模块已通过两种格式验证（默认228/228、隔离286/286），包括真实布尔／IEEE 位／NULL、输出额度拒绝／重试及既有直接查询／后端保护；同步回调使用命名引用，避免新增回调堆缓冲，非空生产布局仍未启用。纯 selected 批次最多访问一个实际成员以检查属性根存在性，只有请求 count 时才完整枚举 owner 成员；未访问的字段体及无关成员来源错误由相应字段读取／integrity 检查，不为投影强制扫描整个 owner。完整读取的预准入可能重复认证大值，raw 通路中的扩展回退也会重复定位；这些开销必须计入最终比较。非空描述符发布、普通字段写入、完整生命周期与成本准入仍未完成，不增加 R3 验收计数。

原生列变更现从已认证的 group／column 及原生 slot locator 规划，并在调用方账户中保留数据库／源代数 fence。no-op 与 ROW 一致（+0 等于 -0，NaN 不等于自身）；NULL、类型变化、删除及单字段最多64单元的降级均不重建其他字段，复杂新值仍借用独立计费的不可变 replacement root。兼容替换按列合并多个 owner，再按实际源 PageId 合并组／列修改；先验证全部源记录，再覆盖私有图像，每个实际源页只复制一次，无关 extension 字节保持不变。额度拒绝及已分配私有帧后的取消可退款重试，源文件／根／ID 不变。这仍是私有准备：降级明确等待 owner／extension 目录原子协调，物理位置登记及 Page Directory／池／allocator／描述符／退役发布尚未安装。不得绕过非空布局保护或据此验收 R3.2／R3.3。

描述符绑定的目录 integrity 原型现按既有固定 map 流式校验：逐组检查64个 primary／存活位置，owner selector、列 presence、extension 归属及组计数必须一致；通过全 map 基数核对排除未登记 group 中的孤立目录项，无需全库 ownership 集合，逻辑键重复在回调前拒绝。inline canonical 及单字段 LargeValue 载体均不构造 PropertyValue；当前大值认证仍通过既有 read API 物化编码体，必须计入完整缓冲开销，流式大值校验及完整 Page Directory／退役版本遍历接入尚未完成。该只读原型不回收存储，也不启用非空布局路由。

当前原生页另有反向核对：每个 live typed slot 必须等于描述符绑定的 group／column／extension 精确 locator（含 slot generation），SlottedPage 空闲字节必须与 Page Directory 一致；目录正向遍历不可见的合法校验和重复物理记录会被拒绝。空页可在描述符移除后保留复用，存在 live 记录却没有描述符则拒绝。这仍是逐页原型，未安装完整全库恢复／回收流程。即便私有列图像为空，writer 也必须先复核源 fence，才能把语句视为已提交的 no-op。

逐页反查还可在认证不可变 manifest／引用完整身份后输出 LargeValue 引用，不读取正文；共享同一值的多个 owner 分别输出。回收必须合并全部 ROW／native 来源，出错丢弃不完整访问结果，并在退役前复核完整引用快照的 fence。测试覆盖同一64KiB账户下两个 owner 共享超过64KiB的编码值、回调中断退款重试，以及错误 manifest 哈希在输出前拒绝；此引用提供器尚未接入全局回收器。
