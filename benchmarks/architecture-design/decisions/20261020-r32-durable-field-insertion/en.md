### 2026-10-20 R3.2: missing-field insertion and new existing-column memberships

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-durable-field-insertion/`.

Explicit lifecycle supports missing scalar/NULL fields in live groups. Authenticate absent owner membership and absence of competing extension locators; for an existing token column, authenticate its actual record and absent presence bit before planning an added member. Merge additions/removals/updates before the final48-slot threshold: compatible values extend the column, incompatible values demote all final members. Otherwise create an independent extension. Empty live groups may establish their first shared owner/extension maps; missing-field erasure is a zero-write no-op. Default compatible-column mode still rejects insertion.

Combine positive/negative membership effects without imposing the4096 conversion-work limit as a group lifetime cap. New member sources, field pages/maps, descriptor and eligible pool retirement share one publication; liveness, main Edge authority and old pins remain valid. Fifteen affected modules: default302 passed; candidate447 passed/1 conditional skip. New checks cover the propertyless49th owner joining columns, same-field removal/addition retaining48 members, NULL/new-token extensions, incompatible insertion demotion, first maps in an empty group and inverse conversion. No full unit suite or performance window ran.

Extension promotion, general values/reference lifecycle, whole-owner/Entity Map coordination, multiple groups, relocation and final costs/ROW control remain unfinished. **No new acceptance:13/24(54.2%); no commit/push.**
