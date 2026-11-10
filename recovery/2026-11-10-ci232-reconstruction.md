# Reconstruction after original workspace became unavailable

Base: remote diagnostic branch at 9545677c5fd0df14cb7280455a98fb8265c46a7b.
Original local target 08a548a29312925a9466cb955fe6068170849990 was unavailable in the current executor; this is a new implementation from recorded source edits, prior CI facts, and the base audit, not an assertion of byte-identical recovery. Original test artifacts/results have not been recovered and cannot validate this new tree.

Reimplemented behavior:
- Allocation-free empty MSVC PMR cache lists, retaining allocator-accounted nodes and existing rejection/refund checks.
- Keep serialized entity-page bytes alive for the borrowing read view; retain duplicate-ID and format checks.
- Test-only pass-through reads borrow the existing process-local Windows lease only on genuine offset-zero lock violations; explicit fault-hook errors and the production single-writer contract remain authoritative. Nine existing hook test units use the helper.
- Explicit CI_CASE_BEGIN/END C-stderr flush, Windows terminate exception/48-frame diagnostic, and 31 source-discovered read-session cases in independent processes. Mandatory unit/stress gates are retained. The terminate handler is best effort and does not guarantee coverage of direct abort/exit, SEH or stack corruption.

Known differences: reconstructed comments/organization and audit are new; lost commit identity, exact diff and old raw evidence cannot be confirmed. Budget limits, public types, format checks, sanitizer settings and CI triggers are not relaxed. Windows budget failures, exit-3 cause and cold ASan RSS remain unclosed.

Initial checks: YAML parsing, diff whitespace, source-size 2791 files/0 over 3000 pass. A new Linux probe forcing the actual Boost list alias passed cold zero allocations, rejected insertion/refund, retry, cross-list splice and destruction under denied credit. This is not Windows execution. Fresh Clang19 Debug -g0 coverage build and regression results are recorded below. No push or CI operation is authorized for this reconstruction before review.

## Fresh validation results

Code source: bd745e2dc0b44cf75e55ce925f257b78c8abb1bd; fresh build directory, Clang19.1.7/Debug -g0/coverage, Debian13 x86_64, Boost1.83. Targeted cache/admission/entity-page selection passed 75/75. Subsequent configuration found no stale objects.

Mandatory unfiltered quick run completed all 11067 enabled cases exactly once (11087 discovered including 20 disabled), with 11066 passes, one failure, zero skips, no timeout. All six stress cases ran once. All 11067 BEGIN/END markers exactly match the XML execution multiset. The single failure is RelocatableStorageBackendSchemaTest.LargeValueMaintenanceJournalResumesAcrossReopenAtEveryDurablePhase at line269: completed_tasks=1 versus expected0. That fixture is unchanged by reconstruction and relies on a 100ms phase interval; this evidence alone does not prove its root cause. Ten isolated repeats passed 10/10; the original full-run failure is retained and not treated as erased. The complete regression is therefore NOT green, and the failing run did not reach final coverage-report generation.

Source-driven NativeReadSession independent diagnostic passed31/31, with 31 paired markers. Existing evidence/process supervisor tests passed29/29 (7.09s), covering failure/timeout capture. YAML syntax, whitespace and source-size checks passed (2791 files, none over3000). Windows terminate code and Windows lease borrow branch have not run natively. The Boost list branch probe is Linux-only; it is not MSVC validation.

Compressed raw logs, shard XMLs, discovery, failed assertion, independent repetition and supervisor results, product hashes and probe source are in evidence/2026-11-10/reconstructed-ci232. A limited common-secret-pattern scan found no matches, not an absolute secrecy guarantee. No old evidence was presented as fresh. Original08a548a2 remains unavailable; this new code is not asserted byte-identical. Windows budget/exit3, cross-writer and cold ASan RSS issues remain outstanding. No push or CI trigger/cancellation was performed. Review is required before publishing this reconstructed change.
