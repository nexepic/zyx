# A7 格式版本、稳定承诺与迁移发布边界

日期：2026-10-13。范围：S1.7 的设计交付。生产格式仍为开发期格式；没有实施迁移器、布局切换或宣布稳定格式发布日期。

## 1. 当前调用链与版本身份

`StorageBackendFactory::create` 用只读 probe 检查现有文件，`StorageFormatProbe::inspect` 最多读取两个固定 4 KiB bootstrap 槽。Superblock magic/codec/CRC 决定文件是否为完整 native 格式；随后 `SuperblockFile::recover` 选取支持且有效的 generation。Segment envelope 是物理槽内的段级边界，不是文件第一个通用 header。各页/记录 codec 在实际访问时验证自身版本、owner、generation、长度、flags 和 checksum；普通 open 不等于遍历并验证全文件。

现行 envelope `RSS_FORMAT_VERSION=4`，64 字节；Superblock `RSS_SUPERBLOCK_VERSION=30`，页宽 4096、header 336 字节。二者都是 u16 小端版本空间。`version-profile.json` 记录从当前 production include/src 提取的 42 个显式版本常量及声明文件 hash；它不是归档版本推测。其余字段、类型 tag、root 归属和解释规则以同一源码 manifest 和现行存储规范为准，不从 C++ struct 的 sizeof/padding 推导 wire layout。

常见局部版本：EntityPage 4；PageOwner/SharedPagePool 2；BTreePage 3、packed/entry 1；Property root 2、property LargeValue payload 3、引用 1；LargeValue manifest 2/data page 1；Vector manifest 8/page 3/codebook 3。Catalog radix 的物理页 4 与逻辑页 5 是不同解释层。数字相同不表示互通，envelope/Superblock/local codec 也不要求同步递增。typed Page Directory bootstrap flag 与普通 radix root 的解释保持其各自已实现的 reader 分支。

## 2. 兼容矩阵及拒绝行为

| 组合/情况 | 当前行为及允许范围 |
|---|---|
| Superblock 30 + 当前合法 flags + 可达记录的正确局部 codec | 当前 profile，可读；可写仍受 open mode、fence、owner/pin 等约束 |
| envelope 4 | 其对应段允许被当前 envelope reader 解释；仍须满足 kind/state/长度/CRC |
| envelope 非 4 | envelope codec 拒绝；不能按当前字段偏移继续读取 |
| 单个 Superblock 29/31 等非 30 | `SuperblockCodec::decode` 返回 unsupported-version 格式错误 |
| 两个槽都不支持/无效 | probe 分类为 incomplete native，factory 拒绝打开；pair/select 层无有效槽时为 checksum-mismatch 错误，不伪造统一错误码 |
| 一个槽为支持且有效的 30，另一个槽无效/不支持 | 当前 recovery 选择有效槽；这是已实现的 bootstrap 故障恢复，不是支持旧/未来 profile 的迁移承诺 |
| 两个有效槽、相同 generation、内容不同 | 拒绝 split brain，返回 invalid-state |
| 两个有效槽、不同 generation | 选择更高 generation 的有效槽；不把可见性/提交确认等同于所有外部故障均已排除 |
| 当前 bootstrap，但可达局部 codec 版本/flags/类型不支持 | 在对应 reader/完整性遍历到达该记录时拒绝；错误可能为 unsupported-version 或 invalid-header，取决于 codec 与校验顺序 |
| 已知的 typed-bootstrap flag 分支 | 只按相应已实现的 bootstrap/route 解释；禁止从另一分支借用 route context |
| 旧 magic、不认识的文件、截断/空的既有文件 | factory 拒绝，既有文件不自动转换、删除或截断 |
| benchmark 属性布局原型/候选 carrier | 不属于获准生产 profile；没有 production graph route/integrity/reclamation 交付不能启用 |

这张矩阵描述实际支持边界，不生成任意局部版本的笛卡尔积。checksum 正确不代表结构或跨结构一致，也不保证识别未访问的全局损坏。发布兼容承诺必须另列 reader/writer 的明确 profile 列表；不得将“能恢复一个已知槽”宣传为 forward compatibility。

## 3. 新布局注册与解释规则

当前 A1/A2 内存接口、字段请求和暂存折叠没有新增持久 root/codec。A3/A4 的 production 编码、A5 冲突元数据、A6 scratch root 仍未定案，因此本记录不分配假版本号/枚举值。它们在 writer 接入前必须交付同一条注册记录：

1. root 或 catalog 归属、PageOwnerKind/实体种类、稳定逻辑 ID 与物理 PageRef 的关系、完整 owner/generation/bootstrap 身份。
2. 每个字段的 offset、宽度、字节序、长度/缺失/null/类型/flags、边界/溢出规则、checksum 范围以及 version magic。禁止默改现有字节含义。
3. 首次 reader 认证、查询复用证明、跨结构 consistency 检查和 integrity walker 的发现阶段；不能用 CRC 替代 A4 的一致性协议。
4. writer、所有 read/导入/维护入口、COW 原子可达发布、失败候选、旧 pin、retirement、搬迁/slot 复用及子块/大值回收。
5. 当前/候选 reader/writer profile 兼容表、拒绝测试、恢复测试和本页所要求的采用/否决证据。

