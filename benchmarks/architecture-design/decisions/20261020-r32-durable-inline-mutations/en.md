### 2026-10-20 R3.2: durable existing-inline-extension updates and removal

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-durable-inline-mutations/`.

- Explicit field lifecycle now authenticates existing inline extensions for scalar/NULL replacement and removal, merging final group counts with column updates/demotion. At most64 distinct owner/token requests share the at-most1 MiB account. Missing fields and LargeValue reference updates/removals still refuse before file writes.
- Clone actual affected field pages only. Erase the old slot and place replacements in accounted private images, compacting or allocating batch-owned logical pages as needed. Exact old directory suffixes are replaced when growth changes the locator, with no duplicate prefixes. One A/B publication still coordinates group counts, owner/extension maps, physical pages and eligible whole-pool retirement. Empty fields preserve owner liveness and main-Edge authority.
- Equal short scalar replacements do not publish; doubles compare bits to retain explicit signed-zero/NaN payload changes. Complex inline sources authenticate their borrowed encoding without decoding the complete owning value. Retained source-record bytes outlive their borrowed views.
- Fifteen affected modules: default302 passed; candidate444 passed/1 conditional skip. New coverage includes sparse fields, writes after column demotion, mixed column/extension batches, NULL-to-int growth, empty namespace removal, duplicate/missing/no-op zero-write behavior, native transactions, pre/post-write cancellation/refund/retry, old pins, A/B recovery and LargeValue-reference guards. No full unit suite or performance window ran.

Missing-field insertion/promotion, general types/value-reference GC, whole-owner/Entity Map coordination, multiple groups, relocation and final full costs/same-executor ROW control remain. **No new stage acceptance:13/24(54.2%); no commit/push.**
