# CI 228: follow-up to the compact-cache full run

Frozen `ff02643c` required unfiltered quick regression completed without timeout: 11039 passed, 7 failed, 20 disabled, 0 skipped. Discovery/execution each contained 11046 enabled names, with no missing, extra or duplicate names. The matching source fingerprint and compressed original XML are in `evidence/2026-11-05/ci228-compact-cache/full`. That negative result is retained unchanged.

## Production changes

Owning selected-column prefetch previously charged destination cache nodes only after the backend returned payloads. The CSV/live-header regression demonstrated a payload read before the destination could fit. The new path reserves missing destination node/key metadata before the first source-admission callback and transfers that reservation to rows on insertion. Payload credit is transferred without a refund/reacquire gap. Invalid shape remains checked before retry, borrowed repeated columns keep their existing separate ownership, and resumable native cell accounting is unchanged. An invariant rejects any untransferred destination credit.

Vector procedure search is synchronous and returns owning hits. The converted request buffers were nevertheless held through output publication. They now die immediately after search and hit-count validation, before the corresponding allocation loans are refunded. A fixture observes valid request coordinates during search, less account usage before selected-property production, escaped result ownership and complete refunds. No public type, source memory limit, or snapshot contract changed.

## Layout-independent test boundaries

GDS negative cases now use impossible lower bounds (a destination row alone, or three independent 6000-byte output payloads), followed by retry/refund checks. PageRank retained output also has an independent fixed 32 KiB efficiency envelope. This replaces false assumptions that the compact row representation must exhaust the old 1024/20-KiB allowances.

Nested-sort positive tests retain the historical frame envelope instead of shrinking their quota with empty private-map headers. Two 4608-byte envelopes remain below the independently reconstructed pre-compaction GNU allowance of 4758 bytes. The explicitly ample carrier case includes an allocated binding node. Low-budget rejection, full ordering, no read-ahead and exact owner-read checks remain. Limit/skip exact-fit estimates include MSVC key-string proxy storage, without expanding their independent 1792-byte ceiling.

Before the last two zero-cost-on-GNU key-proxy fixture corrections, the focused normal same-configuration executable passed 259/259 selected tests (the changed runtime TU and selected test TUs were rebuilt; this is not a replacement for the final full run). The matching XML is preserved. Final same-tree full regression and native Windows/macOS verification remain pending.

## Full-project sanitizer evidence and remaining RSS failure

A separate Linux Clang19 Debug -O0 -g0 build compiled all 254 project production units (including parser and inputxx object libraries) with ASan+UBSan. Three original native budget failures passed without sanitizer reports. A cold 20000-row sort completed its work quickly but still failed its unchanged 64-MiB RSS guard. This is a negative result, not a sanitizer pass or a native macOS result.

An isolated test-only probe on frozen `ff02643c`, with unchanged guard, recorded before/after RssAnon 73152/131028 KiB and RssFile 114080/134244 KiB. At rejection live sanitizer allocation was 5111906 bytes, heap mapping 70004736 bytes. ASan reported 38 MiB freed but zero really freed, and 33404 stack-depot entries. File-page residency and retained instrumented allocator memory contribute to process RSS. These measurements do not justify raising the guard, disabling quarantine, or claiming every retained page is query-owned. Further optimization/attribution remains necessary.

No push, CI dispatch, cancellation or remote monitoring was performed in this phase.

## Completed instrumented native subset

On production source `ca9e7e63` all 128 native tests selected from four test translation units passed in 498.139 seconds, with ASan+UBSan enabled for all 254 project production units. The two changed runtime translation units were rebuilt with the same instrumentation; the other 252 unchanged units came from the verified ff02643c archive. Selected tests were compiled with the same instrumentation and linked in the normal source order. This is a same-production-source instrumented build, not the earlier partial-TU experiment. External dependencies remain their installed builds.

This subset includes all four previously failed CI native tests: tight edge/source admission, selected-value admission, coordinated selected/rejected row lifetime and the actual 20000-row spill/collect sort. The sort passed in 19558 ms with the original 120000-ms timeout, 8-MiB materialization allowance and sampled 64-MiB RSS guard. No sanitizer report occurred. It does not include every compaction/transaction/process-crash test in the remote CI gate, and is Linux/libstdc++, not native Apple libc++ or Windows.

Cold RSS remains a distinct failing diagnostic. An isolated streaming-disposal verification prototype (4-KiB stack buffer, full segment read and incremental CRC) reduced observed cold growth to 66 MiB; combining it with the prior bounded input-buffer reuse prototype reduced growth to 65 MiB. Both still failed the unchanged 64-MiB guard, so neither prototype is integrated. The main production source stays frozen for its full regression.

## Additional cross-STL findings to address after the frozen run

A standalone ownership-lifetime probe uses the actual OwnedStdString/OwnedStdVector classes and an independent counting admission authority. During the synchronous request, GNU charges 3017 bytes and libc++19 charges exactly 3000 bytes (the index name fits libc++ SSO); destroying request data before loan reset returns both to zero. Therefore the new test's `duringSearch > 3000` observation must be `>= 3000`, preserving the guaranteed float-buffer lower bound. That portable correction is prepared only in the isolated candidate worktree while the current main-source regression completes.

