### 2026-10-20 R3.2: extension promotion and threshold round trips

Evidence: `benchmarks/architecture-design/decisions/20261020-r32-durable-field-promotion/`.

- Explicit lifecycle rechecks only actually changed extension tokens: at most64 live slots per token and64 tokens per batch. Read the same source pin and overlay final replacements/removals/additions. Promote at48 or more members when non-NULL values share one int/double/bool kind. All-NULL fields wait for an inferred kind; mixed/complex/LargeValue sources retain independent fields.
- Clone actual affected field pages, erase final members' extension slots, and append a column in shared private pages. Update owner sources, exact extension suffixes, column directory and group counts in the same A/B publication as other changes/retirement. No whole-group rebuild or per-owner trees. Field scans, short-scalar decoding, private records and directory images retain the original1 MiB admission.
- Fifteen affected modules: default302 passed; candidate452 passed/1 conditional skip. New coverage includes the first64-slot dense double field, NULL/-0/NaN bits, promotion A/B recovery,47→48 re-promotion/inverse conversion, two tokens promoting together, fixing the only incompatible extension, all-NULL type inference, quota and pre/post-write cancellation/refund/retry. No full unit suite or performance window ran.

General values/content-index/reference GC, whole-owner/Entity Map coordination, multiple groups, native relocation, production adoption and final full costs/same-executor ROW control remain. **No new stage acceptance:13/24(54.2%); no commit/push.**

The first quota fixture incorrectly expected first-map creation to exceed128 KiB; it legitimately succeeded. The corrected fixture promotes an existing extension directory to exercise incremental-overlay admission. Failure evidence is retained; final module regression passes.
