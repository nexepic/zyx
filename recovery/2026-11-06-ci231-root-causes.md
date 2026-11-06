# Build 231: root causes and bounded local corrections

## Source and CI identity

The completed run is https://github.com/nexepic/zyx/actions/runs/42218204682,
source `94596329cd9fcf09fa892a92fa3a8e0f1267d657`. Linux and macOS TSan succeeded.
Windows (job 127893417535), macOS (127893417738), and ASan+UBSan (127893417244)
failed after successful compilation. ASan's selected native gate ran 711 cases:
710 passed and the two-hop rejection fixture failed; this is not a new sanitizer
memory-error report. Its separate cold RSS checks also failed.

Complete job logs remain local; hashes and sizes are in
`evidence/2026-11-06/ci231/source-log-manifest.json`. The Windows artifact ZIP
request through the configured proxy returned HTTP 403. No bypass or alternate
network path was attempted. Analysis uses the complete accessible job logs.

## Corrections

1. **BTree owner rebinding retained two decoded pages.** The macOS LLDB trace
   reports a 16-byte request with 14 bytes of remaining workspace, through
   `OwnedStdVector::reserve/resize` and `BTreePage.cpp:276 decodeOne`. In
   `rebindOwner`, the source entries remained alive during destination validation.
   Commit `ae9e425f` releases the source's authenticated entries after packing
   the independent destination, before its complete validation. Both full
   validations remain; no format check, workspace limit, or generation rule is
   removed. A regression limits concurrent key allocations to one complete page:
   the old production TU fails with `bad_alloc`, the corrected implementation
   passes. The same-tree BTree/shape selection passed 219/219. This establishes
   the lifetime defect locally; macOS conversion success still needs native CI.

2. **Another lazy map still used the MSVC noexcept allocation path.** Windows's
   isolated GDS catalog preparation fault test exits 3; other isolated cases
   return ordinary assertion failures. `OwnedLazyUnorderedMap` still created a
   standard hash container, despite the previously established MSVC debug
   `_Hash_vec` constructor allocating an iterator proxy inside `noexcept`.
   Commit `8286b5b6` uses the existing Boost hash-container dependency for this
   private wrapper on MSVC, retaining its captured allocator, cold zero-state,
   ownership, and average constant-time lookup. Other standard libraries are
   unchanged. Heterogeneous lookup uses the *stored* hash/equality objects,
   including stateful hashes. The new failure-injection test covers every cold
   allocation, refund, and retry. A Linux probe selecting only this Boost branch
   passed all four allocation failures and stateful heterogeneous lookup. This
   does not supply a fresh Windows termination stack or native result. A second
   independent Boost probe uses the actual `OwnedStdString` key, a stateful hash,
   and an explicit unmanaged-allocation authority: all five PMR/unmanaged
   admission failure points unwind, refund, and permit retry. Both probes
   select only the wrapper branch; they do not pretend Linux is MSVC.

3. **The two-hop rejection fixture did not force a large frontier.** Its 100
   parallel edges collapse to one ID/multiplicity pair; an optimized STL need
   not exceed 512 bytes. Commit `c760e068` adds 32 distinct dead-end intermediate
   nodes: the 33 ID/count pairs alone require over 512 bytes. The result remains
   exactly 100 parallel paths. The same 512-byte rejection and original wide
   retry budgets remain; only record-count limits accommodate the larger graph.
   This tests a genuine bounded working set rather than mandatory container
   overhead. Native macOS/ASan verification remains outstanding.

## Windows failures that are not yet resolved

`windows-isolated-failures.json` lists all 31 failed isolated cases and their
available numerical evidence. The full unit log has 33 distinct failures and
exit 3, ending around suite 151/603. It is not a completed inventory of every
Windows test; the unexecuted remainder cannot be called passed. Twelve isolated failures concern procedure source/result retention;
others concern sort/output/CSV/pagination/traversal, and the GDS fault test above.
Old allocator/string and six unwind/range failures did not recur.

