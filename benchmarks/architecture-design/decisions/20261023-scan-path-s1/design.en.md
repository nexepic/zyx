# S1: complete batch scan contract and candidate decision

Plan `scan-path-20261023`. This is a design, not a production capability claim. S0's 16 semantic checks and frozen baseline are prerequisites. Only the following two candidates are allowed; measurement outcomes cannot change frozen selection rules.

## 1. Decision and exclusions

Retain LogicalGraphReadSnapshot, existing typed-column consumers, single-file COW publication and logical IDs. Add a complete storage scan-batch capability rather than keeping label-index output, entity validation and property supply as three unrelated per-entity interfaces.

A: a query-local authenticated batch view over ROW. Build entity/label and selected-property views together, retaining page pins or budget-owned fallback images. Supply locations, visibility and selected-field proofs for the same batch. First ROW access must really validate its source. No cross-query whole-database materialization, precomputed query answers or expanded caches to win warm measurements. A changes no persistent format and may fail the 5x gate.

B: persistent scan blocks with authoritative entity visibility and label membership, typed fields and general ROW fallback under one snapshot root. Field blocks alone are insufficient: directly enumerate live IDs and labels. Existing 13/39 groups lack complete scan-label proofs and are not already B. B requires a new experimental format and source specification, never a reinterpretation of 13/39 bytes.

Both reuse the same consumers, without new filter/aggregate engines. Do not introduce a persistent update layer: start with local block COW. If write guards fail, reject the candidate rather than automatically adding LSM or multi-layer merging.

## 2. Batch contract

Bind requests to snapshot, logical range and canonical label/property token sets. Scan batches have strictly increasing stable IDs without duplicates. General gather ordering/duplicate restoration stays in the existing gather path.

A batch contains at most 512 EntityRefs, visibility, a selection bitmap for authenticated labels, and property type/missing/NULL/data views. This reuses DEFAULT_PROPERTY_BATCH_ROWS. Byte pressure may shrink it; benchmark-specific enlargement is forbidden. Requested fields include all plan dependencies, not only WHERE predicates.

Authentication includes database/data-region identity, physical root, generation, semantic fence, schema/token identity and applicable I/O epoch, plus batch owner/incarnation. Borrowing lasts only within the producer callback; returned results own their data. Failure, refill or fallback invalidates previous batch handles.

Encode missing separately from explicit NULL. Preserve empty/embedded-NUL strings, mixed types, exact integers, NaN, negative zero, overflow, COUNT, DISTINCT and tuples through existing consumers.

Reuse only validated immutable sources. Check scope, slot generation, owner and complete offset/token directory before ROW selection is exposed. Mutable overlays retain fence checks; old proofs cannot survive transaction mutations. Persistent membership can replace Entity Map rereads only after its write/recovery invariants are implemented and tested.

Return supported/unsupported before the first output. Never restart another whole scan after emitting a prefix. Budget denial may fall back before output; failures after output terminate the query under existing transaction/result handling, never return a successful partial result.

## 3. Resources and complexity

At most 512 entities and 1 MiB aggregate temporary source workspace per batch, also limited by the caller's remaining query account. This is a sublimit, not a new allowance. Keep the 64 MiB raw pool and 128-entry/1 MiB metadata cache; no independent data cache. Output and aggregation retain original accounting.

A persistent block covers at most 512 consecutive logical IDs, not one label table. At most 64 typed fields and 64 block labels; excess, complex or uneconomic values use general fallback, never truncation. Total candidate encoding per block is at most 1 MiB. Large strings/collections need not be inline. Generic density, type stability and total encoded cost select fields, never names.

A still has first-access O(entities × actually validated directory width), while sharing page location, candidate/entity delivery, batch identity and consumer setup. Record residual costs without predicting 5x.

B targets O(visited blocks + pages) metadata location/validation; value processing remains O(values inspected/delivered). Label selection intersects membership and live bitmaps; typed fields bypass per-entity property roots. Count fallback in end-to-end results rather than reporting only adopted blocks.

