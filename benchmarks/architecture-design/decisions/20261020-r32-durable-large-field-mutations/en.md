### 2026-10-20 R3.2: native LargeValue field writes, removal and owner deletion

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-durable-large-field-mutations/`.

Explicit field lifecycle now handles missing-to-large insertion, per-field column-to-large demotion, existing LargeValue-to-inline/large replacement and REMOVE. Controlled Node/Edge owner deletion authenticates selected LargeValues first. New identities/data/manifests/content-index share one A/B publication; equal content for one token shares identity across owners. Authenticate/materialize only the selected field. After streamed content authentication, possibly equal lengths use recursive ROW equality: signed zero is a no-op; equal-byte NaNs publish while reusing content identity.

REMOVE/owner deletion never infers global retirement from a local reference difference. Manifests/content-index remain for exact reachability GC; surviving owners and old pins keep shared content, and corrupt content cannot be silently discarded. Native-active GC remains guarded; production adoption is pending. The unchanged1 MiB account covers immutable inputs, the bounded source-store frontier, reads/decoding and all private images.

Eight new regressions pass: initial content-index creation, batch deduplication, Node/Edge, affected-column-only demotion, old pins/read-only reopen, shared-content retention after replacement/REMOVE/owner deletion, NaN/signed zero, large transaction move/conflict/rollback/prepared cancellation/independent retry,128 KiB/beyond-account refusal, A/B recovery and corrupt-value refusal. The first new-extension path omitted reference-decoder admission; it is fixed with its failure log retained. Old refusal fixtures now check supported behavior and real quota boundaries.

Final verification runs only24 affected storage modules: default386 pass; candidate599 pass/1 conditional skip. No full unit suite or performance window. Native ownership scanning/authorized GC, complete schema/statistics/label coordination, multiple groups, generic maintenance, full costs/same-executor ROW and production/R7 admission remain. **No new acceptance:13/24(54.2%); no commit/push.**
