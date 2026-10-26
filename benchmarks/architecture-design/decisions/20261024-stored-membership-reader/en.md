# Stored membership reader checkpoint

Accepted progress remains 15%; no commit or push. No query benchmark or full unit suite ran.

An internal storage stream reads the descriptor-bound FixedBTreeMap, native field pages and membership slots directly. It borrows one fixed record of at most 64 IDs under a 1 MiB account. File/pin ownership, duplicate directory ownership, ordering, page/slot/record identity and CRC checks remain. Full exhaustion checks descriptor totals; explicit prefix stops remain distinct. Cancellation or a released pin prevents further delivery.

This authenticates stored routing and bytes only. It grants no primary-liveness/label replacement proof or completion capability. BUILDING images may be inspected internally. Public query integration, completed-root proofs, ongoing maintenance and page packing remain unfinished.

Six isolated-profile cases and one default refusal case passed: sparse/propertyless/unlabeled nodes, UINT64_MAX, explicit stops, cancellation/refunds, foreign/released pins, admission before reads, and valid-CRC incorrect descriptor totals. Exact compile/link recipes and the archive hash checked around linking are preserved. This independent reader checkpoint does not accept the ongoing metadata reclamation or 13/39 compatibility work.
