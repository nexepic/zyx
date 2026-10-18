# Avoid encoding in-memory Superblocks for validation

Stop evaluating the candidate requiring repair after the full wide-property contract exceeded its original 1800-second deadline. Preserve complete observations and cancellation records; no time, file or memory limits were raised.

An instrumented 578-operation diagnosis with full typed-value checks found repeated `SuperblockCodec::encode` calls whose bytes and CRCs are immediately discarded. A direct validation entry point reuses the same existing field validator at 29 sites. File encoding, disk CRC decoding and A/B publication are unchanged.

All 567 tests from 39 necessary modules pass, including three new cases covering CRC-free validation, malformed references for all 13 roots, and lifecycle/reserve/identity/alignment fences. The full unit suite was not run.

Both versions pass the same 578 diagnostic operations. The 64-field dynamic-update cases reduce cumulative CRC calls from 106669 to 34153 with identical logical read/write bytes. Instrumentation is enabled and the repaired run overlaps test compilation: no duration comparison or performance acceptance is inferred.

Progress remains 31/34 (91.2%). Full dynamic/wide-property deadlines, write tails and the final unified campaign still require validation. No new acceptance or claim of outperforming Kuzu. See proof.json for raw records, source/binary identities and reproduction scripts.

The repaired Release also passes all 886 full-result checks across 13 smoke families under the 120-second whole-run budget; smoke does not replace full contracts or performance evaluation.