Record actual account peaks, reads/writes, entity/property-root visits, blocks, fallback coverage, first reads and maintenance. sizeof totals are neither memory traffic nor real peaks.

## 4. B format and construction decision

Preserve default 5/31 and experimental 13/39 meanings. Before writing B, extend the storage specification with a new experimental version, every wire field/checksum scope, unknown-flag rejection, page owner, manifest addressing and compatibility rules, plus build/read rejection tests. This decision does not reserve an unchecked version number or authorize writing an unspecified format.

Block descriptions include ID range, live bitmap, label membership or authenticated references, property tokens/types/missing/NULL states, typed-data/general-fallback references and source version. Descriptions and field images participate in the same fence, integrity, old-pin and retirement mechanisms. Compression requires a whole-cost benefit including metadata; RAW remains valid.

Use explicit offline destination construction for a new experimental format. Read the source only, preserve logical IDs and graph semantics, write an independent destination, never overwrite the source or expose incomplete files. Frozen inputs may first be built as ROW through the existing loader and then converted by the real converter. No injected descriptors or benchmark-specific fields.

Stream bounded blocks into a clearly incomplete destination. Only after full integrity, whole-database space and resource admission may the complete target become openable. Failure removes the private target or retains it as rejected evidence; no visible prefix. The wire specification must define incomplete-state recovery/rejection fully.

Retain at least 10% whole-database steady saving. Count source, target and temporary files in conversion peak and the original 2 GiB registered aggregate construction-file budget; do not hide the retained source. Keep the original 8 MiB maintenance workspace and 1 MiB block-encoding workspace. Admission refusal is a workload result, not an excluded sample.

The existing 8×64-owner public converter keeps its limits. Repeated visible prefixes cannot masquerade as whole-target admission. B needs a full-target construction protocol for medium/holdout; this is required candidate conversion, not a general migration product. Ordinary production writes retain single-file transaction publication.

## 5. Updates, snapshots and maintenance

A has no durable state and releases views at query end. New reads after writes bind a new snapshot; old pins retain original sources.

B COWs affected blocks and atomically publishes fields, live/label bitmaps, primary records and directories. Type changes, low density or excessive size demote the field/affected block to ROW. Never leave readers dependent on stale blocks. Creation, deletion and label changes update the same authority.

Maximum persistent read merge depth is one: one published representation plus the existing transaction overlay, no persistent delta chain. Reorganization is explicit maintenance of at most one 512-ID block per operation, never implicit whole-database query work. Pins prevent reclamation; insufficient peak allowance refuses maintenance atomically.

Measure block-update amplification with S0 SET, batch-edge, deletion and full-entity guards. Read benefits cannot hide write regressions. S3/S4 must cover GC, LargeValue, relocation, recovery, cancellation and full integrity before production acceptance.

## 6. S2 execution and decision

Implement the shared batch contract and bounded production A path first; validate semantics, authentication, borrowing and failure boundaries with affected modules. Then implement B's minimum real durable construction/read chain, including reads after changes and reopen. Open the single registered comparison window only after both candidates pass necessary semantics, not after each function edit.

The window covers four primary workloads on both datasets, retaining non-adoption and failures. Use at least three processes, three warmups and ten measured samples; separately retain first-use/build/maintenance costs. Warm performance cannot replace first-use cost. S0's untimed oracles remain the result authority.

Each case must reach 5x and satisfy resource/space/semantic gates. If both pass, prefer lower maintenance/write cost, then end-to-end latency. Allow one correction only for a demonstrated implementation bug, not another candidate search. Failed selection leaves S2 unaccepted with residual-cost evidence; do not award its 20% early.

Only S1 design is delivered here. These capabilities are not implemented and no new performance benefit is claimed. B's complete wire specification is mandatory before implementation and cannot be skipped using this decision.
