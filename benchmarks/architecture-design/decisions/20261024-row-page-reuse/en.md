# Candidate A: reuse the authenticated ROW page across batches

This is an S2 implementation checkpoint, not candidate acceptance. Total accepted progress remains 15%. No timing comparison, commit or push belongs to this checkpoint.

The selected-property cursor now retains at most one authenticated slotted-page view and its pool-charged frame across adjacent batches. Previously it copied the last pinned payload into an owning SlottedPage, reparsed that image and subsequently copied each selected slot. A matching next batch now reuses the validated view; changing pages releases it. Other read modes release the optional pin. Disabled or saturated pools keep the ordinary copied path. Values and selected tokens are still decoded and owner/slot/type checks remain; this change does not eliminate the per-entity ROW root cost.

The cursor remains tied to its metadata store and original active generation. A StorageIO mutation epoch, including cache-pool replacement, invalidates retained data and directory images. If publication occurs during consumption, the remaining owners reacquire pages for the same pinned snapshot, with no replay of delivered owners. Text runs are consumed before releasing their backing page. Consumer/admission errors destroy local retention, so retry cannot reuse a partially consumed batch.

StorageIO-issued BufferPool pins now retain the owning pool until unpin, including move construction and assignment. This is necessary for a view retained across calls to remain safe when the pool is disabled or replaced; directly stack-owned BufferPool instances keep their existing caller-managed lifetime contract. Ownership adds no separate frame copy or allocation.

Validation results and exact source/library identities are recorded in receipt.json after the targeted checks finish. The real consumer tests cover aliases, old snapshots across another handle, text/scalar run lifetime, copied fallback, complex/LargeValue/NULL/missing values, bounded admission and stopped consumers. No complete unit suite or performance matrix is run.

The first isolated run passed 82/83 cases. A new test incorrectly expected a pool pin after a full-read call had already retained an owned image; the selected path correctly reused that owned image. The corrected test asserts that owned-image reuse keeps zero pins, then starts a fresh cursor to independently check pin move and release. No production assertion or fallback was weakened. The original failed JSON/log are retained.

The first default-profile run passed 159/160 cases. A newly added exact counter assertion included both the preceding rank gather and the alias gather (512 owners rather than 256). The test now resets tracing immediately before aliases and asserts that isolated operation's exact count. Result and fallback assertions are unchanged; the first run is preserved.

Final targeted validation passed: default 160 cases (including real query consumers), isolated 83 cases, and isolated source/format 24 cases. Both current core profiles were rebuilt. These are per-configuration counts with overlap. Complete suites, timing comparisons and space admission were not run. S2 remains unaccepted at 15%.
