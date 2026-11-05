# CI 228: follow-up to the compact-cache full run

Frozen `ff02643c` required unfiltered quick regression completed without timeout: 11039 passed, 7 failed, 20 disabled, 0 skipped. Discovery/execution each contained 11046 enabled names, with no missing, extra or duplicate names. The matching source fingerprint and compressed original XML are in `evidence/2026-11-05/ci228-compact-cache/full`. That negative result is retained unchanged.

## Production changes

Owning selected-column prefetch previously charged destination cache nodes only after the backend returned payloads. The CSV/live-header regression demonstrated a payload read before the destination could fit. The new path reserves missing destination node/key metadata before the first source-admission callback and transfers that reservation to rows on insertion. Payload credit is transferred without a refund/reacquire gap. Invalid shape remains checked before retry, borrowed repeated columns keep their existing separate ownership, and resumable native cell accounting is unchanged. An invariant rejects any untransferred destination credit.

Vector procedure search is synchronous and returns owning hits. The converted request buffers were nevertheless held through output publication. They now die immediately after search and hit-count validation, before the corresponding allocation loans are refunded. A fixture observes valid request coordinates during search, less account usage before selected-property production, escaped result ownership and complete refunds. No public type, source memory limit, or snapshot contract changed.

## Layout-independent test boundaries

GDS negative cases now use impossible lower bounds (a destination row alone, or three independent 6000-byte output payloads), followed by retry/refund checks. PageRank retained output also has an independent fixed 32 KiB efficiency envelope. This replaces false assumptions that the compact row representation must exhaust the old 1024/20-KiB allowances.

Nested-sort positive tests retain the historical frame envelope instead of shrinking their quota with empty private-map headers. Two 4608-byte envelopes remain below the independently reconstructed pre-compaction GNU allowance of 4758 bytes. The explicitly ample carrier case includes an allocated binding node. Low-budget rejection, full ordering, no read-ahead and exact owner-read checks remain. Limit/skip exact-fit estimates include MSVC key-string proxy storage, without expanding their independent 1792-byte ceiling.

Before the last two zero-cost-on-GNU key-proxy fixture corrections, the focused normal same-configuration executable passed 259/259 selected tests (the changed runtime TU and selected test TUs were rebuilt; this is not a replacement for the final full run). The matching XML is preserved. Final same-tree full regression and native Windows/macOS verification remain pending.

## Full-project sanitizer evidence and remaining RSS failure

A separate Linux Clang19 Debug -O0 -g0 build compiled all 254 project production units (including parser and inputxx object libraries) with ASan+UBSan. Three original native budget failures passed without sanitizer reports. A cold 20000-row sort completed its work quickly but still failed its unchanged 64-MiB RSS guard. This is a negative result, not a sanitizer pass or a native macOS result.

An isolated test-only probe on frozen `ff02643c`, with unchanged guard, recorded before/after RssAnon 73152/131028 KiB and RssFile 114080/134244 KiB. At rejection live sanitizer allocation was 5111906 bytes, heap mapping 70004736 bytes. ASan reported 38 MiB freed but zero really freed, and 33404 stack-depot entries. File-page residency and retained instrumented allocator memory contribute to process RSS. These measurements do not justify raising the guard, disabling quarantine, or claiming every retained page is query-owned. Further optimization/attribution remains necessary.

No push, CI dispatch, cancellation or remote monitoring was performed in this phase.
