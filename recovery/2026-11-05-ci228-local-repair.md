# Build 228: local repairs and verification limits

## Source and scope

Investigated https://github.com/nexepic/zyx/actions/runs/41962296906 at 68f1220f79b95d7c5ff32b2fff95318901a8cb6f. The original workspace and checkpoint 377ca857bc689c98638ee2fac547bca270530741 remain available. This phase makes no remote writes or CI dispatches. The patch was verified before committing, with a recorded per-file fingerprint. It is now saved as three local-only source commits (listed below), not a published or platform-verified checkpoint.

Linux and macOS TSan jobs passed. Windows, ordinary macOS, and macOS ASan+UBSan failed. The failure manifest records exact job links and the union of named failures across supplementary diagnostics and ordinary tests; that union is not a completed Windows suite count. Windows ordinary tests ended early with exit 3. The latest logs contain no AddressSanitizer memory report or UBSan runtime-error report; this does not make the sanitizer job pass.

## Independent causes and local changes

1. libc++ 19–21 fresh string construction previously used the generic 2*n+1 request bound. The latest Apple diagnostic measured n=3000-like requests near n+alignment, and audited LLVM __recommend uses 8-byte alignment with an SSO-boundary correction. For the audited C++20/24-byte string configurations, the patch uses checked roundUp(n+1,16), keeps the conservative fallback elsewhere, and retains actual-capacity settlement and platform allocator-prefix accounting. It changes no materialization ceiling. Source audit references: https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-19.1.7/libcxx/include/string and https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.6/libcxx/include/string . Apple vendor behavior still needs native validation of the new code.
2. Empty debug STL vectors may own allocator proxies. Existing internal typed columns released storage by constructing/swapping another empty vector, leaving live charged proxy storage; allocator-failure injection can also encounter noexcept STL construction paths. A private trivial-element buffer now admits exact backing allocations before allocation, charges old/new overlap, and releases without allocation. It supports only trivial implicit-lifetime elements; owning/public value types are unchanged. FixedWidthColumn, EntityRefColumn, and BatchEnvelope use it. This addresses the observed release-contract mismatch and removes the proxy mechanism in these buffers, but does not prove all Windows exit-3 paths eliminated.
3. BoundColumnLayout no longer owns one allocator-aware string per name plus a temporary hash/set structure. It validates names with a budgeted temporary view array, releases that temporary, then owns one name-record array and one packed text buffer. Name order, embedded NULs, exact duplicates, descriptor identity, copying and cancellation contracts remain tested. There are at most three upstream allocations independent of column count; no public result/value ABI changes.

## Actual local evidence

- Independent allocation interposition: GNU and Linux libc++19 each checked 196647 fresh constructions (string_view, count/character, and copy from a source with spare capacity). Observed requests stayed below the request bound. This is not an Apple-native or arbitrary-version proof.
- libc++19 codec red/green, identical 4096-byte ceiling and 3000-character payload: old bound rejects before decoding; new bound succeeds, peak 3008, final 0. These small executables compile all codec dependencies consistently with libc++, rather than linking a GNU-ABI core.
- Independent libc++19 codec sample: 64 separately retained 8192-character values produce 64 actual new requests totaling 524800 bytes, actual live and budget held match, and both return to zero. This covers decoder allocations only, not a complete graph mutation/query.
- Layout red/green, fixed 8192-byte ceiling and 256 names: old layouts reject on GNU and libc++19; packed layout succeeds with three allocations, retained/peak 5010 bytes and final zero. The fixed three-allocation efficiency assertion is independent of the measured byte value.
- Current-header focused ASan+UBSan executable: 38/38, including trivial-buffer failure/refund/alignment/overlap, fixed columns, envelopes, layouts, and parent-budget tests. Relevant production code and dependencies are instrumented; this is not the full database or macOS sanitizer gate.
- A separate consistently compiled Linux libc++19 executable (including Google Test sources) also passed the same 38 cases. The first link lacked PropertyTypes.cpp; after including that production dependency it linked and passed. This does not emulate MSVC proxies or certify Apple vendor libc++.
- Source-size script: initially2739 covered code files; after including five small diagnostic source/scripts as evidence, the final check covers2744, none over3000 lines. This script does not validate the historical archives.
- Standard same-tree Ninja build exited 0; a second increment picked up the late layout tests, and a third invocation reported no work. Same-tree unified-binary focused regression passed 43/43. Unfiltered `./scripts/run_tests.sh --quick` completed with exit0 (session 71092). Both shards passed: 5513 + 5512 = 11025 enabled cases. XML/discovery exact-once reconciliation reports 11025 distinct executions, 20 existing disabled, no missing/extra/duplicate/failing/skipped cases. Compressed raw shard XML, discovery, full run log, runner result and inventory are saved alongside the source fingerprint. Coverage is lines95.85%, functions98.13%, regions93.91%, branches86.88%; the region and branch goals remain below95%, and that is not described as an all-metric target pass. The modified-source fingerprint still matches all tested code files. Earlier failed build attempts (nested record type/default-construction and name shadowing) were corrected, not omitted from the work record.

