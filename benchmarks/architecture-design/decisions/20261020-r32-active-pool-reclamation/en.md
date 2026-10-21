### 2026-10-20 R3.2: controlled pool release while native stays active and writable reopen

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-active-pool-reclamation/`.

Existing deferred-reconciliation mode reopens writable metadata, continues native transaction column writes, verifies them through another read-only reopen, then returns to ROW and ordinary writes through controlled inverse. Unadapted pool reconciliation remains guarded.

`tryReleaseRetiredCatalogSegments` now has a controlled active-candidate branch: at most64 retired Property/Index pools; durable queue/Catalog identity/epoch and snapshot-barrier checks precede per-pool authentication of at most31 frame owners and rejection of current routes. One A/B publication removes the queue entries, clears allocator bits and erases these exact Catalog identities. No full Catalog-history scan, field-byte/descriptor modification, generic per-frame reconciliation or relocation is enabled. Live/other-kind pools reject; an older pin blocks the whole batch.

Five continuous native-column update/release rounds reuse slots directly, without inverse each time or accumulation of released Catalog identities. Tests cover old-pin blocking, complete A/B states, oversized batches, duplicate/stale identities and damaged-source rejection/restoration/retry. **No new acceptance:13/24(54.2%), no commit/push and no timing window.** General field insert/delete/type changes, whole-owner deletion, multiple-group maps, general value-reference lifecycle, relocation and full costs remain.
