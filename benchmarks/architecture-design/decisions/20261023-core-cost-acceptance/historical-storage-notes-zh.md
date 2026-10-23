## 19. 历史记录

本次清理前的完整规范与 S0–S6 过程记录原样保存在 `benchmarks/architecture-design/archive/20261019-completed-s0-s6/zh/`，上级 manifest 保存 SHA-256。更早记录在 `20261011-completed-optimization/` 和 `20261009-superseded-plans/`。这些是审计历史，不是实施入口；旧的待批准、待测试和中间百分比不再表示当前状态。


#### 2026-10-20 原生候选池复用与恢复补充

实验格式32的正向转换／字段修改／受控实体与 LabelSet 删除可在元数据私有影子容量中选定已认证的共享池空闲帧，保留精确旧 pin，最终共同发布；每池31帧，固定容量提示只作候选。接收池不得同批退役，同批后续追加不得再清理为孤儿。变化根按实际物理池代际绑定。中断物理写入与可写重开通过写锁内流式验证／恢复再允许新发布；普通未适配写与一般原生池 reconciliation 仍拒绝。默认格式31不变，完整成本及生产准入仍未完成。证据：`benchmarks/architecture-design/decisions/20261020-r32-native-pool-filling/`。


只读 allocation census 另按完整128KiB池核对全部持久allocator位、Catalog、退役／搬迁源和应急元数据reserve；缺失归属／遍历不完整不授权布局，元数据framing包含在完整池内，文件high-water单列。当前只提供已分配成本观测，不是转换前估计器或生产准入凭据。证据：`benchmarks/architecture-design/decisions/20261020-r32-full-allocation-census/`。


格式32显式正向转换可按元数据私有签发的源／完整准备候选实配判断10%字节及准入峰值／文件阈值，拒绝保持ROW。最终metadata／route仅准备一次，刷出dirty页后观察、再选择A/B发布。它允许私有I/O，不是无分配预测；原WRITE／StorageIO硬上限仍有效。无描述符的ROW源在放弃／重开后也须恢复未发布追加再写。默认31未切换。证据：`benchmarks/architecture-design/decisions/20261020-r32-prepublication-allocation-admission/`。

### 2026-10-20 R3.2：完整 map 显式事务暂存与相同槽位更新

证据：`benchmarks/architecture-design/decisions/20261020-r32-map-transactions/`。新增显式 MERGE／REPLACE 暂存入口：合并保留未指定字段，替换删除省略字段；只对真实变化生成字段更新，区分 stored NULL 与缺失，沿用 ROW 的 signed-zero／NaN（含嵌套）比较。活的 propertyless native owner 可认证；大值不变时复用，局部删除不推断全局退役。空合并与相等 map 是有来源 fence 的无写入提交。暂存值归同一预算所有，取消／额度拒绝不遗留半个请求，move／rollback／冲突释放所有权。

宽 map 定向测试发现同长度 extension SET 先删除再插入导致 locator 改变、多写目录帧。修复为认证旧记录后在私有 COW 页保留同一槽位覆盖；仅增长需重新放置时更新定位。192 字段 map 修改一个字段最多准备字段／group 两帧，旧 pin 与全库完整性保持有效。保留失败日志，未放宽帧数断言。

新增12个定向测试。仅验证38个相关模块，默认530项通过；候选799项通过、1项条件跳过。未跑全量单测或中间性能基准，仅验证Linux。入口仍限单个64-ID组、64个owner、输入4096字段、最多64个实际变更和1MiB；不是通用无界map事务。schema／统计／查询索引／未知State及通用拓扑协调、同执行器对照与R5／R7仍待完成，生产保护保持。**总计13/24（54.2%），无验收增长，不提交、不推送。**

### 2026-10-20 R3.2：未知 State 发布保护与同执行器正确性对照

证据：`benchmarks/architecture-design/decisions/20261020-r32-state-query-parity/`。新回归真实复现未知State存在时字段／map修改仍能发布；原来只有owner删除检查。现将同一有界State census移到全部字段修改的共同发布入口，保留规范maintenance journal、拒绝未知派生State／未来代际／不完整census；拒绝前不写私有帧，不更改根／属性／State，原额度退款。未知State并未获得通用写入支持，schema／派生索引保护未移除。

增加同一LogicalPlanReadRuntime执行器的端到端正确性对照：通过公开backend建立schema／属性索引、64节点与64条多重边，用相同查询依次核对ROW、Node候选、Node＋Edge候选、两次逆转换后的结果。覆盖范围过滤、count／sum、字符串与缺失NULL、OPTIONAL与聚合、多重边遍历、ORDER／LIMIT，基线64节点及sum2016有独立断言。每次只读重开后执行，不执行性能采样。初版夹具漏填分页边界，修正为显式正值，失败日志保留。

最终仅运行39个相关模块：默认531通过；候选802通过、1个条件跳过。未跑全量单测或中间性能基准，仅验证Linux。这是小规模语义对照，不代替完整形状／空间矩阵、同条件性能结论及生产准入。schema／统计／查询索引的写入协调、通用拓扑与剩余R5／R7继续待办。**总计13/24（54.2%），没有验收增长，不提交、不推送。**

### 2026-10-20 R5.1：从最终 Edge 状态协调覆盖树双向更新

证据：`benchmarks/architecture-design/decisions/20261020-r5-covered-final-state-coordinator/`。为既有覆盖树实验根增加显式事务协调入口，从最终Edge创建／删除记录生成两个方向的派生变化，不再要求调用方重复抄写endpoint／type。发布前逐项核对旧方向与权威Edge完全一致，新Edge不能覆盖已有候选键；同事务先创建后删除不留下派生记录。保持主Edge认证和全候选integrity；不安装默认backend路由，不新增持久格式或保留IndexId。

入口只允许非schema／非内部保留命名空间、最多64个变化Edge及同一最多1MiB账户；直接手工修改同一候选根与自动协调不可混用。成功后封住继续暂存（含move之后），拒绝／取消后必须成功重试或rollback，不能忽略错误只提交主Edge。PMR合并器／边界缓冲受此账户约束，既有事务暂存和WRITE硬限制保持原合同；没有把局部额度宣称为全部发布内存。

新增4个回归覆盖自环、多重边、最终状态、旧pin、移交封口、命名空间、额度／取消／变更数量拒绝与可重试、合法CRC错误反向项的拒绝及防止失败后漏维护提交。既有只读重开与A/B两种故障测试改走协调入口，断言仍核对主Edge及两方向一致。

40个相关模块的扩大回归：默认545通过，候选816通过／1项条件跳过。随后收口失败提交保护与命名空间，重新构建两种格式并各跑覆盖树14项，均通过。未跑全量单测或中间性能基准，仅验证Linux。原始候选根构建仍显式，完整backend type索引／统计／overlay协调、访问计划／留出负载与R7准入继续待办。**总计13/24（54.2%）；没有验收增长，不提交、不推送。**

### 2026-10-21：R5覆盖邻接协议原型边界

R5.1协议／完整生命周期原型的字节、故障检测与资源合同见`benchmarks/architecture-design/decisions/20261021-r5-lifecycle-protocol/protocol-zh.md`。它复用既有key-only Packed BTree与Index Root／Page Directory，不新增持久metadata根或格式版本；两种构建均可使用显式实验入口。候选root属于调用方实验namespace，默认图读取仍走canonical key-only邻接。只读对照入口自行认证并保留真实pin，不接受外部报告授予权限；真实overlay仅在测试transaction内暂存，不能写只读文件。Node-only与Edge双向／type索引／统计、A/B、旧pin、空root重建、搬迁／退役已验证；生产writer hook、持久采用协议、成本规划与R7准入仍未完成。


### 2026-10-21 R3.3 整批准入与 R7.1 最终候选冻结

证据：`benchmarks/architecture-design/decisions/20261021-r33-cohort-prefix-admission/`。首组转换的实际候选2097152字节高于源1835008，门禁正确拒绝；不能借用未来8组的收益。现增加显式最多8个连续64-ID组的纯列维护转换，每组仍最多4096成员、整体最多512所有者，仍共用同一个至多1MiB账户。全部私有图像、ROW移除、目录、退休与完整成本在一个MetadataWriteBatch内，只在最终空间／峰值／文件门禁通过后发布一次A/B。无新增持久编码／格式、自动晋升或资源额度。已清空ROW页及时只保留ID，避免保存无用页体。

原生事务路径的8项定向检查通过，另有共享额度拒绝1项、公共接入1项通过；候选9个生命周期模块290/290，默认9/9，默认格式提前拒绝1项。分批验证，不是一次总和运行。覆盖完整发布、部分／完整A/B、取消、后续非纯列组、整批收益不足、低额度退款与旧pin。成熟整体源1835008、候选1441792（−21.43%），退休1179648、峰值2621440、文件7208960；字段393216、混合目录262144、元数据393216、实体131072，bootstrap／应急各131072。默认自适应ROW公共接口建库另验证2621440→2097152（−20%）：全部512所有者旧／新值、重开、聚合、64字段完整实体、普通更新、重复／缺失相关OPTIONAL及关闭后拥有型输出均通过。最初误用只读QueryEngine构造器的fixture失败保留，未将它归因于存储。

R5最终候选选择canonical key-only树与有界主记录复用，覆盖／共享分段不自动生产采用；完整理由见`20261021-r52-final-path-selection/`。R3／R5的最终时延和保护准入仍未提前验收。

