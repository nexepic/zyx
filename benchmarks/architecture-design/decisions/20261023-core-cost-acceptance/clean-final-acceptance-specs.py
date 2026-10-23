from pathlib import Path
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance'
for lang in ['zh','en']:
 root=r/f'docs/apps/docs/content/docs/{lang}/zyx/architecture'
 p=root/'native-single-file-storage-plan.mdx';s=p.read_text();history=s.index('## 19.')
 (d/f'historical-storage-notes-{lang}.md').write_text(s[history:])
 if lang=='zh':
  free=s[s.index('### 页级空闲摘要候选合同'):s.index('### P7 紧凑短文本')].replace('### 页级空闲摘要候选合同（P7，尚未验收）','### 19.1 增量提交与供给合同').replace('性能尚待 P7 验收。','性能按最终客观结果报告。')
  short=s[s.index('短文本列新增 scalar kind 4'):s.index('该候选的字节／初步生命周期检查')]
  cold=s[s.index('**唯一权威与位置。**'):s.index('块／位置字节原型的初步证据')]
  footer='''### 19.4 公开转换、完整准入与生命周期

显式实验 13／39 已支持 `stageHybridPropertyConversion(..., coldRowRemainder=true)`。调用必须选择 compact forward 布局；公开发布必须提供全库空间准入，缺少准入在 I/O 前拒绝。准备探针仍不发布。非冷旧 cohort 保持原纯列限制；冷 cohort 允许有热列、全冷或混合字段，最多 8 组，每组 64-ID／4096-membership，共用一个不超过 1 MiB 的原有转换账户。

原 Native writer fence 读取实际 Entity／ROW／Catalog／字段来源，完成指定全部组的私有字段／目录、LargeValue payload／manifest／content-index 及 metadata 规划后，才执行一次最终准入与 A/B 发布。新大值不独立提交；旧源 fence、原逻辑 ID 和 schema／索引语义保留。全库稳态分配至少节省 10%，且原 peak／file 上限满足，才采用。额度不足、无法完整归因、无收益、取消或故障都不得发布部分组；原根保持权威，原恢复／回收处理私有遗留。跨事务全库迁移不是这个有界操作，不以 8 组自身节省冒充全库节省。

同一变更批次的跨组 Node／Edge 共享字段帧必须合并到一个私有页集合；按 namespace 合并目录编辑，最后一次发布。保留 64 实际变化／迁移 owner、64 user replacements、64 总列计划，整列降级先按原热集合认证本组冷目录并迁移所有幸存成员。新 owner 使用原 sealed ID lease／Entity 创建证明。热／冷修改、删除／最后组、清空、逆转换、LargeValue 回收、字段池搬迁与旧 pin 均不增加 OwnerMap／EXT、缓存或后端。

公开转换验收使用实际普通 ROW 源而非描述符字节安装：12 项覆盖 Node／Edge、整批采用／拒绝、生产过滤／group／冷点查／重开、冷热修改／降级、sealed 创建／删除、原子新大值及回收、逆转换保留其余组、字段池移动／退役、取消／重试与部分／完整 A/B。两个实验 membership 配置各 576 项、默认 155 项及两种缓存完整查询 2 项必要检查通过；不执行全量单测，不声称三平台全部实测。空间案例为 512 owner、五字段：整个测试库稳态 1,441,792→1,048,576 字节，节省 27.27%；这不是 medium／holdout 的空间结论。

### 19.5 当前验收和证据

默认格式 5／31 不变；实验 profile 13／Superblock 39 显式启用，352 字节 header 不变，拒绝旧实验 32／33／35／37 文件。无 LZ4 保持 RAW 回退与同一准入，不承诺压缩收益。当前唯一任务清单是[优化文档](optimization)；其完成率按用户最新要求表示实现／验证／客观报告，不等于超越 Kuzu 或任意数据库都能采用冷布局。

最终证据：`benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/`。过时的逐轮状态已移到其中的 historical-storage-notes-zh.md；保留协议合同及原始失败证据，不再把早期“发布关闭／生命周期未完成”当作当前限制。\n'''
  s=s[:history]+'## 19. 当前优化合同与验收\n\n'+free+'### 19.2 紧凑短文本\n\n'+short+'### 19.3 组内热列与冷 ROW 块\n\n'+cold+footer
 else:
  free=s[s.index('### Page free-space summary candidate contract'):s.index('### P7 compact short-text')].replace('### Page free-space summary candidate contract (P7, not accepted)','### 19.1 Incremental commit and supply contracts')
  short=s[s.index('Short-text columns add scalar kind 4'):s.index('Initial byte/lifecycle evidence')] if 'Short-text columns add scalar kind 4' in s and 'Initial byte/lifecycle evidence' in s else ''
  # Preserve the normative text paragraphs, omitting the old progress log.
  if not short:
   a=s.index('### P7 compact short-text');b=s.index('### Frozen isolated candidate',a);part=s[a:b];paragraphs=part.split('\n\n');short='\n\n'.join(paragraphs[2:4])+'\n\n'
  cold=s[s.index('**Unique authority and locations.**'):s.index('Initial block/location byte prototype evidence')] if 'Initial block/location byte prototype evidence' in s else ''
  if not cold:
   a=s.index('**Unique authority and locations.**');b=s.index('The initial block/location',a) if 'The initial block/location' in s[a:] else s.index('20261022-core-cost-cold-row-codec',a);cold=s[a:b];cut=cold.rfind('\n\n');cold=cold[:cut]+'\n\n'
  footer='''### 19.4 Public conversion, complete admission and lifecycle

Explicit experimental13/39 supports `stageHybridPropertyConversion(..., coldRowRemainder=true)`. Require compact forward conversion and whole-database admission for publication; missing admission refuses before IO. Preparation probes remain nonpublishing. Legacy noncold cohorts retain their pure-column restriction; cold cohorts permit hot/cold or entirely cold fields. Keep at most8 groups,64 IDs/4096 memberships per group and the original shared1MiB account.

The original Native writer fence authenticates actual Entity/ROW/Catalog/field sources. Prepare every specified group and its private field/directory, LargeValue payload/manifest/content-index and metadata work before a single final admission/A/B decision. Never commit new values independently. Preserve source fences, logical IDs and schema/index semantics. Adopt only with at least10% whole-database steady allocation saving within original peak/file limits. Resource exhaustion, incomplete attribution, insufficient saving, cancellation and faults expose no prefix; original roots stay authoritative and existing recovery/reclamation handles private leftovers. This bounded operation is not a cross-transaction database migration; cohort-only savings cannot replace whole-database saving.

Cross-group Node/Edge writes sharing a FieldPage merge into one private image set and namespace-specific directory edits before one publication. Keep64 actual changed/migrated owners,64 user replacements and64 total column plans. Whole-column demotion authenticates original cold directories under the original hot set and migrates every surviving member. New owners require original sealed ID leases/Entity creation proofs. Writes, deletion/final groups, clear, inverse, LargeValue reclamation and field relocation preserve old pins without new OwnerMap/EXT, cache or backend.

Twelve public tests use actual ordinary ROW sources rather than descriptor byte injection. They cover Node/Edge, atomic adoption/refusal, production filter/group/cold point/reopen, hot/cold write/demotion, sealed birth/deletion, atomic new large values and reclamation, inverse retaining other groups, field relocation/retirement, cancellation/retry and partial/complete A/B. Both experimental membership variants pass576 related checks each, default passes155, and both full-query cache modes pass2 necessary checks. No full suite or three-platform measurement claim. A512-owner, five-field test database falls from1,441,792 to1,048,576 steady bytes (27.27%); this is not a medium/holdout space claim.

### 19.5 Current acceptance and evidence

Default5/31 remains unchanged; explicit experimental profile13/Superblock39 retains the352-byte header and rejects old experimental32/33/35/37 files. RAW remains available without LZ4 under the same gate, without promised compression gains. The [optimization plan](optimization) is the sole implementation queue. Under the user's updated instruction, completion denotes delivered implementation/verification/objective reporting, not Kuzu superiority or universal cold-layout eligibility.

Final evidence: `benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/`. Historical progress notes moved to historical-storage-notes-en.md there. Retain protocol contracts and original failures; early publication-closed/incomplete-lifecycle notes no longer define the current boundary.\n'''
  s=s[:history]+'## 19. Current optimization contracts and acceptance\n\n'+free+'### 19.2 Compact short text\n\n'+short+'### 19.3 Group-owned hot columns and cold ROW blocks\n\n'+cold+footer
 p.write_text(s)
 p=root/'native-capability-acceptance-matrix.mdx';s=p.read_text();marker='原 Native 协调器已接入冷字段分派' if lang=='zh' else 'Original Native coordination now dispatches cold fields';a=s.index(marker);(d/f'historical-capability-notes-{lang}.md').write_text(s[a:]);s=s[:a]
 if lang=='zh':s+='''## 当前优化验收（2026-10-23）

| 路径 | 当前能力与边界 |
|---|---|
| 公开冷转换 | 实验 13／39 显式启用；普通 ROW 源、最多 8 组／1 MiB、整批准备后一次发布；缺少全库准入拒绝 |
| 空间／恢复 | 全库至少节省 10% 且原峰值／文件上限满足才采用；不满足保持 ROW；取消／故障无可见前缀 |
| 生命周期／消费 | 实际转换后的 Node／Edge、过滤／group／冷点查、冷热修改／降级、sealed 创建／删除、逆转换、LargeValue／搬迁回收、旧 pin／重开／integrity |
| 必要测试 | 公开转换 12 项；两实验配置各 576 项（含公开用例）；默认 155 项；两种缓存完整查询 2 项，均通过 |
| 性能／完成率 | 按用户新要求完成实施和客观报告，不以超过 Kuzu 为前提；未达到的倍率如实报告 |

完整证据及版本身份见 `benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/`。默认 5／31 不自动采用实验布局；空间案例不是全体数据的承诺。历史逐轮状态已归档，不再作为当前能力限制。C0 的更广泛 API ledger 完成标准与跨平台发布门禁仍独立，不被本轮 P7 完成代替。\n'''
 else:s+='''## Current optimization acceptance (2026-10-23)

| Path | Current capability and boundary |
|---|---|
| Public cold conversion | Explicit experimental13/39; actual ROW sources; at most8 groups/1MiB; prepare all then publish once; missing whole-database admission refuses |
| Space/recovery | Require10% whole-database saving and original peak/file limits; otherwise retain ROW; cancellation/faults expose no prefix |
| Lifecycle/consumption | Actual converted Node/Edge, filter/group/cold point, hot/cold writes/demotion, sealed birth/deletion, inverse, large-value/relocation reclamation, old pins/reopen/integrity |
| Necessary tests | Public12; both experimental modes576 each including public cases; default155; two full-query cache modes2; all pass |
| Performance/completion | Updated user instruction requires delivered implementation and objective reporting, not Kuzu superiority; retain unmet speed targets honestly |

Evidence and identities: `benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/`. Default5/31 does not automatically adopt the experimental layout; the space fixture is no universal data promise. Historical round notes are archived rather than current capability limits. Broader C0 API-ledger completion and cross-platform release gates remain independent.\n'''
 p.write_text(s)
