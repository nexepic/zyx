# A2 explicit transaction final state to native property patches

Date: 2026-10-13. Native final publication is connected; 126 selected module tests pass. S3.1 remains in progress.

## Contract

The existing overlay and complete collection staging preserve statement-by-statement read-your-writes. Final canonicalization retains created-reference remapping, ID leases, cancelled creations and last-replacement semantics. Before commit, `extractFinalPropertyPatches` compares each persisted entity's authoritative final collection with the transaction's own pinned base snapshot and derives set/remove operations for the native token consumer. Caller hints are not authoritative; old deltas are never replayed at a newer generation.

Complete map replacement still removes omitted fields. A replacement containing stored NULL retains the original path, since stored NULL and patch REMOVE are different operations. An unchanged complete replacement also retains its original path so an intervening writer still causes a graph fence conflict. Patch source/operation work uses checked cumulative entry bounds. An early fence check precedes comparison; the original authoritative check remains inside the publication lock.

This adapter still reads complete base properties and allocates ordering descriptors and temporary deltas. Statement staging has not migrated to native field requests. The tradeoff between these costs and reduced name resolution/copying awaits the final performance comparison. This does not complete A6 budgeting, physical local updates or the entire S3.1 migration.

## Atomicity and recovery fixes

- An ordinary transaction reports `LGTTS_INDETERMINATE` when the backend detects uncertain publication. It no longer labels a potentially visible generation as rolled back. Known pre-publication failures and fence conflicts retain their respective rollback/conflict states.
- When an earlier persisted Edge property update is followed by DETACH DELETE of its endpoint, final canonicalization drops that implicitly deleted Edge's property update. The native writer still authenticates topology and enforces detach bounds.

## Verification

Only the production core and six relevant test objects were built and linked into a module runner. Four GoogleTest suites ran 126 tests with zero failures. Coverage includes multi-statement Node/Edge updates, a 16 KiB value, overlay visibility, pinned old snapshots, real explicit Cypher transactions and reopening; full map replacement, stored NULL and unchanged-request conflicts; creation remapping/cancellation and statement failure isolation; scalar/composite/Edge index deltas, unchanged roots, constraints and vector deltas.

A final Superblock sync fault during property publication verifies an indeterminate transaction, rejection of further writes on that handle, complete old/new property pairs after reopening, and integrity validation. Existing ordinary write short-write, disk-full and final-sync recovery cases also pass. Streamed bulk mode was excluded because its separate commit branch was not changed.

One failed build was followed by an accidental run of the previous runner; that result is excluded. After the pointer-access fix and fail-fast script correction, the runner was rebuilt/relinked. The final log and verification JSON explicitly confirm the new Cypher, DETACH, NULL and indeterminate tests.

Verification used the current Linux x86_64/C++20 cloud environment. It does not establish macOS/Windows builds, actual process-crash or hardware-fault coverage. No full test suite or benchmark was run; module runtime is not performance evidence.

## Evidence and remaining work

`benchmarks/architecture-design/results/20261013-a2-transaction-final-patches/` contains commands, build/test/link logs, source/core/runner SHA-256 and verification/progress JSON. The formal optimization.mdx remains unchanged.

Field-request staging/consumer migration and full resource/fault acceptance remain open. S0 is 25%, S1 is 14.3%, S2–S6 are 0%, and total acceptance remains 2/34 (5.9%). The latest user rule permits commit/push when acceptance progress increases. No acceptance item was added here, so no commit or push was performed.