**R7.1已验收，总进度16/24（66.7%）。** 最终源delta、before／after二进制与静态引擎库、构建选项及必要模块／完整规模正确性冻结在`20261021-r71-final-candidate/`。medium原19项加修复后的失败项、holdout20项均保持正确，按不同二进制来源记录；两套图索引8MiB只读核验及各6项原始／优化计划对照通过。secondary-index输入另完成每套2次DDL及4个受影响语义合同（12/12）；DDL的实际返回为“Index created”，初始零行oracle错误已单列保留，没有重复已成功DDL。通用完整性扫描的8MiB资源拒绝仍明确不是通过。仅Linux／必要模块，无全库单测。尚未启动新的正式计时窗口。按用户规则，进度增长后提交并推送，随后继续R7统一读写／资源／稳态／Kuzu验收。

### 页级空闲摘要候选合同（P7，尚未验收）

复用既有 U64 B+Tree 布局，预留 Node/Edge IndexId `0x8000000000000040`／`0x8000000000000041`；普通索引 CRUD 不得修改。版本 1 的五字节标记键为 `00 46 53 01 00`（目录语义 generation 加一）和 `00 46 53 01 01`（部分占用页数加一）。候选键为 `01`、大端 `uint16(65535-free_slots)`、大端完整 `uint64(PageId)`，值为一；仅记录有活记录且未满的页。未知版本、缺失标记或非法键拒绝解释，不升级文件格式版本或改变原有页编码。

源证明保留精确 `PhysicalPageRef`，发布端核对它与当前实体目录根完全相等，并核对目标目录根的 generation 等于摘要标记。每种实体目录每次发布使用唯一提交 generation；物理搬迁改变该代际时摘要安全失效，普通写回退，显式重建可重新绑定。空目录要求候选数为零。实际候选仍核对当前映射、页身份及真实空槽数。普通写只验证访问路径；完整 integrity 枚举权威实体页，逐项核对候选、计数和源代际，发现漏项、重复及孤儿；代际过期的摘要也必须与全部权威页一致才通过。

旧文件没有摘要时继续原路径，禁止打开或普通写暗中全量初始化。显式 `rebuildNativeEntityFreeSpaceSummaries` 在既有写预算、取消检查、共享发布锁及原子事务内扫描并重建；失败保留原根，不发布部分摘要。新空目录可随首次正常写原子建立摘要。初始化时间、资源及文件增长必须单列，不能混入后续普通写收益。

BufferPool 的局部写失效候选保留原哈希点查及 2Q，在同一 CACHE 账户内增加按物理偏移/代际排序的非拥有索引；主表与索引同锁登记/回滚。局部及范围失效为 O(log N + R)，其中 R 为实际重叠驻留项；预检所有受影响 pin/private 页后才删除或写回。脏页以 resident Entry 内嵌链跟踪，flush 为 O(D)，不扫描所有 clean 页，也不在标脏时分配。新增索引/链元数据继续计入原缓存额度，不增加 payload 容量；额度不足仍安全驱逐或拒绝。这是运行时数据结构调整，不改变持久页格式。

在 bootstrap 布局下，LocationDirectory 与 Catalog 的普通 COW 直接输出既有版本的逻辑子页 ID：未修改子项保留父页中已认证的 ID，修改子项使用本批已知的子页 ID，删除清空对应 ID。新建内部节点也必须使用逻辑子项，不能因父节点已逻辑化而留下新的物理子树。旧物理子项仍按原合同转换；最终逻辑化入口对已逻辑化候选幂等，不再分配第二份父页或读取兄弟页找回 ID。保留实际路径认证、route 注册、旧 pin 与故障原子性，不改变持久页版本；未访问分支由首次访问或完整 integrity 检出。

SlottedPagePool 的选页空闲索引从所选池已分配帧的有效 owner ID 建立，最多检查 31 个帧，并对当前 pin 的 Page Directory 映射核对 pool／frame／generation；旧 COW／跨池移动只接受该 pin 指向的确切帧。活页继续执行原页身份、payload 与 SlottedPage 校验。owner 无法解码或代际无法认证时，必须回退原权威目录扫描，不能把失败解释为页不存在。完整 integrity 仍检查目录与帧位图的全局一致性；该运行时供给优化不改变格式或缓存额度。

同一 owner 的属性值覆盖可复用原 SlottedPage 槽身份：`overwrite` 保留逻辑 PageId／slot／slot generation，实际数据帧仍 COW 并通过该快照的 Page Directory 路由。槽代际表示删除／重用身份，不代替值版本；删除后再分配必须递增代际。页容量不足才擦除原槽并显式搬迁。同页多 owner 合并必须基于一份私有最终页；多个单独可容纳的增长合并后溢出时，只搬迁溢出的 owner，并保留 LargeValue 规划所有权。

发布端先完成全部实际源映射、owner LSN、槽／owner 身份及替换图像认证，再省略与当前映射完全相同的 EntityPropertyDirectory upsert；重复／冲突请求仍拒绝。目录记录位置，不记录值版本，因此目录根未变不得作为值未变的证明。实际 Page Directory、数据帧、retirement 及 A/B 继续同批原子发布；旧计划即使槽身份未变也必须因源物理页／LSN fence 过期而拒绝。该候选不改变持久格式、额度或完整 integrity 合同，性能尚待 P7 验收。

### P7 紧凑短文本字段页候选合同（尚未验收）

已冻结的短文本 9／35 候选复用既有 64-owner 分组、字段目录、成员位图、POK_HYBRID_PROPERTY 页、COW／退休与原子发布；不将每块独立 LargeValue 载体用于普通短字段。默认构建仍为 5／31 ROW。该历史候选实验 profile 为 9／35（组内列位置阶段为下节冻结的 11／37，当前未完成候选为 13／39），保持 352 字节 Superblock header，拒绝旧实验 7／33、6／32；没有原地迁移或默认采用。旧实验文件及二进制作为冻结证据保留。

短文本列新增 scalar kind 4，仍为 64 字节列头＋64 个 little-endian uint64，共 576 字节，沿用既有 namespace／group／token／semantic generation 与 presence／null／live 位图。一个非 NULL 字的高 8 位保存 0..7 的字节长度，低 56 位按输入字节顺序保存至多 7 字节；未使用字节必须为零。长度大于 7 或非零尾部为损坏。空串为长度零的零字，NULL 与缺失由原位图区分，二者 payload 必须为零。文本是原始字节，支持嵌入 NUL、多字节文本与空串，不截断字符或假定 ASCII。超过 7 字节、混合类型及复杂值保持既有 extension／LargeValue／ROW 路径，数据库的字符串长度支持不因该内联候选缩小。

转换仍要求同字段至少 48 个现有成员，所有非 NULL 值同型且可表示；全 NULL 不猜测类型。短文本等值／no-op 按完整字节比较。单项更新保留同组列时只替换既有列页／必要目录路径；不可表示的值或密度跌破门槛走原有整列降级，最多恢复本组 64 个成员，保留旧 pin 与原预算。复制到 owning 值或类型列时才解包；同步文本 sink 在栈缓冲有效期内消费，禁止把栈视图排队到下一次回调。正式生产批读取仍须按组认证并避免逐 owner 重走字段目录，且满足原共享预算、取消、真实实体／标签／索引验证；字节 codec 通过不等于采用完成。

该候选的字节／初步生命周期检查见 `20261022-core-cost-native-short-text/`：实验格式 180 项相关模块在原成员布局与 compact-membership 两种配置下通过，默认格式 35 项通过；扩展后的短文本单项测试另验证空串／NULL 转换和 no-op。未执行全量单测或性能计时。原生混合布局删除现在依据认证的 Entity 源／最终页，原子更新既有空闲摘要的变化键与计数；没有摘要不隐藏初始化，过期证明保持过期。私有 B+Tree 图像入口分别检查摘要标记／候选的真实键值编码，不能按普通索引 Entity-ID 后缀解释；普通保留索引 CRUD 仍被拒绝。旧失败及修复证据保留。该轮不授予默认采用或 P7 性能验收。


按组读取执行合同：紧凑单字段选择最多 64 个有序同组 owner，通过有序 Entity／Property 目录批认证实际存活与无竞争 ROW；混合组继续验证 extension 权威。仅在完整列认证后同步发出标量／短文本 run，栈内文本视图不得逸出回调。共享原预算，准入拒绝仅可在任何输出前回退，消费端异常不能回放；counts／多字段／不支持形状保留原路径。`20261022-core-cost-native-group-runs/` 的 181 项实验模块检查在两种 membership 配置均通过，默认 35 项通过；这不授予默认采用或完整空间／性能验收。


混合布局物理退役回收合同：仅已有持久 Retire Queue 记录可进入 `reclaimRetiredPhysicalSlots`；逐项认证 Catalog 状态／槽身份、allocator 位图、退役代次及所有活动快照。只释放不再可见的已退役段或已搬迁旧槽，不发现或创建新的属性／字段退役，不改 descriptor、实体、字段或查询索引权威。该窄维护入口可通过混合布局写准入，不能开放其他未适配写入或普通池 reconciliation；空队列／全部仍被 pin 时零发布。故障仍按原子 A/B 发布和恢复合同处理。


### 已冻结隔离候选：组成员内的列位置（11／37，未通过采用门禁）

只为紧凑 `COLUMN_MEMBERSHIP_BITMAP` 组增加直接列位置；默认 5／31 ROW 不变。新实验 profile 11／Superblock 37 明确拒绝旧 9／35、7／33 和 6／32 文件，不做静默迁移。成员记录版本 2 保留 64 字节头、已排序 token 数组和原 owner 位图，尾部按相同 token 次序追加每列 16 字节位置：PageId u64@0、slot u16@8、零 u16@10、slot generation u32@12，均小端。每个已发布列必须有有效位置，未使用的解码位置为规范空引用（PageId／generation 为 0、slot 为 NO_SLOT），不编码这些数组项。逻辑构造阶段可暂未绑定位置，但未绑定记录不得编码或发布；访问时认证实际列的 group／token／代次／presence，完整 integrity 检查全部正反引用与无主物理记录。最多 85 列，记录最大 2788 字节，保持既有页、64-owner、4096-member、1 MiB 和 CACHE 边界。

