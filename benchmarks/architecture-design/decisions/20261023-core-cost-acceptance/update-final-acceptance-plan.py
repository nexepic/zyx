from pathlib import Path
for lang in ['zh','en']:
 p=Path(f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx');s=p.read_text()
 if lang=='zh':
  s=s.replace('当前实施进度 90%；P1/P2/P3/P4/P5/P6 已验收，Kuzu 性能目标未达成。','当前实施进度 90%；P1–P6 已验收，P7 正在完成最终验证与客观性能报告。')
  a=s.index('目标是消除数量级差距');b=s.index('\n\n## 1.',a)
  s=s[:a]+'本次验收遵循用户 2026-10-23 的最新要求：完成本页实现及必要验收，最后客观列出优化前／后／Kuzu 结果，不强制性能超过 Kuzu。完成率表示实施与验收工作完成度，不表示达到指定倍率；原 5 倍与 Kuzu 目标保留为实测观察，未达成必须明示。已有源校验、持久化、资源与空间门槛不放宽。'+s[b:]
  s=s.replace('| P7 | 完整生命周期、正式性能与 Kuzu 目标验收 | 10% | 99／99 进程测量完成；目标未达成，本轮收尾 |','| P7 | 公开转换与安全准入闭环、必要验证、客观性能报告 | 10% | 最终验收中 |')
  a=s.index('权重在 P0 后冻结。');b=s.index('\n\n每轮报告',a)
  s=s[:a]+'P0–P6 的已验收权重保持不变。P7 按本次新要求核验实际公开转换、完整目标的原子发布／拒绝、空间与资源准入、必要模块测试及最终客观报告；全部完成后获得最后 10%。仅改文档不增加进度，性能没有达到倍率不冒充已达到。'+s[b:]
  a=s.index('本轮按固定剩余清单收尾');b=s.index('\n\n## 4.',a)
  s=s[:a]+'只完成上述固定 P7 清单，不增加后续优化分支。全库空间至少节省 10%、预算、校验与持久化仍是采用硬条件；未通过就保留 ROW，不为强制采用而新增跨事务全库迁移协议。'+s[b:]
  a=s.index('## 9. P7:') if '## 9. P7:' in s else s.index('## 9. P7：')
  s=s[:a]+'''## 9. P7：最终实施验收

### 9.0 固定验收清单

- [x] **公开 ROW→冷热混合布局转换。** 显式 `stageHybridPropertyConversion(..., coldRowRemainder=true)` 通过原 Native 协调器读取普通 ROW 源，不直接安装夹具；每组保持 64-ID／4096-membership，每次最多 8 组，共用 1 MiB 转换账户。
- [x] **完整指定目标先准备、后准入、一次发布。** 复用原私有 COW 字段／目录／LargeValue／metadata 规划，不引入跨事务中间根。指定 cohort 的所有组完成后核算整个数据库的稳态分配、退役／旧 pin 峰值及文件长度；只有全库至少节省 10% 且峰值在限额内才切换 A/B 根。拒绝、取消、额度失败与崩溃不得留下可见前缀。
- [x] **真实转换后的生产消费与生命周期。** 已补齐 Node／Edge 源、过滤／真实 group／长 ID 冷点查／重开、冷热修改／整列降级／LargeValue 同批发布、backend sealed 创建／删除、逆转换保留其余组、旧 pin、GC／字段池搬迁及退役、取消重试、A/B 部分／完整恢复与 integrity。
- [ ] **最终必要模块验证。** 包含上述公开转换用例、既有生命周期／cohort／目录／格式回归和默认格式拒绝；不运行全量单测，不声称未执行的平台通过。
- [ ] **最后一次统一性能矩阵与归档。** 沿用 P0 输入、oracle、三独立进程／每进程三预热十测量、原持久化及资源合同，输出全部场景的优化前／后／Kuzu、P50/P95、CPU／I/O／内存／文件观测与限制。

完整目标是本次显式指定 cohort，不是强制把任意大库全部转换。空间准入始终按整个数据库计费，不能将 cohort 自身节省比例冒充全库节省。窄数据或大库中的小前缀可能不具备采用资格；此时保持 ROW 是正确结果，不绕过门槛。若要另做跨事务的全库迁移工具，属于后续单独设计，不纳入本次固定验收。

### 9.1 不变的安全与资源合同

保留局部访问校验＋完整 integrity、data-before-root、原源 fence／实际 Entity／竞争 ROW、原代次／manifest、sealed 创建凭证、全部字段类型、旧 pin 与账户退款。普通变更全批保持 64 实际变化／迁移 owner、64 user replacements、64 总列计划；不按 namespace 或组重置额度。转换保持每组 4096 memberships、最多 8 组、共用 1 MiB；维护仍使用原有独立有界账户。

格式默认 5／31、显式实验 13／39，不能据实验功能宣称默认库自动采用。缺少空间准入时公开冷发布在 I/O 前拒绝。无 LZ4 时仍有 RAW 功能回退与空间判定，不承诺同样节省。4096 字节缓存的完整查询使用原有有界复制回退，不提高缓存或弱化校验。

### 9.2 性能只按实际结果报告

保留六个核心场景及全部回归守护，导入分别展示相同 1000 行事务边界对照和 Kuzu 生产 COPY；不把不同边界混为一谈。展示逐进程与合并 P50/P95、全部样本、失败与 N/A，不删除慢样本。三个导入的最大值不是可靠 P95。历史 CPU 归因不能冒充新 profile；文件峰值轮询不能冒充精确空间 census。

原 5 倍结构收益、Kuzu 优势与 5% 回归线均逐项对照并报告；本次完成不以达到倍率或超过 Kuzu 为前提。出现语义错误、安全／资源问题或稳定回归仍须修复，不能用新的完成定义掩盖。

### 9.3 最终交付

同步[存储规范](native-single-file-storage-plan)和[能力矩阵](native-capability-acceptance-matrix)。最终证据保存在 `benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/`，记录新要求、公开转换的全库空间数值、源码／库身份、必要测试、原始测量与完整对比。前轮 `20261023-core-cost-closeout/` 保留为历史；其“公开转换未交付／P7 未通过”是当时状态，不驱动本轮实施。

最后两项完成后标记实施验收 100%，按用户规则执行一次提交与推送到远程 `work`，随后停止，不追加优化任务。100% 不表示性能超过 Kuzu或所有数据库都会采用冷布局。
'''
 else:
  s=s.replace('Implementation progress is 90%; P1/P2/P3/P4/P5/P6 are accepted and the Kuzu performance objective is unmet.','Implementation progress is 90%; P1–P6 are accepted and P7 is completing final verification and objective reporting.')
  a=s.index('Eliminate the work amplification');b=s.index('\n\n## 1.',a)
  s=s[:a]+"Acceptance follows the user's updated 2026-10-23 instruction: complete implementation and necessary verification, then report before/after/Kuzu results objectively without requiring superiority. Progress measures delivered work, not a latency multiplier. Preserve the previous5x/Kuzu targets as measured observations and explicitly report unmet results. Keep all validation, durability, resource and space requirements."+s[b:]
  s=s.replace('| P7 | Complete lifecycle, formal performance, and Kuzu objective acceptance | 10% | 99/99 workers measured; objective unmet, round closed |','| P7 | Public conversion and admission lifecycle, verification and objective report | 10% | Final acceptance underway |')
  a=s.index('Freeze weights after P0.');b=s.index('\n\nEach round reports',a)
  s=s[:a]+"Retain accepted P0–P6 weights. Award P7's final10% only after real public conversion, atomic admission/refusal of the complete specified target, space/resource checks, necessary module tests and final objective reporting are complete. Documentation alone earns no progress; unmet speed targets must remain explicitly unmet."+s[b:]
  a=s.index('Close the fixed remaining queue');b=s.index('\n\n## 4.',a)
  s=s[:a]+'Finish this fixed P7 checklist without follow-on optimization branches. Whole-database10% saving, budgets, validation and durability remain hard adoption gates; retain ROW when they fail, rather than starting a cross-transaction migration protocol to force adoption.'+s[b:]
  a=s.index('## 9. P7:')
  s=s[:a]+'''## 9. P7: Final implementation acceptance

### 9.0 Fixed acceptance checklist

- [x] **Public ROW-to-hot/cold conversion.** Explicit `stageHybridPropertyConversion(..., coldRowRemainder=true)` reads real ordinary ROW sources through the original Native coordinator. Retain64 IDs/4096 memberships per group, at most8 groups and one shared1MiB conversion account.
- [x] **Prepare the complete specified target, admit, then publish once.** Reuse private COW field/directory/LargeValue/metadata planning without cross-transaction intermediate roots. Once every requested group is prepared, charge whole-database steady allocation, retired/pinned peak and file length. Switch A/B roots only for at least10% whole-database saving within peak limits. Refusal, cancellation, resource exhaustion or crashes expose no prefix.
- [x] **Actual converted production consumption and lifecycle.** Cover Node/Edge sources, filter/group/cold long-ID point reads/reopen, hot/cold writes and whole-column demotion, same-batch LargeValue publication, backend sealed birth/deletion, inverse preserving other groups, old pins, GC, field relocation/retirement, cancellation/retry, partial/complete A/B recovery and integrity.
- [ ] **Final necessary module verification.** Include public conversions, existing lifecycle/cohort/directory/format regressions and default-format refusal. No full suite or unexecuted-platform claim.
- [ ] **One final unified matrix and evidence archive.** Preserve P0 inputs/oracles, three independent processes with three warmups and ten measurements, durability and resource contracts. Report every workload's before/after/Kuzu results, P50/P95, CPU/IO/memory/file observations and limitations.

The complete target is the explicitly requested cohort, not mandatory conversion of an arbitrary database. Charge space across the entire database; cohort-only savings never substitute for whole-database savings. Narrow data or a small prefix in a large database may be ineligible; retaining ROW is correct. A cross-transaction whole-database migration tool would be a separately designed project, outside this fixed acceptance.

### 9.1 Unchanged safety and resource contracts

Preserve local-access validation plus full integrity, data-before-root, original source fences/actual Entity/competing ROW checks, generations/manifests, sealed creation authority, all field types, old pins and refunds. Ordinary mutations retain batch-wide64 actual changed/migrated owners,64 user replacements and64 total column plans; never reset bounds per group/namespace. Conversion retains4096 memberships per group, at most8 groups and one1MiB account. Maintenance keeps its original separately bounded account.

Default5/31 and explicit experimental13/39 remain distinct; experimental functionality does not imply default adoption. Missing admission refuses public cold publication before IO. RAW functionality/admission remains available without LZ4, without promising equivalent saving. One-frame4096-byte full queries use the existing bounded copied fallback, without more cache or weaker validation.

### 9.2 Report measured performance

Preserve all six core workloads and every guard. Show both equal1000-row import transaction boundaries and production Kuzu COPY separately. Retain per-process/pooled P50/P95, all samples, failures and N/A; never remove slow samples. Three-import maxima are not reliable P95. Historical CPU attribution is not a new profile; sampled file peaks are not an exact allocation census.

Compare against the historical5x benefit, Kuzu and5% regression observations, but no speedup/superiority is required for this acceptance. Semantic, safety/resource failures or stable regressions still require correction.

### 9.3 Final delivery

Synchronize the [storage specification](native-single-file-storage-plan) and [capability matrix](native-capability-acceptance-matrix). Preserve the revised contract, whole-database conversion allocation evidence, source/library identities, necessary tests, raw measurements and complete comparisons in `benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/`. The preceding closeout archive remains historical; its undelivered-conversion/failed-P7 status no longer drives current implementation.

After the final two items pass, mark implementation acceptance100%, commit/push once to remote `work` under the user's progress rule, and stop without adding optimization tasks.100% does not imply Kuzu superiority or cold-layout eligibility for every database.
'''
 p.write_text(s)
