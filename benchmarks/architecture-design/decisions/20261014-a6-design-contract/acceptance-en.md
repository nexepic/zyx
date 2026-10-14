# S1.6 complete A6 design-delivery acceptance

Date: 2026-10-14. Decision: S1.6 design delivery is accepted. Complete S5 runtime governance, single-file query spill, parallel execution, and final performance are not accepted. The contract is zh.md/en.md/design-profile.json in this directory; component tests do not imply a completed executor.

| optimization.mdx requirement | Decision and evidence |
|---|---|
| Process/database totals and concurrent-query ownership | Finite totals/category caps, shared query accounts; seven foundation tests include real concurrent threads/multiple databases/global denials |
| Unique cache/write/maintenance/code/query attribution | Capacity/overlap/nested/fixed owners, shared borrowers, disabled code cap, output surviving close/keepalive; remaining owners explicitly block S5.1 |
| Child admission/refund/transfers and failure atomicity | One root mutex checks three levels atomically; same-account split/absorb preserves quotas, PMR preadmission/bad_alloc refunds and size_t boundaries tested |
| Pressure order and deadlocks | No resource waits; optional release, unpinned eviction, eligible workspace-backed spill, one retry then failure; no resource/metadata lock nesting or locked callbacks/IO |
| Cancellation, RSS, external overhead | Task/buffer/lease/pin destruction order, original errors/orphan backlog; global actual requests separate from local estimates/RSS, no RSS hard-limit claim |
| Single-file scratch permission and durable owner | Internal permit, physical-readonly failure, separate SGK_QUERY_SCRATCH, nonreused durable high-water owner, authenticated manifests/locations and five-state publication/release |
| Live-query pins, shared handles, recovery | Private owner/generation pins, preserved shared metadata pins, exclusive startup authenticated orphan cleanup, user-page protection, no query scratch in bulk cleanup |
| Allocator/user-commit consistency | Shared short publication gate and latest-state allocator rebuild; existing write fences preserved, conservative conflicts delegated to A5 without weakening isolation |
| Spill algorithms/order/unsupported paths | Stable sort runs/bounded merge first, canonical partition/hash next; bounded skew/depth/IO/disk failures; COLLECT/DISTINCT/float order/unqualified codecs gated |
| Long snapshots, maintenance, observability | Maintenance caps/protected floor/64-step cycles, finite backlog, pressure errors and metrics; no unbounded growth or pin violations |
| Format/test/performance replacement gates | A7 before query-kind/codec assignment; no current disk/ABI changes; concrete S5.1–5/O7/O8 fault/correctness/resource gates and final falsification/Kuzu window |

Selected ResourceCoordinator/A1 modules passed 82 tests and related runtime modules passed 222, with 26 overlaps: **278 distinct tests passed**. Seven new foundation tests establish only counters/caps/transfers/allocator concurrency, not production integration, slots/floors, keepalive, or spill. Linux/C++20; no full suite, benchmark, or measured cross-platform claim. Matching results contain raw logs/link commands/source hashes.

Among 34 fixed items, S1.6 is newly accepted: S0 1/4 (25%), S1 4/7 (57.1%), S2 0/5, S3 1/5 (20%), S4 0/4, S5 0/5, S6 0/4. Total **6/34 (17.6%)**. current-progress.json is synchronized; optimization.mdx's historical table remains unchanged. The user's standing authorization applies to committing/pushing this progress increase. Complete optimization continues.