实现继续使用现有 namespace、allocator、codec 和 COW/A-B 发布；新增枚举使用跨平台前缀。任何可达字节含义的破坏性改变都需新的 bootstrap profile 与相应局部 codec/version，不能仅让老 reader 忽略新字段或 fallback 到旧 generation。开发期方式为新建目标 profile 的数据库并明确拒绝旧 profile，不在同一文件内混写不兼容 A/B profile。

## 4. 稳定持久格式承诺的触发条件

库版本号、公开 Driver ABI 稳定和持久格式稳定是不同承诺。只有以下条件全部关闭，才可首次公布稳定格式：

- A3/A4/A5/A6 实际采用的布局/状态协议已冻结，S6 最终源码、受影响模块正确性/故障/恢复/资源验收完成；可选未采用机制明确留在 production profile 外。
- 上述每个已采用 encoding 的注册记录、reader/writer matrix、golden bytes、损坏/未知版本与 cross-platform 拒绝行为齐全；现行存储规范与实现一致。
- 稳定 profile 标识、支持生命周期及版本变更规则作为公开发布合同，具有明确实现 owner；不借本轮 34 项比例承诺永久兼容。
- 下面的迁移协议已具体实现并通过其独立发布门禁，或发布合同明确其首个稳定 profile 没有可迁移的旧稳定 profile；开发期数据不能被自动删掉来完成发布。

兼容扩展只在证明所有旧 reader/writer 组合安全后准入，并保持原字节含义；否则按新 profile 处理。持久格式变化、内存物理计划版本/schema epoch 与公共 ABI 各自管理，不复用一个版本值冒充三类身份。

## 5. 稳定发布前选择的迁移方式

选择**显式、离线、流式复制到新数据库**，不选择原位覆写，也不在正常 open 中自动运行。用户指定独立输出路径；源以物理只读方式打开，禁止并发 writer。保持实体逻辑 ID、端点、重复边、标签、属性的 presence/NULL/类型和值、约束与公开能力语义；目标使用独立 DatabaseId，避免两个同时存在的文件共享缓存身份。目标索引/派生表示从权威实体重建并验证，不能盲复制旧解释的页。

构建文件是本次迁移独占的、尚未公开的目标数据库；每个数据库仍是单文件。恢复/进度若持久化，放在这个目标文件内，不产生源库 WAL/SHM/外部 migration journal。先做 source/profile、路径/权限/独占、数据/索引/allocator 空间上界与内存配额预检。空间至少包含保留的源、完整目标、目标构建/COW 余量和有界缓冲；无法可靠估计时必须采用硬 quota，耗尽则终止目标构建，源不变。

流程为 PRECHECK → BUILD（有界读取/编码）→ VERIFY（完整 integrity、ID/端点/成员/精确计数、逐类型 canonical 值及约束/索引/向量一致性）→ DURABLE（目标发布与 sync）→ PUBLISH（同文件系统上把独占构建路径改为事先不存在的输出路径，并满足目录 durability）。默认不替换源路径。NaN/负零等值不能只用宽松 `PropertyValue ==` 当作无损迁移证明。

预检/构建/验证失败：源保持原字节，目标不公开；只清理本次工具创建且确认归属的未发布文件。进程崩溃：不自动把未验证构建文件提升为输出；重试/清理由迁移工具核对身份。发布或最终目录 sync 不确定：返回 indeterminate，由重开输出并完整验证来决定状态，不谎称回滚，源仍保留。不同平台的 rename/目录 sync、disk-full、短写、取消、迁移各 phase 崩溃及重开验证必须通过后才能交付工具。本轮没有实现或运行迁移。

## 6. 验证与交付边界

本次用当前 source/version inventory、Format/Bootstrap/Factory 调用链和相关 codec 模块验证现行矩阵。新增测试以 checksum-valid 的旧/未来 bootstrap 填满 A/B，验证 factory 拒绝后文件长度及全部字节不变。具体通过数与 hash 见对应 results/verification，未运行全量测试或 benchmark。

S1.7 只验收上述完整设计、发布前提和版本边界；不验收迁移器、稳定格式或尚未采用的 layout。A3/A4 的具体 codec 与未来迁移故障矩阵在其实际实现/发布前仍必须关闭。验收记录与本设计的检查项逐项对应，34 项分母不变。

## 7. 成本假设与反证

本设计不增加 runtime profile registry，也不把来源 JSON 放进查询热路径，因此不宣称提速。固定版本比较继续由现有 codec 在其认证边界执行；正常查询不得为了兼容协商重复做完整 integrity walk。新布局必须计入 header/descriptor、定位、验证、页 COW 和回收的实际字节/页成本，不能只报告值载荷。若兼容分支增加重复解析、无法有界计费，或要求跳过认证才能取得收益，该设计不能通过对应生产准入。

迁移的峰值资源按源/目标页与值字节、派生索引重建、allocator 余量和缓冲计费，完整验证不得被代表性查询样本替代。性能/空间假设在后续 A3/A4/A6 与最终窗口验证；本次 codec 模块运行时间不作为收益。
