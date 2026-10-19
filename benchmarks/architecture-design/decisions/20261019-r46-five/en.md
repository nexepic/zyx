# R4/R6 bounded COW and query observation acceptance

Parent: 89f1254928947c8b6fd32cbd8fc2917d2b7f55e9. Scope was registered before measurement: R4.1, R4.2, R6.1, R6.2, R6.3. These are implementation/correctness/resource/observability deliveries, not latency admission. Existing COW and R1/R2 interfaces are ready; section 5 requires common resource/correctness contracts throughout implementation. R3's format prototype and R5's adjacency candidate are not substituted or counted. No new format, backend, cache, spill algorithm, durability policy or resource limit.

## R4.1 changed work and actual modification set

Existing per-owner staging and per-source-page COW coalescing remain authoritative. Changes:

- Existing-owner SET/no-op/REMOVE previously enumerated every eligible property pool before planning any root. Pool discovery is now lazy: existing-owner paths authenticate only their source and return before discovery. New-owner placement initializes the existing batch-local pool vector at most once in PropertyCollectionRootStore; other allocator/planner fallback discovery remains separately observable. No persistent/shared cache is added.
- Move owning root plans into the staging vector and move unmerged/overflow/relocation plans into final owners. Move large-value mutation buffers when joining successful members. Preserve the first merged plan's required ownership and all authenticated source checks; do not mutate published pages or move transaction state before retry-sensitive preflight.
- Count root owners, cumulative provisional owned payload bytes, coalesced members, final replacement requests/payload bytes and actual pool-catalog enumerations. Payload bytes exclude the page owner header; replacement requests can retire an empty source without emitting data. These are not device writes or changed-field byte counts.

| Object | Existing-owner field update / current COW behavior |
|---|---|
| Entity record/map | Property-only update does not rewrite an unchanged fixed entity record |
| Property data | Rebuild current owner's canonical row/reference as existing semantics require; combine same-page changes into one replacement request. Whole large-collection encoding remains an R3 issue |
| Owner/page routing | Update actual owner slot/generation mappings and stable PageId physical mapping; preserve other owners |
| Derived indexes | Existing actual-value diff path remains; no-op/index/unique/type/removal tests protect it |
| Catalog/allocator/control | Change touched allocation/pool state and necessary shared paths; adding/growing pools may require bounded configuration/placement work |
| Metadata route/fence | Keep immutable branch reuse, graph semantic fence and conservative conflict checks |
| Retirement | Retire exact replaced/unreachable pages through current pin-aware protocol; never infer reusable space from file length |

Remaining global work is explicit: new-owner best-fit placement may enumerate current property pools and page candidates; its retained state is admitted by the existing WRITE account and finite storage/ID configuration, but work is not asserted proportional only to the new field. Existing metadata reachability traversal can occur at allocation/free-frame thresholds or explicit recovery/cleanup; it must authenticate current and pinned roots. PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES is 32×31 frames (roughly 4 MiB); the separate fewer-than-eight-pool deferral and free-frame floor still apply. Large-value owner/manifest scans and compaction/relocation use their existing maximum pages/locations/retirements/cycle budgets and continuations. No new unbounded deferred chain. Mature-file latency, sustained writes, complete maintenance backlog/oldest-pin/free/tail observations and space admission remain R4.3/R7.

Exact instrumented fixture: eight owner updates on one shared property page produce eight coalesced members, one 4032-byte replacement payload and no property-pool catalog enumeration; old pins still read rank 0, reopened current reads rank 1 plus unchanged body. No-op emits no property replacement request and performs no pool enumeration. This is mechanism/correctness evidence, not a speed ratio.

## R4.2 publication and maintenance protection

Data/control publication is unchanged: candidate data/metadata → DATA_FENCE → inactive A/B slot → SUPERBLOCK_FENCE → acknowledgment. Failed/partial candidate/control writes, failed sync and uncertain post-root acknowledgment retain existing reopen/recovery semantics. Diagnostic reservation-publication increments cannot throw after durable acknowledgment. Root recovery does not replace complete integrity checking.

