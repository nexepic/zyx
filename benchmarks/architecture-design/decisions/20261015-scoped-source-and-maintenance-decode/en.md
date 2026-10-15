# Private source and maintenance decode admission

Component evidence only: none of S5.3, S5.4, S5.5, S6.1 or S6.3 is newly accepted. Progress remains 22/34 (64.7%).

A borrowed synchronous decode scope validates the complete encoded shape, admits audited standard-container payload bounds before construction, and checks cancellation during recursive construction. Its caller retains credit until source values die. Nested scopes replace admission and inherit cancellation when no checkpoint overrides it. Overlay source loading, final property-delta extraction, integrity scanning and LargeValue ownership scanning install scopes per source collection.

Backing slots, external property directories and standard LargeValue result buffers are admitted before allocation. Full directory/token validation precedes resource rejection. Empty forwarding callbacks no longer suppress implicit accounting. Owned temporary chunk buffers release credit as each chunk dies; RAW transfers its existing allocation, compressed input/output overlap remains charged, and cached chunks retain their allocation resource after the decode scope ends. Explicit callback paths retain their existing admission authority and use a permanent ordinary upstream for chunk allocations to avoid double charging.

Prepaid grants support transactional external-reservation growth: rejection preserves previous credit, shrinking follows storage release, and moved-from reservations reject reuse. Maintenance source payload credit is released per collection rather than accumulated over an entire scan. Tests cover denial/retry, complete results, zero retained credit after cleanup, and unchanged publication/file writes for read-only scans.

Frozen source manifests and verification artifacts distinguish module results from sampled isolated fault sweeps. Heap bounds describe allocation requests, not RSS. No new performance campaign or Kuzu superiority claim is made. Reader names/directory workspaces, compiler/IR, and other publication/maintenance paths still require closure; MSVC default-container proxy physical-OOM behavior and runtime verification on the other two platforms are outside this Linux evidence.
