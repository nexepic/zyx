# Build 246 ownership follow-up / 第 246 轮所有权续查

This continues `2026-11-16-build246.md` using the same preserved native logs. There is no new Windows/macOS execution, push, CI trigger, or Codecov account change. Native baseline remains 13 Windows failures (the two lifecycle failures overlap), three failing optional RSS diagnostics, and an externally rejected Codecov upload.

## Candidate changes / 候选修复

- `SortTransfersLargeRowsToDirectLimitAndSkipBuffers`: bare scalar sort keys borrow values from their live, charged rows. No key copies or empty key-vector proxies are constructed on this path. Computed keys, entity bindings, and bound-expression programs retain evaluation/admission. Stable ordering, nulls, multiple keys, LIMIT/SKIP and cancellation are covered. The key-slot fault test now uses `value + 0`, preserving its allocation rejection and zero-publication assertions; a bare variable no longer has that allocation.
- `CompletePropertiesAndLabelsSplitJointRetentionBeforeCachePublication`: terminal non-MAP scalar output uses a temporary owning ResultValue, releases the consumed source binding, then allocates the destination node. Payload remains charged through the handoff; string/list/byte-vector move proxies are admitted. MAP and unsupported output layouts keep the original path. The 5000-byte refusal, 9000-byte success, result contents/order and retry requirements remain unchanged. Native sufficiency is not established.
- `ChargesInitialCountFrontierCapacityToMaterializedByteBound`: zero/singleton Node labels require no duplicate-detection hash. Empty labels still reject; multiple labels still use the admitted set. A local probe using the Windows path's Boost container and allocator reproduces the first 112-byte reserve allocation for one label, matching the native source-scan rejection with 56 bytes available. This supports the source of the early failure, but native execution must confirm the later frontier assertion. The original budget and exact error assertion remain intact.
- Scratch reads coalesce padding only when the complete header/payload fits the first4KiB page. Small payloads use9 native calls instead of32; larger payloads retain32 page-sized reads. No full-segment heap temporary is introduced. The whole padded tail is read; CRC, envelope/catalog checks and late-truncation precedence remain. Scratch writes remain at4KiB. This increases bounded stack storage by12KiB and needs native ASan RSS verification, including sanitizer stack effects.

## Unresolved ownership / 未闭环的所有权

`NestedGraphScopesReadOnlyTheirSelectedEntityProperties` still copies a shaped target while the callback owner remains live. The recorded513-byte request exceeds202 available. Moving a const callback-owned target or dropping its charge is unsafe; a borrowed binding representation would require changes to all scalar/entity/cache resolution and nested restore semantics. New bounded `CI_SCOPED_BINDING` records the value kind, payload, row composition and available budget before admission. The current logs do not establish the complete lifetime overlap or a safe zero-copy replacement. Linux passing is not native closure.

`NativeEdgeCellsKeepTightAndRetryReadsBounded` still has distinct direct-path and fallback refusals: a16-byte proxy increment at zero headroom, then a42-byte gap during fallback payload admission. New bounded `CI_PREFETCH_CELL_OWNERS` separates ID/key/ordinal/selection working storage, pending destination metadata, buffered rows and source charge. It can reveal which live owner competes with the payload on the next native run. It does not prove that merely removing42 bytes would suffice at later boundaries; real proxy and destination costs remain charged.

The current13-case diagnostic remains independent from the historical137-case log and resets bounded trace counters per process. Unit/lifecycle, recovery and calibration retain their gates and explicit outcomes. Runner loss, outer timeouts or output truncation can still prevent complete evidence; next-run success/root-cause closure cannot be guaranteed.

## RSS and external limits / RSS 与外部限制

The prior Mach resident/internal/external/footprint and sanitizer live/heap samples are retained. They show growth at the guard's first64MiB crossing, not eventual peak or a proven leak. The allocator's absurd RSS field is not used. Removing128KiB read temporaries and amortizing reads are concrete changes, not proof all RSS modes fit. Cold observer-free, traced cold, warmed statistics and the formal ASan/UBSan gate must all be inspected by actual exit/outcome, not green continue-on-error labels. Windows/MSVC and macOS/Mach behavior cannot be reproduced in this Linux environment. Codecov's deactivated-repository rejection requires authorized account/repository administration; changing C++ cannot validate that upload.

## Validation / 验证

Validation and independent backup details will be appended after the final frozen-source run. Earlier baseline11123-case results apply only to0fc22e755 and are not reused as validation of these changes.

Final targeted run passed360/360. Initial63-case iteration found the obsolete bare-key allocation expectation (62pass/1fail); the computed-key correction preserved its intended rejection semantics. Intermediate354/354 passed. Source-size check:2826 files, zero over3000 lines; diff whitespace check passed.

Alternating 4KiB/16KiB probe order across two cycles, eight300-read samples per payload/mode, checked28800 returned payloads. Paged medians(seconds,4KiB→16KiB):64 bytes0.010854→0.007595875;65536 bytes0.09273325→0.0733155;126976 bytes0.132958→0.1545885. Tiny/medium payloads improved in this instrumented Linux sample; the largest slowed about16.3%. Full-read control timings also vary. This is a tradeoff, not a uniform speedup or a production/native benchmark. This unconditional16KiB experiment was superseded by small-payload-only padding coalescing; the final candidate remains subject to native sanitizer-stack/RSS validation; no gate or performance threshold was changed. Probe source, commands, raw rounds and all samples are retained.

