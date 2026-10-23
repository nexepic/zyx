from pathlib import Path
base=Path('/workspace/zyx/docs/apps/docs/content/docs')
zh='''### 9.0 当前唯一实施队列

当前验收进度 **90%**；P7 未通过，不提交／推送，不把机制检查计为 Kuzu 目标完成。被否决候选的 47 个成功进程和未运行项保留在 `20261022-core-cost-final/`，不是当前全部源码的完整性能矩阵。

**只推进主读取供给及其可采用的布局。** 独立 CPU 归因为属性供给／入列约 46.7%、主实体标签验证约 21.3%、B+Tree 候选输出约 17.7%。默认 ROW 过滤／真实分组仍有 100000 次 owner 字段选读；已完成的 run 接入仅有一次有界对比，P50 分别为 102.764→100.154 ms、103.958→100.567 ms，约 2%–3%，没有质变。不能假定改文本编码即可达到 Kuzu，也不能用 P5 合成源约 6.5 ms 当作绝对性能下限。

已完成部分移出待办：有序 gather、同步最多 64 单元的类型化 run、主扫描消费、ROW 槽身份复用和实际源认证后的目录 no-op；证据在 `20261022-core-cost-read-runs-consumed/`、`20261022-core-cost-property-identity/`。写入路径已经具备按需容量、端点只读、源页批准备、逻辑子项保留和局部 radix 更新，不继续边角调参。当前实验 9／35（默认 5／31 不变）已实现短文本列和按组 Entity／竞争 ROW／extension 认证、同步列供给；190 项必要实验模块检查在两种 membership 配置均通过，35 项默认检查通过。生产过滤计数 63、真实分组 person=63／seven=1 与 ROW oracle 一致，并断言消费新路径。见 `20261022-core-cost-native-short-text/`、`20261022-core-cost-native-group-runs/`。这些尚不是布局采用或性能验收。

**当前核心障碍是完整分配成本与转换可达性，已不再只是字段编码。** 以下均是空间模块检查，不是时延基准，不能外推 medium／holdout 成熟比例：

| 两个窄字段（短文本＋整数） | ROW steady | 实验 steady | 结论 |
| --- | ---: | ---: | --- |
| 512 owner，一次 8 组私有候选 | 917504 B | 1179648 B | +28.57%，至少节省 10% 的门禁正确拒绝 |
| 4096 owner，逐组隔离实验并正常排空退役 | 2097152 B | 2228224 B | +6.25%，仍不能采用 |

4096-owner 实验中字段池从 655360 降至 393216 B，但新增目录池 262144 B、metadata 从 524288 增至 655360 B，抵消字段节省。128 KiB 整池粒度、独立目录所有权以及转换产生的 metadata 必须一起计费。保留旧快照时，在既有目标槽范围内完成 30／64 组后第 31 组拒绝；所有新旧内容、原子拒绝、重开和完整 integrity 正常，不能将部分转换当作全库完成。

物理退役排空的窄入口已适配：只处理持久队列、验证 Catalog／allocator／活动快照，不改字段／descriptor 权威，其他未适配写仍拒绝。29 项相关实验检查、24 项默认检查通过，含部分／完整 A/B、旧 pin、全部被 pin 时零发布。完整 integrity 使用既有 8 MiB maintenance 额度，转换／census 仍不超过 1 MiB。两个未修改的 metadata 延迟回收断言在本轮前冻结库上同样失败，单独保留，不计为通过。证据 `20261022-core-cost-layout-scale/`。

1. **先消除不必要的字段定位层和证明完整成本。** 优先评估把选中列的位置随既有组成员结构保存，避免已有 token／presence 之后再查独立字段树；同时核对组定位是否可复用既有 Page Directory，不能仅凭 radix／B+Tree 名称选型。先写清身份、字节、旧 pin、COW、目录权威、损坏与兼容合同，再实现唯一候选。按实际 medium／holdout 字段分布分别核算有效载荷、未用帧、目录、metadata、退役、峰值和文件增长；不得只看紧凑编码字节。
2. **证明转换能够启动并完成。** 保持原 64-owner／8 组／1 MiB 边界及最终全库空间门禁；不得用无准入的隔离实验发布冒充生产采用。若必须在私有目标中分段构建，先明确源 fence、原 ID／Catalog／索引语义、崩溃清理、最终完整准入和一次可见性切换，不引入新 backend、缓存、标签索引或更高额度。初始化／迁移费用独立记录。
3. **完成真实供给与生命周期后收敛剩余主成本。** ROW／紧凑字段走同一生产 filter／真实 group，覆盖稠密／稀疏、数值／短文本、随机 gather、NULL／缺失和长文本／复杂／LargeValue 回退。按组定位不能省略真实实体、竞争 ROW 和标签认证；分别记录定位、认证、供给／入列和算子成本。再针对已归因的标签验证和候选输出处理剩余主成本，不扩展到写边角、WAL 或向量。保留单项更新／删除／降级、旧 pin、重开、搬迁／退役、取消、故障、退款及完整 integrity；4096 字节缓存的完整查询 pinned-page 驱逐问题仍未解决，不宣称修复。
4. 上述核心工作完成后冻结最终候选，只跑一次 P7 统一矩阵。保留 P0 口径、全部样本和实际 oracle；失败不能改权重或宣布 100%。

已实现的局部访问校验＋完整 integrity、源 fence、原 CACHE 账户、BufferPool 偏移索引／脏链及 Overlay 64 项／16 KiB 上限必须保留。详细合同见[存储规范](native-single-file-storage-plan)，历史实验保留在证据目录，不再作为待办。

'''
en='''### 9.0 Current implementation queue

Accepted progress is **90%**; P7 fails. Do not commit/push or count mechanism checks as completion of the Kuzu objective. The rejected candidate's 47 successful processes and unrun cases remain in `20261022-core-cost-final/`; they are not a complete performance matrix for current source.

**Work only on primary read supply and an adoptable layout.** Independent CPU attribution is about 46.7% property supply/delivery, 21.3% primary entity-label validation and 17.7% B+Tree candidate production. Default ROW filtering/real grouping still selects 100000 owner fields. One bounded run-integration pair yields P50 102.764→100.154 ms and 103.958→100.567 ms, only about 2–3%, without a qualitative gain. Text encoding alone cannot be assumed to reach Kuzu; P5's roughly 6.5 ms synthetic source is not an absolute lower bound.

Completed work is no longer pending: ordered gather, synchronous typed runs of at most 64 cells, primary-scan consumption, ROW slot-identity reuse and directory no-ops after actual-source authentication. Evidence: `20261022-core-cost-read-runs-consumed/` and `20261022-core-cost-property-identity/`. Writes already use demand-driven capacity, read-only endpoints, source-page batches, logical-child preservation and local radix updates; stop marginal tuning there. Experimental 9/35 (default 5/31 unchanged) implements short-text columns, grouped Entity/competing-ROW/extension authentication and synchronous column supply. All 190 necessary experimental module checks pass in each membership configuration, with 35 default checks. Production filtering returns 63 and real grouping person=63/seven=1, matching ROW and asserting new-path consumption. See `20261022-core-cost-native-short-text/` and `20261022-core-cost-native-group-runs/`. These are neither layout adoption nor performance acceptance.

**The core obstacle is complete allocation cost and reachable conversion, beyond field encoding.** These are space module checks, not latency benchmarks or mature medium/holdout ratios:

| Two narrow fields (short text plus integer) | ROW steady | Experimental steady | Result |
| --- | ---: | ---: | --- |
| 512 owners, one eight-group private candidate | 917504 B | 1179648 B | +28.57%; the >=10% saving gate correctly refuses |
| 4096 owners, isolated group publications with retirement draining | 2097152 B | 2228224 B | +6.25%; still ineligible |

In the 4096-owner experiment, property pools fall from 655360 to 393216 B, but 262144 B of new directory pools and metadata growth from 524288 to 655360 B cancel the saving. Charge 128 KiB pool granularity, separate directory ownership and conversion metadata together. With the old snapshot retained, the existing destination range admits 30/64 groups and refuses group 31. Current/old values, atomic refusal, reopen and full integrity remain correct; partial conversion cannot be counted as whole-database completion.

The narrow physical-retirement entry is adapted: it consumes durable queue records and authenticates Catalog/allocator/active snapshots, changes no field/descriptor authority, and leaves other unadapted writes refused. All 29 related experimental checks and 24 default checks pass, including partial/complete A/B, old pins and all-pinned zero publication. Full integrity uses the existing 8 MiB maintenance allowance; conversion/census remains <=1 MiB. Two unchanged deferred-metadata-reclamation assertions also fail on the frozen pre-round library and are retained separately, never counted as passing. Evidence: `20261022-core-cost-layout-scale/`.

1. **Remove unnecessary field positioning layers and prove whole cost first.** Prefer evaluating column locators alongside existing group membership, avoiding a separate field-tree lookup after token/presence authentication. Determine whether group positioning can reuse the existing Page Directory; radix/B+Tree names alone cannot choose the design. Specify identity, bytes, old pins, COW, directory authority, corruption and compatibility before implementing one candidate. Use actual medium/holdout field distributions to account for useful payload, unused frames, directories, metadata, retirement, peak and file growth separately; compact encoded bytes alone are insufficient.
2. **Prove conversion can start and finish.** Keep existing 64-owner/eight-group/1 MiB bounds and final whole-database space admission. Ungated isolated laboratory publications cannot authorize production adoption. If private staged construction is needed, specify source fencing, original ID/Catalog/index semantics, crash cleanup, final complete admission and one visibility switch first. Add no backend, cache, label index or larger allowance. Report initialization/migration separately.
3. **Finish actual supply/lifecycle, then converge remaining primary costs.** ROW/compact fields use the same production filter/real group, covering dense/sparse data, numbers/short text, random gather, NULL/missing and long-text/complex/LargeValue fallback. Grouped positioning cannot omit actual entities, competing ROW or label authentication. Attribute positioning, authentication, supply/delivery and executor work separately. Then address the already attributed label-validation/candidate-output costs, without write margins, WAL or vector expansion. Retain bounded updates/deletion/demotion, old pins, reopen, relocation/retirement, cancellation, faults, refunds and full integrity. The 4096-byte-cache full-query pinned-page eviction failure remains unresolved.
4. Freeze the final candidate only after this core work and run the unified P7 matrix once. Preserve P0's comparison contract, every sample and actual oracle. Failure cannot change weights or become 100%.

Preserve implemented accessed-path validation plus full integrity, source fences, original CACHE accounting, BufferPool's offset index/dirty chain and Overlay's 64-operation/16 KiB bounds. Contracts remain in the [storage specification](native-single-file-storage-plan); historical experiments stay in evidence, not the pending queue.

'''
for lang,body in [('zh',zh),('en',en)]:
 p=base/lang/'zyx/architecture/optimization.mdx';s=p.read_text();a=s.index('### 9.0 ');b=s.index('### 9.1 ',a);p.write_text(s[:a]+body+s[b:])
