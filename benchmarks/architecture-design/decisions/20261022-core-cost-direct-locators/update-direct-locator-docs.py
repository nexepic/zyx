from pathlib import Path
b=Path('/workspace/zyx/docs/apps/docs/content/docs')
texts={
'zh':'''### 9.0 当前唯一实施队列

当前验收进度 **90%**；P7 未通过，不提交／推送，不把机制检查计为 Kuzu 目标完成。被否决候选的原始进程和未运行项保留在 `20261022-core-cost-final/`，不能作为最新全部源码的完整性能矩阵。

**只推进主读取供给及其可采用的布局。** 独立 CPU 归因为属性供给／入列约 46.7%、主实体标签验证约 21.3%、B+Tree 候选输出约 17.7%。默认 ROW 过滤／真实分组仍有 100000 次 owner 字段选读；此前 run 接入的有界 P50 对比为 102.764→100.154 ms、103.958→100.567 ms，仅约 2%–3%。合成内存源、编码字节和机制计数都不能代替最终时延。

组内列位置已完成并移出待办：当前隔离候选 **11／37** 使用成员记录版本 2 保存 token 对应的列位置，紧凑列不再建立 FieldDirectory；按组认证、类型化供给、全实体窗口、兼容更新、缺失成员加入、降级／删除、逆转换、旧 pin／故障／完整 integrity 已适配。234 项必要模块检查在两种 membership 配置均通过，默认 62 项通过；另有实际四字段形状的过滤／真实分组／长 ID 点查单项检查通过。默认 5／31 不变，拒绝旧 9／35 等实验文件。见 `20261022-core-cost-direct-locators/`。有序 gather、主扫描消费和写入核心机制均已完成，不继续边角调参。

**完整空间仍不合格：** 下表是模块空间检查，不是时延基准或成熟 medium／holdout 比例。

| 当前 11／37 隔离形状 | ROW steady | 实验 steady | 结论 |
| --- | ---: | ---: | --- |
| 4096 owner，短文本＋整数，正常排空 | 2097152 B | 2097152 B | 仅持平，未达到至少节省 10% 的门禁 |
| 64 User，长 ID＋age＋country＋score | 1441792 B | 1966080 B | +36.36%，目录池占 393216 B |

移除字段树确实使前一 9／35 的 4096-owner 实验少分配 131072 B，但字段节省仍被组目录和 metadata 抵消。保留旧快照时，在既有目标槽范围内完成 31／64 组后第 32 组原子拒绝；这是部分转换，不能算全库完成。物理退役回收已支持持久队列与活动快照验证、被 pin 时零发布及部分／完整 A/B 恢复；详细证据在 `20261022-core-cost-layout-scale/`。完整 integrity 使用既有 8 MiB maintenance 额度，转换／census 仍不超过 1 MiB。

**真实输入决定下一项必须解决混合布局。** 导入器将外部 `id` 同时存为实际字符串属性；medium 225000 个节点、holdout 163000 个节点的 `id` 全部超过 7 字节。所有有数据的节点组都需要非列字段，既有“仅纯列组可多组转换”的合同因此不能直接用于真实数据。长 ID 的功能回退已正确，但独立 OwnerMap／ExtensionDirectory 及其逐 owner 权威检查仍在主读取路径上；“长文本有回退”不等于性能问题已解决。原始 CSV 哈希、字段字节统计与导入源码证据见 `input-field-shapes.json`，不把连续 ID 算术估计当作实际文件分组。

1. **唯一优先候选：热字段列＋既有 ROW remainder。** 分析并明确把长 ID／其他非列字段保存在既有 ROW 存储，避免为普通冷字段另建逐字段 OwnerMap／ExtensionDirectory；热列位置继续随组成员保存。必须先在[存储规范](native-single-file-storage-plan)写清字段级唯一权威、ROW／列合并、主实体／实际访问路径认证、未访问冷字段损坏的完整检查、兼容更新／跨表示移动／删除／旧 pin 和原子发布，再实施，不能直接放松现有竞争 ROW 检查。所有类型、长文本和 LargeValue 功能保持；不新建缓存、标签索引或 backend，不扩展 WAL／向量，不提高额度。
2. **完整成本与转换启动同时证明。** 按真实字段分布核算有效载荷、未用帧、ROW remainder、目录、metadata、退役、峰值和文件增长；保留原 64-owner／8 组／1 MiB 边界及最终全库空间门禁。必须明确冷 ROW 的重新打包／退役，否则缩短记录却保留原整池不构成空间节省。若需私有目标分段构建，先规定源 fence、原 ID／Catalog／索引语义、崩溃清理、最终完整准入和一次可见性切换。无准入的隔离实验不能当作生产采用；初始化／迁移费用独立记录。
3. **真实消费和生命周期完成后收敛剩余主成本。** ROW／紧凑字段走同一生产 filter／真实 group，覆盖稠密／稀疏、数值／短文本、随机 gather、NULL／缺失及冷字段／复杂／LargeValue 回退。分别记录定位、认证、供给／入列和算子成本，再处理已归因的标签验证和候选输出；不扩展写边角。保留更新／删除／降级、旧 pin、重开、搬迁／退役、取消、故障、退款及完整 integrity。4096 字节缓存的完整查询 pinned-page 驱逐问题仍未解决。
4. 上述核心工作完成后冻结最终候选，只跑一次 P7 统一矩阵。保留 P0 口径、全部样本和实际 oracle；失败不能改权重或宣布 100%。

已实现的局部访问校验＋完整 integrity、源 fence、原 CACHE 账户、BufferPool 偏移索引／脏链及 Overlay 64 项／16 KiB 上限必须保留。历史实验保留在证据目录，不再作为待办；两个旧 metadata 延迟回收断言在本轮前冻结库同样失败的证据仍单列，不计为通过。

''',
'en':'''### 9.0 Current implementation queue

Accepted progress is **90%**; P7 fails. Do not commit/push or count mechanism checks as completion of the Kuzu objective. Rejected-candidate raw processes and unrun cases remain in `20261022-core-cost-final/`, not a complete matrix for current source.

**Work only on primary read supply and an adoptable layout.** Independent CPU attribution is about 46.7% property supply/delivery, 21.3% primary entity-label validation and 17.7% B+Tree candidate production. Default ROW filtering/real grouping still selects 100000 owner fields. The preceding bounded run-integration P50 pair is 102.764→100.154 ms and 103.958→100.567 ms, only about 2–3%. Synthetic memory sources, encoded bytes and counters cannot replace final latency.

Group-owned column locators are implemented, not pending: isolated candidate **11/37** uses membership record version 2 for token-aligned column locators and builds no compact-column FieldDirectory. Group authentication, typed supply, full-entity windows, compatible updates, missing-member insertion, demotion/deletion, inverse conversion, old pins/faults/full integrity are adapted. All 234 necessary module checks pass in each membership configuration and 62 default checks pass; an additional actual four-field-shape filter/real-group/long-ID point-lookup case passes. Default 5/31 is unchanged; old 9/35 and other experimental files are rejected. Evidence: `20261022-core-cost-direct-locators/`. Ordered gather, primary consumption and core write mechanisms are complete; stop marginal tuning there.

**Whole space is still ineligible.** These are module space checks, not latency benchmarks or mature medium/holdout ratios.

| Current isolated 11/37 shape | ROW steady | Experimental steady | Result |
| --- | ---: | ---: | --- |
| 4096 owners, short text plus integer, drained | 2097152 B | 2097152 B | Only equal; fails the >=10% saving gate |
| 64 Users, long ID/age/country/score | 1441792 B | 1966080 B | +36.36%; directory pools cost 393216 B |

Removing the field tree saves 131072 B against the preceding 9/35 4096-owner experiment, but group-directory/metadata costs still cancel property saving. With the old snapshot retained, the existing destination range completes 31/64 groups and atomically refuses group 32; this is partial conversion, not whole completion. Physical retirement supports durable-queue/active-snapshot authentication, all-pinned zero publication and partial/complete A/B recovery; see `20261022-core-cost-layout-scale/`. Full integrity uses the existing 8 MiB maintenance allowance; conversion/census remains <=1 MiB.

**Actual input makes mixed layout the next required task.** The importer stores external `id` as a real string property. Every ID in medium's 225000 nodes and holdout's 163000 nodes exceeds seven bytes. Every populated node group therefore needs noncolumn fields, so the existing pure-column-only multi-group conversion cannot directly serve actual input. Long-ID fallback is correct, but separate OwnerMap/ExtensionDirectory and owner-level authority checks remain on the primary supply route. Functional long-text fallback does not solve its performance cost. CSV hashes, field-byte distributions and importer evidence are in `input-field-shapes.json`; consecutive-ID arithmetic estimates are not persisted group counts.

1. **One priority candidate: hot columns plus existing ROW remainder.** Analyze keeping long IDs/other noncolumn fields in existing ROW storage, avoiding per-field OwnerMap/ExtensionDirectory for ordinary cold fields, while retaining group-owned hot-column locators. Specify field-level unique authority, ROW/column merging, actual primary/accessed-path authentication, full checks for unvisited cold-field corruption, compatible updates/representation transitions/deletion/old pins and atomic publication in the [storage specification](native-single-file-storage-plan) before implementation. Do not simply relax current competing-ROW checks. Preserve every type, long text and LargeValue behavior. Add no cache, label index or backend; expand neither WAL/vector work nor allowances.
2. **Prove whole cost and reachable conversion together.** Account for useful payload, unused frames, ROW remainder, directories, metadata, retirement, peak and file growth using actual field distributions. Keep existing 64-owner/eight-group/1 MiB bounds and final whole-database space admission. Specify cold-ROW repacking/retirement: shorter records in the same sealed pools are not allocation savings. If private staged construction is needed, specify source fencing, original ID/Catalog/index semantics, crash cleanup, final complete admission and one visibility switch. Ungated laboratory publication cannot authorize production adoption. Report initialization/migration separately.
3. **Complete actual consumption/lifecycle, then converge remaining primary costs.** ROW/compact fields use the same production filter/real group, covering dense/sparse data, numbers/short text, random gather, NULL/missing and cold-field/complex/LargeValue fallback. Attribute positioning, authentication, supply/delivery and executor work separately, then address the already attributed label-validation/candidate-output costs without write margins. Retain updates/deletion/demotion, old pins, reopen, relocation/retirement, cancellation, faults, refunds and full integrity. The 4096-byte-cache full-query pinned-page eviction issue remains unresolved.
4. Freeze the final candidate only after this core work and run the unified P7 matrix once. Preserve P0's comparison contract, every sample and actual oracle. Failure cannot change weights or become 100%.

Preserve accessed-path validation plus full integrity, source fences, original CACHE accounting, BufferPool's offset index/dirty chain and Overlay's 64-operation/16 KiB bounds. Historical experiments remain evidence, not pending tasks. The two old deferred-metadata-reclamation assertions also fail on the pre-round frozen library and stay separate, never counted as passing.

'''}
for lang,body in texts.items():
 p=b/lang/'zyx/architecture/optimization.mdx';s=p.read_text();a=s.index('### 9.0 ');z=s.index('### 9.1 ',a);p.write_text(s[:a]+body+s[z:])
 p=b/lang/'zyx/architecture/native-capability-acceptance-matrix.mdx';s=p.read_text()
 if lang=='zh':s=s.replace('当前隔离候选为35（profile 9）','当前隔离候选为37（profile 11；拒绝旧35）').replace('尚未通过完整成本／性能门禁。旧格式33','组内列位置的 234 项双配置检查及 62 项默认检查见 `20261022-core-cost-direct-locators/`；真实输入全部节点含长 ID，混合布局与完整成本／性能门禁仍未通过。旧格式33')
 else:s=s.replace('current isolated candidate is 35 (profile 9)','current isolated candidate is 37 (profile 11; old 35 rejected)').replace('complete cost/performance gates remain open. Frozen format33','group-owned locator evidence in `20261022-core-cost-direct-locators/` passes 234 checks in each configuration and 62 default checks. All actual-input nodes retain long IDs, leaving mixed-layout and whole cost/performance gates open. Frozen format33')
 p.write_text(s)
