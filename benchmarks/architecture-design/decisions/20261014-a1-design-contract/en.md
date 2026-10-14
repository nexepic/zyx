# A1 binding representation and physical-plan contract

Date: 2026-10-14. Scope: S1.1 complete design delivery; production migration and performance belong to S2/O1/O2/O6/O7. This fixes initial representations, algorithms, lifetimes, and replacement gates. Component tests do not prove complete production adoption.

## 1. Audited call chain and boundary

QueryEngine::buildPlan clones from logical PlanCache or parses/optimizes into QueryPlan. executePlan still uses LogicalPlanReadRuntime executeOnSnapshot/visitOperator. Expressions and mutation reads share evaluateExpressionWithSnapshot; QueryResult/Driver ABI own values at the output boundary.

Existing foundations are BoundSlotRegistry scopes/slots/sealing, BoundProjectionReferences, fixed columns/rectangular batches, selected predicates/numeric columns, shared/task state, conservative bindingSchemaIdentity, and immutable BoundPropertyBindings. They are not a complete binding traversal or physical executor/cache. Current joins already use charged right-side hash indexes over shared binding keys; sorts use bounded owning rows and evaluated keys.

The new boundary is after logical optimization and before execution instances. It consumes the query-owned QueryPlan.root into sealed query-local physical descriptors. Frontend and logical semantics remain; transfer the existing root instead of cloning ASTs per task. Const-root callers must continue to keep ASTs immutable throughout execution. Descriptors expose const views and their expression owner outlives tasks. Mutable execution state never enters logical caching.

## 2. Concrete internal interfaces and cache policy

These are target data contracts, not claims that a complete plan container already exists. Production integration belongs to S2. They are in-memory representations, without persistent ABI/endian/version changes.

| Descriptor/field | Decision | Basis and constraints |
|---|---|---|
| Program/slot/scope | uint64 program, uint32 slot/scope ordinal, nonreuse and checked exhaustion | BoundSlotId/BoundScopeId/registry; never object addresses or EntityIds |
| Operator table | Charged contiguous table; uint32 operator/child/input/output ordinals referencing program slots/layouts; separate root ordinal | Validate edges/ranges/acyclicity/arity; seal read-only; no cursors/rows/parameter values |
| Expression program | Bounded instructions, input/output slots, original semantic-expression identity, typed/dynamic kernel and lazy/short-circuit flags | Reuse evaluator/accumulator rules; unresolved references retain deferred errors |
| Property descriptor | BoundPropertyId uint32 index/uint64 program, source slot, owning key, optional uint32 token | Implemented BoundPropertyBindings; missing token means name access, not NULL/absence; no permanent type inference |
| Access/algorithm | Scan/index/logical-index identity, read shape, join/aggregate/sort algorithm and limit source | Resolve roots/definitions in the execution snapshot; no cached page address; unverifiable indexes use explicit bounded scans |
| Pipeline | uint32 ordinal, operator range/dependencies, streaming/blocking boundaries, input/output layouts | Single-task non-JIT first; row adapters are declared nodes |
| Parameters | Plan names/ordinals/type guards; query owner holds actual values | Validate before specialization; logical cache holds no parameter value/result |
| Schema/binding | Database/format, six index-root-directory/page-directory tokens, overlay view/revision and capabilities | Implemented conservative observation; data COW can invalidate; not an independent persistent schema epoch |

**Initial physical-plan caching is disabled.** Logical caching retains IR only; each execution binds tokens/access paths against its actual snapshot. Future physical caching requires database/reader domains, semantic profile, layouts, capabilities, parameter specialization, and complete invalidation tests. Independent schema epochs require explicit atomic catalog publication/recovery; logical-root lifetime generation and graph fences are not relabeled as epochs. This closes first-version stale-token correctness, without claiming cache gains.

## 3. Bindings, types, errors

Bind names once. WITH exports a new visible layout, isolated subqueries import declared correlation slots, and shadowing preserves parents. Projected `n.rank` columns win over property interpretation, as in BoundProjectionReferences. Unresolved references become dynamic instructions: empty input does not acquire new row-dependent errors. Corruption, invalid descriptors, and binding budget failures propagate rather than masquerading as optimization unavailability.

Resolve tokens under authenticated query observation. Overlay changes rebind; equal revisions in distinct views differ. Unsupported readers use declared charged name/evaluator adapters without invented schema identities. EntityRef kind/ID belong to the data snapshot; schema observations do not replace data/fence identity.