紧凑列的唯一定位权威改为组成员记录；新转换不向 FieldDirectory 写紧凑列，读者不得回退到竞争树。非紧凑组继续既有 FieldDirectory，混合组的 extension 仍有独立 owner／extension 权威。完整检查必须拒绝紧凑列在树中的竞争条目。兼容 SET 保留 PageId／slot／generation，数据页仍 COW；列降级／删除原子移除成员位图、token 和对应位置，剩余位置不变。转换先准备列位置，再将成员和 group 放入同一帧，所有字段／组／目录／ROW 移除在同一 A/B 发布；旧 pin 保留原位置。不得通过改 high-bit PageId 顺便引入尚未证明的组 ID 命名空间。此候选须独立通过字节、损坏、读写／降级／删除／逆转换、旧 pin／重开／故障、完整空间和生产查询门禁后才可采用；不授予性能进度。


### 当前核心候选：组内热列与冷 ROW 块（13／39，集成中／未采用）

依据 `20261022-core-cost-direct-locators/input-field-shapes.json`，两套真实输入的全部节点均有长 ID。下一候选只处理该混合布局问题：保留热字段列，将非列字段合并为至多 64-owner 的冷 ROW 块，复用现有 canonical ROW／整根 LargeValue 引用及 LZ4／RAW 算法；不为每个冷字段建立 OwnerMap／ExtensionDirectory，不新建 backend、索引或缓存。该候选需要独立 profile 13／Superblock 39，拒绝旧 11／37 等实验文件；当前实验源码已切换到 13／39，默认 5／31 不变；读取已按下述新证据接入库内入口，实际转换与变更／完整 ownership 生命周期仍未完成，不能生产采用。以下是实施合同，不是完成声明。

**唯一权威与位置。** 新组模式 `HPGM_COLUMN_ROW_REMAINDER` 使用成员记录版本 3。热字段仍按 token→列位置认证；冷 owner 位图及冷块位置也在成员记录中，冷字段不进入普通 EntityPropertyDirectory。原“真实 Entity 存在且无竞争普通 ROW 根”的访问检查保留；不能简单跳过。热 token 在整个组中由列唯一拥有：若 token 已列化且该 owner 的成员位为假，则为缺失，不回退冷块；成员位为真则只读该列。只有未列化 token 才读取该 owner 的冷块，由 canonical ROW 目录判定存在／缺失／NULL。冷 ROW 的任何 owner 都不得保存组内已列化 token。完整检查验证热／冷 token 不重叠、所有 owner／字段计数、全部正反引用、无主记录和不存在竞争逐字段树。未访问冷字段的损坏在首次访问或完整 integrity 时发现。

`HPS_COLD_ROW` 只表示库内枚举的冷源，旧 OwnerMap 的持久 source 语法仍仅允许列／扩展（1／2），字节 3 继续拒绝。

成员版本 3 保留既有 token／owner 位图／热列位置；header@40 保存冷 owner mask u64，@48 保存冷字段总数 u32，@52 保存行块模式／位置编码，@53 为零，@54 保存分组位置条目数 u16，@56..63 为零。只要是新模式，允许热列数为零；全部降级后仍可保存冷 ROW 和原生 owner 生命周期。冷位置的解码形态固定为 64 个 SlotReference；线性编码在“按冷 owner 依次存 16 字节位置”和“每个不同块存 mask u64＋16 字节位置”之间选择更短者，同长选前者。分组条目按首个 owner 槽排序、mask 非空且互斥、并集精确等于冷 mask；相同块不能重复成多条。未使用的解码位置为规范空引用（PageId／generation 为 0、slot 为 NO_SLOT），不编码这些数组项。最坏编码至多增加 1024 字节，成员记录最大 3812 字节；成员与 group 仍须同帧，原页／1 MiB／CACHE 边界不变。

**冷块字节与资源。** 冷块是既有 POK_HYBRID_PROPERTY 页内记录，magic=`0x5243595A`、版本 1、64 字节头。未压缩体最多 4096 字节，最多 64 行；超过上限就拆块，不提高额度。RAW 为通用回退；有既有 LZ4 支持且实际缩小时才编码 LZ4。行内容复用完整 canonical ROW root 字节；大根使用既有 owner-keyed ROW LargeValue 引用，实际引用／media／manifest／目录仍必须认证，不能跨数据库借用引用。压缩块不是 LargeValue 新 backend，也不分配独立 Manifest 池。

| 冷块头偏移 | 字段 |
| --- | --- |
| 0／4／6／7 | magic u32／version u16／codec u8（RAW=0、LZ4=1）／owner kind u8 |
| 8／16／24 | group ID u64／semantic generation u64／owner mask u64 |
| 32／36 | 未压缩体字节 u32／实际存储体字节 u32 |
| 40／42／44 | 行数 u16／目录字节 u16／字段总数 u32 |
| 48／52／56 | 未压缩体 CRC32／存储体 CRC32／LargeValue owner mask u64（须为 owner mask 子集） |

体以每行 4 字节的结束 offset 目录开头；owner 槽由头部 mask 的置位顺序导出，不重复保存，根种类由 LargeValue owner mask 决定。inline 行直接保存既有 canonical ROW root，由真实根头／目录取得字段数；LargeValue 行保存字段数 u32＋既有 owner-keyed ROW 引用。再连接行字节；mask／计数／连续边界必须精确匹配。这消除根内已经保存的 owner／类型／计数在逐行目录中的重复；旧 12 字节原型在实际 ID 形状中需要 4237 字节、超过一帧，其失败证据保留，不能通过放宽页上限来通过检查。RAW 编码后仍须满足原 slotted record 上限，必要时拆块或使用既有大根引用。分配解压缓冲前先认证声明上限、存储长度和 CRC，解压必须恰好得到声明长度，再验证未压缩 CRC、所有行边界和 owner／类型／目录身份。只有已认证块内的选中字段可以同步借用；后续块失败不得 seal 或重放先前输出。最多复用原游标的一个 4 KiB ROW 缓冲表示来保留解码块，不能额外叠加第二个 ROW 缓存；类型化批次的临时缓冲仍走原共享准入。

**发布、变更与迁移。** 转换必须从实际源 ROW 构造冷根，按组打包并实际退役已脱离的源页／整池；热列、冷块、成员位置、group、Page Directory、LargeValue、普通 ROW 映射移除及派生索引在一次 A/B 发布中共同生效。兼容热更新不重写冷内容；冷更新或热列降级只重建受影响的有界块并同步计数／位置，删除处理完整 owner，空冷根移除位置；旧 pin 始终保留旧图。先完成 cold ROOT／LargeValue 读写与跨表示、取消、故障／退款、逆转换和全部物理反向检查，才允许真实输入转换。多组混合转换仍最多 8 组／512 owner／4096 member 每组／1 MiB，总成本与最终空间门禁不放松。若有界前缀不能通过全库门禁，使用尚需定义的私有目标分段构建，不能发布不合格前缀或临时取消准入。所有步骤完成后才进入 P7，当前进度不增加。


块／位置字节原型的初步证据见 `20261022-core-cost-cold-row-codec/`：13 项必要检查在实验、默认、仅 codec 禁用 LZ4 的变体通过，覆盖 RAW／LZ4、身份／目录／CRC／膨胀上限、稀疏 owner、取消／退款、引用保持、最小位置编码与非法别名表示。实际 ID 形状的未来页内几何为 4013 B；它包含未来成员表的布局计算和实际位置 codec，不是已持久化的完整页。该冻结证据阶段未切换格式。其后的实验源码已切换为 13／39，并接入成员版本 3、同页 group／成员校验、冷块私有页准入和借用读取接口；该成员／页接口冻结阶段尚未接入 Native 转换／生产读写／反向 ownership；后续读取接入见下方新证据，进度仍为 90%。无 LZ4 时按原界限使用 RAW 拆块，不承诺相同压缩密度；引用的实际 LargeValue／字段计数仍须下一层存储认证。


当前格式／页接口检查见 `20261022-core-cost-row-membership-v3/`：实验基础 66 项、默认 57 项通过；扩大后的相关生命周期检查在普通及紧凑 membership 配置各 253 项通过。后者证明既有列／扩展路径未回归，不是冷 ROW 生产生命周期完成。未运行全量单测或新性能基准，不增加验收进度。


冷 ROW 根的选读组件见 `20261022-core-cost-cold-row-inspection/`：实验／默认各 21 项必要检查通过。它复用 canonical ROW 与现有 LargeValueStore，按冷块真实 generation 校验整根引用，认证实际 media／manifest／内容哈希及目录长度／字段数，拒绝组内任何热 token，再同步借用选中 canonical value 字节；不构造嵌套 PropertyValue。错误源、未排序请求、取消、额度拒绝、消费者异常与已释放 pin均不输出／重放或泄漏账户。未加入新缓存或提高额度。该选读组件冻结阶段尚未接入 Native 路由；后续接入见下方读取证据。实际转换／变更／完整 ownership 仍未完成，不是冷 ROW 生命周期或性能验收。


