### 2026-10-20 R3.2: directory-pool relocation, mixed physical epochs and reverse retirement checks

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-directory-pool-relocation/`.

- Relocate one reserved native Index namespace pool at a time, at most31 frames, copying current routes only into a fresh SegmentId with an incremented physical generation. Root moves atomically rebind Page Directory, Index Root and descriptor root. Replacement descriptors retain the existing current-publication generation fence while field values/record semantics remain unchanged. Child-only moves retain the exact descriptor root.
- Native private B+Tree overlays authenticate children through their pinned actual Page Directory epochs; Index Root still fences the root. Supports whole directory relocation, one dirty-path update, remaining-child relocation, more writes and inverse conversion. Ordinary overlay behavior remains unchanged.
- Controlled retirement/relocation/release now probes valid owners at all31 frame positions, including bitmap-free positions. A current route to an unallocated, retired or foreign-epoch frame refuses, preventing valid-checksum bitmap omissions from hiding live pages. Unrouted blank/truncated/stale bytes confer no ownership. This bounded local check does not replace global integrity; its fixed probe I/O belongs in final costs.
- Sixteen affected modules: default303 passed; candidate464 passed/1 conditional skip. Coverage includes Node/Edge, root/child moves, incremental writes/inverse across physical epochs1/2/3, exact descriptor fences, A/B recovery, old pins/release/reuse, quota/cancellation and valid-checksum free-bitmap omissions. No full unit suite or performance window ran.

The first directory publication required correcting the replacement descriptor's current-generation fence. A fixture that drained its source after two edits now relocates remaining children after the first edit. An execution-environment restart then removed unarchived/tmp failure logs. Source and prior frozen evidence survived; the module runner was restored to persistent workspace and current checks rerun.

General values/content-index/reference GC, whole-owner/Entity Map coordination, multiple groups, production adoption and final full costs/same-executor ROW control remain unfinished. **No new stage acceptance:13/24(54.2%); no commit/push.**
