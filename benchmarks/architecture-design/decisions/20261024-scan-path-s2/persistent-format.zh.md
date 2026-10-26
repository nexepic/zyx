# S2 候选 B：完整文件协议（冻结合同）

本协议固定候选 B 的格式与发布边界，配合 `membership-wire.zh.md`。它不表示实现完成，不提高进度。当前默认 5/31 和实验 13/39 原义保持不变；不得往两者写入新的扫描成员记录。所有以下规则均须落实并测试后才能开启候选 B 的性能比较。

## 1. 隔离版本与根

新增显式构建选项 `ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT`，默认关闭，要求同时开启既有 hybrid 格式。该构建使用 SegmentEnvelope 版本 15、Superblock 版本 41；普通构建仍为 5/31，旧 hybrid 构建仍为 13/39。正常打开只接受本构建的对应版本，不原地迁移。15/41 的 Superblock 保持 352 字节头，offset 336 的 16 字节 hybrid descriptor 物理引用以及 offset 352 的 32 字节 emergency reserve 不变。

新增 Superblock flag `1 << 2` 表示 BUILDING，仅 15/41 认识。普通数据库打开入口拒绝选中的 BUILDING 根；只有离线构建器持有的私有能力可访问它，不提供可绕过检查的公共 open option。flag 清零仅表示完成下面的最终准入与原子发布；不是任意调用者能自行设置的状态。

15/41 使用 hybrid descriptor 版本 2：原 32 字节帧头及 CRC 合同不变，payload 从 192 增至 208 字节。既有八个根位于 payload offset 64..191 且保持原义；第九个根位于 offset 192，仍为 PageId u64 + physical-pool generation u64。该根专用于节点扫描成员目录，内部 index ID 为 `0x8000000000000038`，不得出现在用户 schema index 定义中。原八个命名空间不挪用。

payload offset 24 为 live_node_count u64，32 为 membership_group_count u64，40..63 为零。计数跟随当前已安装的成员目录，构建期间可以不覆盖全部节点；最终发布必须精确覆盖全库当前节点。descriptor 的 database_id、semantic_generation、物理帧 generation、LSN、零尾部及两级 CRC 保持既有认证规则。descriptor body CRC 覆盖帧头后的全部字节（包括零尾部），header CRC 计算时自身字段清零。

## 2. 目录、记录与字段

目录复用既有固定宽度 B+Tree 页协议：key 为 8 字节大端 group_id，value 为既有 16 字节 SlottedSlotReference 编码。key 非零、严格递增且唯一。目录根绑定 descriptor 的数据库、根 generation 与快照；不能把独立传入的 tree root 当作已认证根。

目录每个条目引用同一数据库 Page Directory 中 `POK_HYBRID_PROPERTY` 页上的 `ScanBlockMembership` 记录，字节格式见配套文档。读入时同时验证 page owner、物理版本、槽代际、record database/group、可见 semantic_generation 及 LSN。通用 hybrid 页分派必须显式识别这个 magic；未知记录继续拒绝。不得以 CRC 正确代替根到记录的认证链。

成员目录只记录非空 64-ID 组，必须覆盖所有 live 节点，包括无属性节点。标签并集超过 64 的组不得截断：候选构建必须将该组标为一般实体读取回退，使用独立可判定的回退目录条目；在回退条目编码确定并实现前，构建遇到此情况必须整体拒绝，不能发布不完整成员目录。拒绝记入采用率，不能删掉此输入。

类型字段仍复用原 hybrid group/column/冷 ROW 表示及已有目录根；未列化字段走完整一般读取路径。成员认证仅能免除同一组、同一不可变根下重复的 Entity/标签检查，不能免除字段 owner、slot、类型、长度及实际访问页检查。证明通过私有作用域传递，绑定 database/data-region/root/generation/fence/schema/I/O epoch；不能以一个 public bool 开关关闭实体认证。超出 source 工作区的不可拆单值走原查询预算约束的一般拥有式读取，并独立计数，不计作类型块采用。

## 3. 独立目标构建