当前读取代码证据见 `20261022-core-cost-cold-row-routing/`：点查、NULL／缺失、冷热 token 有序枚举、完整值、实际 LargeValue 和同步类型化借用均使用既有 Native 读取入口；热列批次仍执行全部 Entity Map／竞争普通 ROW 校验，并不解码冷块。使用已有私有候选描述符和真实页／索引／Entity Map 的隔离夹具，不能解释为公开转换后真实生产查询已完成。source 字节／字段格式仍为 13／39 和默认 5／31。普通及紧凑配置各 282 项相关模块检查、默认 68 项通过；未运行全量单测或新性能基准。原描述符检查中的旧版本 32 断言按当前明确合同更新为 39，原始失败保留。该读取冻结阶段的变更回调、group 变更入口及完整 ownership 仍拒绝新模式；后续只读字段图核验见下方，变更仍未开放；特别是 LargeValue 可达性枚举／退役尚须覆盖冷 owner，完成前不得发布新模式数据。

| 读取请求 | 必须读取／证明 | 不应执行 |
| --- | --- | --- |
| 已列化 token 且 owner 成员位为真 | Entity／无竞争 ROW、组成员、选中列 | 读取冷根 |
| 已列化 token 但成员位为假 | 同一组／owner 的成员证明，返回缺失 | 回退冷根查同 token |
| 未列化 token 且 owner 有冷根 | 实际冷块及 canonical ROW／LargeValue 的选中字段 | 借用旧 EXT 目录作为权威 |
| 仅判断 owner 是否有属性 | 已认证的热成员位或冷 owner 位 | 为有无属性解压冷根 |
| 精确字段数／枚举／完整值 | 实际冷目录，与热 token 有序合并 | 从组内冷字段总数猜 owner 字段数 |

游标借用同步 `ColdRowRepresentationLease` 交接已有 ROW 表示：只在真正解码冷块前释放原图像；热读取不触发交接，普通 ROW 局部性不变；嵌套作用域和异常恢复父交接函数。回调与借用字段不能逃逸作用域，后续失败不能重放前面的输出。单字段类型化冷选读已在同一既有窗口按同引用连续段复用解码体；多字段／精确计数选读现也通过同步借用复用同一解码体，不新增第二份 ROW 缓存。


冷字段图的只读完整性证据见 `20261022-core-cost-cold-row-ownership/`：普通／紧凑配置各 293 项相关模块检查通过，默认 68 项通过。每组实际校验全部 Entity Map／无竞争普通 ROW 及无旧 owner selector；每个热列的位置／presence 必须与成员表完全匹配。冷位置按不同 SlotReference 遍历，每块只保留一个有界解码体，核对完整 owner mask、实际每行目录／字段值、冷热 token 不重叠、实际 LargeValue 和跨块字段总数。冷源不建立逐字段 selector，因而全局旧 OwnerMap／FieldDirectory／ExtensionDirectory 基数仍排除孤儿条目。报告 `cold_blocks` 统计不同物理块引用，`cold_rows` 统计非空冷 owner，不把别名计作多个物理块。

反向阶段逐个当前 Native 物理页确认 group／成员／热列／冷块的精确位置与 slot generation，拒绝重复／无主块及成员记录。冷块的 owner kind 在 offset 7（offset 6 是 codec），RAW 的 Edge 检查避免把 codec 误当 owner namespace。实际 LargeValue 引用须匹配 owner／media／冷块 generation，再验证 manifest／内容与真实目录；物理引用 visitor 每个引用行只交付一次，异常不重放。只有目录正确不代表物理记录唯一，两阶段必须共同完成。

这些组件已接入既有全库 scanner 的字段图调用，但测试仍使用真实页／索引及私有候选描述符；实际公开转换后的全库、旧 pin／故障／搬迁／退役尚待验证。原始失败记录保留：直接冷读取缺少显式账户的夹具已补账户；独立 LargeValue 的逻辑 generation 常为 1，不能用 metadata epoch 推算。未来引用故障现在显式伪造晚于块、但不晚于组的 generation，并断言在实际 manifest 不匹配之前以本地 INVALID_HEADER 拒绝。未提高额度或新增缓存／存储格式，未跑全量单测／性能基准，进度仍 90%。

**生产发布仍关闭。** 既有回收扫描器已接入冷整根枚举，正反字段图核验仍不能替代实际公开转换后的回收／旧 pin／退役及完整变更生命周期；不得从不完整的 mark 或未耗尽的扫描推导可回收性。


可达性扫描实现合同（已接入既有扫描器）：按稳定 EntityPropertyKey 逻辑 ID 合并普通 ROW、既有显式 selector owner 和冷 ROW 引用 owner。沿现有 group 目录检查至多一个当前／后继组，逐个有界解码冷块；无整根引用的范围以最后 live owner 形成逻辑边界探针，计一个工作单位并推进 cursor，避免在有限 metadata 预算下反复扫描纯列／inline 冷组而无法前进。探针必须与普通 ROW／显式 owner 按 ID 合并，不能跨过未检查的 ROW。冷整根按一个 owner 记录计费，不按每字段重新解析；先认证块／owner／引用的 kind、media 和 generation，再由既有 sink 对照实际 manifest 保留引用。内容／完整目录／值核验仍由字段读取及完整 integrity 执行，mark 不物化整个值。原 owner／metadata／内存额度不增加，不增建目录／缓存／格式位；扫描未耗尽或异常不得产生 sweep 权限。新模式生产发布保持关闭，实际转换后的回收／旧 pin 验收后才能开放。


冷引用扫描证据见 `20261022-core-cost-cold-row-reachability/`。新测试使用原私有候选描述符和实际页／索引／Entity Map，通过私有测试入口执行生产扫描循环，不另写模拟遍历；生产入口仍从实际持久描述符打开。冷整根只交付一次引用，由既有 sink 认证实际 manifest，不读取完整值内容。共用同步冷块窗口不增加缓存或格式位。组探测／冷块访问显式计入保守目录工作预算，避免单叶游标没有 sibling boundary key 时遗漏计费；预算上限不变。旧混合夹具的引用仍为 5 个，新增无引用组尾探针计一个工作单位，因而总工作单位由 5 变 6，恢复后的尾部由 3 变 4；引用顺序／集合和最终 cursor 不变。初始预算测试失败及修复记录保留。此证据只完成扫描组件，不授予未完成的公开转换／实际回收验收，进度仍 90%。


单字段类型化冷选读实现见 `20261022-core-cost-cold-selected-window/`：严格有序、同组且至多 64-owner 的请求在输出前批量核对实际 Entity Map／无竞争 ROW，并拒绝旧显式 selector。若 token 全局归热列所有，立即交回热路径，缺失不读冷块。冷行按请求顺序的同 SlotReference 连续段共用一个解码体；跨无冷行的空隙仍可复用，切换到不同引用时释放原体，不改变输出 ordinal／顺序，不持有多个冷块或新增上下文缓存。借用的文本／scalar 同步消费，复杂值仍走原 owning codec；缺失单元只对有根 owner 输出，NULL 保持独立。原 ROW 交接函数每次实际块解码调用。允许首次输出之前的额度不足退回既有点路径；输出开始后额度／sink／取消异常直接传播，不重放。64-owner 夹具的两个共享冷根只解码 1 次；分块 RAW Edge、NULL／缺失、真实 LargeValue、竞争 ROW、输出后额度异常及退款均有模块测试。该单字段证据不替代后续多字段／精确计数证据或公开转换后的真实查询验收；没有性能时延结论，进度仍 90%。


多字段／精确计数窗口实现见 `20261022-core-cost-cold-general-window/`。沿请求顺序划分同冷引用连续段，原点查／计数逻辑在同步栈借用作用域内消费同一个账户拥有的解码体；借用指针不拥有／保留数据，不是跨请求缓存。段间销毁，嵌套与异常恢复父借用；重入到不同引用仍走原账户读路径。原输出矩形／counts 直接借用子区间，sink ordinal 平移回原请求位置，不复制结果数组。全部请求 owner 的实际 Entity Map／竞争 ROW／旧 selector 在输出前检查；纯热、无计数请求不打开冷窗口。为避免嵌套消费者复用既有成员缓存而失效，组边界副本为固定大小并计入原账户；不提高额度。解码体已拥有字节，因此冷物理页在回调前释放 pin，随后读热列／manifest 不无谓占住该页。已有 owning 矩形或 counts 可能写入后，不允许额度异常回放。测试涵盖混合冷热／NULL／缺失／完整 counts、真实 LargeValue 的 count-only、输出后额度异常与后续点读借用恢复，以及坏冷块下 owning 纯热读取。此处完成选读窗口复用；完整值 collection API 的后续证据独立记录，不能替代公开转换后的真实查询／生命周期或最终性能门禁，进度仍 90%。


完整属性集合窗口见 `20261022-core-cost-cold-collection-window/`：同一 owner 的实际目录／计数、逐字段预准入、最终 owning 值构造均在同一同步冷块借用内执行；原入口、排序／NULL／复杂类型、原 LargeValue 内容认证及预算／slots admission 合同不变。原结果在借用退出后才返回，不能带出冷解码视图；构造失败不能以额度回退重放准入。未进入值构造的窗口准入不足可以回到原有点读路径。真实 LargeValue 夹具同时读取热整数、冷长 ID 和 8193 字节文本，并证明整段仅一次块解码、恰好三次逐字段准入，异常／原生命周期相关模块继续验证。至此点查／选读／精确计数／完整集合共用同一解码基础；公开转换、变更及实际回收／旧 pin／退役、成熟全库成本和最终性能门禁仍待完成，进度仍 90%。


