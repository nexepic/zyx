# A1 binding identity and physical-interface audit

Date: 2026-10-14. Status: source audit and 116 selected component tests passed; S1.1 remains in progress. This record adds no acceptance item and does not treat existing column fast paths as a complete physical plan.

Subsequent foundations are documented in `../20261014-a1-schema-observation/en.md` and `../20261014-a1-property-descriptors/en.md`. The table preserves the audit-time state. Native System Catalog resolves through a dedicated B+Tree and two directories; Superblock.catalog_root or logical root PageId/lifetime generation cannot directly represent schema content versions.

## Verified implementation

| Boundary | Current implementation and limits |
|---|---|
| Frontend/cache | QueryEngine::buildPlan looks up the existing logical PlanCache by query string, parses/optimizes on a miss, and clears it for mutatesSchema. There is no separate immutable physical-plan cache. Snapshot tokens cannot simply be inserted into this cache |
| Bindings | BoundSlotRegistry uses uint32 slot/scope and uint64 plan identities, rejects foreign descriptors, and seals its layout. BoundProjectionReferences borrows row inputs and clears pointers at scope exit. It remains a query-local row adapter, not a complete AST/operator binder |
| Fixed columns | Contiguous int64/double/bool values; bool uses uint8. Missing/NULL/value states are separate. BudgetMemoryResource accounts for capacity and allocation overlap. Clear retains charged capacity, release refunds it, and rectangular append failure rolls back unpublished rows |
| Selection | SelectionVector uses a uint64 bitmap, not a per-row index array. Physical positions and duplicate input positions remain distinct. Its standard vector capacity is not automatically PMR-accounted; the complete physical owner must admit retained capacity explicitly |
| Dynamic fallback | PrimitiveNumericColumns validates shape/types before use. Mixed types or optional-buffer budget pressure fall back to the same dynamic accumulator before changing aggregate state. Cancellation, allocator errors, and malformed shape are not optional budget fallback |
| Execution state | ExecutionSharedState borrows snapshot/bounds/context and owns budget, PMR caches, and result reservations. ExecutionTaskState owns stats/injected-row/property-column state. Execution is single-threaded; shared caches/budgets cannot be accessed concurrently without more work |
| Output | QueryResult owns result rows. Public row callbacks and the final deferred-EntityRef result pipeline have different borrow/read-ahead contracts. Column/page spans must not escape into the ABI |
| Schema identity | Mutation fences contain backend/format identity, 21 opaque tokens for seven graph roots, and overlay revision. They are not an independent schema epoch. Superblock has database_id/catalog_root, but the query layer has no authenticated schema-observation interface yet |

The ten selected suites cover slots/scopes/stale plan IDs, projection borrows, fixed columns and rectangular rollback, selection, allocation admission, dynamic numeric fallback, NULL/NaN/overflow, column filtering/aggregation, and nested execution/budget/input restoration. Linked against the current Linux C++20 core, all 116 tests passed. No full suite or benchmark ran.

## Interfaces to settle before the next implementation

1. Complete query-local immutable physical descriptors: slots/layouts, types, access paths/algorithms, pipeline/blocking boundaries, and semantic identity. Existing components do not replace full LogicalOperator/Expression binding. Keep logical IR/cache; do not put mutable task state in cached plans.
2. Snapshot-owned token resolution and schema-observation identity: database identity, backend incarnation, authenticated schema/catalog stamp, capability contract, and overlay revision. Do not relabel the graph fence as a schema epoch. Initially disable physical caching; enable it only after this interface and cross-database/reopen/DDL/overlay invalidation tests are complete.
3. A common batch envelope: fixed columns/three states, EntityRef, dynamic owners, selection, source identity, capacity admission, and batch incarnation. Define variable-size ownership and actual decoder capacity. Standard vectors or encoded byte counts do not substitute for a complete allocation owner.
4. Exact flat-first and grouped-extension fields/consumer contracts: parents, offsets, correlation groups, multiplicity, order, NULL extension, and overflow. Set DISTINCT/group-by/path/subquery expansion boundaries. Representative branching/multihop experiments remain in the final window; scan gaps without joins do not prove factorization benefits.
5. Lifecycle and removal inventory: immutable plans, query shared state, task cursors/batches/accumulators, and blocking merge ownership are separate. Close single-threaded lifecycle first; O7 adds scheduling later. Plans must explicitly declare, account for, and test legacy row adapters.

This is the remaining concrete interface worklist, not an implementation or S1.1 acceptance claim. A1/A6 capacity and lifetime dependencies must close before their production adoption. Current code has not been replaced with a second unverified executor.

Evidence: logs, module link commands, and source hashes in `benchmarks/architecture-design/results/20261014-a1-binding-audit/`. Source baseline is pushed commit `646298ae`. This record remains in the working tree until acceptance increases. Accepted progress: S0 25%, S1 28.6%, S2 0%, S3 20%, S4–S6 0%; overall 4/34 (11.8%).
