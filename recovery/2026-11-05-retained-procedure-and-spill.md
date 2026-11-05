# Retained procedure memory and spill-buffer follow-up

Production/test source anchor: `b462b0177fa2a5b3480867702d8539a4fe9c5e40`.
This report supplements the earlier query-cell, snapshot-fence and platform
diagnostic reports; historical failures remain preserved in those reports.

## Procedure accounting and portable boundaries

`BufferedRows.inc` previously charged an embedded `ResultValue` again after
charging its enclosing result hash node. Conversely, advancing the publication
row index could stop accounting for still-live row-vector capacity and moved-from
container storage. Result accounting now uses the common retained-capacity model;
publication accounts for the full live backing/heap, with the old logical floor
for remaining rows. Reallocation admission includes old/new backing overlap.

The metadata fixture's owning result has an independently calculated Linux
lower bound above its historical 7500-byte success allowance. The user approved
correcting this inconsistent boundary without changing the public value layout.
The fixture retains 6000-byte rejection, adds explicit 7500-byte rejection, and
uses a fixed reviewed 32 KiB success budget. A separate fixed 16 KiB retained
result cap and concrete vector/string-capacity caps guard efficiency. These caps
are not computed by multiplying the measured result into a larger allowance.
The independent lower-bound calculation does not call the production heap model;
it does not include every hash-node/bucket/proxy cost. It is Linux evidence, not a
Windows allocation measurement.

The converted-vector rejection regression uses a real shared query account and
checks refund after refusal and after the successful retained result is released.
The hit-buffer regression retains the tiny-envelope rejection and separately
reaches the intended hit-buffer stage under a fixed request envelope. Typed quota
exceptions are recognized where the enforcing owner can legitimately differ by
STL implementation; backend/semantic error messages remain independently tested.

## Spill allocation churn

The spill writer reuses its bounded encoded-byte buffer and budgeted PMR binding
scratch. The merge cursor reuses decoded map nodes and outer key storage; stale
bindings/caches are removed and all borrowed input views are cleared on exit,
including errors. The existing fixed spill workspace, canonical wire bytes,
record bounds, checksums, cancellation and publication checks are retained.
Regression tests exercise canonical equivalence over 1000 encodes, stable buffer
storage after warm-up, failed admission, changing decoded shapes, stale-field
removal, malformed trailing bytes and owning lifetimes after source release.

## RSS evidence and limitations

Linux RSS sampling previously allocated an `ifstream` buffer on every sample.
A separate global-new probe observes 1000 requests / 8192000 requested bytes for
1000 old samples, and zero requests for the fixed-buffer sampler. Both sample the
actual `/proc/self/statm`; the limit and RSS units are unchanged. macOS continues
to use `task_info`, so this Linux improvement is not a macOS root-cause claim.

The current Linux partially instrumented probe **still fails** the original
64 MiB RSS guard with default ASan options: observed growth 68124672 bytes,
peak live allocator bytes 13515463. Only the test translation unit is
instrumented in this probe; this is neither a complete sanitizer build nor
Apple validation. It does not establish a leak or attribute all resident growth
to query live allocations. No threshold or sanitizer quarantine setting is
changed to accept this outcome.

The diagnostic workflow retains its mandatory full ASan/UBSan gate and adds a
separate default-options RSS-spill preflight. Explicit test-hook tracing reports
real RSS plus optional sanitizer live/heap metrics after a rejection decision;
these metrics never replace admission or RSS. Optional weak sanitizer imports
have been syntax-checked for Apple/Windows targets, but this is not a native
platform build. The ASan RSS issue remains unverified/unresolved pending native
evidence, rather than being declared fixed by Linux targeted passes.

## Validation status

Before enabling coverage, the standard same-tree build completed and focused
QueryGuard, procedure/buffered sort, external sort/codec and query-cell fence
selection passed 108/108. The supplied small XML and probe files are under
`recovery/evidence/2026-11-05/budget-sort/`. Some exploratory mixed-object results
from earlier reports are not counted as final validation.