冷 ROW 转换准备合同：仍使用已有 owner-keyed ROW／owner-neutral LargeValue canonical 字节，不复制整棵 PropertyValue 来筛选字段。源 owner 与 token 已由原 Native 协调器认证；借用有序非热 entry，通过同一 canonical codec 在原 PMR 账户直接编码。先按实际 canonical 字节数判断单行能否进入 4096 字节块；超限由同一原子批的整根 LargeValue resolver 处理，缺少 resolver 必须拒绝，不能另行发布值或退回逐字段 EXT。按 owner 顺序有界分块，RAW／已有 LZ4、完整校验和、mask 与字段总数保持现合同；热 token 全部排除，冷成员仅存整根位置，不生成 OwnerMap／ExtensionDirectory。组／成员同页、热列直接位置与既有 ID／pool 绑定不变；默认旧准备行为保留，只有完整生命周期验收后才允许 Native 生产入口选择新模式。失败丢弃整个私有批，不推进源根或独立保留逻辑 ID；固定 64-owner／4096-membership／8-group／1-MiB 边界及全库最终空间门禁不变。


冷 ROW 私有转换准备已实现，证据见 `20261023-core-cost-cold-conversion-preparation/`。借用 entry 的 canonical ROW／owner-neutral LargeValue 编码逐字节匹配原 codec（包括 Node typeId=0、Edge、NULL、嵌套 map/list、长文本），不克隆整树，预检实际大小并直接写原 PMR 输出；LargeValue 的旧 PMR 路径与新借用路径共享同一实现。既有准备器仅在私有显式选项下构建模式 3：热字段直接位置、冷根有序分块与 owner mask／字段总数、同页 group／成员，以及零 OwnerMap／ExtensionDirectory 条目。RAW／已有 LZ4 不够时按 RAW-safe 前缀拆分；单行压缩后能放下可保留 inline，单行确实放不下才要求同批整根 resolver。冷-only 与属性清空组也保持精确身份。失败只丢弃私有批；源根、实际文件／ID 不独立发布。

同批整根 proposal 复用既有 `HybridPropertyLargeFieldPreparation`，借用字段编码后通过原内容索引／候选上限查重，保持相同数据库／源 generation／owner、token=0 与新 ID 提案合同。真实普通 ROW 大值夹具验证实际 manifest 复用和冷根绑定，且准备前后文件大小／I/O epoch／持久根／大值计数不变；未来引用在本地拒绝并退款。此证据仍不等于 Native 生产协调器接入：多组混合转换、最终完整空间准入、原子新大值发布及完整变更／公开转换生命周期尚待完成。生产选择参数仍保留旧行为，新模式没有开放。原始失败记录保留：测试误把有效 Node typeId=0 当成非法值，现使用 UINT32_MAX 并对照原 codec 同样拒绝；没有修改 owner 语义。编译夹具的错误 ListType 名称改为实际 `std::vector<PropertyValue>`。不跑全量单测／性能基准，进度仍 90%。


Native 协调器冷 ROW 接入合同（仅不发布探测已实现）：在现有转换请求末尾增加显式、默认关闭的冷 remainder 选择，仅允许单组、compact、ROW→hybrid 的 preparation-only 请求；`publish_candidate=true` 在任何源解码／新值规划前明确拒绝，不能成为绕过空间或生命周期门禁的入口。探测仍进入同一个 Native writer fence，认证实际 Entity／普通 ROW／token，使用既有大值 proposal 容器、候选去重、同批连续 ID、物理位置／data／manifest／目录／描述符私有图像准备；冷整根编码借用已认证 entries，禁止独立调用 LargeValueStore.write。新旧字段与整根 proposal 共用 carrier 内容去重，完整字节相等才复用。最后沿原 preparation-only guard 丢弃整批，持久根、计数、I/O epoch、文件和旧 pin 不变。工作计数用于测试准备路径是否真的经过，不作为性能／完整空间准入证据。多组／正式发布及其新值 content-index 原子安装必须等完整生命周期与最终空间合同完成后接入。


真实 Native 不发布探测证据见 `20261023-core-cost-cold-native-probe/`。既有请求新增默认关闭的 `cold_row_remainder`，仅单组 compact 正向 preparation-only 可用；正式发布、非 compact、多组与逆向形态在源分类前拒绝。源 Entity／ROW／token 仍由原协调器认证，冷准备器、同批整根 proposal／连续 ID、data／manifest 及最终字段／目录／描述符私有图像均走原规划器。字段与整根 proposal 共用原完整 payload 比较与 holder/ID 保留算法；旧字段路径的检查点顺序不变。实际 Node 与 Edge 各 64-owner 的整数＋长 ID 夹具进入私有物理规划，只有一个字段 frame 和一个 group-index frame，不产生冷 per-field 目录；另一夹具从含热字段的旧整根大值提取冷部分，确实计划一个新同批大值及 data／manifest frame。以上均在原 guard 处拒绝发布，文件／I/O epoch／持久根／计数／旧 pin 不变，预算归还。frame 计数不是时延／完整空间节省证明；没有提升原门禁或开放生产，新值 content-index 原子安装、多组及完整冷热变更生命周期仍待完成。编译夹具曾误把返回 owning root 的 readEntity 当成 optional，现按实际接口读取；未修改源语义。进度仍 90%，未全量测试／跑性能／提交／推送。


本轮新增既有 ordinary-conversion 模块的紧凑变体后，发现两个旧夹具假设：稠密例的紧凑直接位置只需一个 field pool＋一个 Group-index pool，恰好等于源 ROW 的两个 pool，不能沿用旧多目录布局“必须更大”的断言；紧凑降级也可能在原 128 KiB 测试额度内成功，不能靠固定 cap 假定故障。已用上一轮冻结 preparation 库及由其 source.patch 精确还原的旧头文件复现相同两败，排除 ABI 混用；修正后的相同两例在该旧库也通过。新夹具保留旧布局的大于断言，并对紧凑布局明确核对一 field pool／一 group index 及两个源 ROW pool；额度仍为 128 KiB，在真实 private-map 准备事件占用剩余共享 credit，确定下一阶段额度拒绝且没有物理写入，随后两处取消与正常重试仍须通过。未提高预算或放宽 10% 最终完整空间门禁，未删测／跳过；原失败及基线／修正源码配对、指纹、原始报告均记录在 Native-probe 证据中。


兼容热更新准备合同（受限私有计划已实现）：沿原 ColumnDelta／DemotionPreparation 构建受限私有计划；只允许既有全局热 token 的兼容 scalar／NULL／保持 ≥48 presence 的增减，不改变 live mask、不删除 owner、不降级、不处理冷 token。私有读取入口可交付经实际 Entity／无竞争 ROW／无旧 selector、成员 presence 与热列精确位置验证的热源，不能通过公开 ColumnVisitor 绕过尚未完成的 mode3 变更 guard。合并多 owner 后仍需重新判断阈值，不能把单项兼容计划合并成未实现的降级。更新同一私有字段页的热列、必要的成员 presence 与组 generation／总数，冷 mask／位置／字段总数／字节保持不变；不生成 OwnerMap／ExtensionDirectory，不重打包或解码冷块。Source 页 CRC／slotted 布局和实际访问热记录照常校验；未访问冷记录损坏由其首次读取／完整 integrity 发现。此准备不构成发布权限；实际 mode3 变更入口保持拒绝，后续 pool 绑定／原子发布仍须完整局部校验合同及生命周期。


兼容热 COW 的 pool 绑定合同：仅由 FieldPageStore 在活 pin 下验证实际 Catalog／location／PageOwner、页 CRC 和 slotted 布局后产生“原 slot 未修改”的瞬时证明。新建或普通 open 图像不能获得该证明；clone 可继承，insert／overwrite／erase 必须在统一入口清除相应 slot 标记。绑定仍完整验证所有新／修改记录及非冷记录；仅证明未修改的既有冷块可保持原字节而不解压，不能把证明序列化为格式位／缓存。公开打开同一序列化字节必须回到完整校验。此合同允许未访问冷块损坏延迟至首次读取／完整 integrity，不允许新块或访问／修改过的冷内容跳过校验，也不替代 Native 源 fence／目录／原子发布／退役。固定每页位图属于现有私有图像对象并计入原 holder 预算，不提高额度。


兼容热准备与 pool 绑定证据见 `20261023-core-cost-cold-compatible-hot/`。原 ColumnDelta 的私有读取路径只交付已认证全局热列；公开 ColumnVisitor／groupRecord 仍拒绝 mode3 变更。兼容值／NULL／保持阈值的 presence 增减通过原 DemotionPreparation 更新必要页与精确总数，冷位置、字段数和原字节不变，不新增 selector。NaN／signed-zero 继续使用原逻辑相等规则。17 个单项兼容删除合并后降至 47 必须拒绝；16 个降至 48 可准备，并保持冷字段总数。类型不兼容、owner 删除、待提交 owner、冷 token 和实际降级继续不授予计划／发布权限。

只有 FieldPageStore 认证的 clone 能给原 slot 瞬时来源证明；私有图像 clone 继承，普通 open／新建不能继承，统一 append／overwrite／erase 清除对应证明，insert 失败则清空证明以 fail closed。绑定仅跳过未变既有冷块的解压，其他／新／修改记录继续完整局部校验；位图不序列化、不新增缓存，holder 大小计原额度。坏冷 CRC 下热准备与可信 clone 绑定成功且字节不变，冷首次读取仍拒绝；同样字节经普通 open 后绑定必须拒绝。新 slot 与擦除后复用 slot 均不能继承证明。Edge 的缺失热成员插入 NULL 保持准确冷热总数、冷字节与绑定。所有准备／绑定仍无文件／根／ID 变化，真实 mode3 变更与发布保持关闭；冷更新、全局降级、owner liveness、删除／逆转换及实际公开生命周期尚待完成。此中间组件仅归档源码／报告及库指纹，关键前序冻结库保留，不重复复制中间二进制；无全量单测／性能基准，进度仍 90%。


