# Build 243 budget continuation / 预算后续

This continuation preserves every original cap and failure. Native outcomes below belong to d411fcd7f, not the local candidates. Diagnostic references are not downloaded artifacts. 本轮保留原预算和失败；原生结果属于 d411fcd7f，不能归于本地候选修复。

## Evidence-based classification / 依据证据分类

The 19 unit failures are not 19 identical bugs. The independent diagnostic failures are duplicates, not additional failures. Names below omit suite prefixes only where unambiguous; complete identities and the richest per-case available diagnostic lines are retained in budget-followup/failed-case-diagnostics.json.gz.

| Cases | Confirmed boundary / 已确认边界 | Change or remaining uncertainty / 修复或未决项 |
| --- | --- | --- |
| Hybrid: EightBoundedGroupsKeepWholeDatabaseCostsAndEveryOwnerExact; ConversionFillsPartialNativePoolBeforeSpanningAnotherPool | Empty key proxy requests16 with4 remaining at1MiB. For the tail-reuse case, Windows LLDB frames14–24 identify packedEntries decode during private owner rebinding in HybridPropertyDirectoryPreparation; the mature-shape case reports the same requester but has no equivalent retained stack. | Replace full retained decode for private-owner validation with two charged reusable key buffers. Ordering, restart alignment, child IDs, trailing bytes and envelope validation remain mandatory before mutation. Native outcome pending. |
| AggregateAndCsvRetainedHeadersShareTheExecutionBudget | Independent10-group visitor fails group vector growth at unchanged5000 cap. | Aggregate doubling can reject although a smaller new buffer fits. Fall back to the minimum required capacity, retain old/new overlap charging via PMR. Linux regression demonstrates10 complete groups at4192; Windows5000 outcome pending. |
| NativeAndOverlayNodePredicatesAdmitBeforeDecodeAndSplitColumns; NodePredicateSplitPreservesScanOrderMissingCellsAndCancellation | Returned columns1151 vs admitted1012/1014, row structure624, only58/60 remaining. | Real single-row retention conflict; no fee waiver. Need reduce retained representations or establish test/API contract change. Unresolved. |
| NativeEdgeCellsKeepTightAndRetryReadsBounded | Prefetch one-row working-set rejection, requested3266 replacing252 under3936 cap. | Existing detailed prefetch trace covers next run; no confirmed safe ownership change yet. |
| BareNodeAliasesAdmitSourceAndIndependentCopiesBeforePublishing | Source640 + result/output4480 exceeds5000. | Independent alias copies remain independently charged. Minimum overlap needs structural ownership analysis; unresolved. |
| CompletePropertiesAndLabelsSplitJointRetentionBeforeCachePublication | Two source rows3240 + result/output5760 plus external48 exceeds9000. | Batching/preparation still retains conflicting data; do not remove external proxy fees. Unresolved. |
| CompleteThreeHundredRowsKeepTheUnchangedOwnedQueryQuota | Parent requests168 with137 free at262007/262144. Scalar lower bound alone does not explain actual failure. | Added allocation-free phase line before optional heap-geometry calculation and private source preparation phase. Each has a separate16-line channel so general400-event trace exhaustion cannot suppress it. Original exception preserved. Still no demonstrated native fix. |
| NestedSortPrefetchesOuterPropertyAndTransfersRows; ProjectBetweenSortsTransfersProjectedRowReservations | Output1065 with975 free, source row624 retained. | Source/output handoff boundary, unresolved; do not equate retained minimum with transient feasibility. |
| FilterBetweenSortsTransfersSelectedRowReservations | First result capacity growth1145 replacing1065, only55 free. | Empty-swap growth already active; no safe additional move/fee removal established. |
| SortPreservesProjectedFilteredRowsWithoutOuterPrefetch | Result relocation2786 replacing2210, only142 free. | Retained output plus relocation; unresolved. |
| SortAdmitsExpandedRowsWhileUnwindRetainsItsInputCharge | Minimum capacity5 still needs7841 replacing5825, only2007 free. | Geometric fallback already occurred; merely another capacity heuristic cannot establish a fix. |
| NestedGraphScopesReadOnlyTheirSelectedEntityProperties | Scoped property513 with202 free under3936. | Scope/property retention, unresolved. |
| PreflightsListAndMapSlotsBeforeConstructingWideCsvRows | Output10267 with8592 free while source10344 retained. | Wide row source/output overlap, unresolved. |
| SortTransfersLargeRowsToDirectLimitAndSkipBuffers | Sort4 rows retained18076, key slots352, used30953 and available3543. | Sort/limit/skip lifetime boundary, unresolved. |
| ChargesInitialCountFrontierCapacityToMaterializedByteBound | Earlier source_scan request112 exceeds56 before count frontier. | Preserve typed earlier quota exception; this fixture does not currently reach its intended native failure stage. No broad error retagging. |
| CoordinatedPropertyPrefetchReleasesSelectedAndRejectedRowsBeforeClose | Hierarchical resource rejection while multiple query/storage owners compete. | Account/refund trace retained, but rejected request's ownership lifecycle is not fully closed. |

