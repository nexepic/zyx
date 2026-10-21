### 2026-10-20 R3.2: Entity-pool relocation and write-contract correction

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-entity-pool-and-write-contract/`.

Controlled maintenance now relocates fixed Node/Edge pages and LabelSet overflow pages. Check all31 physical positions against the current Page Directory, preserve logical references, copy live pages and publish new routes with old-pool retirement in one A/B update. Old pins prevent release; drained pools permit physical-slot reuse. Field, directory and Entity maintenance reject valid-CRC free bitmaps hiding currently routed frames.

Review against the fixed R3 decision found automatic re-promotion contradicted explicit-only conversion. That code is removed and its archived evidence is marked withdrawn. Dense insertion,47-to48 membership, a scalar appearing after NULL-only extensions and restored homogeneous values retain extensions. An explicit inverse conversion followed by ROW-to-hybrid may create columns. Inline scalar no-op now follows ROW equality: signed zeros do not publish; equal-bit NaNs publish and preserve their bits.

Only17 affected modules run: default305 pass; candidate counts are in proof.json. No full unit suite or performance window. General values/LargeValue lifecycle, schema/statistics/label coordination, multiple groups, production adoption and final full costs/same-executor ROW control remain. **No new acceptance:13/24(54.2%); no commit/push.**