受影响冷块重打包的字节准备证据见 `20261023-core-cost-cold-block-repack/`。既有 ColdRowBlock codec 新增受限 repack：先认证原块 CRC／owner／目录，再合并最多 64 个完整 owner-root 替换或删除，未改行复制原 canonical 字节；最后一行删除返回空结果，插入缺失 owner 仅是字节准备，不授予 Entity Map liveness 权限。复制、替换或删除前，所有原大值引用均按原块语义代次校验 owner／BYTES／媒体／最小长度，避免升级块代次洗白未来引用；新引用按目标代次检查。实际 manifest／token 热冷排他、源 fence 与原子绑定仍由 Native 协调器认证，codec 不提供发布权限。新块超过原 4 KiB 原文或物理记录边界则整体拒绝，不返回成功前缀；后续需由受影响块布局规划器拆分，不扩大限额。实验／默认配置各 18 项 codec／binding 模块测试通过，含 Node／Edge 未改字节、NULL、删除最后行、缺失 owner 插入、未来引用、重复／非法删除、超限、取消与额度退款。此阶段尚未接入真实变更协调器，不能计作完整冷字段写入或 P7 验收；进度仍 90%，无性能基准／全量单测／提交／推送。


单个受影响冷块的实际来源准备已接入既有 `HybridPropertyDemotionPreparation::prepareColdBlock`，证据见 `20261023-core-cost-cold-block-preparation/`，仍不开放生产变更。入口使用原 snapshot／batch、GroupDirectory／实际 Entity Map／竞争 ROW／旧 selector 认证，读取实际 Group／membership／ColdRowBlock，逐个认证受影响 owner 与全局热 token 排他。既有大值由原只读 LargeValueStore 验证实际 manifest／内容／目录，源引用继续按原块代次检查。输入是完整 owner-root 替换／删除；新引用只允许原 pin 可见的既有大值，同批新大值仍须接入原 proposal 协调器，不允许把未来目标代次传给已发布快照检查器。

原 FieldPageStore clone 产生最多两页私有图像；原 PageImage 的 typed overwrite 清除冷 slot 的未修改证明，更新同 slot 冷块及精确 cold_fields／cold mask／binding／group memberships，保持原 hot locators、liveness 和语义。删除最后冷行释放该私有 slot，仍有 live owner 的无属性组保留。所有结果生成失败均丢弃私有图像，不消耗物理／逻辑 ID，不修改文件／根／I/O epoch；未触及冷块的损坏可保留到首次访问／完整 integrity。模块覆盖 Node 替换保留邻居字节、可信 pool 绑定、Edge 既有 owner 的 NULL 冷字段插入、含热列／纯冷组清空、竞争 ROW 拒绝、冷热冲突／重复变更、真实大值及另一冷块损坏的局部合同。初轮新输入错误传入目标代次，三个用例暴露快照边界错误；已修源码为原 pin 认证，没有放宽检查或测试。

该入口目前只准备一个已存在且原页可容纳的块；块增长拆分／成员矩阵增长搬迁／新 slot 绑定、同批新大值、与热变更合并、token catalog／约束／索引协同、owner liveness／降级／删除／逆转换、正式 Native 发布及旧 pin／退役／故障生命周期仍待完成。完整 owner-root 私有准备不能解释为公开字段写入或性能验收，进度仍 90%。


冷热同批共享私有图像的准备已接入既有 `prepareColdBlockWithHot`，证据见 `20261023-core-cost-cold-hot-merge/`。先调用原 DemotionPreparation 构造器，认证所有热计划的数据库、源／目标代次、组与实际列字节；再在同一准备结果上定位已有 COW 图像，缺少的实际源页才 clone，不能为同一页产生另一份从旧快照开始的冷图像。合并继续限制冷热输入合计最多 64 项，不提高原限额，不授予 liveness／全局 token 变更／降级权限。私有目标组／成员矩阵保留热值与 presence 修改，验证冷位置／总数和全局热 tokens／locators 未被热准备改变，再将受影响冷块及最终总数更新到同一图像。使用实际 source 只读接口，原准备器的三个构造器收紧为 const FieldPageStore 引用，不经 const_cast。

14 项定向用例通过，其中一个合并用例分别覆盖热值修改与缺失热 NULL 成员插入后再替换冷行，验证共享页只有一份、冷热修改均保留、成员位图与精确总数一致、文件／根／I/O epoch／ID 不变及账户退款。这仍是私有准备入口；实际 Native 写入分派、块增长拆分／矩阵搬迁、同批新大值、全局降级／owner 生命周期／逆转换、公开发布与完整生命周期及最终性能门禁尚未完成，进度仍 90%。


冷块增长布局已接入原准备器，证据见 `20261023-core-cost-cold-growth/`。单块与多块重打包共用一次原源认证／owner 合并逻辑，原大值引用仍按原块代次校验；新增 `repackBlocks` 只返回完整、原账户持有的结果，任何后续大行拒绝都会释放已准备前缀。每块仍不超过原 4 KiB 原文／原物理记录边界，沿用转换器的 RAW-safe 前缀策略。原 slot 可容纳单块时保持引用；其他情况擦除旧私有 slot，用原 MetadataWriteBatch 的私有逻辑 ID 及原 PageImage 创建目标页，更新每个实际 owner 的冷绑定与精确字段总数。额度／取消失败要求丢弃整个既有批次，不允许单独发布已分配 ID。

成员矩阵原页放不下时，与组记录一起搬到同一新私有页，保留 hot locators 与 owner liveness；生成实际 GroupDirectory upsert。既有 Native 目录准备循环现消费该 upsert，与已有擦除一样进入原 dirty-path overlay／最终描述符原子发布，不建立额外目录。正式 mode3 分派／公开发布保护仍保留。有效源 fixture 用另一个合法的大冷行制造原页容量压力，63 个 owner 的增长产生 63 个有界块，并实际搬迁矩阵／组；验证新组位置、全部冷热总数／冷绑定／canonical 字节、未动冷块原位置、私有 ID 不单独发布、文件／根／epoch 不变及原账户退款。另一 codec 用例验证拆块后的字节与后续超大行失败时无成功前缀泄露。

这完成了受影响块的私有增长布局及目录编辑，未完成实际 Native 字段分派、同批新大值、全局热字段降级、owner 生命周期／逆转换、正式 mode3 公开发布与全生命周期／完整空间准入／P7 性能验收；验收进度仍 90%，不提交／推送，不跑性能基准或全量单测。


同组多冷块准备已由既有 `prepareColdBlocks` 合并，证据见 `20261023-core-cost-multi-cold/`。结构预检要求一个实际组、不同源块、不重复 owner-root，冷热输入合计仍最多 64 项；逐块源认证始终读取原 pin／GroupDirectory／membership，私有变更则读取当前累计图像。某块触发组／成员矩阵搬迁后，下一块沿当前私有 GroupDirectory 编辑继续更新；已改 source owner 不能重入，最终同一组只有一个最新目录 upsert。既有热 presence／NULL／值及前面冷块修改保留，后续失败丢弃整个原批次，不独立发布 ID。16 项定向用例通过，包括 63-owner 拆块搬迁后再修改第二冷块、两个冷块清空与热 NULL 同批合并、重复块拒绝。此机制不授予实际 Native 分派／新大值／全局降级／owner liveness／公开发布权限，这些生命周期及完整成本与 P7 仍待完成，进度 90%。


首次冷块与逆转换读取准备的证据见 `20261023-core-cost-cold-entry-inverse-read/`。原重打包 codec 接受空源，仅表示私有新块的字节准备：仍验证 kind／group／源与目标代次、canonical owner／字段数、既有大值媒体／代次，不能删除不存在的行。原冷准备器仅将标准空 reference 识别为新增；新 owner-root 必须属于实际存活 Entity Map owner，且当前没有另一冷根，不授予新建实体权限。没有旧块时不读取虚构页／不执行退役，沿原私有 ID／PageImage 路径创建冷记录，保留其余冷热权威及精确总数；合并批中仍最多一个空源 bucket，冷热总输入不超过 64 项。新增用例覆盖 Node／Edge 字节编码、在既有冷 owner 旁加入 NULL 冷字段、完全无属性 live 组的首个冷字段、禁止覆盖另一个冷根及源文件／epoch／根／账户不变。初轮一个断言把 `optional<bool>(false)` 的 engaged 状态当成 false；修正为检查明确的路由 false，不改读取实现，原失败报告保留。

既有 `HybridPropertyRowPreparation` 的只读组查找现允许 mode3，再沿原实际 Entity、owner 枚举和 ValueReader／同账户解码逻辑投影完整 ROW。22 项定向检查通过，包括实际 8193 字节大值、NULL、缺失／无属性 owner、Node／Edge 命名空间、全组精确字段数及访问坏 CRC 后退款。公开 `groupRecord` 变更保护未放宽，原逆转换发布协调器在投影前显式保留该保护；只读投影不会开放新模式发布。真正的逆转换目录／cold block 位置擦除、ROW／大值安装与退役、同批新大值／全局降级／owner 生命周期和实际公开转换仍待完成。进度仍 90%，无全量单测／性能基准／提交／推送。


同批未发布冷整根的准备校验证据见 `20261023-core-cost-cold-root-proposals/`。原冷准备入口可借用原 `HybridPropertyLargeFieldPreparation` 不可变候选列表：新增引用必须落在本批已保留的 ID 区间，匹配候选的完整 LargeValueRef、数据库／源代次及实际 owner-keyed 包身份，并校验原载荷长度／目录／字段数／canonical 值与全局热 token 排他。载荷本身 owner-neutral，允许不同 owner 的原有去重；字段 carrier 与整根 carrier 字节完全相同时也必须复用，不以 proposal.token 强行区分。现有源冷行永远不使用新候选证明，仍按原块代次及原 pin 验证实际 manifest；新候选仅用于明确的目标行，不读取尚未发布的 manifest。