Fixed kernels initially support int64/double/bool. Literals or validated parameters can specialize. Generic properties remain dynamic until the actual whole batch is validated before kernel effects. Mixed values and strings/lists/maps/temporal/bytes use existing semantics; previous batches do not determine later types. Preserve missing versus NULL through presence/input boundaries. AND/OR/CASE use active selection and do not eagerly evaluate failing inactive branches. Integer SUM checks before updating; NaN/infinity/comparison and floating input order reuse existing rules. Empty ungrouped COUNT is 0; existing SUM/AVG are NULL, not another engine's convention.

## 4. Batch envelope and dynamic owners

First representation: **flat**. Header fields are program/layout, query data-snapshot owner, uint64 batch owner/incarnation, uint32 physical row/column count. Cardinality meets configuration, uint32 limits, and available bytes; benchmark sizes are not constants. Selection is the existing uint64 bitmap. Duplicates retain distinct positions; filtering preserves order. Append/reserve/truncate/clear/release end borrowed views; incarnation checks prevent cross-mutation/owner reuse.

Reuse FixedWidthColumn/FixedWidthBatch with int64, double, uint8 bool and uint8 missing/NULL/value state. Charge actual capacity/allocation overlap and publish rectangular rows only after every column succeeds. EntityRef remains kind/ID, not an integer; query snapshot owner/pin interprets it without page borrows.

Target dynamic ownership is a query-budget PMR arena: byte buffer plus value/child-entry tables, uint8 PropertyType tag and independent cell state, uint64 offsets/lengths/child ranges. Strings/bytes differ; lists and map/functions preserve current public rules; temporal types remain typed. Check arithmetic, depth/elements/bytes, owner/incarnation before dereference. No cross-budget moves or cross-reuse views. Results own data after query release.

The complete arena is a S2/A6 target. Existing PropertyValue/rows/dynamic-column adapters remain explicit. Account for source, conversion, evaluator temporaries, selection, and results. Vector capacity is not logical payload; encoded-byte admission is not allocator accounting. Unsupported arena types use declared legacy value adapters under the same query limits. Decoder/nested/fixed-parent preallocation accounting must pass before S2/S5 resource acceptance; this design does not claim complete budget implementation.


The machine-readable contract is `design-profile.json`. Dynamic descriptors use inline bits for bool/int64/double/date/datetime with checked conversion/bit_cast, typed PMR duration arrays preserving three int64 components, and bounded child/entry tables for lists/maps. These are in-memory targets; UNKNOWN/COMPOSITE are not ordinary dynamic query values.

## 5. Algorithms and blocking boundaries

| Operator | First-version decision | Adoption gate |
|---|---|---|
| Scan/filter/project | Shaped/authenticated-index access, flat columns/selection, homogeneous kernels plus existing dynamic semantics, compiled bindings | Native layouts replace row inputs; complete entities materialize only on explicit demand |
| Aggregate/DISTINCT | Existing typed/dynamic accumulators, complete visible-tuple keys, charged hash groups, final takeResult | No per-column deduplication; input/groups/results coexist in budget; parallel floating merge unapproved |
| Join/OPTIONAL | Existing bounded shared-binding-key build/probe and precise candidate compatibility; explicit Cartesian without shared bindings; NULL extension on unmatched inputs | Column/reference candidates preserve keys/collisions/multiplicity/correlation |
| Sort/Top-K/skip/limit | Existing comparison/order, bounded blocking row/key ownership; Top-K only for proven equivalent order+limit | Respect group/repeat boundaries; unsupported spill or budget exhaustion fails, never truncates |
| Unwind/procedure/subquery/path | Flat first, explicit output layouts/imports, original duplicates/calls/ordered paths | Keep declared charged adapters per unmigrated consumer; paths are not sets/endpoints |
| Mutation reads | Same bindings/expressions/data snapshot; A2 patch/result preflight | Conflict replans whole statement, never post-publication replay; durability/INDETERMINATE unchanged |

## 6. Grouped/factorized extension

Envelope layout discrimination reserves flat/grouped contracts, but initially only flat producers/consumers are enabled. A grouped owner contains parent columns, uint64 offsets of length P+1 (monotonic, final offset equals child cardinality), child columns, correlation dependencies/ownership, query/operator/group identity, and positive uint64 multiplicity. Group IDs are not EntityIds. Every child belongs to a precise parent/correlation group. Checked arithmetic and logical-work admission bound expansion/weights; physical cardinality cannot bypass work quotas.

Share parents only inside proven correlation groups; matching names do not establish independence. OPTIONAL empty ranges are not NULL cells: emit one NULL extension retaining parent multiplicity. Paths preserve ordered node/edge sequences and repeated identity. Subqueries import the exact parent group. Writes, volatile functions, observable calls, and short circuits retain original execution counts.

