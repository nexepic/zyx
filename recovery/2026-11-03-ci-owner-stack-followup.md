# CI owner and stack follow-up

The diagnostic run `41580967988` uses commit `0de8bad6`. Its macOS
ASan+UBSan job still reported a worker stack overflow while copying a cached
Catalog radix page. The earlier 128 KiB static zero buffer did not suffice.

The subsequent local candidate `7f71e410` moves conversion-only planning to
an immediately invoked function frame. On Linux clang 19 with ASan+UBSan,
the ordinary graph publication frame measured by `-fstack-usage` decreased
from 195352 to 163224 bytes. A same-source, difference-object incremental
link passed 87 selected tests: 73 ordinary hybrid conversion, 10 SCAN field
cohort, three ABI ingest cases, and one blind rebase. This does not prove the
macOS sanitizer succeeds or substitute for a clean full build.
The blind rebase case also passed 20 repetitions under a 256 KiB Linux stack
limit without sanitizers, and 20 repetitions in a separate Linux binary with
only the graph publication translation unit built with ASan+UBSan. Neither
experiment establishes the behavior of the fully instrumented macOS binary.

The two original ABI ingest cases used independent writable database handles.
The native public contract permits one writable owner; on Windows, its
exclusive byte-range lease rejected the second handle's write. The local
`db8182b3` test-only bridge injects actual competing publication through the
same backend, retains the ABI ingest commit and conflict assertions, and
leaves the public active-ingest exclusion intact. Related Linux suites passed
14/14 using a same-source difference-object link. Test hooks are compile-time
guarded; syntax checking the production unit with hooks absent passed.
Windows has not yet run this candidate.
The ABI cases still stage through the public ingest interface and verify
commit/rebase or conflict/rollback and reopen; the real competing write is
injected only through the same backend by the test-only bridge. The diagnostic
workflow filter follows the renamed same-owner rebase case.

The macOS `shared_large` fixture retains 64 decoded 8192-byte strings in one
1 MiB workspace. The current libc++ fresh-string admission upper bound alone
is 64 × 16385 = 1048640 bytes. This proves refusal under that accounting
policy, not that real allocations exceed the budget. A Linux libc++ 19
measurement observed smaller requests; it does not establish an Apple libc++
bound. Other macOS budget and prefetch failures remain independent and open.

Additional unpushed diagnostics report the actual CSV error, prefetch split
count, native string allocation samples, and individual Windows plan-cache
test exit codes. They do not relax the existing unit or sanitizer gates. The
Windows plan-cache suite previously exited 3 before reporting an individual
case, so its root cause remains unknown.
