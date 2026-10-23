# Group-owned column locators and actual mixed-input applicability

Accepted progress remains 90%; P7 is not accepted. No timing benchmark, full unit suite, commit, push or default adoption.

## Implemented

Experimental 11/37 rejects prior 9/35, 7/33 and 6/32 files. Default 5/31 is unchanged. Membership version 2 appends one canonical 16-byte locator per sorted token; unbound logical builders cannot encode or publish. Compact conversion no longer creates a FieldDirectory. Point, grouped and full-window reads use membership locators, preserving actual Entity/competing ROW/extension checks. Compatible writes retain locator identity, missing-member insertion consults the matrix, and demotion/deletion removes the corresponding locator. Inverse conversion and forward/reverse/orphan integrity use the same authority and reject compact entries in a competing field tree. Group/membership placement reserves both records so page tails cannot separate them. Noncompact preparation retains its previous cancellation/placement ordering.

234 checks across 13 selected suites pass separately in legacy and compact membership configurations. Default passes 62 checks. An additional actual-schema mixed long-ID case passes in each configuration: CN filter=32, true grouping CN=32/US=32, and ID `user-000007` returns its unchanged cold string and country. These are separate runs, not a single full-suite result.

## Cost and remaining root problem

For the isolated drained 4096-owner two-field shape, directory allocation falls 262144→131072 bytes versus the preceding 9/35 candidate. Whole steady allocation falls 2228224→2097152 bytes, equal to ROW 2097152, still below the required saving. With the old snapshot retained, group 32 refuses after 31 complete groups in the unchanged finite destination range; this is partial, not mature whole-layout acceptance.

For 64 Users with actual ID/age/country/score shape, ROW steady allocation is 1441792 bytes and compact mixed native allocation is 1966080 (+36.36%). Three directory pools cost 393216 bytes. This small-fixture ratio is not extrapolated to mature databases.

`input-field-shapes.json` hashes and inspects actual medium/holdout CSVs. The importer (`apps/compare_benchmark/main.cpp`) stores external IDs as real string properties. All 225000/163000 nodes have IDs longer than seven bytes, so every populated group needs noncolumn fields under the current codec. The current multi-group conversion explicitly requires pure columns; its successful pure-column fixture is therefore insufficient for the actual dataset. Consecutive-ID group arithmetic in the receipt is labeled as an estimate, not inspection of persisted ID leases/gaps.

The next single architectural question is hot columns plus existing ROW remainder, removing ordinary cold fields from per-field OwnerMap/ExtensionDirectory. Before implementation specify unique field authority, bounded ROW/column merging, accessed-source validation/full integrity, all write/old-pin/retirement paths and atomic publication. Preserve the current actual competing-ROW check until that contract is explicit; never just skip it for speed. Account for packing and whole sealed-pool retirement, not shorter records alone. No new cache, index, backend or larger quota is authorized by this result.

## Retained failures

The first point/enumeration boundary still demanded the removed FieldDirectory; its source authority checks were adapted. Moving noncompact group preparation after its columns changed cancellation-before-ID ordering; source now preserves noncompact ordering instead of relaxing that existing test. Raw failures are retained. The two unrelated delayed-metadata-reclamation assertion failures proven against a frozen baseline remain in the preceding layout-scale evidence and were not included in this 13-suite selection.