| Grouped consumer candidate | Initial decision/experimental gate |
|---|---|
| Parent-only filter/project | Only verified pure deterministic expressions can evaluate per group; child fields use their child position; otherwise expand |
| COUNT/SUM/AVG | COUNT weighting needs equivalent logical-work/cancel/overflow/NULL rules; SUM/AVG initially repeat in original order without floating weighting/reassociation |
| DISTINCT/group-by | Complete visible tuple keys; initially expand, never independent column deduplication |
| Join/optional/subquery | Validate dependencies/NULL/call counts first; initially flat; not every Cartesian factorizes |
| Path/sort/skip/limit/output | Initially bounded charged ordered expansion with owning output, not borrowed offsets/pointers |

S2/O6 prototypes compare parent fanout, correlated/truly independent branches, multihop/supernodes, empty/NULL children, duplicates/self/parallel edges, paths/ORDER/LIMIT/subqueries, and wide parents. Measure metadata/copy/expansion/allocation/logical work and validate complete value/order/multiplicity oracles. Performance stays in the final window. Failed target/protection/resource gates retain flat with rejection/revision records; factorization is not forced or claimed, and scan gaps without joins do not prove its benefits.

## 7. State/lifetime

| Owner | State | Lifetime/first-version restriction |
|---|---|---|
| Immutable query program | Owned IR, sealed layouts/instructions, algorithms/pipelines/output names | No mutable task; release after tasks; physical cache off |
| Query shared | Snapshot/pin, context/cancel, common budget, charged cache/results, global work/first error | Tasks/batches/blocking/result temporaries die before shared/budget; current shared containers are not thread-safe |
| Task local | Cursors, batches/selection/arena, correlated input, accumulators/stats | RAII restores scopes; no shared writable cursor/row borrow |
| Blocking operator shared | Build/groups/sort/keys, explicit merge/finalize | Charged/bounded; merge/release partials before output, never place task aggregation in plan |
| Public output | Owning QueryResult/ABI values/entities/paths | No batch/page borrow; public callbacks forbid observable read-ahead; preserve current final-result boundary |

Single-threaded first. O7 coordinates global memory/work, cache access, first-error/cancel, snapshot readers, checked stats, stable output/floating order before multiple tasks. Do not clone full budgets per task. Compile/input/snapshot/type/cancel errors propagate; consumed aggregates/public callbacks are not replayed. Binding conflict explicitly requires rebinding. Plans/batches are in-memory, without new WAL/recovery/retirement. Existing storage handles pin reclamation; A6 owns file-internal spill, not external logs.

## 8. Explicit adapters/removal

| Adapter | Responsibility | Removal condition |
|---|---|---|
| BoundProjectionReferences row → slot | Compile names once; scoped row borrows | Same native sealed layout, projected-column/unknown-reference regressions |
| Dynamic columns → primitive buffers | Shape/type validation, optional buffering before effects | Owning native columns/typed consumers with source/conversion accounting/guards |
| LogicalPlanReadRow/evaluateExpression | Unified complex/dynamic/unmigrated semantics | Complete AST/operator binding/accessors/errors/budget/cancel per path; one fast path is insufficient |
| Legacy reader/name access | Correct unsupported-schema/token fallback | Authenticated equivalent provider adoption; no invented tokens |
| QueryResult/ABI materialization | Protected owning output | Final boundary remains; optimize inputs without ABI changes |

Plans declare layouts/owners. Adapters are not a permanent second semantics engine. Unclosed accounting/guards cannot pass S2/S5 acceptance; missing operators stay incomplete.

## 9. Verification/hypotheses/falsification

Evidence includes 103 property/schema/overlay tests and rebuilt fixed-column/batch/slot/projection/numeric/predicate/selection/budget/nested-execution modules. acceptance-en.md/results specify counts. These prove existing components, not an unimplemented general compiler/arena/grouped executor. Remaining required tests cover full scopes/parameter changes, database/DDL/reopen/overlay, rectangular/selection/incarnation, dynamic nesting/entities/paths, every migrated operator, empty/NULL/duplicate/ORDER, budget/cancel/early-stop, task/merge isolation, and owning output.

Hypothesis: slots, once-resolved properties and typed batches reduce hashes/RTTI/boxing. Costs are compilation/key metadata, source+conversion overlap and adapters. Falsifiers include small-query overhead, wide copies, mixed/budget conversion, grouping/expansion dominance, uncharged fallback, or changed floating/errors/duplicates. Test duration is not speed evidence. Final matched control/current/Kuzu uses optimization.mdx section 6: primary P50 target at least 10%, protection P50/P95 stable regression no greater than 5% outside control noise, no raised resource limits, non-equivalent/unsupported Kuzu cases N/A. No overall Kuzu win is promised.

This closes S1.1 design decisions only. S2 adoption, A6 complete resources, O7 parallelism and final performance remain unfinished. Formal optimization.mdx retains historical text; acceptance records/JSON are authoritative.
