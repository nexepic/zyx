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
3. extension：键 owner u64+token u32，一份 canonical 字段或认证独立 LargeValue 引用；共享页打包。认证 body 含 owner/token，含 framing 不超记录上限；复用已有 canonical 大字段/media 合同，不自造未认证 blob。

map 使用已有 B+Tree framing，R3.2 实测全部成本；16/28 字节只是 payload 下界。组 header 目录是每命名空间第四张共享 map，键 group u64，值使用列目录相同的 16 字节 slot locator，额外计 O(G) 成本。

明确选择新增 Superblock 物理元数据根 hybrid_property_descriptor_root，并作为额外图语义 fence 成员。它指向认证的 192 字节描述符 payload：0 magic u32；4 version u16；6 zero flags u16；8 semantic generation u64；16 database ID u64；24..63 reserved zero；八个 B+Tree 根引用均为 stable PageId u64+expected root generation u64，offset 64/80/96/112 为 Node owner/column/extension/group-header，128/144/160/176 为 Edge 对应根。空树两个字段均为零，部分零拒绝；描述符 generation 必须对 Superblock 可见。组 mode 只存在组 header。ROW 继续通过现有 entity_property_directory_root 路由，不在描述符重复 ROW 根。

R3.2 在任何文件编码前，同时分配新 Superblock 字段 offset/format version 和 metadata page-kind，配套所有 reader/writer/搬迁/integrity。首次转换和后续字段修改均替换描述符、推进额外图 fence，即使 B+Tree PageId 稳定且只改物理映射也如此。故意保留保守冲突；原七根 fence 不含该描述符；当前已追加第八个描述符成员，并使默认/实验格式具有不同 format token。

selector、列 presence、extension 存在性精确一致；无 selector 是缺失，存储 NULL 有 selector。extension 不留完整第二份行。点读验证所访问归属，完整 integrity 双向枚举发现孤儿/重复/错 owner/group/type；不在每次点读扫描全图。当前列类型不成为永久 schema。

R3.2 物理映射实现定案：复用既有 key-only B+Tree entry，以有序逻辑键前缀加 canonical 值后缀编码。owner selector 为 12+4 字节，列定位为 12+16，组定位为 8+16；这些仍是 packed framing/restart/内部分隔符/填充前的下界，值后缀参与压缩和成本计算。逻辑前缀唯一性独立于完整原生键唯一性：点读匹配项并前看一项，同前缀第二个值即拒绝，跨叶也不能漏检。类型化所有者须验证完整 integrity，并把事务内最终修改归并成每前缀一次 erase-old/insert-new；不能从同一旧快照多次未归并替换。内部 B+Tree child 仍为 U64；原生复合变更和 fresh rebuild 显式保留 EMPTY 叶布局。`FixedBTreeMap` 仅提供通用定宽编码/查找，不安装 hybrid 根、不解释定位目标、不定义图 fence。extension 变长值 framing 仍需独立类型化 codec，不能认为定宽 helper 已实现它。上述共享 map 测试使用通用 index 命名空间，不代表安装了 hybrid 字段路由或完成生产转换。

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

R3.2 原生描述符 frame 编码准备：`HybridPropertyDescriptorCodec` 使用 32 字节物理头（0 ZYXH magic u32，4 version u16=1，6 header bytes u16=32，8 physical generation u64，16 committed LSN u64，24 payload CRC32，28 header CRC32），其后为上述 192 字节 payload（magic ZYXD、version 1），其余 4096 字节页尾必须为零。payload CRC 覆盖 offset 32 至页尾，header CRC 覆盖 32 字节头且计算时自身字段归零。区分搬迁的物理代际与属性语义代际，要求 semantic<=physical<=snapshot、committed LSN 可见、database 与预期物理根代际精确匹配；物理代际还须能由 Superblock 的 59 位 packed reference 表示。八个非空命名空间根不能共用 PageId，部分零或未来根拒绝。内存 codec 检查点之后，已接入下述隔离的空描述符元数据生命周期；仍不能据此验收 R3.2 或发布非空 hybrid 属性布局。