The prior report's statement that the Hybrid originating caller was unavailable is superseded by the recovered LLDB frames in the original job log. No artifact download was needed for those frames.

## RSS and access limits / RSS 与访问限制

All three optional RSS cases failed in243, although formal ASan711/711 passed. Cold growth67567616 and warmed growth67633152 exceed67108864. Both traced runs reach merge level1 and35471 cumulative decoded rows. ASan live bytes near10.99MB vs heap90.98MB and warmed quarantine growth from11.61MB to44.25MB show allocator-state growth alongside RSS growth, but do not attribute resident growth to a particular allocation site or establish a complete root cause. Current telemetry gives merge phase, scratch read/write traffic, live/heap/free, Mach residency and bounded allocator statistics. It cannot guarantee next CI identifies every allocation owner. No quarantine, guard, timeout or budget was weakened.

Supported connector download of Windows artifact14025644103 returned a file reference. The single subsequent byte-transfer attempt failed with `URLError: Tunnel connection failed: 403 Forbidden`. Zero bytes were verified; expected SHA256 `056437480730acf3d8dfe7668c9276976bf3bfc1b8eda29000852284d33b7275` remains unverified. No retry or alternate route bypassed this denial. Stage10 artifact14025898026 was not fetched. Existing complete decoded job logs remain available locally and in prior independent backups.

Windows process-launch candidate c9529e76b remains pending native validation. Ubuntu Codecov repository deactivation remains an external service blocker; no account settings changed. Recovery/calibration ran after Windows failures in243; disabled P0/endurance are not passes.

## Local validation and limits / 本地验证与限制

Source checkpoint:9147d73ad0883924ef8f1a3dc6471c38ac3f5db4. BTree/Hybrid targeted tests43/43 pass; CSV/aggregate32/32 pass, including typed parent denial, full refunds and successful retry. A separate final-binary probe passes85/85, with a complete10-group aggregate result at4192 bytes. The aggregate failure channel emits exactly16 lines and one truncation notice. The two vector terminal-phase channels do not trigger in the Linux probe; their actual Windows failure-path emission remains unverified, and failures outside those scopes remain possible.

Python regression82/82 passes. Source-size check covers2824 files with0 over3000 lines. Selector audit finds all137 Windows cases, all19 macOS cases and11 matches for5 ASan patterns. All19 Build243 Windows failed identities are explicitly included. Selection coverage does not guarantee execution completes before a native timeout or that every causal owner is identified.

Two earlier local full runs were deliberately interrupted: the first to include the BTree repair, the second to preserve typed parent admission errors. Neither is counted as passed. A compilation run was also interrupted and restarted. The final unfiltered run uses a separate run ID and XML directory, so its evidence cannot be confused with those superseded attempts.

本轮已落实聚合必要容量回退、私有 BTree 页的双键缓冲完整验证，以及不受通用日志限额抑制的有界阶段诊断。既有预算、安全门禁和原生失败清单均保留。Windows19项及macOS RSS尚未闭环；下一轮CI不能保证全部通过或一次定位全部根因。

Independent Library checkpoint34 retains fa2ab3ea0, including the production fixes, RSS audit and first complete regression failure and was verified by fresh bare restore, identical HEAD/all reachable objects and fsck. Backup receipts are kept separately in Library; saved external bytes have not been reverse-downloaded after403. No push, CI dispatch/cancel, main change or Codecov account change occurred.