## Still open

Windows procedure/result failures include true empty hash-container sentinel/bucket/proxy storage and simultaneous source/output lifetimes. Lowering these charges or changing all test ceilings would be unsound; the local buffer/layout patch does not resolve all 29 distinct names in the diagnostic/main union. Some macOS tests also depend on obsolete platform-independent rejection assumptions; others hit real shared-domain admission. They require per-case ownership analysis and native rerun, not a blanket threshold change.

The sanitizer job has four named test failures: two native cell admission cases, coordinated materialization, and read-only spill sort. Its mandatory warm spill-sort execution timed out at the existing 120000ms query deadline; a separate cold supplementary preflight instead exceeded a 64MiB whole-process RSS growth guard. Those are separate observations. Neither guard is disabled or raised by this patch. The latest fixed stack-overflow reproducer passes, but that cannot substitute for the failed full job.

No new CI is triggered. Linux-only native execution cannot certify Windows/MSVC or Apple libc++ behavior. The existing full-suite pass at the base checkpoint is historical evidence and cannot certify this modified tree.

The first quick invocation was safely interrupted with SIGINT/exit130 early in execution because per-shard XML output had not been configured. Its log is `/tmp/zyx-ci228-quick-initial-interrupted.log`; no completed result is claimed. The replacement unfiltered invocation uses a distinct coverage run ID and XML directory, so aborted profiles cannot contaminate its final report. No source changed between these invocations.

## Current-source isolated RSS reproduction

A separately linked current-source executable, with only the scratch test translation unit ASan-instrumented and the real Linux RSS sampler preserved, still fails the original 64MiB guard: baseline 111460352, peak 180101120, growth 68640768 bytes; sanitizer peak live bytes 13519988, heap 93081600, 240625 sampler calls. Exit1 is the test assertion/guard failure, not a sanitizer memory report. The sample took 1437ms. This is a cold Linux partial-instrumentation diagnostic, not an Apple native/full ASan result; it does not attribute the entire resident difference to active query objects or prove a leak. The layout/buffer changes therefore must not be presented as an RSS fix. No threshold, quarantine setting, or guard was relaxed.

## Remaining allocator-construction audit

Read-only review during the frozen full regression found that DynamicValueArena still owns six std::pmr::vectors, and PrimitiveNumericColumns, BoundProjectionReferences, and BoundPropertyBindings retain PMR vectors as well. Some existing fault tests construct these owners with an already-failing upstream allocator before the EXPECT_THROW around later growth. Therefore the new trivial buffers must not be described as eliminating the debug-STL proxy hazard globally. Replacing the remaining constructors/containers requires a consistent allocator/ABI review and additional regression. An isolated Boost.Container 1.83 vector plus std::pmr::polymorphic_allocator experiment verifies empty construction without allocation, propagating reserve failure, no allocating empty-container construction during release, and a C++20 contiguous_range. The project Conan requirement is Boost 1.87, but no production container substitution has been made during this frozen regression. It is a possible established-container alternative for nontrivial elements, not proof of Windows behavior or a completed fix.

The Windows log reports MSVC 19.51.36260 / toolset 14.51.36231. Microsoft STL source review confirms that both the vs-2022-17.14 reference and current main implement the allocator-taking vector constructor as noexcept while calling _Alloc_proxy: https://raw.githubusercontent.com/microsoft/STL/vs-2022-17.14/stl/inc/vector and https://raw.githubusercontent.com/microsoft/STL/main/stl/inc/vector . This supports the debug-proxy exception mechanism; it is not a native execution of each remaining owner or a substitute for the runner's exact installed header snapshot.

## Local checkpoints after verification

The tested source files still exactly match fingerprint `47848ccc2002f16e3c90d2315ce5a80583e32acf36c0dc1d758da262e6620efb`. Source commits, newest first:

- `0b7edcd8013360a7215eec3d65ab0a2f69ab13b5`
- `d9cb29215572f9e52dc377481b3d39d2780725ad`
- `46f3442f963b8cf402e7deb67c263d7995b00bf2`

All six historical stress cases are present and passed in the unfiltered full-run XML. No redundant stress rerun was needed. No remote ref was written. The only intentionally untracked build helper is `buildDir`, a symlink to `/workspace/zyx-ci-config`.
