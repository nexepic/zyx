# A2 field staging and interface refinement

Date: 2026-10-14. Status: native explicit transaction field staging is implemented; S3.1 acceptance is in `acceptance-en.md`. This record refines the carrier choice in section 2.1 of 20261012-a2-design-contract while preserving its field semantics and migration criteria.

## 1. Carrier refinement

The initial design proposed raw patch and entry-bound fields in `LogicalGraphMutationBatch`, enabled after every consumer migrated. Native publication already uses a separate combined patch entry point; legacy batch consumers still represent complete final states. Adding fields they could ignore would require every legacy call site to support or reject them and widen the risk of silently losing operations.

The complete batch retains its type and semantics. A separate owning `stageMutationBatchWithPropertyPatches(companion, patches, bound, control)` entry point transports raw staging. `LogicalGraphTransaction::supportsPropertyPatchStaging()` defaults to false, and the default raw entry point throws instead of ignoring operations. The native transaction enables support after migrating preview/overlay, shape checks, creation references, last-operation merging, full replacement interactions, commit, and failure cleanup. Legacy transactions and test backends retain the verified complete-collection adapter. There is one Cypher semantics path, no new codec, and no new Driver ABI function.

This refinement replaces the proposed batch fields and `supportsLogicalGraphPropertyPatchBatch()` name without reducing source-fence, positive-bound, identity, cancellation, atomic derived-state, or legacy-backend regression requirements. Unsupported consumers never receive hidden raw fields inside a complete-state batch.

## 2. Statement visibility and durable staging

The adapter's normalization is extracted into `normalizeLogicalGraphPropertyPatchBatch`. It validates every request before reading through the source snapshot/fence and shaped admission. It returns a complete visible batch, actual entry work, and changed persisted patches. Equal SET values and absent REMOVE keys do not enter durable patches. In mixed updates, equal payloads move from the source into the visible collection rather than being copied from input again. Creation-ordinal changes fold into creation properties.

The overlay previews and applies the same complete visible batch to preserve statement read-your-writes. For capable transactions it passes only companion mutations and owning changed fields to raw staging. The owner-local `StatementUndo` covers all changes and failure recovery. Exceptions do not advance the overlay revision, and preview/stage creation IDs must agree.

This remains a visibility adapter for the current layout: the overlay owns complete entities and source reads/undo can process all fields of a touched owner. The change removes full-collection copies from raw property accumulation and the base/final diff reconstruction before raw commit. It does not eliminate every full read, candidate copy, or physical write amplification.

## 3. Transaction final state

Native staging validates full references, unique keys, non-NULL SET values, deletion/full-replacement conflicts, positive bounds, and overflow before building candidate accumulated state. Each persisted field retains only its last SET or REMOVE, distinguishing equal numeric Node/Edge IDs. A later full map clears earlier raw operations for that target. A patch after a full replacement updates that complete candidate and preserves unrelated explicitly stored NULLs. Creation ordinals and later provisional IDs rebase and fold into creation payloads.

Cancellation checks cover validation, field movement/merging, and the point before candidate publication. Failure leaves original fields, issued IDs, lease ranges, and work bounds unchanged; the overlay restores its statement state. Complete and field mutations share one final transaction publication. A raw-only transaction counts as existing writes and cannot mix with bounded ingest. Read-only staging rejects before mutation.

At commit, complete replacements retain their base/final adapter, while accumulated raw fields enter the single native patch publication directly. Explicit deletion and later DETACH discard earlier property changes. A changed graph fence causes conflict and replanning, including when fields return to original values. NativeGraphMutationExecutor retains token merging, unchanged root/index skipping, constraints, vectors, statistics, and the single COW publication protocol. Temporary fields added and removed before commit do not create catalog tokens. A final sync error that may follow publication remains INDETERMINATE; reopen determines the complete old/new state.

## 4. Work bounds and remaining resources

Normalization records actual source-field and original-operation work per statement. The transaction sums admitted statement bounds as a conservative upper bound for final native merging; native reads recheck actual sources and final operations. Direct raw calls require a positive bound. Creation/full-candidate sources are admitted before folding, and overflow rejects only the new statement. Historical work allowance can exceed surviving final fields. It is neither allocation bytes, a schema bound, nor a performance result.

Encoded-source-byte admission and synchronous owning read controls remain active. Query-wide allocator accounting for container capacity, overlays, candidates, and write-transaction state belongs to A6 and remains unfinished, as does S3.5 resource/space/performance acceptance. Entry counts do not substitute for those resources. Linux module validation does not establish cross-platform builds.

## 5. Validation and evidence

All 141 selected native property/transaction/index tests, 72 overlay tests, 293 mutation/Cypher-transaction/legacy-property-layout tests, and one separately selected raw/ingest/read-only mode test passed: 507 distinct tests. The wide-property regression was additionally rebuilt and rerun with reopen integrity verification. No full suite or benchmark ran.

New evidence includes 32 statements, equal numeric Node/Edge IDs, and unchanged 16 KiB fields. Maximum retained complete property targets were zero, raw targets two, and raw entries two. Statement values, pinned old values, reopen values, and integrity remained correct. These counters measure retained field scope, not throughput or RSS improvements. SET→REMOVE→SET/type changes, full-map resets, stored NULL, creation/provisional references, invalid requests/source bounds/overflow, concurrent conflicts, and unchanged roots after restoration passed.

For three real transaction states (prior raw fields, a prior complete map, and a transaction-created entity), the test observes actual checkpoints and injects cancellation at each one. Overlay fences/values and prior transaction state remain intact; commit retains only the earlier statement's value. Existing real Cypher, scalar/vector/constraint, final-sync, old LargeValue/pin, deletion, and reopen regressions also pass.

Nine new tests initially used the fixture's default read-only open mode and correctly failed at writer entry. After checking the real option default, only those test constructors were corrected. Production read-only behavior was unchanged, and final regressions were rebuilt successfully.

Raw build/test logs, module link commands, source/binary hashes, and acceptance metadata are in `benchmarks/architecture-design/results/20261014-a2-field-staging/`. Formal optimization.mdx remains unchanged under the existing agreement; its historical table is not the current acceptance state.
