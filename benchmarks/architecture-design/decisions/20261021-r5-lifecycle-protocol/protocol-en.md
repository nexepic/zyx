# R5.1 adjacency protocol and covering-tree lifecycle acceptance

This accepts the consistency protocol and complete lifecycle prototype, not production adoption or performance benefits. R5.2 production writer hooks/access admission and final covering-versus-shared-segment choice, R5.3 costs/planning/holdouts, and R7 comparable performance/Kuzu remain open. The denominator remains 24.

## Bytes and authority

One shared native Packed BTree replaces neither the canonical tree nor production routing in this prototype. Logical keys contain direction u8, big-endian NodeId u64 and EdgeId u64 (17 bytes). Logical values contain little-endian NeighborId and TypeToken u64 (16 bytes). FixedBTreeMap encodes each record of 33 bytes into key-only leaves (BLVL_EMPTY); full separator/framing/child/pool costs remain. Each Edge requires both directions, including self-loops. There are no per-node roots or unbounded delta chains.

The low-level coordinator requires an existing caller-owned laboratory IndexId in the low 62 bits. Bootstrap uses final master records and a bounded bulk tree in the same NativeGraphWriteTransaction. Delta finalization derives both directions from private final Edge states, ignoring create-then-delete transitions. Success seals further staging; failure remains pending and cannot commit main-only state. Move/retry preserve index/type-planner/catalog identities.

Master Edge records remain authoritative. Complete private pages and lookahead validate identity/endpoints/type/order before filtering or callbacks.64 records is a page bound, not a database cap. A coordinator operation permits at most 64 changed Nodes/Edges, excluding endpoint read-cache entries. Node-only create/relabel/delete updates known statistics without rewriting candidate pages while authenticating its existing root/first-leaf shape. Other label/property indexes remain caller-staged through existing schema planners in the same transaction; this low-level API is not an automatic production hook. Provided collaborators must share its metadata domain.

## Detection matrix

| Failure | Candidate reads | Controlled commit | Explicit integrity |
|---|---|---|---|
| Valid CRC, wrong endpoint/type | Entire visited page/lookahead compared to master before output | Affected old directions must exactly match master | Both directions and reverse master lookup refuse |
| Missing forward/reverse | An unvisited missing entry cannot be proved by a single-direction point read | Deleting an affected Edge requires both directions | Master-to-both-directions detects omission |
| Duplicate logical prefix/different value | Fixed-map lookahead rejects, including cross-leaf | Affected-prefix authentication refuses | Global logical uniqueness refuses |
| Orphan/extra entry | Missing/mismatched master refuses | Affected old state refuses; unrelated data is not scanned | Tree-to-Edge and Edge-to-tree reconcile |
| Foreign pin or wrong root/page/physical epoch | Domain, registered pin, Index Root, Page Directory/header checks | Same pinned source and existing page/root preparation checks | Complete routing/ownership checks |
| Stale/forged continuation | Private identity binds DB/generation/node/direction; logical pair revalidates previous and exact next | Grants no write authority | Does not replace global integrity |

Integrity reports are observations, not externally forgeable adoption credentials. The explicit physical-read-only backend factory performs its own bounded/accounted/cancellable global verification in the actual pin it retains, outside point reads/planning. Subsequent media damage is detected on access or the next integrity pass according to this matrix, not through a promised full scan on every read. Default readers, other interfaces and ordinary writers do not select this candidate automatically.

## Lifecycle, resources and counters

Pages, both directions, master Entity Maps, provided type indexes and known graph/complete token counts share data fence→inactive A/B→superblock fence→confirmation. Unknown statistics remain unknown; token 0 is a legal untyped count while type-label indexes omit it. Under/overflow refuses. Node/label counts, last-Edge removal, active empty roots/recreation and nonreused IDs are covered.

Old pins retain old master/tree versions and reopen selects complete A/B state. Existing COW, controlled pool relocation, retirement, pin barriers, release and physical-slot reuse remain. Private coordinator workspace is at most 1 MiB; general transaction/maintenance resource contracts still apply. A local account is not claimed as complete RSS or final production resource admission.

Reference/topology output uses existing buffer admission for retained vector/name capacity and conversion overlap. Borrowed spans do not escape; invalid names never enter caches. setup_edges measures explicit setup census; reference_pages/candidate_records include returned pages/lookahead, continuation_records adds predecessor verification, topology_pages counts adaptation. These are not independent IO counts. Formal timings still require instrumentation off and R7 inclusion rules.

## Evidence and acceptance

- 90 Edge bootstrap, parallel Edges/self-loops, bidirectional create/delete, master/type-index/statistics atomicity, old pins, A/B, reopen, pool relocation/retirement/alias barriers and global corruption are archived in prior checkpoints.
- Real read-only backend and the same LogicalPlanReadRuntime cover actual WITH-bound candidate work, OPTIONAL/aggregation/unsorted duplicate neighbor gathers and hybrid properties. The real overlay uses a test-only transaction that cannot publish to its read-only file. Typed/untyped names and output quota lifetimes are checked.
- New Node-only lifecycle/wrong-root/entity-bound cases, last-Edge empty-root/recreation and four fixed-seed skewed insert/delete cycles check every direction for 34 Nodes plus complete 0/1/2/7 counts. Same-executor two-hop traversal retains four parallel-Edge paths.
- Whole90 Edge hot-source allocation is 1703936 B/key-only versus 1835008 B/canonical+covered, retired 0 B, peak 1835008 B, file 5373952 B. No hypothetical production replacement or latency benefit is claimed.
- Final 56 related modules exclude only the independently verified unaffected mature multi-group space case:897 default passes;1223 candidate passes/1 conditional skip. Linux only, no full repository suite or intermediate timing benchmark. This directory retains source/hash/build/link/JSON/log/failure evidence.

R5.1 protocol/prototype is accepted:14/24 (58.3%). This closes neither R3.2/R3.3, R5.2/R5.3 nor any R7 gate.