隔离元数据原型定案：`ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT` 默认 OFF；默认 Superblock 31/header 336/profile 5 不变，非空描述符根不能编码或发布。ON 构建仅用于实验，使用 Superblock 32/header 352/profile 6，新增 physical root 位于 336..351（slot u64 + 既有 generation/frame packed u64），emergency reserve 从 352 开始，共 32 字节；其后保留字节仍为零。两种构建互相拒绝文件，不迁移。当前实验 writer/read/open/integrity 只允许八根全零的空描述符；任何非空根在分配 frame 前拒绝，合法校验和下的非空路由或错误身份也会在打开/读取时拒绝，不能静默回落 ROW。普通 Node/Edge/ROW 能力继续可用，但实际子块转换 API 尚未安装。

描述符是 metadata pool 中独立的物理 frame，不分配 routed MetadataPageId，不加入 12 根 logical bootstrap 列表，也不伪装为 State/radix。元数据池恢复、只读发现、可达性、旧 pin 保护和保守整树搬迁显式访问它；局部搬迁遇到额外物理 frame 时保留原来的保守 fallback。搬迁更换 physical generation/LSN 并保留 semantic generation。清除 root 后按现有 pin/reachability 协议回收，不另加退役 sidecar。第八根 fence 在两种构建均存在（默认文件根为零），format token 区分 31/32；该根变化不能被误认成 ID-only 预留或盲邻接 rebase。文件发布仍使用既有 data fence → inactive A/B → superblock fence。完整成本必须计算实际 4 KiB 描述符 frame、128 KiB 保留池及旧 pin 占用，不能仅计 192/224 字节。非空根保护只能与完整字段 reader/writer/integrity/retirement 一起替换。证据：`20261019-r3-native-root/`。

已打开句柄取得新 metadata snapshot 时也认证描述符；失败必须释放刚取得的 pin，不能只在首次 open 验证后让新快照静默沿 ROW 继续。描述符读取使用文件 view 构造时固定的 database ID，避免与 writer 对 mutable routing Superblock 的 rebind 竞争。PROFILE 固定计费 registry 记录 `native_graph.read.metadata_decodes.hybrid_descriptor`（当前线程累计解码尝试数）；未增加资源上限，registry/staging 上界按实际条目数派生。

4096 个字段归属仅限制一次显式转换计划，不是转换后组的永久容量上限。组头用既定 u32 字段记录实际数量；普通写入必须保留现有动态属性能力，并检查计数运算溢出。正向／反向转换超过计划工作上限时仍应拒绝，不能截断、静默拆分发布，也不能把 4096 解释成永久模式限制。

## Extension 信封准备

尚未安装路由的 R3.2 extension codec 使用 48 字节 ZYHE（`0x4548595A`）信封，版本 1：magic u32 位于 0，version u16 位于 4，owner kind u8 位于 6，mode u8 位于 7（1=inline，2=LargeValue 引用），原生 owner ID u64 位于 8，token u32 位于 16，body 字节数 u32 位于 20，语义代数 u64 位于 24，32..47 保留为零。inline body 恰好为一个现有规范 PropertyValue；缺失时没有 extension／selector，NULL 则保存规范 NULL。认证外层 owner/token，要求 extension<=group<=descriptor<=snapshot，引用的逻辑代数不得超过 extension。

大值模式复用现有 LargeValueRefCodec 和 `application/vnd.zyx.entity-property-root` media，但无 owner 的内容必须恰好包含一个 token/value；目录 token 和字段 token 均须匹配外层。多字段、尾随数据、错误 kind/media、未来身份都拒绝。现有 LargeValueStore 必须先认证 manifest、chunk 和内容 hash，再提供数据；信封 decoder 本身不认证内容。不能再复制整行，也不能把任意同 media 的整行当作单字段。相同单字段内容允许沿用现有跨 owner 不可变内容去重及精确退役规则。

完整 extension 记录必须满足真实目标上限，且至多 3952 字节（4032 字节 owner payload 减去 64 字节 slotted header 和 16 字节槽位）。若放入 packed B+Tree key，还要容纳 owner/token 有序前缀、packed framing、页头／槽位和内部 separator；后续 map planner 必须据此降低记录上限，不能假定 3952 字节记录能直接放入 B+Tree key。真实成本须计入 48 字节信封及额外目录／值编码开销。目标缓冲区和规范编码工作区使用调用方 PMR；引用 media 容量及字段／目录解码须分别持有预约。已计费转换计划只能输出实际 inline-extension 成员，拒绝 COLUMN 来源和超限值。此 codec 不安装 variable map 路由、不分配 data ID、不发布大值，也不移除非空根保护。
