# R5.1 covering candidate: global reconciliation and physical lifecycle

This continues R5.1 within the fixed five. No stage is newly accepted; progress remains **13/24 (54.2%)**. No commit/push or new performance window.

## Implementation

CoveredAdjacencyCandidate::verifyIntegrity performs two complete scans under one source pin:

1. Enumerate the candidate fixed map, rejecting duplicate logical prefixes and checking every direction/owner/neighbor/type against the authoritative Edge. Reject orphan records and checksummed incorrect suffixes.
2. Traverse the authoritative Edge Entity Map with a new logical cursor and an independent metadata-page cap. Authenticate every main Edge and endpoint Node; require exact outgoing/incoming entries and total cardinality equal to twice the Edge count. Correct outgoing point reads cannot hide missing reverse entries.

Candidate/main record bounds and overflow-safe lookahead arithmetic apply before work. The typed Edge metadata-tree page limit is independent of record count. The report returns only after both scans exhaust and the final checkpoint succeeds; database/generation identify its source pin, not cache/write/adoption authority. Cancellation or incomplete work cannot publish a success report; independent retry completes. This global check is not a planning or point-read fallback.

Physical lifecycle reuses existing typed B+Tree compaction: evacuate all source pages, preserve logical root PageId and bind the destination pool's physical generation. Old pins still pass complete reconciliation and prevent source release. After releasing the old pin, reuse the source physical slot for a different Segment/Index/generation; candidate reconciliation and reopen still pass. Old continuation rejects a new generation. No retired per-node root/delta layout or main-record-check removal is introduced.

## Verification

Only the covering candidate and LocationDirectory modules ran: **60/60 default**, **60/60 experimental**, four suites. Cases cover missing reverse/orphan/duplicate keys, record/page bounds, cancellation/no partial report, logical seek cursors, invalid/released pins, actual relocation/deferred release/slot reuse/reopen, and existing directory COW/recovery/boundaries. Linux x86_64 only; no full unit suite. Raw JSON/output/build logs and source/binary SHA-256 are retained.

## Remaining

This remains a laboratory candidate. Complete backend type-index/statistics/overlay coordination, segmented lifecycle, actual read/write/space costs, adopted path integration, new cost plans/held-out workloads and final R7 admission remain outstanding. The candidate's global reconciler is not registered in production integrity-scanner consumers. R3.2/R3.3 still need the previously recorded physical atomic publication and cross-field/whole-owner lifecycle. This checkpoint grants no stage count.