输入仅接受默认 5/31；其他格式在复制前明确拒绝。目标路径必须全新且不能与源指向同一文件；源只读。以至多一个 128 KiB 段的缓冲复制源图像，不创建全库内存副本。源有效 A/B 头、文件身份及长度在复制前后保持一致；发生提交/替换或无法建立稳定输入时删除私有目标并拒绝。源完整性及目标完整性均不可跳过。

复制只是保留稳定逻辑 ID、schema、索引、向量、LargeValue 和普通记录的中间步骤，不是通过修改版本号宣称支持新布局。受控转码必须先验证源 31 头的 magic/version、336 字节头 CRC、payload CRC、保留位及全部原字段，再按 41 编码语义相同的头：hybrid root 为空、原 emergency reserve 从 336 移到 352、设置 BUILDING、重新计算两级 CRC。不得接受任意旧版本或先改字节再跳过旧校验。源 A/B 的选择与冲突判定复用现有规则。目标建立后先写入并同步 BUILDING 头，再复制数据区及其余 bootstrap 内容；绝不把可正常打开的源 5/31 头复制到目标。空文件、部分头或 BUILDING 头均不能被普通入口视为已完成数据库。

对需要 SegmentEnvelope 的已分配/保留段，先验证源版本 5 的完整 envelope 及 payload，再用 15 重新编码 envelope 和 header CRC；其余已核对为字节兼容的内部页/业务记录不改语义。空闲区域不能被提升成有效数据。若遇到未确认兼容的页种类或版本，拒绝构建。元数据/目录引用及所有逻辑 ID 保持原值，不依靠 benchmark 重建数据。

仅在私有 BUILDING 目标中逐组构造真实成员记录与字段布局；节点和边的原语义保持，已有公开 8×64 转换入口的单次边界不放宽。内部构建进度可以多次持久化，但所有根保持 BUILDING，对外不可打开。原转换准备/局部发布、取消、COW 和回收合同必须保留。输入、目标及临时文件合计不超过 2 GiB；块/source 工作区各不超过 1 MiB，维护工作区不超过 8 MiB。

最终先完整 integrity、全库节点/成员计数和标签/字段 oracle，再按整个目标文件核算至少 10% 稳态空间节省；同时检查构建峰值，不能以最终节省掩盖峰值超限。不合算或无法完成安全回收/压缩时拒绝。只有通过全部准入后，先 flush 数据，再以一次既有 A/B 原子发布清除 BUILDING，持久化确认后返回成功。源文件不删除、不覆盖。

## 4. 更新与恢复

任何节点创建/删除/标签修改必须 COW 对应成员记录并同步更新 live/group 计数，与 Entity、属性及 descriptor 在同一事务发布。仅属性更新复用成员事实，但批证明必须重新绑定新根；字段类型变化/降级遵循现有一般表示。不能先改主节点再异步维护成员目录。尚未实现的一类修改必须在任何 I/O/发布之前拒绝；S2 至少实现更新后读取与重开，S3 再补齐全部已登记生命周期。

普通打开对 A/B 选择出的 BUILDING 状态一律拒绝；不能退回另一个较旧的完成根制造成功。最终发布中断时，要么选到完整已确认根，要么保留 BUILDING 拒绝，不能打开部分图。构建失败目标不自动续建；重新构建使用新私有目标。旧 pin 持有旧 descriptor、目录和成员页，回收沿原 retirement 协议。物理搬迁更新路由与 root/fence 身份，不能复用旧指针证明。

完整 integrity 双向检查目录与物理记录，排除孤儿/重复/缺失成员；按稳定 ID 与主 Entity 流做有界合并，验证 live 集和每个节点完整标签集相等。还检查成员字段目录一致、全部页 owner/槽/代际、descriptor 计数和可见性。不能用全库哈希集合隐藏内存成本。

## 5. 写入实现前检查点

实现前必须核对：版本号/命名空间无冲突；13/39 的旧根和字节不被重解释；完整 source clone/transcode 对所有保留段成立；BUILDING 拒绝覆盖公开 API；超过 64 标签的拒绝无前缀发布；新成员记录进入 integrity/retirement；所有写事务入口在更新支持补齐前安全拒绝。只完成成员 codec 不满足此检查点。