Necessary native durability, shared-pool replacement/evacuation faults, row-chunk fault matrix, bounded metadata reclamation, large-value reachability/journal, long-pin file-pressure/reuse and tail-truncation modules pass. Tail tests distinguish logical release, physical relocation and truncation, including pre/post-truncate sync failure. Pinned generations remain reachable; quota/file-pressure refusal preserves prior roots and permits later reuse after release. Limits and publication fences were not relaxed. Existing maintenanceMetrics, metadata-pool stats and reclaim/truncation results remain their owners; a per-query PROFILE is not a complete database maintenance-status API.

## R6.1 ownership and resource contract

DirectPropertyColumns/BatchEnvelope remain query-owned and admitted. Borrowed decoded pages retain their allocation owner; lookup invalidation does not free a live view. Existing tests protect stale identity, duplicate ordinals, capacity pressure, cancellation, pinned views, result escape and admission/refund.

New observation uses a fixed metric registry and fixed samples, with a MaterializedByteBudget reservation covering the observation object and conservative owning report/map/string staging before execution. Dynamic column-name views allocate from that same budget. QueryResult admits its retained rows/columns/map before observation staging is destroyed, so there is no refund/re-admission gap. Some temporary admission is conservative; no cap is raised. The native parent QUERY account includes simultaneously retained execution, observation and output. Standalone backends without a parent still use explicit local metadata admission.

Native tests show results and physical-work maps survive backend close; dropping the result returns the same caller account to zero. Pre-execution metadata refusal cannot publish CREATE. An injected memory guard failure inside observation and a semantic error both restore thread state, refund the account and permit retry. Output admission errors after durable mutation retain the existing committed/uncertain-result semantics: do not replay side effects automatically.

Sort and DISTINCT/UNION retain current internal-file spill. Group-by/hash join still fail clearly on excess quota; no new general spill claim. Logical read-only scratch still requires existing internal permission and a writable file; physical read-only profile performs zero writes and preserves file/fence. Existing permission, cleanup and native spill tests pass.

## R6.2 public observation contract

PerfTrace::ScopedObserver is synchronous/thread-local and stack-scoped. It neither toggles nor resets global collection; nested scopes are inclusive and other threads are excluded. Receivers are noexcept and allocate no storage. Global page-pin ownership is unchanged; PROFILE does not enable the global unique-image set. Optional metrics never replace authoritative I/O or recovery checks.

PROFILE executes the inner plan exactly once and preserves normal data values and the legacy final summary row. Original column order is retained; summary fields are appended if not already present, so existing Driver ABI map getters can discover physical_work. Regular rows have NULL in these extra fields when accessed by column. No ABI function or value type changes. Test verifies scalar data, final map/count, cursor completion and owning lifetime through the existing ABI.

Summary: phase, total_time_ms, calls, actual_output_rows, access_path, physical_work, physical_work_scope, estimates_available, estimated_root_rows, direct_selected_fraction. The last row is metadata; actual_output_rows excludes it. Query-owned columns with these names remain present in original data rows; summary fields describe the metadata row.

| Metric family | Unit / aggregation / scope |
|---|---|
| direct_property rows/batches/rejected | Cumulative source batch rows/batches and source-selection rejection |
| aggregate.direct_filter_rejected / direct_input_rows | Attached literal-filter rejection / rows admitted to actual aggregate; distinct from source selection |
| direct_selected_fraction | (direct rows − source rejection − attached aggregate-filter rejection)/direct rows; NULL without direct input. Not arbitrary whole-query selectivity |
| consumer_adapter.*.calls | Actual bounded dispatch invocations, not rows or distinct plan nodes; nested/correlated calls repeat |
| batch.materialized_bytes | Cumulative supplied materialization, not a live peak |
| materialized_budget_peak_bytes | Maximum of observed individual local budget peaks, including observation metadata; not the sum of all query accounts, cache usage or process RSS |
| read metadata/cache/decode/copy | Existing cumulative visits/hits/owner decodes/fixed-struct bytes; visits are not unique pages |
| checksum.bytes | Cumulative checksum input bytes |
| buffer_pool.physical_read_bytes | Buffer-pool positioned-read bytes, not storage-device traffic |
| file_size_calls / completed_write_bytes,calls | Existing completed I/O scope, separately from logical work |
| property_plan_* / pool_catalog_scans | Provisional payload, coalescing and replacement-request work; see R4.1 |
| id_reservation_calls / id_lease_cache_hits | Reservation attempts / hits in existing acquired lease, not durable commits |
| id_reservation_publications | Successful acknowledged durable ID-lease publications, including direct and cached-acquisition miss paths |
| graph_commit_calls / maintenance_reclaim_calls | Ordinary graph commit attempts / explicit maintenance reclaim calls; background maintenance threads excluded |
| sync.data_fence / sync.superblock_fence | Completed fence counts across all publications in this query thread |
| *.ns | Inclusive cumulative nanoseconds for selected fences/publication; do not add nested timers to estimate wall time |

