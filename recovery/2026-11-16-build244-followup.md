# Build 244 follow-up / 第 244 轮继续排查

Source baseline: 308bf48f11c5f26ab24ce63395824fa9c6d32bca. Native evidence remains Build 244 at 38bdc185d; no new native run was triggered.

## Changes / 修改

04885590a releases each consumed procedure output binding after checking the real source/output overlap. Only physically destroyed storage can be refunded; the output and empty-map handoff remain admitted. The existing alias test still uses its original 5000-byte successful bound, independent-copy assertions, and negative bounds; it additionally requires four actual binding-release events across its two successful executions. The full owned-budget, failure and cancellation suites also run. This is a candidate for the two procedure materialization failures, not a claim that either now passes MSVC.

6302b0eb4 addresses a concrete diagnostic failure: Windows LLDB crashed in liblldb.dll (0xC0000005) after PDB type-completion errors during the coordinated-query trace; the second trace timed out. Windows now reads symbols/PCs without GetFunctionName or GetLineEntry. At most three rejection stacks of at most 32 frames are collected; the second target is the still-failing MatureShape case instead of the already-passing TailReuse case. The 300-second supervisor bounds and explicit failed diagnostic outcomes remain. Native LLDB reliability is not guaranteed by mocked Python tests.

The warmed RSS diagnostic now reports bounded allocator changes during scratch reads/writes and row encode/decode. The cold controls and formal gates remain unchanged. Measurements are net live/heap changes within each operation, accumulated separately for increases/decreases; they are not gross allocation counts, disjoint allocation-site profiles, or causal Mach RSS attribution. Concurrent activity can affect intervals. Unavailable APIs are explicitly marked. Linux ASan smoke observed one 1 MiB allocation as live_increase=1048576 and heap_increase=1056768; this validates instrumentation only, not macOS behavior or the RSS root cause.

本轮释放真实已消费的过程绑定，同时保留源/结果重叠与移交费用。补齐了 Windows 调试器自身崩溃这一诊断盲区，并增加 warmed RSS 的操作类别统计。预算、RSS 阈值、异常类型、正式门禁及 Codecov 配置未放宽。

## Remaining 16 cases / 其余 16 项逐类缺口

Exact identities and byte observations remain in the previous report and windows-failure-diagnostics.json. Counts below total 16; the earlier two compaction/aggregate candidates are excluded.

| Cases | Count | What is known / next evidence / unresolved work |
| --- | ---: | --- |
| BareNodeAliases; CompletePropertiesAndLabels | 2 | Consumed procedure bindings remain live unnecessarily. New incremental destruction candidate; native per-binding overlap trace and unchanged tests will show whether this is sufficient. Independent output copies still charged. |
| NativeAndOverlayNodePredicates; NodePredicateSplit | 2 | Already at one row; 624-byte row plus returned columns exceeds remaining allowance. More batch splitting cannot solve this. Needs row/cache ownership redesign; trace distinguishes admitted estimate from corrected returned storage. No claim native data alone solves it. |
| NativeEdgeCells | 1 | Retained-cell admission remains. Independent case and bounded owner traces preserve denial identity, requested/available bytes; exact surviving owner/lifetime still needs attribution. |
| NestedSort; ProjectBetweenSorts | 2 | Output 1065 versus 975 available while 624-byte source row survives. A source/result lifetime transfer needs proof of actual destruction and MSVC move scratch costs. Existing result-rejection diagnostics expose these numbers. |
| FilterBetweenSorts; SortPreservesProjectedFilteredRows; SortAdmitsExpandedRowsWhileUnwind | 3 | Distinct output-growth peaks with live source/Unwind owners. They are not all covered by the procedure fix. Existing growth/source metrics identify capacity and retained pressure. A generic move would require exception-safe grant transfer, not early refund/re-admission. |
| NestedGraphScopes | 1 | Selected graph property admission remains; preserve selected-snapshot identity. Case-level diagnostics are present, but a complete allocation history is not. |
| PreflightsListAndMapSlotsBeforeConstructingWideCsvRows | 1 | Source 10344 and result request 10267 versus 8592 available. Code confirms scalar copy during final result publication. A move path is plausible, but no unproven patch: MSVC container proxy/move temporaries, partial-construction exceptions and buffered reservation ownership must all be admitted correctly. |
| SortTransfersLargeRowsToDirectLimitAndSkipBuffers | 1 | Four rows 18076 plus key slots352 already retained; 3543 free. Sort/Limit/Skip transfer needs separate lifetime accounting; not fixed by final procedure binding release. |
| ChargesInitialCountFrontierCapacity | 1 | Source scan rejects before expected frontier error. Current narrow assertion is kept. Needs isolated frontier fault injection or a proved source-lifetime change; errors must not be relabelled. |
| CoordinatedPropertyPrefetch | 1 | QUERY account 32 KiB boundary. Previous native stack acquisition crashed. New bounded symbol-only stacks target this case; missing evidence is the rejecting caller and remaining live owners, not merely another pass/fail result. |
| MatureShape | 1 | BTreePageEntry proxy requests16 with2 bytes free under1 MiB. New targeted symbol-only stack replaces the irrelevant passing TailReuse stack; caller path and simultaneous retained graph owners remain unproved. |

下一轮能提供原生布局、拒绝阶段/所有者、过程逐字段重叠数据，以及两个指定案例最多三组符号栈；不能保证 LLDB 不再崩溃、所有失败自动定位或全部通过。有限事件上限可能截断历史。旧 artifact 403 未绕过；现有完整原始日志已保留，但 ZIP 引用不等同于已校验下载。若符号采集仍失败，需要受支持的原生调试证据，而不是把无栈解释为无错误。

RSS remains unresolved: compare new category deltas with existing Mach internal/external/footprint, guard samples and phase counters. Even a dominant category is a lead, not proof of leak or allocator quarantine. Do not use the allocator's nonsensical RSS field. Codecov still requires service-side repository activation by an authorized owner; no settings change was made.

## Fault injection review / 故障注入复核

The previous full-run failure followed removal of unused DISTINCT sets. The test allowance changed from12000 to4096 (stricter); it still requires the specific accumulator-object admission error, no delivered rows, and successful fresh execution with128 null MIN outputs. Zero source scans is newly asserted. It therefore preserves and strengthens the original before-scan rejection contract. It is not a production budget change. Native structure sizes still require native validation; the full Linux regression is the local evidence.

## Validation / 验证

Targeted:106/106 (four suites including new RSS counters). Python:83/83. ASan instrumentation smoke:passed. Source-size:2826 code files, none above3000 lines. Full unfiltered regression is recorded below when complete. No push or CI trigger.

Final unfiltered Linux regression:11116 enabled cases executed exactly once; zero failures, skips, missing, extra or duplicate identities. All six stress cases executed once;20 disabled cases excluded. Binary SHA256:84155a26b7e4294cc8ca4f77f96894a0e5e33a0e5e9c91014a1baf03770f7c45. Source matches6302b0eb4. Coverage:{"regions_percent": 93.74, "functions_percent": 98.09, "lines_percent": 95.77, "branches_percent": 86.86}. Region/branch coverage below95% remains unmet. These Linux passes do not establish Windows or macOS native closure.
