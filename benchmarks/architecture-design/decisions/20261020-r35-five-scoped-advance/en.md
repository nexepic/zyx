# Fixed five R3/R5 implementation checkpoint

Scope is R3.2, R3.3, R5.1, R5.2 and R5.3. None is newly accepted. Progress remains 13/24 (54.2%); no commit or push is authorized by the percentage condition.

- R3.2 prepares independent-field LargeValue data/manifest frames, consecutive logical pages and shared pools in the same MetadataWriteBatch. Scratch and retained images are charged; refusal/cancellation refunds without file writes. Private pool-byte counters now include these pools, but exclude remaining publication costs.
- R3.3 adds internal NativeGraphWriteTransaction staging with retained account/source pin, an exact source fence, and exclusion of ordinary writes/ID leases. Moves/rollback preserve ownership; intervening publication conflicts. Commit still reaches the uninstalled-publication guard and follows the existing conservative INDETERMINATE exception contract without file/root changes. This is not successful conversion or complete backend/public routing.
- R5.1 binds native global adjacency continuation to database, generation, direction and node. Both adapter translations carry the database identity; existing paged-resume regressions pass. Complete higher-level cross-pin cursors and native covering/segmented lifecycle remain outstanding.
- R5.2 verifies the entire private multi-node adjacency batch against main Edge records before cache publication or callbacks, preventing early stop/filtering from hiding a later inconsistency. Main Edge checks remain; no new layout is adopted.
- R5.3 restricts existing anchor-cost reversal to proven transparent output envelopes. LIMIT, SKIP, OPTIONAL, DISTINCT, volatile/property expressions and unknown wrappers retain their direction. New-path costs and held-out validation remain outstanding.

Six affected modules, including the paged adapter, pass 121/121 default and 135/135 experimental cases. Raw results are retained under raw; source-manifest.json identifies actual source bytes. Linux only. No full unit suite or new performance window ran. The frozen starting observation remains 20261020-work-resume/start; final admission is pending. Corrected build failures are retained and are not performance evidence.

Still required: physical pool/directory/descriptor atomic publication; complete graph-property read/write/overlay/index routing; both conversion directions, deletion, recovery/integrity/retirement/relocation/reuse; complete actual allocation costs and same-executor ROW controls. R5 candidates likewise need atomic bidirectional lifecycle and cost validation. Keep guards until these contracts hold.

## R3.2 follow-up: private single-field demotion coordination

HybridPropertyDemotionPreparation fences the exact source of the same MetadataWriteBatch, authenticates group/column records before changes, copies each actual source frame once, adjusts group counts and removes the old column slot. It emits at most 64 extensions, final owner selector changes/deletions and extension locators for that field only. Extensions share pages; unrelated extension bytes, including independent LargeValue references, remain unchanged. Oversized replacements require the same-batch independent-field resolver and fail without it. Durable source IDs, file and roots remain unchanged; failures discard all batch-private IDs.

Tests cover type changes, field removal, NULL, actual locators/values, unrelated bytes, old columns, 1 KiB refusal, cancellation after frame copying and after private ID allocation, and the independent large-field resolver. Whole-owner deletion still refuses because all owner fields/live bits must change together. Multi-field final map merging, physical COW registration/retirement and atomic publication remain incomplete. This checkpoint does not accept R3.2/R3.3 or claim complete lifecycle.

The latest demotion checkpoint passes 4/4 default and 45/45 experimental affected-module tests. Initial missing-admission test-call and oversized-sibling inline-fixture failures and corrections remain under raw. Final results and source identity are recorded in demotion-proof.json / demotion-source-manifest.json.
