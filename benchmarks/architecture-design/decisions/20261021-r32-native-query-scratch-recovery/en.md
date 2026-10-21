
### 2026-10-21 R3.2: Recover orphan query scratch under native layout

Evidence: `benchmarks/architecture-design/decisions/20261021-r32-native-query-scratch-recovery/`. A regression reproduced writable-open refusal when sealed orphan query scratch coexists with a converted native group. The existing query-specific release now enters the typed native writer after authenticating bounded kind/owner/generation/allocated identities and rejecting every public Page/Extent reference. It relocates the unchanged descriptor in the same metadata publication. Existing private-reader quiescence and envelope/checksum validation remain.

Seven related modules: candidate277 and default195 passed, including startup recovery with unchanged native query results and integrity, query/bulk kind isolation, foreign generation/owner/duplicates and corrupt public Page/Extent registration refusals. The original failure and exact pre-bulk source are retained. Bulk scratch retirement/recovery continues separately; other native writer and production/R5/R7 gaps remain. No full suite or timing benchmark. **Total14/24 (58.3%), accepted growth0; no commit or push.**