For example, SelectedColumns reports source backing 624, retained source 1360,
external source 48, result/output 3114, requested vector backing 160, and possible
result relocation heap 1437 against a 5346 limit. These counters describe
simultaneously live source, output, and vector relocation, not a uniform stray
16-byte charge. The Windows independent two-entry map probe observed 1384 actual
PMR bytes versus the model's 1405 upper bound, and complete refund. This supports
the structural model for that sample; it does not establish all nested payload
or publication lifetime calculations. It is unsafe to subtract proxies, suppress
relocation admission, or mechanically raise every test limit.

A bit-mask accounting experiment was withdrawn: adding proxy bytes only at
resize did not also cover default construction and temporary move/assignment
lifetimes. Its patch remains local at `/workspace/zyx-ci231-bitmask-review.patch`
and is not part of these corrections.

## Cold ASan RSS remains unresolved

The observer-free and observed cold processes both crossed the unchanged 64 MiB
RSS growth limit (rounded 438 -> 502 MiB). In the traced process, live requested
allocator bytes rose roughly 5.7 -> 10.5 MB and allocator heap 40.9 -> 90.6 MB;
Mach resident/internal/external measurements also grew. These measurements
have overlapping scopes and do not identify one allocation stack or prove that
all excess is quarantine, stack-depot, file pages, or query payload. Later
in-process measurements starting at multiple GiB are not cold controls.

The traced cold rejection is precisely 459800576 -> 527269888 bytes, a
64.34375 MiB increase against 64 MiB. This is the first observed rejection, not
the peak of a completed query; the 0.34375 MiB excess does not justify a small
threshold increase or prove completion would fit. Later baseline-zero 1 MiB
rejections in the same log are deliberate injected guard tests, not cold-sort
measurements.

No RSS limit, sanitizer configuration, spill requirement, result cardinality,
or real RSS sampling has been relaxed. Native phase/allocation attribution is
still needed before a defensible further production correction can be claimed.

## Validation status

Final same-tree targeted regression on `c760e068` passed 51/51 using
`./scripts/run_tests.sh --quick --gtest-filter ... --file src/storage/relocatable/index/BTreePage.cpp`.
The unfiltered `c760e068` regression completed with exit 0: two shards each
passed 5533 cases, for 11066 total. This invocation did not set an XML output
folder, so the saved supervisor/log counts are not a per-name XML exact-once
proof. Coverage: regions 93.76%, functions 98.09%, lines 95.80%, branches 86.89%;
region/branch metrics remain below the repository's 95% goal. These results do
not validate the subsequent changes below; their final full run must explicitly
save per-shard XML. Initial 219/219 and the old-TU red comparison are separate. The forced Boost probe is a Linux structural
cross-check, not MSVC execution. No code has been pushed and no CI operation
has been triggered, cancelled, or rerun.

## Subsequent local corrections

`fea38b5f` completes the fallible-storage-container audit: BufferPool's hash and
ordered indexes, metadata payload-cache hash, label/read-adapter indexes, vector
search ID sets and two native index-candidate deduplication sets use the existing
admission-safe internal containers on MSVC. GNU/libc++ retain the original
standard containers. Vector ID sets retain their captured shared allocator owner.
No cache quota, validation, or admission callback is removed. The new cold-cache
regression rejects the first page allocation, permits retry and verifies refund,
without spending cache credit merely to create empty metadata.

