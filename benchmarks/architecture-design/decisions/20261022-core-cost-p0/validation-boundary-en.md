# P1/P2 validation boundary decision (user confirmed; implementation pending)

## Verified current behavior

`MetadataRouteBranchReuseTest.UnmodifiedCorruptLeafIsRejectedBeforeAnyPublicationFrameAllocation` changes a route in the first leaf of a three-leaf old tree, corrupts the third leaf, and requires rejection before allocating publication frames. `collectRouteBranch` also checks target/bootstrap aliases and duplicates globally. Current caches invalidate on the global StorageIO mutation epoch; no complete cross-commit validation proof is retained.

## Requirements that cannot both hold unconditionally

An unrelated page can change after prior validation while the modified path and root references remain identical. A commit reading only the modified path cannot distinguish the healthy and corrupted files. A checksum or Merkle digest in a root cannot prove actual unvisited disk bytes still match it. A previous validation certificate likewise cannot rule out subsequent corruption.

If the existing guarantee means every commit must discover fresh corruption in unvisited branches, strict O(k×h) ordinary commits cannot simultaneously satisfy it. Define the boundary explicitly rather than deleting assertions to pass tests.

## Recommended contract: validate accesses plus explicit complete integrity

- Validate CRC, type, identity, generation, key range, and visibility for every accessed page; fully validate newly built modified paths before publication.
- Reuse unchanged subtrees by exact immutable root reference. New targets must come from exclusive current-batch allocations. Supply an inductive proof or bounded auxiliary index for every former global invariant; do not assume them away.
- Preserve atomic publication, synchronization order, old snapshots, cancellation/failure handling, and reclamation.
- Detect subsequent corruption in an unvisited branch on first access or explicit full integrity, rather than promising discovery by unrelated commits. This changes observable error-detection timing and must be documented.
- Keep global collection/integrity rejection of corruption, duplicates, orphans, and aliases. Separately test that local writes skip unrelated leaves, accessed corrupt pages fail, and complete checks fail. Do not simply delete the old global test.
- If local metadata cannot establish new relationships, add and version the necessary metadata before enabling the path.

## Conservative alternative

Retain global validation on each commit, reuse its result within that commit, and reduce copies/construction. Do not claim that this is independent of total route count M. One full validation at startup additionally requires accepting O(M) startup and the boundary for subsequent storage corruption; it is not a free solution.

The user selected the recommended contract on 2026-10-22, authorizing its implementation. P1/P2 remain unaccepted. P0 baseline work can continue independently. Neither a new format nor an in-memory certificate should obscure this decision.