24 项定向检查通过：无候选时未发布引用必须拒绝；同批预留 ID 与原整根候选可准备真实 8193 字节新冷值；未预留 ID／包含全局热 token 必须拒绝；合法字段 carrier 作为相同 owner-neutral 整根保持去重。所有准备仍无文件／根／epoch 变化，ID 仅保留在原未发布批中，失败丢弃批次，账户退款。实际 Native 调度器必须将传入的同一候选 holders 接入原 Immutable／manifest／content-index 规划和原子发布；本阶段不宣称这些写入安装已经接通。该实际协调、全局降级／owner 生命周期／逆转换安装、完整空间与公开生命周期及 P7 仍待完成，进度 90%。


逆转换的字段页保留边界见 `20261023-core-cost-inverse-field-retention/`。原 Native 逆转换协调器先验证整页归属，在私有 COW 页中只移除所选组的槽；重复／失效槽拒绝。仅空页移除逻辑位置，非空页保留 PageId 和其他组槽，输出保持 HYBRID 页类型并使用独立的有界打包游标；原快照／退役合同不变。实验两种 membership 各通过 502 项相关模块测试，默认通过 154 项，含既有逆转换取消／故障／退款／重开测试。新增公开测试验证八组中只逆转第一组，其余组权威、旧 pin 及完整 integrity 保留；私有共享页测试验证精确槽删除与保留。**证据限制：当前公开转换器实际生成各组独立逻辑页，公开测试共享逻辑页数为 0，不能声称已覆盖共享页发布分支。** 原错误布局假设的失败报告保留。冷模式公开逆转换／写入仍受原 guard 拒绝，尚未完成实际冷布局安装与生命周期验收。进度仍为 90%，未跑全量单测／性能测试，未提交／推送。


原 Native 字段协调器的冷布局准备接入见 `20261023-core-cost-native-cold-field-coordinator/`。仅私有测试入口可借用同一文件／metadata／精确源 pin 的候选 ReadContext；正常调用仍读取持久化描述符并保留冷模式发布 guard。私有候选一律在首次物理写入前拒绝，不提供可选发布开关。先以 membership 的全局热 token 集分派字段；同一列的多个热更改先合并，冷更改按实际 owner 合并最终字段，保留 NULL／缺失语义并过滤热字段。只读取相关冷根，原有 canonical 编码／压缩边界决定 inline 或同批 whole-root 载体；相同 payload 跨 owner 去重。既有 largeFields holder 同时供冷根认证与原 Immutable／manifest／content-index 准备，禁止独立写入或读取未发布 manifest。membership 按值复制后释放物理 pin，再读取冷根；标准参考编码的两块临时缓冲先计费。

冷块分拆／组搬迁沿用原 prepareColdBlocks 和 GroupDirectory 脏路径。修复了原 Native 目录循环遗漏“只有 groups upsert”的外层过滤条件；否则已生成的目录更新可能被提前跳过。29 项定向测试验证兼容热更新／缺失热插入、混合冷热更新／同列多 owner 合并、NULL 插入、删除冷根／缺失删除 no-op、8193 字节新载体跨 owner 去重及原大值规划、63-owner 冷块增长触发组目录编辑；文件、源根、I/O epoch、公开 ID 不变，原账户退款。不同 pin、竞争普通 ROW 拒绝。测试入口使用公开写入相同的 16 KiB 源存储预留和 1 MiB 总额度。

测试修正保留原失败报告：只读 store 必须在 writer 之前打开；原路由夹具直接修改 Bootstrap 开启 compact flag，须先完成真实 writer admission 的外部源重同步，再测量“字段准备零写入”，不能把重同步写入误归因于准备器。候选仍为私有内存描述符，没有假称真实公开转换。实际 Catalog／索引／约束源的新模式覆盖、物理安装、全局热降级、新 owner／删除／liveness／最终组清理／逆转换、完整空间准入与公开生命周期仍未完成，P7／进度仍为 90%。未跑全量单测或性能矩阵，不提交／推送。


实验布局两种 membership 各通过 507 项相关模块检查，默认通过 154 项，无失败／跳过；这些检查不替代公开转换与最终性能验收。


Native 派生旧值读取现复用字段准备的同一认证 ReadContext；实际 ROW owner 才回到原 ROW 入口，不再在候选字段准备与索引／约束之间切换数据源。证据：`20261023-core-cost-native-cold-derived-source/`。新增用实际 Catalog、两棵实际标量索引树、实际节点标签和 NOT NULL 定义构成的私有候选：热更新必须读取未修改的冷字段以满足约束，并生成原标量索引 COW；删除该冷字段则在发布前拒绝。原根、旧索引键、文件、I/O epoch 不变，账户退款，公开模式 guard 保留。仅恢复修复前旧值读取的同编译参数单对象对照，会把未修改的冷字段当成缺失而误报 `required_cold` 非空违例；当前源码通过相同测试。对照源码／编译／链接／失败报告保留，不能计入当前通过项。

30 项定向检查通过，实验两种 membership 各 508 项相关模块、默认 154 项通过，无失败／跳过。测试构造必须对新索引使用既有 stageScalarIndexCreate，不能对不存在的树使用 upsert；初始失败报告保留。这证明实际 schema／索引源与候选字段的准备协调，不是已公开转换／物理安装，也不替代其余 schema 组合、全局降级／owner／逆转换、完整空间及最终性能验收。进度仍为 90%，无全量单测／性能矩阵／提交／推送。


整列降级的原 Native 准备路径见 `20261023-core-cost-cold-global-demotion/`。ColumnDelta 的显式准备选项只输出不可变完整列计划；默认调用、owner liveness 和生产发布 guard 不放开。原协调器先合并同列全部用户变更，再按最终列状态决定保留或降级。降级时把实际原／最终成员并集中的 owner 合并进最终冷根，保留其他冷字段、NULL／缺失、原 ID 与 Node／Edge 命名空间；不存在的空根不伪造删除。删除列记录／membership 和重写冷根位于同一个私有 MetadataWriteBatch，仍由既有目录、大值与物理分配规划消费，不生成 OwnerMap／EXT。

所有最终非缺失列成员必须有目标根；目标根中的每个降级字段与原完整列计划逐项比较 canonical 值，而不只检查 token／数量。同批未发布整根载体仍用原 immutable holder 认证，不读取未发布 manifest。整列权限变化会影响本组所有冷目录，所以按原始热 token 集认证本组现有冷根，包括不重打包的冷块；原 block generation、实际 Entity／普通 ROW／legacy selector 和 manifest 检查不变。此步骤只扫描本组最多 64-owner 的目录，不扫描数据库，也不把其他冷块重写。去掉这项认证的单对象对照会放过分离在未重打包冷块中的旧非法热字段；当前源码拒绝，原对照报告单独保留。

64-owner、64 用户 replacement、最多 64 个列计划、4096 个实际迁移 membership 和 1 MiB 额度不变；结构列计划与输出冷根可指同一 owner，不重复计作第 65 个 owner。非整列降级调用仍保留原冷热合计 64-change 检查。36 项定向测试验证全部 64 个 owner 的实际值／NULL／原冷字段、遗漏 owner／字段及错值拒绝、同列 17 次删除合并后的阈值降级、缺失成员异类型插入、8193 字节同批大值、清空全部热字段后保留存活无属性组，以及真实 Edge namespace。实验两种 membership 各 514 项相关模块、默认 154 项通过。文件／根／I/O epoch／公开计数器不变、账户退款，公开 guard 保留。原子物理安装、新 owner／删除／liveness／最后组清理／逆转换、其余 schema 组合、完整空间和公开生命周期仍未完成；P7／进度仍为 90%，无全量单测／性能矩阵／提交／推送。

冷布局逆转换的原协调器准备见 `20261023-core-cost-cold-inverse-coordinator/`。私有入口只借用精确源 pin／文件／metadata 的候选 ReadContext，要求禁止 candidate 发布，并在原首次物理 I/O 之前无条件拒绝。正常公开路径保持冷模式 guard。逆转换使用原完整 ROW 投影，按 membership 的全局热集合判定字段来源；认证实际冷块、owner／字段计数、原 block generation／manifest、竞争普通 ROW 和 legacy selector。源字段页仍由既有完整所有权检查与 COW 保留／退役逻辑消费。没有热成员且完整投影恰为原冷整根时复用实际大值引用；有热字段需并入时规划新的完整 ROW 载体，不能把不完整冷载体当完整属性集。Node／Edge 命名空间、无属性存活 owner 和原 ID 分配语义不变。

真实大冷值测试暴露并修复了 writer 内再次 openSnapshot 的锁冲突：ReadContext 的冷读取现在通过私有只读构造器把同一已拥有源 pin 附加到原 LargeValueStore／SlottedPagePool／SharedPagePoolAllocator，不再次获取快照，不执行重同步，也不授予独立发布权限。公开 store 构造和实际 manifest 认证保持原契约。保留原 40 项测试中两项死锁异常的失败证据；修复后 43 项定向检查全部通过，包括原有大冷根的字段修改、完整冷载体复用、冷热合并的新 ROW 载体、真实 Edge、无属性组，以及额度失败／已分配私有 ID 后取消／退款／重试。相关模块结果与源码身份见同一归档。文件大小、根、I/O epoch 和公开计数器不变。此阶段仍只是原协调器的零写入准备，不是实际公开转换／物理安装验收；owner 生命周期、完整空间准入、公开生命周期与最终 P7 仍未完成，进度 90%，无全量单测／性能矩阵／提交／推送。

