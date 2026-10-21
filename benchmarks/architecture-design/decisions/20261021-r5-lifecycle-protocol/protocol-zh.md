# R5.1 邻接协议与覆盖树生命周期验收

本项验收的是一致性协议和完整生命周期原型，不是生产采用或性能收益。R5.2 的生产writer hook、访问路径准入及覆盖／共享分段方案的最终选择，R5.3 的成本／规划／留出负载，R7 的同条件性能与Kuzu对照均保持未完成。分母仍为24。

## 字节与权威合同

候选复用一张共享原生Packed BTree，不恢复per-node roots或无界delta链。逻辑key为direction u8、NodeId big-endian u64、EdgeId big-endian u64，共17字节；逻辑value为NeighborId和TypeToken两个little-endian u64，共16字节。FixedBTreeMap将33字节记录编码为key-only叶（BLVL_EMPTY），内部separator仍有完整记录／child成本；不能把33字节当实际分配。两个方向每条Edge各一条，自环也必须有两个不同direction项。

低层协调器使用调用方拥有的、低62位、已存在的实验IndexId；不安装生产namespace／持久激活标记。bootstrap使用同一NativeGraphWriteTransaction中的最终主记录与有界bulk tree构建；增删使用stageCoveredAdjacencyCandidate，从私有最终Edge状态推导两方向。创建后同事务删除不生成方向变化；成功finalize封闭后续暂存，失败保持pending，不能忽略错误只提交main。move和重试保留index／type planner／catalog参与者身份。

主Edge是权威。私有完整页及lookahead先核对主Edge身份、source/target/type和顺序，再过滤或交给消费者。每页64项不是数据库上限；写协调一次最多64个Node/Edge变化，endpoint只读缓存不计作变化。Node-only创建／改标签／删除可维护已知统计而不改写候选树，但仍认证现有root／首叶类型。其他label/property索引仍由同一事务中的既有schema planner暂存，低层API不冒充自动生产hook。提供的type/index及catalog协作者必须属于同一metadata域。

## 故障检测矩阵

| 情形 | 普通候选读 | 受控提交 | 显式完整性 |
|---|---|---|---|
| 合法CRC、错误endpoint/type | 访问的整批及lookahead在输出前与主Edge比较并拒绝 | 修改的旧方向必须精确匹配权威记录 | 两方向遍历及主Edge反查均拒绝 |
| 缺forward/reverse | 未访问的缺项不能由单方向点读证明 | 删除受影响Edge时核对两方向，缺任一拒绝 | 主Edge逐项查两个方向，发现遗漏 |
| 同逻辑前缀的重复value | Fixed map lookahead拒绝，包括跨叶 | 受影响prefix认证拒绝 | 全树逻辑唯一性检查拒绝 |
| 孤儿或多余项 | 主Edge不存在／端点不符则拒绝 | 修改范围旧值不符则拒绝；不扫描无关存量 | tree→Edge和Edge→tree双向核对 |
| 外域pin、错误Root/PageId/物理generation | 域、注册pin、Index Root及Page Directory/header认证 | 同一事务source和页/根准备继续已有认证 | 完整路由／归属认证 |
| 陈旧/伪造continuation | 私有token绑定database/generation/node/direction；逻辑pair重新核对previous和精确next | 不授予写权限 | 不代替全局完整性 |

完整性报告是观测数据，不是可伪造的采用凭据。显式只读backend入口自行在它实际保留的pin中做有独立Edge／metadata页上限、账户和取消的全局核对；不是点读／规划fallback。之后的介质破坏按上表在访问或下一次integrity发现，不承诺每个点读都扫描全库。默认读入口、其他接口及普通writer均不自动采用候选。

## 生命周期、资源与观测

数据页、两方向树、主Entity Maps、提供的type索引及已知图／完整token统计共用原有data fence→非当前A/B→superblock fence→确认。unknown统计不变，token0是合法无类型计数但不生成类型标签索引项；上下溢拒绝。Node标签/数量及最后Edge删除／空候选root／重建均验证；逻辑ID不复用。

旧pin保持旧主记录和树，持久重开按完整A/B选择版本。现有索引页COW、受控整池搬迁、退休、pin保护、释放／物理槽复用保持原协议。原型协调私有工作账户上限1MiB，通用事务和维护仍遵守既有资源合同；不将局部账户等价为完整RSS或最终生产资源准入。

reference和topology页输出使用既有buffer-admission持有实际vector／名称容量；私有认证、类型名转换及重叠容量在交付前计费。无借用span逃逸，未验证名称不缓存。显式setup有单独setup_edges观测；reference_pages／candidate_records包括返回页及lookahead，continuation_records另计前驱核对；topology_pages是适配次数，不能相加当独立IO。正式计时仍须关闭instrumentation并按R7登记包含范围。

## 验收证据

- 初始90边、多重边、自环、双向create/delete、主记录／type索引／统计、旧pin、A/B、重开、pool搬迁／退役／别名保护与全局corruption核对，见之前独立检查点。
- 真实只读backend加同一LogicalPlanReadRuntime：WITH绑定源的实际候选调用、OPTIONAL／聚合／无序重复邻居gather、hybrid属性；真实overlay在只读文件上仅暂存并拒绝commit；typed/untyped名称及输出额度生命周期。
- 新增Node-only生命周期／wrong root／变化上限、最后Edge删除后保留空root并重建、4轮固定seed偏斜插删，34个节点逐一核对两方向及0/1/2/7完整统计；同执行器两跳保留四条parallel-edge路径。
- 完整90边热点分配：key-only1703936B，保留canonical及covered后1835008B，退休0B、峰值1835008B、文件5373952B。未扣除尚不存在的生产替换，也没有宣称时延收益。
- 最终56相关模块排除已有独立证据且未受影响的成熟多组空间例：默认897通过，候选1223通过／1条件跳过。只验证Linux；无全库单测或中途性能基准。源码补丁、hash、build/link参数、原始JSON／日志及初始失败随本目录保存。

据此R5.1协议／原型验收完成，总进度14/24（58.3%）。本项不关闭R3.2/R3.3、R5.2/R5.3或任何R7门槛。
