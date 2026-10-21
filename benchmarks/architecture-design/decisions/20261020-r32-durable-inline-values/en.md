### 2026-10-20 R3.2: general inline values and immutable transaction inputs

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-durable-inline-values/`.

Explicit field lifecycle now handles strings, bytes, nested list/map values and date/datetime/duration. Reuse `OwnedPropertyValue`: cross-authority copies admit heap requests before allocating, and both transaction staging and direct metadata writes retain immutable inputs in the supplied budget. Same-authority payloads share ownership; move/conflict/rollback/cancellation refund correctly. Complex type changes demote only the affected column; other columns and old pins remain valid. No automatic promotion.

Decode only the selected inline field for recursive ROW equality, without materializing its owner. NaNs nested in lists/maps remain unequal to themselves. Reject values exceeding the native inline limit before retaining copies or writing data; preserve the1 MiB account and the pending LargeValue content-index/reference-GC guard.

Seven new candidate regressions pass: complex demotion, missing insertion, Node/Edge, every inline kind, recursive no-op/NaN bits, read-only reopen, old pins, inverse conversion, transaction move/conflict/rollback, direct/staged budget refusal/refund, cancellation after private preparation/retry, A/B recovery and field removal. Final verification runs only18 affected modules; exact counts are in proof.json. No full unit suite or performance window. A missing direct include caused the first build failure; it is fixed and its log retained.

LargeValue lifecycle, schema/statistics/label coordination, multiple groups, complete maintenance/production routing and final full costs/same-executor ROW control remain. **No new acceptance:13/24(54.2%); no commit/push.**
