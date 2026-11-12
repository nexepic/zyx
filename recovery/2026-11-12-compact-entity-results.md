# Compact physical entity values and bounded result retention

Follow-up to Build #241 (`dc2db2386`) and evidence HEAD `c00a7d4e0`. No push or CI request is authorized.

## Contract and confirmed physical waste

`QueryResult::getRows()` returns a const reference to the complete vector of public unordered maps. `ResultValue::VariantType` and its const-reference accessor are public. Streaming or paging cannot transparently satisfy that same retained-result contract at a lower final footprint. The original 300-row/256 KiB gather therefore cannot be repaired solely by splitting transient source batches under the old representation. The same coarse old-layout floor proves the four-row/2304-byte CSV nested-sort and two-row/1500-byte pagination materialization assertions impossible on that representation (3584 and 1792 bytes respectively, before buckets/proxies/payloads).

Further inspection found a smaller compatible source-level correction: physical `Node` contains a private unused `padding[PADDING_SIZE]`, and physical `Edge` contains a private unused `typeBuffer[TYPE_BUFFER_SIZE]`. Neither array has a reader/writer. The large alternative inflates `ResultValue`, each public hash data node and its separately allocated sentinel. Both entity serializers/deserializers write/read metadata fields individually and never access these arrays; their 256-byte record-slot constants are separate from actual packed bytes.

The correction removes both unused arrays. It preserves all public types, variant alternatives/accessor signatures, value ownership/copy behavior, metadata fields, record-slot constants and serialized field order. It adds no indirection, sharing, borrowed result or new allocation. C++ object sizes change, so C++ consumers must rebuild; this is not a promise of compatibility with previously compiled C++ objects. Opaque driver ABI and serialized formats are unchanged by construction and will be regression-tested.

A standalone compiler comparison using the configured Linux compiler/STL, the old headers from `c00a7d4e0`, and the new headers reports Node 320→152, Edge 320→136, ResultValue 328→160 and result pair 360→192. Both record slots remain 256. The utility statically verifies that the public VariantType remains exactly the same five alternatives. These are Linux measurements, not native Windows measurements. Native one-entry and two-entry inert-layout allocator probes now print actual structural requests and a 300-row final structural floor; the earlier two-entry probe remains available and its per-probe counters reset.

No budget, payload shape, alias independence check, semantic exception or gate was relaxed. New Node/Edge regressions check compact memory, fixed record-slot size and independent packed v2 byte fixtures (Node 86 bytes, Edge 77), including stream and Node buffer roundtrips.

## Scope across all 27 native failures

`windows27-impact.json` enumerates every failed identity. Seventeen have direct result-copy, publication-overlap, growth or result-account evidence whose cost depends on the now smaller ResultValue. This is an affected set, not seventeen proven native fixes. Seven fail at distinct source/column/frontier/sort/conversion boundaries and are not claimed fixed by padding removal. Three have unconfirmed indirect effects (CSV aggregate/header, scoped graph properties, coordinated parent-prefetch admission). The two conversion cases retain the separate earlier BTree fix and still require native confirmation. No failure counts from overlapping diagnostics are added.

The three coarse old-layout impossibility examples must be recomputed after compaction. No unchanged 256 KiB success is claimed for Windows before its native allocator and complete-query evidence. Source/result lifetime overlap, transient empty map grants and global parent limits remain real costs even if the final result can now fit.

## Validation and recovery

Source-size check passes: 2816 code files, none above 3000 lines. Targeted rebuild and regression are in progress; final unfiltered regression is pending. This checkpoint preserves the implementation and explicit pending status. The existing complete Build #241 evidence, local full baseline and verified Library version 18 remain available. A further independent checkpoint will preserve this source after fresh bare restore, matching reachable objects and fsck. Previous reverse-download 403 remains respected.
