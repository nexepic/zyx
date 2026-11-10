# Reconstruction after original workspace became unavailable

Base: remote diagnostic branch at 9545677c5fd0df14cb7280455a98fb8265c46a7b.
Original local target 08a548a29312925a9466cb955fe6068170849990 was unavailable in the current executor; this is a new implementation from recorded source edits, prior CI facts, and the base audit, not an assertion of byte-identical recovery. Original test artifacts/results have not been recovered and cannot validate this new tree.

Reimplemented behavior:
- Allocation-free empty MSVC PMR cache lists, retaining allocator-accounted nodes and existing rejection/refund checks.
- Keep serialized entity-page bytes alive for the borrowing read view; retain duplicate-ID and format checks.
- Test-only pass-through reads borrow the existing process-local Windows lease only on genuine offset-zero lock violations; explicit fault-hook errors and the production single-writer contract remain authoritative. Nine existing hook test units use the helper.
- Explicit CI_CASE_BEGIN/END C-stderr flush, Windows terminate exception/48-frame diagnostic, and 31 source-discovered read-session cases in independent processes. Mandatory unit/stress gates are retained. The terminate handler is best effort and does not guarantee coverage of direct abort/exit, SEH or stack corruption.

Known differences: reconstructed comments/organization and audit are new; lost commit identity, exact diff and old raw evidence cannot be confirmed. Budget limits, public types, format checks, sanitizer settings and CI triggers are not relaxed. Windows budget failures, exit-3 cause and cold ASan RSS remain unclosed.

Initial checks: YAML parsing, diff whitespace, source-size 2791 files/0 over 3000 pass. A new Linux probe forcing the actual Boost list alias passed cold zero allocations, rejected insertion/refund, retry, cross-list splice and destruction under denied credit. This is not Windows execution. Fresh Clang19 Debug -g0 coverage build and regressions are pending. No push or CI operation is authorized for this reconstruction before review.