Missing metrics mean no recorded event or unsupported path, not proof of zero general workload. access_path reports observed direct cells/indexed candidates/bounded property rows/other; exact counters expose mixed/nested paths. PROFILE reuses existing bounded planning statistics for supported roots and marks unavailable estimates NULL, including unsupported aggregate roots and overlays. Estimates can precede the execution snapshot and are not correctness bounds on a concurrent result. EXPLAIN remains snapshot-free, preserves operator/details columns, and appends truthful candidate/adapter reasons to details; it does not pretend to know actual runtime access or scan the graph for estimates.

Example fixture: 64 direct rows, 0 source rejects, 32 attached-filter rejects, 32 aggregate input rows, direct_selected_fraction=0.5. CREATE observes one graph commit attempt plus one ID publication and two data/two superblock fences; internal publication cost is not hidden. Raw timing counters are correctness diagnostics only. Formal R7 timing must use ordinary queries with instrumentation disabled.

## R6.3 verification, responsibility and limits

Observation registry/report/explanation belongs to src/query/api/LogicalProfileObservation.hpp. PerfTrace owns only scoped delivery and existing global aggregation. MaterializedByteBudget owns local peak observation. PropertyCollectionRootStore owns batch placement/coalescing. No parallel implementation of executor, storage or resource coordination.

Raw first run: 166 cases, 163 passing, three failures. A source-selection assertion confused downstream filter rejection with source filtering; add and validate the actual aggregate rejection counter. ID counters initially missed acquisition and were corrected at the actual path. A test constructed the read-only QueryEngine overload; supply both interfaces, preserving its public constructor contract. No assertion deletion to hide production faults. Intermediate checks are retained. Final necessary run: 245/245; after final PROFILE column/report/ABI work, 99/99 affected checks. Their latest distinct union is 263 passing cases, not a single 263-case run.

Executed Linux x86_64/Clang 19. Portable C++20, prefixed existing enums, no new OS persistence primitive. macOS/Windows/WASM execution and full language-binding matrices were not performed and are not claimed. Only necessary modules ran; no full unit suite or fresh formal benchmarks. Existing DBs/frozen before binaries were not replaced. Local task binaries/logs remain under /workspace/zyx-r46-five-20261019; tests use private temporary files. Future R3/R5 production paths must satisfy the same ownership, format, failure and resource contracts and add affected tests before R7; this acceptance is not evidence for unimplemented paths.

All five registered foundational deliveries are accepted. Active total derives to 13/24 (54.2%); R1.3/R2.3 and R4.3/final performance gates remain pending. R3.2/R3.3/R5 work is unchanged.

The total_time_ms interval covers inner execution, excluding the added bounded statistics probe and summary construction. ABI metadata does not change normal non-PROFILE query columns.

PROFILE enables local value observation only, not duration-timer construction. This avoids newly enabling allocating timer-label strings outside the admitted report. Selected *.ns values are available when existing global timing is already enabled (or an explicit duration is delivered); otherwise they are omitted. Inner total_time_ms and completed fence counts remain available. Global diagnostic collection retains its existing ownership policy.