The distinction is specific, not a blanket ban on STL: Microsoft's current
[list allocator constructor](https://raw.githubusercontent.com/microsoft/STL/main/stl/inc/list)
can propagate failure; the allocator-taking `_Hash_vec` constructor in
[xhash](https://raw.githubusercontent.com/microsoft/STL/main/stl/inc/xhash)
is `noexcept` while allocating its debug proxy. A Linux shadow-header probe
selecting the actual Boost aliases passed all six existing admission-container
cases (including nested allocator identity and every injected failure). Four
storage production TUs and the metadata-cache test TU compiled with those
aliases. This is not MSVC compilation or execution. The final native-backend TU (including both index-candidate replacements) and
cold-cache test TU also passed forced-Boost syntax checks. The final same-tree rebuild and execution passed as recorded below.

`980d91f8` shortens procedure publication lifetimes. It first checks the real
source-plus-output construction peak, then destroys the consumed source before
result-vector relocation. It retains output and moved-from-map handoff fees,
reconciles only after destruction, and retains all still-live row backing. The
output owns its properties independently; exceptions unwind the private result
rather than retrying consumed rows. Initial scalar preallocation now covers both
retained and copied payload capacities, including strings/lists/maps, without
constructing a speculative copy; every publication prefix must fit. Entity
outputs keep incremental admission. Existing byte limits and assertions are
unchanged. Two isolated Linux runtime-object variants each passed 103 related
cases; these are targeted incremental links, not the new full-tree result or
Windows proof. Final same-tree build and tests passed as recorded below.

During status inspection, reading `/proc/222202/environ` was denied with EACCES;
no retry or alternate privilege was used. It was not needed to determine the
completed test result from the supervisor and shard logs.

`c5295ddf` corrects the CSV source test's exception-origin assumption. The
execution account may reject source setup or allocator growth before the local
CsvReader check; its existing typed MaterializedByteLimitExceeded must be
allowed to propagate. Other runtime errors must still have the CsvReader prefix.
The 1024/8192 limits, zero delivered rows and wide-budget retry remain unchanged.
The c760 binary with a 200-character temporary-directory component reproduces
both original prefix assertion failures on Linux without changing its code or
budget. The corrected c5295ddf binary passes the same long-path test (1/1); both red and green XML are saved.

Final verification will use four shards on this workspace's four-CPU quota,
without a test filter, with XML for every shard. The earlier c760 run used two.
This changes test scheduling, not enabled inventory or production limits.

The frozen c5295ddf tree completed the final targeted selection: 318/318, exit 0. A subsequent Ninja invocation reported no work, and the long-path CSV regression passed. The single-shard runner does not translate ZYX_TEST_XML_DIR into GTEST_OUTPUT, so this target is evidenced by its log rather than XML; the separate long-path test explicitly saved XML. The unfiltered four-shard run explicitly maps and saves each shard XML and completed successfully. Source-size check: 2790 code files in the script scope, zero over 3000 lines.

## Final local verification

Frozen source `c5295ddfb7afdd9a4c5d76e1aa75c3cb47850797` completed the mandatory
unfiltered `./scripts/run_tests.sh --quick` with exit 0 in about 14m38s of test
execution. Four shard XMLs were compared with the same binary discovery:
11087 discovered, 11067 enabled, 11067 distinct executions and passes, 20 disabled;
zero missing/extra/duplicate/failure/skipped cases. All six stress cases executed
exactly once within this run. The precise disabled and stress lists are in
`final-full-exact-once.json`. Binary and core archive hashes remained unchanged
from before the run; no production/test source changed during it. The final
318-case target and explicit long-path CSV 1/1 are separate successful checks.

Final full coverage: regions **93.76%**, functions **98.09%**, lines **95.79%**,
branches **86.88%**. Region and branch coverage do not meet the repository's 95%
goal; this is not reported as a passing coverage threshold. These are Linux
Clang 19 Debug -O0 -g0 results with coverage, not Windows/macOS or ASan execution.

The saved audit is local only. No push, workflow dispatch, rerun, cancellation,
or main-branch change was performed. Windows's remaining budget/error-priority
cases and native verification of these candidates remain outstanding. The cold
macOS ASan RSS excess remains unresolved; the existing complete job logs cannot
separate allocation-stack/phase contributions, and the denied artifact ZIP was
not accessed by another route. No allowance or sanitizer guard was relaxed.