冷模式 owner 删除的原协调器准备见 `20261023-core-cost-cold-owner-deletion/`。私有候选入口在单个已认证组内接入既有实际 Entity／邻接／LabelSet 删除规划：按原 membership 合并被删 owner 的全部热字段删除，最多保留 64 个完整列计划，而不是同时保留 4096 份计划；降级时把所有幸存成员并入完整冷根，被删 owner 只移除实际存在的冷根。先完成同批热成员／冷根删除，再改变存活位；遗漏任何实际字段删除、重复 owner、错误 namespace 或跨组源都拒绝，不通过清位隐藏遗漏。存活组同步 membership.live；最后 owner 删除同时移除 membership／group 记录及 GroupDirectory 条目，包括无属性组和发生过目录搬迁的组。该准备仍无物理写入，公开冷发布 guard 保留，新 owner／pending liveness 和实际原子安装未因此验收。

实际唯一约束支持树还复现了另一处独立的旧值读取缺口：生成 canonical 约束键仍经单独 ROW 入口读取，导致冷字段重复值被误报为 stale support，并让 owner 删除漏掉支持键删除。现已统一使用同一已认证 ReadContext，仅实际普通 ROW owner 回退 ROW store。保留修复前 50 项中的 1 项失败（包含两条独立断言），修复后验证真实重复键拒绝及非零支持树删除准备；源支持树／Entity／文件／根保持不变。测试还覆盖 Node 整列降级与 64-owner 最后组、实际 Edge64 且 Node64 不存在、未删除 owner 的值、保留热列、原有 8193 字节冷载体、额度失败退款／重试。结果／身份见归档；不是已公开转换数据的生命周期验收。进度仍 90%，没有全量单测、性能矩阵、提交或推送。

冷 pending-owner 字段／存活位准备见 `20261023-core-cost-cold-pending-liveness/`。夹具现在可以构造实际只有 63 个 Node 或 Edge 的源；第 64 个实体在实际 Entity Map 中不存在，源 live／列／冷目录与这 63 个实体一致，不靠假清位模拟新 owner。显式准备选项可计划兼容热值／NULL 或不兼容值的整列降级，并在同一私有页集中合并 pending owner 的冷根、存活位和列；原先不声明 pending 协调的默认调用仍拒绝，clear_live 仍不能通过列合并入口放行。准备器验证实际源实体缺席、唯一 owner、组／namespace、无竞争 ROW／legacy selector，目标冷根中的降级值逐个匹配不可变列计划；结构性展开仍受 64-owner／64 列计划／4096 实际迁移 membership／1 MiB 边界。

先准备已声明 pending 集的存活位与兼容热计划，再将新冷根／整列降级合入同一组的原 COW 图像；现有冷根仍按原 pin／原块代次与原全局热集合认证。可同时移除旧 owner 并增加新 owner，但删除只有在全部实际热成员／冷根已移除后才清位；目标仍有新 owner 时不得误删组／membership／GroupDirectory。59 项定向检查涵盖 Node 热 NULL、无属性 Edge、新冷根、全部原列成员迁移、删除原 63 个 owner 同时新增第 64 个、重复／存活／跨组／错 namespace 拒绝和账户退款。原 59 项报告中混合 birth/deletion 被旧合并 liveness guard 拒绝的失败保留；新增独立显式 pending-merge 选项后通过，默认合并依然拒绝。相关模块结果与源码身份见归档。

这些仍是零写入私有字段计划，不能替代 NativeHybridTopologyPublication 的既有封闭实体创建凭证、逻辑 ID lease、实际 Entity／邻接／标签／派生状态以及同批大值安装。负向检查证明只传私有候选与 pending 字段，原 Native 协调器仍拒绝缺少 sealed new-owner graph publication 的请求。与实际创建协调器的衔接、跨组协调、冷模式物理安装、真实公开转换／完整生命周期、完整空间准入和最终 P7 仍待完成；生产冷转换入口不放开，进度 90%，无全量单测／性能矩阵／提交／推送。

单组冷模式的实际 Native 发布见 `20261023-core-cost-cold-native-lifecycle/`，本段取代前述“只有零写入准备”的阶段限制。正常协调器现在从精确持久化描述符／源 pin 读取 Mode3，原同一批次实际安装 field pages、cold binding／GroupDirectory、Immutable／manifest／content-index、Entity Map／计数／派生树和最终描述符。原有 data-before-root、A/B、原源 fence、额度、逻辑 ID lease 与封闭 NativeHybridTopologyPublication 凭证不变。私有 candidate ReadContext 入口仍无条件在首次物理写入前拒绝；前向公开冷转换／采用仍由原 guard 关闭，不新增放行开关。只在已适配的 metadata／封闭图协调器中使用冷模式 source view，通用 groupRecord／未适配 ColumnVisitor guard 保留。PropertyCollectionRootStore 的只读 group／live 探针改用同一实际 Entity Map／竞争 ROW 认证的 ownerLiveness，区分无组、未存活 pending slot 和存活无属性 owner，不解压冷体，不授予写权限。

17 项定向测试实际提交冷热 SET／整列降级、带大冷值的新 Node、无属性 Edge（含邻接／type count）、删除、63-old-owner＋1-new-owner 混合提交、属性清空保留存活 owner、最后组清理及完整大 ROW 逆转换；验证未直接修改的列成员、NULL、旧 pin、普通读取与完整 integrity。真实字段索引／唯一支持树随冷值一起更新，重复键／非空违规保留原根和 I/O epoch。部分 inactive A/B 故障重开只见旧值，完整 inactive A/B 故障重开见完整新值与 manifest；实际私有写之后取消保留旧根、退款并可重试。实验两种 membership 各 554 项相关模块、默认 154 项通过，无失败／跳过。

这些实际生命周期测试使用完整、一致的持久化冷源夹具：实际 Entity／邻接／Catalog／索引／字段页／目录已存在；在测试观察 pin 之前用既有测试字节 seam 将候选目录装入既有描述符帧，并先通过普通读取及完整 integrity。该夹具初始化不是公开 ROW→冷转换，也不证明前向 data-before-root、空间准入或采用。生命周期夹具使用符合既有 Catalog／Retire Queue 合同的初始 epoch1；读专用 epoch99 夹具仍保留以区分物理／语义代次，不用其证明实际退役。保留原失败报告：调用者读取必须带原资源账户；不带 sealed 创建凭证的底层事务不能替代后端同一逻辑执行器；LargeValue-aware 源 store 和只读池附件遵循后端原合同。没有为通过测试关闭这些检查或放宽退役校验。

仍未验收：跨组／混合 namespace 协调、共享字段帧的实际逆转换保留分支、完整回收与搬迁、前向公开冷转换及其实际生命周期、完整空间／私有 staged-target 准入、4096 字节完整查询与剩余主成本，以及最终 P7。进度仍 90%，无全量单测／性能矩阵／提交／推送；本轮测试与归档不能代替 Kuzu 目标。

共享字段帧、实际回收与搬迁补齐见 `20261023-core-cost-cold-shared-lifecycle/`。新增夹具在同一实际逻辑 FieldPage 内存放 Node／Edge 各一组，二者有相同逻辑 owner ID、独立列／cold block／membership／group 记录及各自 GroupDirectory；实际 Entity、邻接、类型计数和 Catalog 均一致。Node 逆转换实际删除本组四个记录，保留另一个 namespace 的四个记录、原 slot／generation／逻辑 PageId 和独立目录，并通过普通 ROW／冷读取、旧 pin 与完整 integrity。该测试暴露并修复原逆转换写入边界错误：保留页虽已绑定 Hybrid owner，物理写池却使用普通页入口。现在只对独立保留池且全部 appends 为 Hybrid 的完整池调用既有 native 写入校验；新普通 ROW 池不混入，不扩大写权限。保留修复前 19 项中 1 项失败的实际证据。

实际 LargeValueReachabilityReclaimer 验证当前冷大值可达、旧载体从当前 manifest／content index 退役、旧 pin 仍可读取旧值且不释放旧容量；pin 排空后再次回收证明物理槽释放或仍存活共享池的空闲帧增加，存活冷值及完整 integrity 保持。完全释放的 obsolete Catalog 记录可以被原回收器清除，检查通过公开 allocator／Catalog 接口判断槽或容量，不读取私有池头。实际 Node／Edge 共用字段池搬迁保留两类数据和逻辑位置、旧 pin，旧槽仅在 pin 释放后由原持久队列回收。20 项定向检查以及实验两种 membership 各 557 项相关模块、默认 154 项通过。

这些仍是完整持久化测试夹具上的实际生命周期，不是前向公开转换或全库空间采用证明。跨组／混合 namespace 的同一变更批次、公开前向转换／实际转换后完整生命周期、全库私有 staged-target／空间准入、4096 字节完整查询与剩余主成本、最终 P7 仍未验收。进度 90%，没有全量单测、性能矩阵、提交或推送。

### 2026-10-23 固定清单收尾验证

实验 13／39 现已支持共享 FieldPage 的跨组／混合 Node、Edge 冷写入，兼容热计划与冷重打包合并到同一私有页集合后一次原子发布；全批次仍受 64 实际变化／迁移 owner 上限。27 项真实持久化生命周期用例包含混合创建／删除、逻辑 API 与两组降级超限拒绝。单帧顺序扫描使用既有有界复制路径，4096 字节缓存的完整过滤查询已由失败变为通过，无缓存额度或校验放宽。实验两种模式各 564 项、默认 154 项及完整查询两种缓存 2 项必要检查通过。

前向冷转换／全目标空间准入仍未交付，公开入口保持关闭；上述夹具不构成转换证明或性能采用。本轮不再展开新迁移协议。最终性能结果、未达条件与源码／库身份见 `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/`；验收仍 90%，不能称 Kuzu 目标完成。
