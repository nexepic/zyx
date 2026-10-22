# Incremental route commit: candidate and proof obligations

P1 is accepted at 25% progress. P2 remains unaccepted: metadata-pool ownership enumeration and threshold-triggered global reclamation still need incremental treatment. The user confirmed local-access validation plus complete integrity. The persistent format, COW/A-B protocol, and synchronization order are unchanged.

## Algorithm

1. Read the sorted final MetadataPageId change set. Reject targets not exclusively allocated by this batch and distinct IDs sharing a new physical frame.
2. Decode only changed leaves and ancestors from the published bootstrap route root. Validate accessed CRCs, types, visibility, levels, ranges, sibling aliases, and every changed-leaf record's ordinal.
3. Plan shared ancestors once. Grow the root through zero-prefix wrappers for far IDs, privately changing the old ROOT to INTERNAL. Reuse untouched physical subtrees without mutation.
4. Plan frames in postorder. Capacity is changed leaf/radix frames plus one header. Planning allocates no persistent identity or frame; publication allocates and encodes in that order.
5. Capacity and publication share an accounted directory-local plan. Root, batch identity, pending routes, or StorageIO epoch changes invalidate it. Release the directory's reference before materialization, so scratch never becomes a cross-transaction cache.
6. Obtain each required directory root's logical ID from that root page and verify its route instead of reverse-searching a complete set. Root count is fixed.
7. Keep full processing for explicit refresh, pruning, initial migration, and integrity; never use it as a hidden ordinary-update fallback.

## Invariants

- Old pins: source pages never change; old bootstrap references preserve old visibility.
- Target ownership: the allocator rejects duplicate batch frame allocations and does not reuse pinned old pools. Check change-set target uniqueness; later bootstrap frames are separately allocated, so new publication does not introduce aliases.
- Existing corruption: accessed pages are validated. Unrelated commits do not promise detection of fresh corruption in untouched branches; full traversal/integrity still detects global aliases, duplicates, misrouting, and corruption.
- Resources: frame/child vectors, leaf payloads, target sets, and shared control blocks must retain accounting. Admission failure refunds work and never silently invokes a global rewrite.
- Failure: no route publication frames are allocated before planning completes. Existing MetadataWriteBatch rollback owns partial materialization; A/B ordering remains intact.
- Capacity closure: new pools can change Catalog/Allocator and pending routes, invalidating the plan. Retain bounded convergence, but never rescan complete routes on each pass.

## Required verification

Change one route across trees with 3/300/900 leaves and measure reads/allocations: growth follows height rather than total leaves. Cover shared ancestors, root growth, duplicate targets, changed corruption, first-access/full-check rejection of untouched corruption, old snapshots, and reopen. Count/count/build must plan once; new routes or I/O writes must invalidate it. Production metadata-store tests cover Catalog/Allocator/Location publication, pin-delayed reclamation, faults, and migration.

Shared planning and production preparation passed 183 necessary module cases, including routes, Metadata Store, Batch, faults, and migration. Three-size fixed-change mechanism records are in `route-module-tests.json` and `acceptance.json`. No full suite or candidate performance window ran; neither the 5-fold gate nor the Kuzu objective is claimed.
