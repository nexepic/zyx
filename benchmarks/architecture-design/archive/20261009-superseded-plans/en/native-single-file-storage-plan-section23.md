## 23. Framework-Level Read-Path Optimization and Implementation Gaps

This section records the framework-level gaps exposed by benchmark comparison and the implementation sequence that closes them. These are not
workload-specific tricks. They must not reintroduce Segment Chains, per-node adjacency roots, Edge linked lists, a second database file, or
fast paths that match a particular query text or workload. Storage-neutral fast paths based on reusable logical-operator shapes remain part of
the execution framework. Every change remains within the single-file format, A/B Superblock, generation-aware Catalog/Page Directory, shared
Buffer Pool, global key-only adjacency Packed B+Tree, and no-migration policy. A faster write path does not mean that the read path is complete;
the Stage 2, 4, 5, 6, and 8 production-read gates therefore remain open until this section is implemented and verified. Existing "native core"
statements mean correctness-core completion only.

The latest performance assessment, execution priorities, and prohibited regressions are maintained in the [Optimization Strategy](optimization#1-current-assessment-and-evidence), with the implementation order defined by its [staged execution plan](optimization#4-staged-execution-plan). This section remains authoritative for the single-file physical format and storage invariants.

### 23.1 Benchmark evidence and measurement contract

The current and historical architecture were compared on the same macOS arm64 host, AppleClang 17, Conan toolchain, Release configuration,
smoke dataset, and seed. The historical binary was rebuilt from a separate worktree whose commit is not a linear ancestor of the current branch,
so the result is a reference baseline, not a strict before/after claim. The `smoke` data set has 250 Nodes and 660 Edges. Load without extra
property indexes improves from 85.244 ms to 60.228 ms, about 29%, while load with property-index construction is effectively equal at 117.047 ms
versus 116.409 ms. Read paths regressed by tens to thousands of times for label/property scans, relationship scans, one- and two-hop expansion, shortest-path
chains, top-k sorting, and indexed point/equality/range lookups. Exact p50/p95/p99, RSS, database size, and logical/physical byte counts must be
stored in reproducible benchmark artifacts rather than encoded as format constants.

Profiles show repeated metadata-directory lookup, page-route lookup, Catalog and segment validation, page reads, payload copies, slotted-page
decoding, and complete property-root decoding on node scans. Property optional lookup accounts for about 88% to 98% of label filtering,
all-Node property filtering, aggregation, and Top-K execution. Relationship-property filters can issue an adjacency range from each scanned Node
before materializing complete Edges, while shortest path reads complete Edge and Node objects when it needs only topology. Complete current samples
do not yet exist for `small` and larger scales, so hot-cache `smoke` ratios do not establish scaling behavior. Every phase below must record structural
counters as well as wall-clock time.

### 23.2 Phased implementation

#### Phase 0: freeze observable contracts again

- Freeze scan, indexed, multihop, and write workloads across smoke, small, million-scale, and ten-million-scale corpora, with opened, warm, and
  cold-ish modes.
- Record p50/p95/p99, RSS, file size, logical/physical read and write bytes, and deterministic structural counters.
- Count Page Directory lookups, Catalog lookups, envelope/control validations, typed payload-copy bytes, B+Tree root descents, complete entity
  materializations, decoded property count, and dynamic allocations.
- Establish the current baseline first; an optimization is accepted only when it improves more than one workload class without changing results.

#### Phase 1: add `NativeReadSession`

Introduce a query-owned, snapshot-bound, normally single-threaded lock-free read session owning a `MetadataSnapshotPin`, bounded PageId-to-PageLocation and SegmentId-to-CatalogEntry caches,
validated envelope/control views, a snapshot-local token dictionary, and Directory/B+Tree cursor state. Caches are generation-scoped and bounded
by entries or bytes. Physical offsets remain inside storage and never cross into query, binding, or public ABI layers. A fixed 256-slot,
`id % slots` direct-mapped Entity cache guarded by a shared mutex does not complete this phase: it collides during sequential scans and caches the
wrong granularity. The generation-aware Buffer Pool continues to own cross-query and cross-thread sharing.

#### Phase 2: add zero-copy typed page views

Separate `PinnedTypedPageView`, `DecodedRecordView`, and `MutableOwnedPage`. A reader keeps a Buffer Pool pin and references bytes only after checksum,
owner, generation, and slot-generation validation. Mutation continues to use owning COW representations. A cache hit must not copy an entire page
payload or bypass integrity validation.

#### Phase 3: unify streaming Directory/B+Tree APIs

Provide `pointLookup(key)`, `multiLookup(sorted keys)`, and `openRangeCursor(lower, upper)` for Entity Map, Property Directory, indexes, and adjacency.
Each range cursor descends the root once, streams leaf/sibling pages, emits callbacks or spans instead of constructing unbounded vectors, and may use
bounded sibling prefetch. Cursors carry logical continuation only; physical PageIds never leak.

#### Phase 4: true Entity-page scans

Add a page-local EntityId array to Entity Pages so full scans enumerate live slots in page/bitmap order without an Entity Map lookup per entity.
Use an Entity Map range cursor only when ID order is required. Sort and group batch point reads by PageId, pin each distinct page once, then restore
caller order. Add a counter-based regression proving that the per-entity tree lookup pattern is gone.

#### Phase 5: selective, tiered Property storage

Use the hierarchy: no property means no directory entry; a small set uses a packed shared slotted record; a large set uses a shared overflow/per-key
tree; one oversized value uses `LargeValueRef`. Sort the Property-token offset directory and support binary search/selective decode. Full decode is
allowed only for `properties(n)`, `keys(n)`, or an explicitly complete result. Overflow records are keyed by
`(EntityKind, EntityId, PropertyToken)`. A size/pressure solver with hysteresis chooses representations; no fixed property-count assumption is allowed.
Small mutation batches use sorted `multiLookup`, never a full Property Directory mirror.

#### Phase 6: projection-aware delayed materialization

Add a query-text-independent `EntityReadShape` carrying entity identity, label/type token, requested Property tokens, and an `all Properties` flag.
Internal rows hold an `EntityRef` and selected-field cache by default instead of eagerly constructing full `LogicalGraphNode`/`LogicalGraphEdge` values.
Public result types may still materialize at the boundary. Execution carries token IDs and resolves names only at result or binding boundaries.

#### Phase 7: topology-first adjacency

Implement three cursor layers: `AdjacencyKeyCursor` returns EdgeIds, `EdgeTopologyBatchCursor` returns endpoint/type tokens, and
`EdgeMaterializationCursor` reads selected or full Properties only after a predicate passes and output requires it. Degree/count, relationship-type filters,
and shortest path must not touch unrelated Property pages; relationship-property predicates decode only requested tokens. Resolve EdgeIds in Entity-page
batches and pin each distinct page once. Do not add a type-qualified tree or per-node persistent delta unless measurement independently justifies it.

#### Phase 8: separate index candidates from entity reads

The index layer returns an `IndexCandidateBatch` containing EntityId, covered values, and root/snapshot identity. Runtime validates liveness, evaluates
covered predicates directly, and loads only missing requested fields; it must not reread a complete Node after an index hit. Full cross-store integrity
checks belong to an integrity scanner or paranoid mode, not every normal query.

#### Phase 9: restore the batch execution framework

Add typed column batches, selection vectors, and composable predicate, filter, project, and aggregate pipelines to the logical runtime. Query
compilation resolves label, relationship-type, and Property names to tokens once; execution does not construct a string-keyed `unordered_map` for
every row. Restore storage-neutral count, aggregate, and traversal fast paths based on logical-operator shapes. They must not match exact Cypher text
or refer to the retired physical format. Point lookup and scan may choose different strategies while sharing typed-page, Directory-resolution,
snapshot-validation, and Buffer Pool primitives.

#### Phase 10: scale writes and maintenance

Keep format semantics while adding target-page grouping, one COW candidate per page, sorted directory merges, incremental free-space indexes, and a
shared B+Tree WriteBatch for online and bulk paths. Do not add an adjacency-only write buffer. A general bounded write buffer may be considered only after
measured COW write amplification proves it necessary.

### 23.3 Structural acceptance gates

- Full Entity scans must not perform one independent Entity Map tree lookup per entity.
- Envelope/control validation must be reusable within one snapshot/segment.
- A Buffer Pool hit must not copy a complete page payload.
- A B+Tree range cursor descends the root once; adjacency count never reads Property pages.
- Shortest path never reads unrelated Properties; an index-candidate path never reads the same entity twice.
- Requesting one Property decodes one Property; a batch point lookup pins each distinct Entity Page at most once.
- A global Edge-property filter uses a sequential Edge scan or equivalent batch path, not one adjacency-root descent per Node.
- Scalar-only operations such as `count` do not construct complete per-row results; name-to-token resolution occurs at most once per query/session.
- Cache, cursor, batch, and materialization memory is bounded.
- Relocation, old snapshots, split/merge, reopen, and checksum/owner/generation corruption preserve results or fail closed.
- Each phase adds pin-lifetime, cursor-continuation, selective/full-equivalence, topology/full-Edge-equivalence, and deterministic-counter tests.
- Run final four-dimensional coverage only after the source set is stable. Coverage is a final gate, not the current blocker; do not inflate it with
  mocks, fabricated corruption, impossible plans, TTY/I/O failures, or scale workloads.

The production-read status for Stages 2, 4, 5, 6, and 8 can be closed only after these gates pass. Until then they remain correctness-core complete,
not production-read complete.

### 23.4 Current implementation snapshot (2026-09-24)

This snapshot resolves the distinction between the storage-core ledger and the
production-read optimization ledger. `Native core complete` means that the
native owner, format, durability/recovery behavior, and required correctness
evidence exist. It does not mean that every logical-plan, procedure, GDS,
overlay, index-verification, or maintenance consumer uses the optimized
read-shape and batch contract. The optimization document is the authoritative
stage ledger for that second question.

The numbered Stage 0-10 sections above remain the normative target and list
stage-specific release gates. Dated checkpoints inside those sections are audit
history; they must not be read as a new current status when they predate this
snapshot. When a dated paragraph appears to conflict with the table below, use
the table for the current implementation boundary, the capability matrix for
public-contract retirement, and the optimization document for production-read
consumer migration.

References in older paragraphs to format v24, v25, or other intermediate format
labels describe historical cleanup checkpoints, not formats accepted by the
current database. The current source constants are the format envelope v4 and
the independent Superblock codec v30. The current format rejects older versions
and does not provide migration or compatibility fallback. Likewise, dated full
suite, coverage, and benchmark results in the historical ledger do not validate
the current uncommitted worktree.

For the current read-optimization evidence, the earlier shape-aware
projection/adapter checkpoint is `15/15`; the named `LogicalGraphProjectionTest.*`
binary passed `14/14` after the one-pass projection staging change. The recorded
runtime/executor `306/306` and direct-edge `77/77` runs are historical focused
evidence from before the latest correlated-subquery complete-Entity change; the
direct-edge `78/78` result is also pre-Vector-change evidence. The last verified
`QueryEngineLogicalBackendBoundaryTest.*` boundary suite passed `57/57` before
the current Vector implementation, with `VectorProcedure*` at `8/8`. These
counts are not current-worktree evidence until the new implementation is rebuilt
and rerun. The focused `ShortestPath*` boundary group was last recorded at
`3/3`. `gds.shortestPath.dijkstra.stream` uses the same
sorted path-Node materialization helper as `algo.shortestPath`; it restores
logical path order and accounts for duplicate path positions under the byte
bound. None of these counts is a current full-suite result or a production-read
stage close condition.

Later dated audit paragraphs are retained as history. If they use “current
direct-edge 78/78” or Vector `8/8`, those words refer to the working tree
before the lazy-`EntityRef` Vector change. The snapshot above and the
“pending verification” Vector row are authoritative for the current
uncommitted tree.

| Area | Current status | Evidence boundary and next action |
| --- | --- | --- |
| Typed Entity scan | Implemented | Node/Edge typed Entity-directory cursors reuse the decoded Entity page and have focused relocation, reopen, corruption, early-stop, and obsolete-source cleanup coverage. Preserve this boundary while adding later prefetch or continuation work. |
| Shape-aware GDS projection scan | Partial | `LogicalGraphReadSnapshot` and the native page-backed adapters now expose a resumable Node scan that honors identity/label-only, selected-column, and complete-property shapes while preserving continuation, ID ordering, and metadata-page bounds. `LogicalGraphProjection::build()` requests labels only when a label filter exists and consumes the scan in one pass, retaining only selected logical IDs instead of a second full transient Node vector. The focused projection/adapter run passes 15/15 tests, and the projection suite passes 14/14 on the current binary. This independent checkpoint is not part of the recorded 306/306 result and does not close Stage 2/3. |
| Selected reads in native/topology paths | Partial | Source scans, topology targets, Edge batches, and native index candidates have selected-before-complete checkpoints. The shape-aware GDS projection scan is the newest native source-scan checkpoint. The remaining consumers still require an audit and structural counter evidence. |
| Logical runtime batch execution | Partial | Map Projection, alias-cache propagation for bare Entity projections, dynamic list literal, list comprehension, reduce, quantifier, `labels()`/`type()` metadata, outer-row pattern-comprehension Property access, pattern-local target/relationship bindings, and cross-row NamedPath payloads use shared bounded batch collection. The pattern-local path batches selected columns per candidate reference/topology adjacency page and restores source adjacency order. Complete-Entity requirements (`properties(entity)`, `keys(entity)`, and `entity{.*}`) now use one bounded prefetch for Filter, Project, Aggregate, and Sort while preserving safe selected-column siblings. The generic selector, complete-payload checkpoint, direct Filter contract, and direct JOIN/OPTIONAL MATCH payload tests are covered by the latest focused evidence; complex consumers, procedure paths, and the remaining lifecycle audit stay open. |
| Cross-row deferred EntityRef output materialization | Partial / verified checkpoint | `executeToQueryResult(...)` performs bounded sorted/deduplicated Node/Edge batch materialization and restores row order; QueryResult-only NamedPath processing also prefetches path references across UNION/JOIN/OPTIONAL MATCH rows. Public row visitors use one-row flushes to preserve strict early-stop. The historical 306/306 runtime/executor regression and the pre-Vector-change direct-edge 78/78 regression are focused evidence; the latter includes the read-only correlated-subquery complete-Entity change. The checkpoint also covers the deferred-EntityRef Sort-key and earlier imported-Property regressions. Complete Entity payloads outside the four core operators and other Stage 6 consumers still require audit. |
| Vector procedure hit materialization | Partial / pending verification | The current `LogicalPlanProcedureRuntime` implementation keeps hits as `EntityRef` rows through validation, filtering, projection, `SKIP`, and `LIMIT`; selected and complete Property caches are distinct; complete Nodes are read only for surviving final Node outputs in sorted bounded batches and restored to search-score order. Scalar-only and Property-only outputs should avoid complete Node reads. The previous `QueryEngineLogicalBackendBoundaryTest.VectorProcedure*` `8/8` result predates this implementation and must be rerun after compilation. Cross-row procedure Property-expression batching and the broader procedure lifecycle audit remain open. |
| Shortest-path procedure Node materialization | Partial / verified checkpoint | Native `algo.shortestPath` and `gds.shortestPath.dijkstra.stream` share the bounded sorted Node helper. It validates batch cardinality and logical IDs, restores algorithm path order, and charges duplicate path positions separately to the materialized-byte bound. `ShortestPath*` passes 3/3; this does not close general procedure Property-expression batching or the broader procedure/GDS lifecycle audit. |
| Snapshot/cache and write batching | Partial | Snapshot-local routing/page/session caches and selected write planning exist, but lifecycle, reclaim, fault, and generic write-batch gates remain open. |
| Final evaluation | Deferred | The latest recorded full macOS quick run (`native-20260923T211232-90355`) predates the newest uncommitted working-tree changes. The historical focused 306/306 run covers complete-Entity prefetch, the mixed-expression selector, cross-row NamedPath batching, the deferred-EntityRef Sort-key regression, and the earlier read-only correlated-subquery imported-Property regression; the pre-Vector-change direct-edge suite passed 78/78 after the complete-Entity subquery change. These are not full-suite results. Cross-platform no-profile Release comparison, scale matrix, and final four-dimensional coverage remain release gates. |

The shape-aware projection scan is an implementation checkpoint, not a new
completion percentage. Its default overload preserves the old complete-scan
behavior for generic adapters, while native adapters avoid opening labels or
Property roots that the caller did not request. The focused 15/15 run covers
selected/complete equivalence, label-filter semantics, missing-property
behavior, continuation, and metadata-page accounting; the capability and
production-read rows remain open for the broader consumer, scale, and lifecycle
gates.

The approximately **34%** progress figure belongs only to the production-read
optimization roadmap. It is not a storage-migration percentage, a correctness
percentage, a coverage percentage, or a speedup percentage. The latest recorded
diagnostic coverage was Region 94.33%, Function 99.11%, Line 95.55%, and Branch
89.13%; it is not a C7 close result. The focused Stage 6 pattern-local binding
work is complete for its candidate-page path; the historical combined runtime/executor
run passes 306/306 tests and its direct-edge suite passed 77/77. The current
direct-edge suite passes 78/78 (the preceding 305/305, 304/304, 298/298, 297/297
and 296/296 runs and older 229/229, 222/222, 220/220, 219/219 and 104/104 runs
remain historical evidence).
The current focused run includes the generic mixed-expression selector for Filter,
Project, Aggregate and Sort, plus complete-expression fallback coverage. The
cross-row NamedPath checkpoint also covers path references propagated through
UNION rows while preserving visitor early-stop.
The current-worktree full regression and
the remaining Stage 6 consumer audit are still open. This plan must not claim
that the target design is fully implemented or that the native architecture is
globally faster than the retired architecture.

The last verified checkpoint contains a cross-row deferred-`EntityRef`
output-materialization checkpoint in `LogicalPlanReadRuntime.cpp`. Its completion
callback runs while `executeOnSnapshot()` or `executeInternal()` still owns the
`ExecutionState`; the focused Node/Edge, duplicate-row, early-stop,
complete-Entity, mixed-expression, and cross-row NamedPath regressions pass
306/306, including the historical separate direct-edge 77/77 result; the current
direct-edge suite is 78/78. This
checkpoint is only a partial Stage 6 consumer boundary. Complete Entity payloads
in logical-plan JOIN/OPTIONAL MATCH/UNION, procedure, GDS, overlay, maintenance,
and full-regression gates remain open; it does not change the approximately 34%
roadmap estimate.
