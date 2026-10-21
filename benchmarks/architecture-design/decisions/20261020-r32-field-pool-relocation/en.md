### 2026-10-20 R3.2: bounded field-pool relocation, retirement and physical reuse

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-field-pool-relocation/`.

Explicit metadata maintenance fences the exact database/generation/descriptor/source SegmentId and checks at most31 frames in one source pool. Compare each with the current Page Directory and clone only currently routed native field pages; reject current ROW/retired frames. Write a fresh field pool before one A/B publication updates Page Directory, old-pool retirement and Retire Queue. Retain PageIds, logical fields/directories and the exact descriptor root. Source bytes remain immutable; no full Catalog or database field scan is needed.

Old pins retain original fields and block release. After pins drain, controlled whole-pool reclamation removes the old identity; a fresh logical SegmentId may reuse the physical slot. Fragmented pools copy current frames only. The shared account remains at most1 MiB; unavailable destinations, quota, cancellation and stale fences fail before publication.

Sixteen affected modules: default303 passed; candidate459 passed/1 conditional skip. Checks cover wide multi-page groups, fragmented sources after updates, stable logical roots, Node/Edge, read-only reopen, old pins/release/reuse, A/B recovery, quota and pre/post-write cancellation/refund/retry, unavailable destinations and damaged source pools. The first reuse fixture attempted a short readAt; it now reads one complete page before decoding its envelope, with failure logs retained. Extracted the shared test fixture to keep test files below2000 lines. No full unit suite or performance window ran.

Directory-tree relocation/writes across physical epochs, general values/reference GC, whole-owner/Entity Map coordination, multiple groups and final full costs/same-executor ROW control remain. **No new stage acceptance:13/24(54.2%); no commit/push.**
