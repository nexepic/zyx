# A2 statement rollback saves modified owners

Date: 2026-10-14. Status: implementation and selected module verification complete; S3.1 remains in progress and no acceptance item is added.

The explicit transaction overlay previously copied all of `nodes_` and `edges_` before each nonempty statement. Updating one property or label copied the properties of every staged owner, including unrelated wide values. Failure recovery copied the entire overlay again.

Each statement now owns a local `StatementUndo`. Before its first modification to a Node or Edge, it saves the original state; an absent owner receives an empty marker. Repeated modifications save the state at statement entry only once. Separate maps distinguish Node and Edge IDs with the same numeric value. Creation, property and label replacement, explicit deletion, and DETACH of persisted or transaction-created relationships all follow this protocol. Backup allocation fails before the corresponding modification; earlier changes are restored by the same undo.

Rollback swaps saved states into existing map entries and erases entries created or first materialized by the statement. It does not allocate or copy property payloads. Production code checks `is_nothrow_swappable` for both state types. Successful statements still preview/apply, stage through the transaction, and then advance the overlay revision; failures leave the revision unchanged. Transaction publication, file formats, and Driver ABI are unchanged.

All 72 selected overlay tests and 127 selected native property, transaction, and index tests passed: 199 tests, zero failures. Five new regressions cover first-state recovery for repeated targets, equal IDs across entity kinds, failed staging and identity retry, and recovery after DETACH bound or staging failures. With 32 owners holding 16 KiB values, eight statements modifying one Node each report maximum undo counts of one Node and zero Edges; unrelated Node/Edge values remain correct. These counters measure backup targets, not RSS, allocation bytes, or throughput gains.

The first module run had one incorrect exception expectation in a new test. The in-memory fixture reports a missing relationship through `map::at`, which throws `out_of_range`. After checking that source path, the expectation was corrected and all 72 tests rebuilt and rerun successfully. Production error semantics were not changed.

Evidence: `benchmarks/architecture-design/results/20261014-a2-statement-owner-undo/`, including source hashes, raw build/test logs, module link commands, and verification metadata. Only relevant modules were tested. No full suite or performance benchmark ran; macOS/Windows builds were not verified.

Limits: touched owners still retain complete entity backups. Field-level property staging, the accumulated transaction candidate copy, the base/final commit adapter, and shared resource accounting remain unfinished. This change does not accept S3.1 or S3.5. Overall acceptance remains 3/34 (8.8%); formal optimization.mdx is unchanged. Under the existing agreement to commit and push when acceptance increases, this change remains in the working tree.
