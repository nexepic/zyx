# Whole allocation, bounded conversion and physical retirement

Progress remains 90%; P7 is not accepted. No latency benchmark, full unit suite, commit, push or default adoption.

## Result

The 4096-owner narrow fixture (short country text plus integer) finishes all 64 isolated one-group conversions when existing retirement is drained. ROW steady allocation is 2097152 bytes; final experimental steady/total allocation is 2228224 bytes (+6.25%). Property allocation falls 655360→393216 bytes, new native maps cost 262144 bytes, and metadata rises 524288→655360 bytes. Encoding savings alone therefore fail whole-layout admission. This is an isolated shape/cost census, not actual medium/holdout performance or production migration authorization.

With the old ROW snapshot retained, all original property slots stay allocated, group 31 refuses atomically after 30 completed groups within the original destination interval [40,80]. Partial steady allocation is 2490368 bytes and total allocation 6291456 bytes. It is a partial conversion, not an extrapolated complete ratio. Every current/old value, read-only reopen and full integrity remain correct. Releasing a pin alone does not drain durable retirements; the underlying separate maintenance operation is explicit.

## Necessary lifecycle correction

The generic hybrid guard initially blocked even physical reclamation of existing durable retire-queue entries. The storage contract was revised before implementation. Only `reclaimRetiredPhysicalSlots` now enters hybrid write admission and marks its privately prepared queue-only batch as coordinated. Existing Catalog/allocator/disposition/generation/active-snapshot validation is unchanged. No arbitrary field mutation, new retirement discovery, descriptor rewriting, normal pool reconciliation or other unadapted write is admitted. Active pins prevent reclamation; all-pinned no-ops do not advance roots or the data epoch. Existing portable bootstrap failpoints verify partial A/B preserves the old allocation and complete A/B recovers the new allocation, with native values/descriptor and integrity intact.

29 selected experimental and 24 default checks pass. Two unchanged tests expecting allocation-threshold-delayed metadata reclamation also fail on the frozen pre-round 9/35 library (`frozen-retire-checks-receipt.json`); their current and baseline failures are retained and excluded explicitly, never counted as passing. The current documented implementation uses authenticated retirement deltas rather than the old periodic threshold.

The first fault fixture used native syscall hooks with an fstream-only StorageIO (invalid native descriptors), so no hook could fire. The corrected test uses existing portable `BootstrapPublishFailpoint`; it also seeds a durable retired segment explicitly because a tiny ROW conversion need not retire a complete pool. The larger integrity scan needs the existing default 8 MiB maintenance allowance, separate from the unchanged <=1 MiB conversion/census account. Neither production quota nor fixture destination range was increased.

## Next core decision

Remove redundant field positioning and charge actual pools/directories/metadata together. Existing compact membership already authenticates sorted tokens and presence, but the reader then needs a distinct FieldDirectory lookup for a column locator. Co-locating that locator with group membership is the first focused candidate; persistent bytes, source authority, COW/pins, deletion/demotion and compatibility must be specified before implementation. Group positioning through existing Page Directory needs explicit ID namespace/collision/high-water-mark proofs if considered; it cannot be achieved safely merely by assigning high-bit IDs. Avoid a speculative tree replacement or new cache/backend. Actual medium/holdout space and migration bootstrap remain unaccepted.
