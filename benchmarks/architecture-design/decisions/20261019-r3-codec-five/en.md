# R3.2: five bounded payload and admission steps

Parent: `6fa1d4e5a2c39f678b8cf5e10ee493616069800b`; registration preceded implementation. These five steps belong to **one incomplete deliverable, R3.2**, not five accepted R0–R7 deliverables. Progress remains derived from the active optimization document: 13/24. R3.2 is in progress.

| Step | Implemented contract | Verification |
|---|---|---|
| Column record | Exact 64-byte header; 576-byte int/double or 72-byte bool; 64 slot-indexed values, identity/generation, presence/live/NULL subsets, zero absent/NULL payload; at least 48 present slots | Exact offsets/little endian, 47/48/64 boundary, missing versus NULL, raw int/double identity, signed zero/NaN/infinities, invalid type/owner, every truncation |
| Group header | 64 bytes; owner/group/generation and hybrid mode, actual membership/column counts, reserved zero | Wrong identity, empty group, sparse columns, count limits, reserved bytes |
| Descriptor | 192 bytes; database/semantic generation; eight independent (PageId, generation) roots, zero flags/reserved | Exact offsets, empty versus partially zero roots, future root/descriptor generation, wrong database |
| Membership and positioning | 16-byte owner/token/source; 16-byte locator; 28-byte column-directory and 24-byte group-directory records | Sole source tags, owner/token/group identity, page/slot/generation comparison, reserved bytes and truncated records |
| Conversion gate | Explicit known evidence for nine native cost categories, ROW lower and candidate upper bounds; >=10% saving, complete peak within file cap, <=64 owners/4096 memberships/1MiB staging and remaining account | Unknown component/peak refusal, exact 10% and rounding boundaries, overflow, all work/resource limits, inconsistent peak refusal, reserved-pool counterexample |

Code: `benchmarks/architecture-design/property-subblock/PropertySubblockCandidate.hpp`. Fixed-size payload operations allocate no heap; column encode borrows an exact-sized admitted destination. Other encodes return fixed arrays. This is **not** an implementation of conversion account reservation, query routing or native full-cost estimation. Boolean `known` evidence is supplied by a future audited native estimator, not inferred by this gate. Categories include data/map reserved pools, descriptor, catalog/allocator, page-directory/routes, large manifests/content, query indexes, retire queue and retained pins; unknown or uncancelled costs must not be marked known. Peak includes currently allocated old/new/retired/pinned capacity and must be at least both steady totals. Test evidence is synthetic gate input, not native allocation measurements.

No file writer or native caller is connected. The payload tags/version are laboratory-local and are **not** allocated MetadataPageKind IDs or a Superblock format update. Native Superblock remains version 31, format profile 5. Caller-expected identities do not replace authentication of an outer native page/slot. Checksums, complete bidirectional selector/extension/column integrity, actual tree ordering/framing, slot usable bounds and native allocation/generation lifetime remain integration obligations. Bit-exact NaN preservation here does not replace executor comparison/aggregation semantics.

Logical IDs are not compacted. Group/slot arithmetic rejects ID zero and the impossible 64th slot in the final uint64-ID group. Labels do not influence placement. The codec does not automatically promote/demote, rewrite owners, allocate pools or persist descriptor roots.

## Checks and reproducibility

20 final targeted cases pass, including 10,000 deterministic byte mutations; accepted mutations round-trip canonically, invalid structures reject. Scalar payload mutations can be valid; native outer authentication is still mandatory. `first-tests.json` has the earlier 18-case pass; `final-tests.json` has 20. CMake/CTest reproduction passes the same suite. An initial compile exposed two EXPECT_THROW macros on one line (GTest label collision); split the assertions before the first runnable test, with no production API change.

AddressSanitizer and UndefinedBehaviorSanitizer pass all 20 cases with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`. The first sanitizer process passed GTest but exited 1 because LeakSanitizer cannot operate under this environment's ptrace restriction. Preserve `sanitized-tests.json` and `sanitizer-process-status.json`; do not describe that process as successful. Final report is `address-undefined-tests.json`. Leak detection was not validated. Only Linux x86_64/Clang 19.1.7/GTest 1.15.0 was executed; portable C++20/fixed LE encoding and CMake do not establish Windows/macOS test results.

Reproduce with installed CMake and GTest:

```bash
python3 benchmarks/architecture-design/decisions/20261019-r3-codec-five/run-tests.py /tmp/zyx-r3-codec-build -DCMAKE_BUILD_TYPE=Debug
```

Pass `-DCMAKE_PREFIX_PATH=...` for a non-default GTest installation. `-DZYX_CANDIDATE_SANITIZERS=ON` enables the optional Clang/GNU sanitizer build. The root project also discovers the new test through its existing relocatable property-test glob. No global test suite or formal performance window was run.

## Remaining acceptance boundary

R3.2 still requires the atomic native format/root/fence/kind/reader/writer/integrity/relocation/retirement update, shared maps and independently authenticated extensions/large bodies, charged bounded forward/reverse conversion and field-only demotion, graph/index/overlay consistency, old/new pins/reopen/reuse/retire/quota/fence failures, complete actual native allocation versus estimate, and ROW control through the same R1 executor. Follow the full R3.1 matrix; these five steps do not shorten it. R3.3 production and all R7 performance gates remain pending. No performance improvement, conversion acceptance or storage saving is claimed.
