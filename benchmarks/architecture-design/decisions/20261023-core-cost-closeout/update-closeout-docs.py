from pathlib import Path
for lang in ['zh','en']:
 p=Path(f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx');s=p.read_text()
 if lang=='zh':
  s=s.replace('更新：2026-10-22。','更新：2026-10-23。',1)
  s=s.replace('候选未达门槛；继续定位核心成本','最终候选已冻结，统一测量与收尾中')
  s=s.replace('性能目标未达到时继续在本范围定位和修复，不另加无关优化凑进度。若证据证明必须改变持久化协议或公开语义，先提交具体设计修订供讨论，保持目标未完成，不能偷偷放宽验收。','本轮按固定剩余清单收尾，不再新增优化分支。Kuzu 对照是最终实测结果，不是无限继续实现的条件；未达到门槛时如实保留 P7 未通过与 90% 验收进度。需要新的布局启动协议、持久化协议或公开语义变更时，不在本轮自动展开，也不放宽原门禁。')
  a=s.index('### 9.0 当前唯一实施队列');b=s.index('### 9.1 性能窗口与准入',a)
  s=s[:a]+'''### 9.0 固定清单收尾与采用边界

验收进度仍为 **90%**；P7 与 Kuzu 目标不能用测试数量或报告完成替代。本轮完成下述实现与验证后冻结，不再自动新增优化任务。最终证据及完整逐场景对比统一存放在 `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/`。

- **跨组原子修改已补齐。** Node／Edge 可共享同一逻辑 FieldPage；兼容热列先合并，各组冷块继续使用同一私有页集合，按 namespace 合并目录编辑，最后发布一次。实际持久化用例覆盖冷热混合更新、整列降级、逻辑 API、混合创建／删除、旧 pin 与完整 integrity；两组降级累计超过 64 个实际变化／迁移 owner 时，写盘前原子拒绝，不按每组重置总额度。
- **4096 字节缓存的完整查询问题已修复。** 顺序扫描在单帧缓存下使用原有有界复制路径，避免唯一帧的 pin 跨越消费回调／换页。保留全部路由、所有权与校验，不加缓存、不重试消费输出。保存修改前失败与修改后通过的实际完整过滤查询，并验证正常缓存、类型化供给、更新和旧快照。
- **必要模块检查已完成。** 实验 13／39 两种 membership 模式各 564 项、默认 5／31 共 154 项，以及缓存两种额度的完整查询 2 项全部通过；每种实验模式包含 27 项真实持久化冷源生命周期用例。没有全量单测或跨平台通过声明。
- **冷布局仍未采用。** 测试初始化的冷源验证不等于公开 ROW→冷转换。前向冷转换入口继续拒绝；没有通过真实分布、全库至少节省 10% 空间的准入，也没有具备完整源 fence／崩溃清理／一次可见性切换的全目标构建证据。因此不以 8 组前缀或夹具绕过门禁，不将该实验布局用于最终生产性能成绩，不在收尾中另起迁移协议工程。它是本轮明确未交付的限制，不能标为已完成或宣称从中获得性能收益。

保留局部访问校验＋完整 integrity、data-before-root、sealed 创建凭证、原源代次／manifest、实际 Entity／竞争 ROW、旧 pin、回收与账户退款。64-owner／64 user replacements／64 总列计划／4096 memberships／8 转换组／1 MiB 边界保持不变。两个旧 metadata 延迟回收断言在冻结基线同样失败的证据继续单列，不计为本轮通过。

最终统一矩阵沿用 P0 的输入、语义 oracle、三进程／每进程 3 次预热＋10 次查询测量、持久化与资源合同。只在本轮最后运行一次；报告所有样本、失败、未达门槛及限制。已有 CPU 归因中的字段供给约 46.7%、标签验证约 21.3%、候选输出约 17.7% 是历史采样，不能冒充本轮优化后的 CPU 占比，也不自动产生新实现任务。

'''+s[b:]
  s=s.replace('若仅小幅收益，继续分析剩余主成本，不能以工作量指标替代时延结果。','若仅小幅收益，最终报告据此判定结构性目标未达成，不以工作量替代时延，也不自动扩展任务。')
  s+='\n本轮收尾与性能验收分开记录：收尾结束后停止扩展，目标未通过仍为未通过，不将进度改写为 100%。\n'
 else:
  s=s.replace('Updated: 2026-10-22.','Updated: 2026-10-23.',1)
  s=s.replace('Candidate rejected; core attribution continues','Final candidate frozen; unified measurement and closeout underway')
  s=s.replace('If performance targets remain unmet, continue diagnosis and repair within scope rather than adding unrelated tasks. If evidence requires changing durability or public semantics, present a concrete design revision for discussion, retain incomplete status, and never silently weaken acceptance.','Close the fixed remaining queue without opening further optimization branches. Kuzu parity is a final measured outcome, not a condition for indefinite implementation. Retain failed P7 acceptance and90% progress when thresholds are unmet. A new target-bootstrap, durability or public-semantics protocol is not automatically opened during closeout, and existing admission gates remain unchanged.')
  a=s.index('### 9.0 ');b=s.index('### 9.1 ',a)
  s=s[:a]+'''### 9.0 Fixed-queue closeout and adoption boundary

Acceptance remains **90%**. Test counts and report completion do not satisfy P7 or the Kuzu objective. Freeze after the following implementation and checks; open no further optimization branches. Final evidence and complete workload comparisons are in `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/`.

- **Atomic cross-group mutation is complete.** Node/Edge groups can share one logical FieldPage. Merge compatible hot plans first, reuse one private page set for each group's cold changes, merge directory edits by namespace, and publish once. Actual durable tests cover hot/cold writes, whole-column demotion, logical API, mixed births/deletions, old pins and full integrity. Two demotions exceeding64 actual changed/migrated owners refuse before writes; the total bound never resets per group.
- **The4096-byte full-query failure is fixed.** One-frame sequential scans use the original bounded owned fallback, keeping the sole frame unpinned across consumer callbacks/page changes. Preserve routing, ownership and validation; add no cache or output retry. Retain the original failed full filter query and passing checks for both cache capacities, typed supply, updates and old snapshots.
- **Necessary module checks pass.** Each experimental13/39 membership variant passes564 checks, the default5/31 profile passes154, and both full-query cache variants pass2 checks. Each experimental report includes27 actual durable cold-source lifecycle cases. No full-suite or cross-platform success claim.
- **The cold layout remains unadopted.** Test-initialized cold sources do not prove public ROW-to-cold conversion. Forward cold publication still refuses. There is no accepted real-distribution, whole-database10% allocation saving or complete target-build proof covering original source fences, crash cleanup and one visibility switch. Do not bypass admission with an8-group prefix/fixture, use this experimental layout for final production performance claims, or start a migration-protocol project during closeout. This remains explicitly undelivered, not completed or a claimed source of performance gains.

Preserve local-access validation plus full integrity, data-before-root, sealed creation authority, source generations/manifests, actual Entity/competing ROW checks, old pins, reclamation and refunds. Keep64 owners/64 user replacements/64 total column plans/4096 memberships/8 conversion groups/1MiB. Two inherited deferred-metadata assertions also fail the frozen baseline; retain that evidence separately rather than counting them as passed.

Run the final unified matrix once with unchanged P0 inputs, semantic oracles, three processes, three warmups plus ten query measurements per process, durability and resource contracts. Report all samples, failures and unmet thresholds. Historical CPU attribution of46.7% field supply,21.3% labels and17.7% candidate output is not a measurement of this final build and opens no new implementation queue.

'''+s[b:]
  s=s.replace('If gains are small, continue analyzing remaining primary costs; work counters cannot replace latency.','If gains are small, report the structural objective as unmet; work counters cannot replace latency or open an automatic follow-on task.')
  s+='\nCloseout and performance acceptance are recorded separately: stop expanding after closeout, retain unmet targets as unmet, and do not rewrite progress to100%.\n'
 p.write_text(s)
for lang in ['zh','en']:
 for name in ['native-single-file-storage-plan.mdx','native-capability-acceptance-matrix.mdx']:
  p=Path(f'docs/apps/docs/content/docs/{lang}/zyx/architecture/{name}');s=p.read_text()
  if lang=='zh':s+='''\n### 2026-10-23 固定清单收尾验证

实验 13／39 现已支持共享 FieldPage 的跨组／混合 Node、Edge 冷写入，兼容热计划与冷重打包合并到同一私有页集合后一次原子发布；全批次仍受 64 实际变化／迁移 owner 上限。27 项真实持久化生命周期用例包含混合创建／删除、逻辑 API 与两组降级超限拒绝。单帧顺序扫描使用既有有界复制路径，4096 字节缓存的完整过滤查询已由失败变为通过，无缓存额度或校验放宽。实验两种模式各 564 项、默认 154 项及完整查询两种缓存 2 项必要检查通过。

前向冷转换／全目标空间准入仍未交付，公开入口保持关闭；上述夹具不构成转换证明或性能采用。本轮不再展开新迁移协议。最终性能结果、未达条件与源码／库身份见 `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/`；验收仍 90%，不能称 Kuzu 目标完成。\n'''
  else:s+='''\n### 2026-10-23 fixed-queue closeout checks

Experimental13/39 now supports cold mutations across Node/Edge groups sharing a FieldPage. Compatible hot plans and cold repacking share one private page set and publish atomically once, retaining the batch-wide64 actual changed/migrated-owner bound. The27 actual durable lifecycle cases include mixed births/deletions, logical API and rejection of two over-limit demotions. One-frame sequential scans use the existing bounded owned fallback; the4096-byte full filter query now passes without raising capacity or weakening validation. Both experimental variants pass564 checks each, default passes154, and both full-query cache variants pass2 necessary checks.

Forward cold conversion/whole-target admission remains undelivered and its public entry remains closed; fixtures prove neither conversion nor performance adoption. Closeout opens no migration-protocol project. Final measurements, unmet conditions and source/library identities are in `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/`; acceptance remains90%, with no Kuzu-objective completion claim.\n'''
  p.write_text(s)
