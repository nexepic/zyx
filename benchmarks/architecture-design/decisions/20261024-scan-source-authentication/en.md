# S2 source authentication and initial calibration (unaccepted)

No acceptance weight is added. S2 remains unaccepted at 15% total, with no commit/push. The documentation site's optimization.mdx remains the progress authority.

BUILDING cloning now validates the source envelope and declared payload CRC before changing format bytes. Rejection leaves the input segment unchanged and does not copy the corrupt segment to the target. Full integrity and final admission remain mandatory.

Internal streamPrimaryMembership reads actual Entity Map/records and complete LabelSets from one store into 64-ID membership groups, including propertyless and unlabeled nodes. Records carry source database/generation and sorted label tokens. Single-node or cross-node label unions above 64 refuse the complete group without truncation. Cancellation/consumer failure never replays a prefix. Source workspace is capped at 1 MiB and must share the caller account; retained destination state needs a separate target account. The function provides construction material only: no scan certificate, directory publication, or clearing of BUILDING.

Seventeen distinct necessary cases passed: isolated codec/file cloning 8, default-profile rejection 1, real Entity/LabelSet source 8. They cover payload corruption, source preservation, cancellation, budget release, propertyless/unlabeled nodes, group boundaries, old snapshots, empty input, single/cross-node excessive labels and consumer failure. No full unit suite ran.

## Initial performance calibration

Linux / Intel Xeon Platinum 8370C, four CPU quota and 16 GiB environment memory; one thread, 6 GiB process address-space limit, original 64 MiB cache budget, Kuzu 0.11.3. For each dataset, three interleaved independent process pairs run three warmups and ten measured samples per case: 480 samples total. Complete typed rows and multiplicities pass the oracle; multi-key ordering is also checked. The table pools P50; initial-summary.json preserves per-process P50/P95. P95 uses linear interpolation; ten samples do not establish precise tails.

Historical AMD artifacts were unavailable. All twelve regenerated CSV hashes match the registered inputs (medium seed 42 / holdout seed 314159), but rebuilt ROW database physical identities differ. These results calibrate current work only, replacing neither the historical selection baseline nor the candidate/final acceptance windows. The query binary is frozen at 9d65a397; subsequent private construction headers are absent from its query call chain. The timing archive recompiles the two I/O/metadata translation units without test-hook dispatch, retaining O3/NDEBUG and no profile/coverage. Library/binary hashes, commands and the raw-data index are retained.

A ratio above one means ZYX is slower. None of these cases beats Kuzu.

| Case | Scale | ZYX P50/ms | Kuzu P50/ms | ZYX/Kuzu |
|---|---|---:|---:|---:|
| Label filter | medium | 341.934 | 5.278 | 64.78 |
| All-node filter | medium | 490.376 | 1.734 | 282.78 |
| Single-key grouping | medium | 339.292 | 9.884 | 34.33 |
| Multi-key grouping | medium | 409.214 | 14.478 | 28.26 |
| Label filter | holdout | 228.068 | 2.991 | 76.25 |
| All-node filter | holdout | 363.803 | 1.245 | 292.32 |
| Single-key grouping | holdout | 230.327 | 4.762 | 48.37 |
| Multi-key grouping | holdout | 317.227 | 8.578 | 36.98 |

Evidence: receipt.json, initial-summary.json, three module-test JSON reports, initial-benchmark.py and two compile/link recipes. Raw samples are under benchmarks/architecture-design/results/20261024-work-scan/, indexed by SHA-256 in the receipt. Old evidence is unchanged.

Remaining: atomic BUILDING membership/count/field-root publication, bounded complete integrity, whole-file admission, certified persistent scan consumption, update/read/reopen, remaining lifecycle and S2–S5 acceptance. Public candidate opening remains unsupported. No 5x benefit or 100% completion is claimed.