## RSS phase audit / RSS 分阶段核查

The allocator output `55M mapped (4608425984M rss)` contains an invalid RSS value. It is excluded entirely from resident-memory comparisons. Mach samples below are bytes, and belong to distinct phases; they must not be substituted for one another. The other synthetic1MiB guard checks in the formal log are not these focused64MiB runs.

| Run / phase | resident | internal | external | footprint |
| --- | ---: | ---: | ---: | ---: |
| cold traced / before-query | 465403904 | 119422976 | 345980928 | 123452672 |
| cold traced / rejection-before-unwind | 533004288 | 175702016 | 357302272 | 180504768 |
| cold traced / after-backend-destruction | 543195136 | 185057280 | 358137856 | 189860032 |
| warmed statistics / before-query | 465289216 | 119308288 | 345980928 | 123354432 |
| warmed statistics / rejection-before-unwind | 533479424 | 176177152 | 357302272 | 180996352 |
| warmed statistics / after-backend-destruction | 543162368 | 185024512 | 358137856 | 189843712 |
| cold traced / before-query to rejection delta | 67600384 | 56279040 | 11321344 | 57052096 |
| warmed statistics / before-query to rejection delta | 68190208 | 56868864 | 11321344 | 57641920 |

Cold guard baseline465436672 differs from the earlier before-query Mach sample465403904 by32768; warmed baseline465846272 differs from465289216 by557056. At rejection, each guard current exactly matches that run's rejection-before-unwind Mach resident. Thus guard growth67567616/67633152 and before-query-to-rejection Mach growth67600384/68190208 are different intervals, not contradictory measurements. The later warmed543162368 resident/189843712 footprint belongs to after-backend-destruction, after intervening diagnostic/cleanup work; it is not the rejection snapshot. Internal/external/footprint movement does not by itself identify an allocation site or prove a leak.

allocator 的异常 RSS 数字不可信，已排除。guard 拒绝时的 current 与同阶段 Mach resident一致；较早的 before-query 与较晚的销毁后样本不是同一时点。不能据这些差异直接断言泄漏、根因或预算计费错误。

## First complete Linux regression / 首轮完整 Linux 回归

The first completed unfiltered run on4bd61eb3a executes11111 enabled cases exactly once, with11110 passed and one failure: RelocatableStorageBackendSchemaTest.ExecutesNativeDbmsConfigurationAndStatisticsProcedures throws QueryTimeoutException at the existing45ms deadline. All four XML files and the failure console are retained under first-complete-run. Coverage was not generated after that failure and is not represented as final coverage.

The unchanged case subsequently passes20 repeated executions in isolation. This supports timing sensitivity but does not establish its root cause or erase the original failure. A diagnostic-only wrapper now names the precise configuration procedure on timeout and rethrows the same exception; it also prevents an unrelated timeout from satisfying the broad read-only runtime_error assertion. The45ms configuration and production guard remain unchanged. The labelled targeted case passes; the complete unfiltered repeat passes, as recorded below.

首轮完整回归的1项45ms超时失败被保留，隔离重复20次通过不能替代完整回归，也不能证明根因已关闭。补充仅标记超时调用并原样抛出异常，没有提高或停用期限。

## Final complete repeat / 最终完整重跑

The unfiltered repeat on source9147d73ad executes11111 enabled cases exactly once, with0 failures, skips, missing, extra or duplicate cases. All six stress identities run once;20 disabled cases are not passes. The binary hash matches the capture at the start of this run, and src/include/tests/scripts match the frozen source commit. Coverage: regions_percent=93.75, functions_percent=98.1, lines_percent=95.76, branches_percent=86.86. Region/branch coverage remains below the95% target.

The original45ms timeout remains a timing-sensitive unresolved observation, despite20 isolated passes and a passing full repeat. No timeout/budget threshold was increased. Windows19 failures, the two Windows launcher cases and native RSS diagnostics still require native verification; no claim is made that this Linux repeat closes them.

最终重跑11111项全部通过且恰好执行一次；首轮45ms超时仍保留为未闭环风险，未被补跑结果抹去。全部原生平台结论仍按实际CI证据区分。
