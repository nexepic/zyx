# A1 typed-column foundation contract

Tasks: local S1.1/S2.1 foundation. Status: fixed-width component implemented and module-tested, not connected to query execution. This is not full design, execution or performance acceptance.

## Current path and boundary

LogicalPlanReadRuntime uses string-keyed rows, property caches, EntityRef and selected-read fast paths. BoundedBatch charges logical row bytes; BudgetMemoryResource already charges some PMR containers. This component reuses the latter for contiguous fixed-type columns. Parser/logical IR, storage format, QueryResult, Driver ABI and database backend are unchanged.

Implementation: `src/query/execution/FixedWidthColumn.hpp`. The forthcoming binder maps variable names to slots and column types. A map-to-column adapter alone is not A1 completion; query dispatch is not changed here.

## Local interface and ownership decisions

- Support int64_t, double and bool. Boolean payloads use contiguous uint8_t rather than vector<bool> proxies. There are no string keys or per-row dynamic type dispatch. Owning variable-length/complex columns and semantically equivalent mixed-type fallbacks still need implementation; existing database types are not rejected.
- Store one contiguous value array and one state byte per row, distinguishing missing/null/value. Null/missing payloads initialize to zero and must not be consumed without checking state. This is an in-memory representation, not a persisted codec or public ABI.
- Explicit maximum rows and shared query budget; no benchmark-specific size. Grow capacity from one by bounded doubling. Charge actual array capacity and simultaneous old/new allocations during growth.
- clear reuses charged capacity; release/destruction refund allocations. truncate only shrinks, allowing rectangular-batch rollback without incorrectly refunding live capacity.
- Columns own their data and cannot copy/move because their PMR resource addresses must remain stable. Budget outlives columns. Borrowed spans/references expire on any mutation. No storage page/snapshot pin is borrowed here; EntityRef must not be treated as an unchecked integer.
- Columns and allocation state are task-local. Multiple columns share a single-threaded budget, not concurrent mutation. Future parallel tasks need local charging and coordinated shared allowances; this non-atomic budget is not a worker-shared resource.

## Failures and lifecycle

Invalid limits/null allocators/out-of-range reads and reading absent/null values fail explicitly. Both reserves precede length publication, so budget/allocation/row-limit failures preserve existing cells. If one reserve succeeds before another fails, retained capacity remains accurately charged until release. Upstream allocation exceptions roll back unsuccessful budget admission.

Upper batch/operator boundaries check cancellation and propagate errors without silent truncation. A multi-column frame records previous lengths, publishes batch cardinality only after success and truncates columns on failure; nonrectangular intermediate state cannot reach consumers. These are process-local values without a persisted recovery protocol. Destruction releases arrays before allocator state and the external budget.

## State separation and remaining A1

Immutable physical plans own slot layouts and expression/access descriptors. Query-shared state owns snapshot/guard/budget/output contracts; task-local state owns columns/selection/cursors/local aggregates; blocking state owns merges/sorts/future scratch. Plans cannot retain reusable batches or borrowed page pins. Binding/cache schema identity, variable-length owners, all operator consumers, grouped representation and owning public output still require full S1.1 decisions; this component does not independently accept S1.1/S2.1.

Hypothesis: contiguous typed data and capacity reuse reduce map/dynamic-object costs. Falsification: reject or revise adoption if the final equal-input execution path has no gain or unacceptable protected-workload/resource regressions. Module tests do not prove performance. Experiments remain deferred until implementation is complete under the latest user instruction.

## Module verification

Ten `FixedWidthColumnTest.*` GoogleTests passed, compiled as C++20 with Wall/Wextra/Werror from the current module using existing cached GoogleTest dependencies. Tests cover three states, contiguous types, reuse/release/destruction, shared limits, overlapping growth, partial two-array allocation failure, upstream bad_alloc, row/range limits, non-power-of-two bounds and batch rollback. File: `tests/src/query/execution/test_FixedWidthColumn.cpp`. No benchmark/full suite/commit/push; temporary module executable removed.

## Rectangular batch composition

`src/query/execution/FixedWidthBatch.hpp` combines known column types into task-local batches. Inline recursive storage constructs nonmovable columns at their final addresses without uncharged per-column heap owners; arrays reuse the shared budget allocator. Types are compile-time and limited to the three primitives above, not a complete runtime slot layout. Fixed object metadata belongs to the caller; future execution-state accounting must cover it. Column charges are not complete query memory or RSS.

TypedCell factories expose only value/null/missing states and reject value access on absent cells. Public column access is const, preserving rectangularity. Append writes columns in sequence and publishes cardinality only after all succeed. Budget/upstream failures truncate every column to the previously published cardinality and rethrow the original error. Retained capacity remains charged; rollback is not complete memory release. clear/release apply to all columns. Batches cannot copy/move and all views expire on mutation.

Seven `FixedWidthBatchTest.*` and ten column tests passed together under the same C++20/warnings-as-errors compilation. Cases cover cross-column states, later-column failure, partial growth after previously published rows, retry after budget pressure release, retry after upstream bad_alloc, reuse and release. File: `tests/src/query/execution/test_FixedWidthBatch.cpp`. Slot binding, selection, EntityRef/variable-length owners, physical operators and output integration remain pending. No additional S1.1/S2.1 acceptance or performance run.

## Plan-local slots and scopes

`src/query/execution/BoundSlotRegistry.hpp` maps compilation-time names to uint32_t plan slots. Hash lookup is for binding; later consumers retain slots rather than parsing names per row. Slot/scope IDs carry a nonreused process-local plan identity, rejecting stale IDs even if registry addresses are reused. This is not database/schema/snapshot identity and does not authenticate property tokens or plan-cache dependencies.

Scopes reference previously created parents and cannot form cycles. Lexical scopes inherit visible parent bindings and declarations shadow with distinct slots. Isolated scopes resolve only locals. Explicit importFromParent aliases a visible direct-parent name to its original slot, without crossing isolation boundaries or observing siblings. The complete Cypher binder must select visibility according to existing expression/subquery semantics; this mechanism does not make all subqueries isolated or alter public visibility contracts.

Explicit limits cover bindings including aliases, scopes and name bytes. PMR vectors/hash nodes/buckets/long strings charge a plan-owned compilation budget that outlives the registry and cached plan, not a per-query temporary allowance. Internal records cannot copy; vector growth moves with original allocators. Parent owners must charge fixed object metadata. Declaration publishes counts only after both name index and descriptor succeed, erasing newly indexed names on failure while retained capacity remains charged. seal prohibits further layout changes; sealed layouts permit read-only reuse within owner lifetime.

Twelve `BoundSlotRegistryTest.*` passed under C++20/Wall/Wextra/Werror. Cases cover declaration/shadowing/import/isolation, foreign/stale address-reuse IDs, sealed layouts, limits, allocation-stage faults/retry, scope/descriptor growth and shared-budget refunds. File: `tests/src/query/execution/test_BoundSlotRegistry.cpp`. Real LogicalOperator/Expression traversal, type inference, schema/cache identity and query operator integration remain pending; S1.1/S2.1 remain unaccepted. No benchmark/full suite/commit/push.
