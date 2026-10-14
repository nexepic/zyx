# A4 adjacency authority, candidates and fault model

Date: 2026-10-14. S1.4 design scope. Ordinary Edge records remain authoritative. Production covering adjacency and S4 acceptance are not implied.

## Current behavior and first adoption

The ordinary global Packed B+Tree uses 17-byte `(direction, NodeId, EdgeId)` keys and empty leaf values. It has neither NeighborId nor TypeId coverage. PageBackedGraphReadAdapter resolves Edge records under the same metadata snapshot, authenticates identity, and checks the endpoint against the requested direction. The batch path gathers bounded sorted Edge IDs, reads records, checks cardinality/missing records/endpoint agreement, and emits original key order. NativeReadSession reuse remains bound to the exact snapshot; cache hits are not certificates for another generation.

The first covering candidate retains this master-record verification, using bounded batch reads and authenticated session reuse. Covered neighbor/type fields must agree with the master before emission or type-filter rejection. A filter must not hide a malformed encountered entry. No certificate-only trust, skip-master fast path, or change to corruption detection timing is authorized by this decision. A later proposal must update the public fault contract, format specification, and fault tests before enabling it. Checksum success alone never proves cross-structure consistency.

## Fault responsibility and timing

| Fault | Page/codec loading | Atomic writer/recovery | Local query and full integrity scan |
|---|---|---|---|
| Torn/short data or superblock write | Exact lengths, format/checksum validation reject damaged encountered pages | COW data precedes durable root publication; A/B recovery selects a complete valid generation; uncertain writes require reopen | Query rejects damaged pages it touches; scan visits live reachable structures; neither checksum nor recovery repairs arbitrary corruption |
| Wrong owner, logical page or generation | Authenticated metadata mapping/header/owner/generation must match; type/layout/codec validated | Publish matching routes and pages together; retirement cannot reuse pinned identities | Encountered identity mismatch is a format error; scan also detects foreign/unreachable/orphan relationships |
| Wrong endpoint, type, Edge ID or direction in covering fields | Valid bytes can encode semantically wrong fields | Create/delete update authoritative record plus both directional memberships in one graph generation | First adoption compares with master before emission/filter; scan checks every expected directional membership and any extra entry |
| Missing membership with otherwise valid pages | Not locally detectable from checksum or entry decoding | Atomic dual-direction maintenance prevents omission during correct writes | A local scan may return fewer rows without detecting omission; full graph/adjacency cross-check must reject it |
| Stale cache/continuation/overlay identity | Exact snapshot/program/owner identity checked before reuse | Invalidate on publication; old pins retain old complete layout | Reject foreign continuation; merge staged deletes/creates without exposing partial commits |
| Out-of-range links/cycles/sibling/restart/separator corruption | Validate encountered bounds/layout/owner and bounded traversal | Writer emits canonical structure; relocation atomically updates mappings | Local detection depends on path touched; full structural scan checks remaining paths |

Integrity is explicit validation, not an automatic repair promise. Durable publication failures are distinguished from external bit flips or a buggy writer that emits valid checksums with wrong semantics. Error discovery cannot be guaranteed on pages or omitted entries a query never visits. A whole-file scanner also needs bounded working memory, cancellation and pinned generation; concurrent publications must not mix its compared structures.

## Two candidates behind one logical interface

The interface emits EdgeId, NeighborId, TypeId, direction and snapshot visibility, with continuation tied to snapshot and comparator. Candidate formats are targets, not existing codecs.

| Dimension | Shared Packed B+Tree coverage | Shared segmented adjacency blocks |
|---|---|---|
| Routing | One global key space; u64 leaf locator into shared fixed-width topology blocks | One shared Node/direction/type directory routes to bounded blocks; nodes share blocks/pages |
| Low degree | Shared leaves/blocks; no per-node root or exclusive tail page | Multiple low-degree nodes share block capacity; descriptors charged explicitly |
| Supernodes | Bounded range scans and batch master gather; splits bound mutation work | Ordered block chain/tree of block IDs with bounded continuation; directory split bounds work |
| Covered fields | Locator resolves EdgeId/neighbor/type with authenticated block/page identity | Entries contain same fields with authenticated block/page identity |
| Writes | COW affected leaves/topology blocks/directory paths; both directions and master atomic | COW bounded touched blocks/shared directory paths; no whole-supernode rewrite |
| Relocation/retirement | Stable logical page mapping, owner/generation authentication, old-pin protection | Same; no raw physical locator survives generation change |

The current tree supports empty/u64 leaf values, not arbitrary covered payloads. The first candidate uses a u64 locator only after A7 assigns a codec and proves its page/slot bounds; do not quietly reinterpret the reserved key-only tree. A side-by-side new internal namespace or explicit version migration is required. Segmented headers define block IDs, count, owner, generation, sort range, next continuation and exact codec. No new persisted constants are assigned in this decision.

## Semantics, lifetime and replacement gates

Parallel edges retain separate Edge IDs. A self-loop has one outgoing and one incoming membership; BOTH-direction behavior follows the existing query deduplication contract. Ordering and continuation use unsigned key order, and type filtering is applied after verification. Staged create/delete overlays merge in the same order and honor continuation exactly once. Endpoint/type mutation is not added to the public API for this experiment; existing supported create/delete operations drive updates.

A graph publication atomically installs master, both directional representations, routing, indexes and retirement records. Any candidate whose semantic mutation changes only page mappings must also advance an included graph fence root/state epoch, per A5. Old snapshots resolve old routes; candidate blocks cannot be reclaimed while pinned or shared elsewhere. Reachability proof, not a caller-owned root list or an LVID difference, authorizes retirement. Recovery never publishes one direction alone. Maintenance relocates logical pages without bypassing identity checks and accounts for retained old generations.

S4 prototype gates compare point neighbors, batch traversal, type selectivity, random low degree, skewed supernodes, parallel edges/self-loops, overlay, concurrent old snapshots, repeated updates/deletes, relocation, steady-state space and bounded memory. Measure all route/descriptor/data/master reads and writes, not payload alone. Inject short writes/ENOSPC, torn A/B publication, owner/generation swaps, valid-checksum covered disagreement, omitted/extra memberships, stale continuation, and pin-protected retirement failures. Retain current layout if end-to-end costs fail the optimization thresholds. Final performance stays in the final window.

Current selected evidence covers adjacency codec/index, full integrity corruption and native transaction atomicity/fault modules. Existing tests explicitly reject missing memberships and valid-codec wrong endpoints, foreign owners/generations and malformed trees. This is design/fault evidence, not a working covering format or eliminated master IO.
