# A5 isolation and conflict contract

Date: 2026-10-14. Scope: S1.5 design delivery. Fine-grained production validation, S5.1/S5.2, and performance acceptance remain separate gates.

## Supported histories

The native logical graph contract is pinned snapshot reads with conservative optimistic write validation and atomic durable publication. Ordinary transactions observe their staged overlay. Read-only transactions retain the original membership and values across later commits. Publication is serialized within one writable backend. This contract does not advertise serializable isolation for every API, procedure, schema operation, independent writable handle, or process. The bulk-session file lease is not evidence that ordinary publication gates are shared across handles. Applications must use one writable database owner; cross-owner publication requires a separate ownership protocol and tests.

Several ordinary writers may stage against the same snapshot. A graph-changing commit invalidates another writer's graph fence, including writers changing distinct entities. A rejected transaction is terminal: start a new transaction and repeat its reads before retrying. Read-only completion is allowed despite intervening commits. Staged changes are invisible to other snapshots. Dirty reads, lost updates, dangling edges, and publishing both sides of the demonstrated write-skew history are forbidden. Streamed bulk append does not provide ordinary overlay read-your-writes; its bounded ID/scratch reservation and publication remain on their existing path.

## Current validation and costs

`makeLogicalGraphMutationFence` authenticates database/format plus seven root triples (physical frame, slot, generation): Node, Edge, property, large-value manifest, vector location, vector manifest, and state directories. Comparison is constant-size: 21 root words plus identity. Allocator-only metadata publications need not invalidate this fence. Index-root and page directories are not themselves fence members. Therefore future stable-page property COW must also advance a semantic property root or the included state directory; merely changing page-directory mappings is insufficient.

Property source validation precedes final planning; the mutation executor revalidates the expected fence at publication and validates constraints against the applicable current catalog. Concurrent DDL must not be inferred to conflict solely from the graph fence. There is no independent schema epoch today; conservative binding identities and current constraint validation remain required. Constraint failures and logical conflicts are distinct outcomes. Candidate planning and IO costs remain proportional to affected pages, values, indexes, and adjacency; the constant-size fence does not make commit constant-time.

## Target validation keys

The first finer protocol uses entity versions, not independent field versions. Record read and write dependencies for `(kind, logical entity ID, lifecycle/property/label version)`, endpoint existence, unique-key membership, range/predicate/index semantic epochs, adjacency membership, and schema epoch. Different fields of one entity may still conflict. Missing reads carry an absence dependency; creation IDs are leased and never reused. An index root's unchanged generation is not a semantic change counter: leaf COW requires an explicit epoch. Unsupported predicates, untracked procedures, and incomplete dependency sets retain the whole-graph fence. No production fence relaxation is permitted until every path records dependencies or takes that fallback.

Validation occurs against latest state under the publication gate. Every writer validates reads as well as writes: entity write sets alone would admit write skew. Unique validation checks the latest constraint/index membership even after a transaction restart. Endpoint deletion and edge creation conflict through endpoint lifecycle dependencies. Range inserts invalidate predicate epochs, including empty-range observations. Schema changes require rebind or rejection; append-only token registration is not automatically semantic DDL. Version overflow fails before publication, and new persistent version fields require A7 compatibility/recovery review.

Dependency memory is charged to the query/write account before growth. For R entity dependencies, P predicate/schema dependencies, W writes, and C affected constraints, validation is O(R+P+W) authenticated lookups plus actual constraint work C; storage/tree depth and IO must be reported separately. Bounded dependency admission may conservatively fall back to the old fence, never discard a dependency and continue optimistically. Metrics distinguish logical conflicts, physical rebuilds, fallback causes, and retry exhaustion.

## Logical independence and shared physical pages

Two logically independent entities can occupy one COW page. A candidate built from an old page must never overwrite a newly published neighbor. After successful logical validation, rebuild affected shared pages, directory paths, and derived indexes from the latest pinned roots, applying only recorded write intents. Large-value references are resolved through the latest authenticated manifest; physical owner/generation checks remain mandatory. The first bounded implementation permits at most three candidate builds, then rejects if publication keeps advancing. It must revalidate dependencies at each publication attempt. Until that implementation is proven, conservatively reject stale candidates, as today. Read-modify-write values from a conflicting entity are never replayed over latest values.

## Retry, cancellation, and uncertain commits

Logical conflict releases staged owners/pins and abandons outstanding ID leases; gaps are allowed. Retry means a new complete transaction and new reads, not another `commit()` on the terminal object. The engine must not replay user queries, CSV callbacks, or procedures with external side effects automatically. Internal candidate rebuilds contain no such callbacks and retain cancellation/resource admission checks. Definite rollback permits a fresh transaction. An indeterminate durable write fences the backend for close/reopen recovery; inspect the recovered complete generation before deciding whether application-level retry is appropriate. Never acknowledge an uncertain commit twice. Private page retirement and lease cleanup obey existing snapshot pins and resource accounting.

## Two-transaction evidence and replacement gates

| Interleaving | Current observed result | Required finer protocol |
|---|---|---|
| Both read two on-call doctors, each disables a different doctor | First commits; second conflicts; pinned reader still sees both enabled | Validate both read dependencies; forbid both commits |
| Writer counts range; another inserts; first derives a summary | Old membership remains stable; derived write conflicts | Predicate epoch or whole-graph fallback |
| Edge staged; another transaction deletes endpoint | Deletion commits; edge conflicts; no dangling publication | Endpoint lifecycle validation |
| Two writers stage the same unique key | First commits; second conflicts; fresh duplicate retry fails constraint | Latest unique membership plus schema validation |
| Different entities share one property page; both update | First commits; stale second conflicts; fresh retry preserves both; reopen agrees | Latest-page rebuild or conservative rejection |

Five new native module tests implement these histories, including actual shared page-ID/different-slot assertions and integrity verification. Related transaction-validation and atomicity/fault modules are also selected for regression verification. Tests establish these histories for one backend on Linux; they do not prove every history or cross-platform/process concurrency. Before S5 adoption, add actual concurrent threads, absence/label/adjacency/DDL dependencies, bounded rebuild starvation, resource/cancellation, and failure-at-each-publication-step tests. Final O6/O7 comparisons stay in the final benchmark window.

Production update: S5.1 now admits the bounded blind-Node-property exception described in `../20261014-s5-bounded-blind-rebase/en.md`. Read-dependent/unsupported transactions retain this original fence contract. The historical five interleavings above remain read-dependent validation evidence.