Another independent stateless tracking allocator observes public unordered-map node/bucket requests: for 0/1/8 numeric properties actual owned bytes including the map header are 72/184/1040, whereas current inline/complete preflight is 56/129/640. This demonstrates missing public destination structural admission; post-construction result accounting alone does not make the preflight sound. An isolated candidate adds the existing audited fresh map/key/value-copy upper bounds while retaining all prior logical floors and validation errors. It is under targeted validation and is not yet integrated.

## Follow-up unfiltered run and completed candidate

The `ca9e7e63` unfiltered required quick run finished without timeout: 11045 passed, one failed, 20 disabled, zero skipped. Discovery and execution each contain 11046 enabled names, with no missing/extra/duplicate names. The sole remaining failure, `UsesBoundedTopKForSortSkipLimitPipeline`, assumes the converted request vector is still live. Its rejection allowance includes those now-retired 2000 bytes, so the limited heap legitimately fits. The corrected boundary retains the live hit array plus only an empty row header, less than a populated row, while keeping the positive bounded top-K and full-sort rejection oracles.

The independent public-map structural admission candidate passed 261 focused tests; after adding a guarded-cache-copy destination bound and the top-K correction, the combined selection passed 287/287. All vector/backend API tests in the focused module set passed 144/144. The new allocator-request/cancellation/map-growth regressions passed GNU and Linux libc++19 (3/3 in libc++). A deliberately adopted high-load-factor map actually grows buckets during a guarded copy, proving that the source retained capacity is not a destination copy bound. The cache pre-admits the audited copy bound; when an explicit procedure handoff already funds that local reservation, it uses the raw guarded copier instead of charging the same heap a second time through the decode scope. No fallback to CACHE funding is introduced.

The full-project ASan+UBSan `-O1` comparison (all 254 units, Debug assertions and default sanitizer checks retained) still failed the isolated cold 64-MiB guard, with 67915776 bytes observed growth. Adding the isolated streaming-disposal prototype also still failed (67637248 bytes). These are failures, not speedup/peak-completion benchmarks. No workflow optimization flag or scratch streaming API is integrated: neither experiment establishes a fix for that cold diagnostic. The actual O0 native group, including all four original CI failures, remains 128/128 passed. Native Apple/Windows and the complete remote sanitizer job are not claimed as verified.

## Final integrated source and sanitizer validation

Production/test/CMake source `561460b30174e3dbc167f7a87bfceae6e1e7f138` integrates the destination-map preflight, guarded-copy handoff, request retirement and corrected portable boundaries. The required unfiltered quick run completed with exit 0: 11049/11049 passed, 20 disabled, zero failures/skips. Discovery and execution have no missing/extra/duplicate names. All six stress names executed exactly once within this unfiltered run; they were not run a second time. The two XML documents and source fingerprint are preserved in the final evidence directory.

The matching Linux Clang19 Debug O0 ASan+UBSan executable completed 131/131 selected tests in 399.460 seconds, exit 0 and no sanitizer report: 128 native backend tests and three independent public-result map allocation/admission tests. All 254 project production units are instrumented. `archive-state.json` records the original unchanged units and same-source replacement runtime objects; external dependencies are installed builds. The current O1 sanitizer executable independently passed the three map-admission tests. Neither result claims native Windows/Apple validation or the entire remote sanitizer gate.

Both changed runtime translation units also passed syntax-only O3/NDEBUG compilation with test hooks and coverage instrumentation removed. This verifies production-configuration compilation, not a Release link or executable benchmark. Raw sanitizer results and commands are under `evidence/2026-11-05/ci228-final-admission`.

The isolated cold-process RSS diagnostic still exceeds the unchanged 64-MiB guard; the O1 and scratch-stream experiments remain excluded from production and CI configuration. Native cross-platform results and the cold RSS failure remain explicit limitations. No push or CI operation is authorized in this phase and none was performed.

### Final full-run result and remaining scope

The final source fingerprint is `507c067e1cffdff8d993a419668ea16440ae99984e64b7466acd6a943c96d4fe`. Shards passed 5525 and 5524 enabled tests in 1152.652 and 1551.387 seconds, respectively. All seven ff02643c failures and the later top-K boundary failure are green in this full run. Test-disabled names are explicitly listed in `ci228-final-admission/full/result.json`; none was newly disabled to repair this run.

Full-run adjusted coverage: lines 95.85%, functions 98.15%, regions 93.89%, branches 86.89%. Region and branch metrics do not meet the repository's 95%-all-metrics goal. The unfiltered script generated coverage successfully; this is not a claim that its separate coverage goal is satisfied.

The matching O0 ASan+UBSan native subset's 20000-row spill/collect sort completed in 9.781 seconds under its original 120-second query deadline, 8-MiB materialization and sampled 64-MiB RSS guard. This warm multi-test execution is deliberately distinguished from the still-failing cold isolated diagnostic. The workflow already treats the latter as an additional continue-on-error diagnostic; that configuration was not changed in this work. The full remote gate also includes transaction, compaction, maintenance and process-crash suites beyond the 131 local selected cases.

Current deliverables are local only. Required native Windows/macOS execution is unavailable in this Linux environment, and authorization for pushing or running CI is pending. There is no denied write or uncertain remote operation: none was attempted. No production memory cap, sanitizer setting, timeout or RSS threshold was raised to obtain the local result.