The frozen query changes also passed the independent current13-case diagnostic on Linux:13 exits zero,13 result journals, no output truncation. Both new ownership trace formats were observed, including Edge fallback source/destination owner separation. This establishes diagnostic execution, not MSVC equivalence. The final small-payload-only reading change is validated separately below.

Final adaptive physical-read tests passed6/6, including a new contiguous full-tail/9-read small-payload case. Final source1b3c3d0bbf6faf258f5df93aff09ff0d4670f3b7; binary SHA256d49568c856f70ed9e43934ea503109259b75fefd030503dd84e0f342165a2bb1. Complete unfiltered regression is running against this frozen source.

Final alternating old4KiB/adaptive probe checked another28800 payloads. Paged medians(seconds):64 bytes0.009394035→0.006320345;65536 bytes0.076137→0.09089985;126976 bytes0.2190165→0.229083. Small payload improved32.7% in this sample. Medium/large timings remain worse/noisy despite the same32 read calls and page-sized copy behavior; no overall throughput non-regression is claimed. Production/native performance and sanitizer stack residency remain verification risks. The earlier unconditional16KiB experiment is retained as evidence, not the final algorithm.

Evidence/supervisor Python regression passed45 tests via the configured environment's pytest. The first unittest invocation used the wrong interpreter/runner and failed import before executing tests; both logs are preserved. Checkpoint90bc6ac30 is independently retained as Library bundle/receipt v49:321584775 bytes, SHA256f350777baa21e84f038b61c8bccafd3b12b36893fec7b5a0df38c46a5b92c672. Local fresh-bare restoration matched45344 reachable objects and HEAD; bundle verify/full fsck exited0. Provider retention succeeded; external reverse-download remains unverified after the prior403.

Additional limits: the Edge ownership trace describes the fallback cell path; the direct-path16-byte PMR proxy request still lacks a uniquely identified concrete allocating object. Native rejection/result correlation remains necessary, and next-run diagnostics cannot guarantee closure. Sort's optional key carrier and corrected key-vector proxy/backing charges are included in actual batch/key-slot admission; native layout differences may expose later shortages even when a copied payload is eliminated. No fee is subtracted merely to recover a passing test.

First complete run executed11125 enabled cases exactly once, with no missing/extra/duplicate identities, but failed one test:SortKeyQuotaFailureIsTypedAndASeparateLargerExecutionCanRetry. Its32 bare keys no longer allocate duplicated owning slots. The test now uses32 equivalent computed keys(x+0), retaining the original4096-byte denial, typed exception, larger-budget retry and result assertions. A separate positive test verifies32 repeated bare keys fit4096 bytes. No production logic or quota was changed to suppress the failure. First-run XML/logs and timing deltas are preserved. Final full regression is rerun after this test correction; earlier pending/failing states are not passes.

## Final verification / 最终验证

The final unfiltered run passed all11126 enabled cases exactly once across8 shards:zero failures/skips/missing/extra/duplicates, all6 stress cases once.20 disabled cases are excluded. Source714fe61ca3bbf95c6190e8db6fcb89d6f471423b; binary SHA256d20e2c50ded626053abecab5d090316c22dfa0a961342e44045813c347e67111. These final results supersede the earlier pending/failing run statuses. Coverage:{"regions_percent": 93.71, "functions_percent": 98.1, "lines_percent": 95.75, "branches_percent": 86.84}. Metrics below95% remain unmet; no threshold/exclusion changed.

The corrected sort quota selection passed47/47; earlier query/physical selection360/360, final adaptive physical tests6/6, independent current-case diagnostic13/13 and evidence/supervisor Python45/45 remain separately scoped evidence, not additive totals. The LLDB diagnostic helper tests and workflow configuration validation are recorded alongside final results.

Two additional bounded Windows LLDB cases now target NativeEdgeCellsKeepTightAndRetryReadsBounded and NestedGraphScopesReadOnlyTheirSelectedEntityProperties, each with max3 rejection stacks/32 frames and a300-second process bound. The same helper successfully ran on Windows in Build246; no new native execution is claimed. This supplements the fallback ownership trace with direct-allocation stacks, subject to debugger/symbol/timeout limits. Existing diagnostics and gates remain.

Compiled source remained frozen throughout the final run; the subsequent workflow-only change was parsed and shell-checked separately. No push, CI trigger/cancellation, main mutation or Codecov account change occurred. All native sufficiency/RSS/performance limits above remain. Final independent bundle retention and restore verification accompany the final report commit; reverse-download remains unverified.

Commits before this final evidence commit / 最终证据提交前的提交清单:

- eabda614e Borrow charged scalar rows while sorting bare keys
- 5e879fef8 Release consumed scalar bindings before publishing procedure cells
- 835335712 Avoid trivial label hash allocation and trace unresolved owner overlaps
- bf4294efe Amortize bounded scratch reads across four pages
- 1b3c3d0bb Coalesce only small-payload padding to retain large-read behavior
- 90bc6ac30 Preserve follow-up ownership and adaptive read validation evidence
- 714fe61ca Preserve owning sort-key quota rejection and test borrowed-key admission
- 2265c0446 Retain full-run failure and corrected sort-quota coverage evidence
- aa9228110 Capture bounded rejection stacks for unresolved Windows owners

Quota-debugger helper tests passed11/11 (56 Python tests total across three distinct files). The Windows retained-ownership diagnostic also includes the new small-payload padding test. Both modified workflow shell blocks passed bash syntax validation; YAML parsed successfully. The transient post-run exec transport disconnect recovered before final verification and did not rerun or alter the completed tests.