The required unfiltered `./scripts/run_tests.sh --quick` is currently compiling
the same source with LLVM coverage enabled (run id `ci-fix-20261105`, two shards,
Debug `-O0 -g0`). This paragraph is a pending status, **not** a full-suite or
coverage pass. Final counts and coverage must be appended from its actual result.
Native Windows/macOS, complete sanitizer verification and final remote delivery
are not implied by the focused Linux result.

## First unfiltered covered run: b462

The complete unfiltered run finished with exit 1: enabled/discovered/executed
11019, distinct 11019, disabled 20, skips 0, missing/extra/duplicates 0.
11016 passed and three failed: the administration, Vector Training and GDS
catalog retained-result budget tests in QueryEngineLogicalBackendBoundaryTest.
All six stress cases passed in this run's actual XML. Original XML, discovery,
runner result and exact inventory are preserved under `b462-full/`; this failed
run is not relabelled as a pass. The script stopped before report generation.

All three tests supplied a fake backend without a query resource domain or an
explicit shared account, and expected the old duplicate-size estimate to deny
a 1024-byte scalar publication. After correcting that estimate, the logical
result can fit and the tests no longer exercise authoritative shared ownership.
The corrections keep the original 1024/4096-byte limits, explicitly supply a
real QUERY account, check typed quota refusal without publication, check actual
retained-output ownership and refund, and retain successful retry. Plans are
built before account-scoped execution so parser setup cannot substitute for the
intended execution test. The Vector Training case also confirms preparation
was reached before denial. A separately compiled candidate test TU against the
same frozen production objects passes all 68 boundary tests. An initial missing
typed-exception include was corrected before that pass. Final same-tree build
and another full unfiltered coverage run are still pending.

## Final same-tree covered run

Validated production/test source: `dce421b6dd02924b8167f48512f0700748e2d1ec`.
The required unfiltered `./scripts/run_tests.sh --quick` completed with exit 0,
including report generation, using two shards and run id
`ci-fix-20261105-v2`. Actual discovery/XML comparison: **11019 enabled, 11019
executed exactly once, all passed**, 20 disabled, zero missing, extra, duplicate,
failure or skipped cases. All six stress cases passed in this same run; their
individual names are preserved in `final-full/stress-inventory.json`. The two
shards took 1610.011 and 1522.948 seconds respectively.

The repository coverage script's adjusted whole-source metrics are lines
**95.85%**, functions **98.15%**, regions **93.92%**, branches **86.89%**. The
region/branch figures do not meet the 95% goal; a passing script exit is not
claimed as meeting all four targets. No exclusions or checks were added to
accept these values. Full compressed XML, discovery, runner result, report log,
coverage summary and hashes are preserved under `final-full/`; raw profiles
and executable remain local rather than being committed as large binaries.

### Remaining verification and blockers

This is Linux Debug with LLVM coverage, not a fresh Windows/macOS or full ASan
pass. The MSVC-debug PlanCache tree-index exception-safety change and all
platform-specific accounting still need native verification. The ASan RSS issue
is **not fully solved**: the retained Linux partial-instrumentation comparator
still exceeds the unchanged 64 MiB real process-RSS limit. The production buffer
reuse and Linux sampler fix reduce unnecessary allocation churn, but are not a
proof of that Apple gate. The native default-options preflight and optional
allocator diagnostics are included to expose this remaining issue without
weakening the mandatory sanitizer gate. No CI dispatch, package publication or
main update is part of this delivery.

The original checkpoint `377ca857bc689c98638ee2fac547bca270530741` and isolated
experimental branches remain local. Rejected CACHE-domain and extra-IO precheck
prototypes are not presented as production fixes. After evidence-only delivery,
production/test/workflow content must remain identical to the validated source
above; the diagnostic-branch push is followed by a pause, without CI polling.
