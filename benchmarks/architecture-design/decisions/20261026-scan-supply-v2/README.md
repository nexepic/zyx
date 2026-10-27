# Scan supply optimization: stage exits and adoption decision

The registered V0–V4 round is complete:100%. The independent Kuzu target failed: frozen primary geometric-mean gain is2.0021887x, while candidate P50 remains9.31–88.97x slower than Kuzu. The margin is small and process dispersion is material; this is not a stable2x guarantee.

Choose local field-page sharing while retaining64-owner logical groups and the existing persistent format. Private BUILDING cohorts have at most8 groups, same-token shared pages and separate metadata/cold bodies. Width1 remains default; wider cohorts require explicit selection. Medium score pages fall3437→859 and group/score union3443→1718, with entity counts and encoded values unchanged. This supports a local change, not a whole-backend rewrite or a claim that the architecture can already match Kuzu.

## Required adoption limits

Accept only explicit experimental SCAN use with long-lived handles and read-heavy analytics. This applies to the combined candidate and does not endorse default rollout in other profiles. Write-heavy, strict point/tail-latency and frequent-open workloads are outside performance guarantees. Correct writes remain supported; faster writes are not established. Offline conversion, initial full authentication and version retention must remain visible costs.

| Original flagged case | Review candidate/control P50 | Review P95 | Original+review pooled P50 | Pooled P95 |
| --- | ---: | ---: | ---: | ---: |
| medium / edge_batch_1000 | 1.3285 | 0.8218 | 1.1385 | 1.2241 |
| medium / point_lookup_indexed | 0.8944 | 0.7301 | 0.8705 | 0.8762 |
| medium / property_noop | 1.1663 | 1.1010 | 1.0770 | 1.3179 |
| holdout / edge_batch_1000 | 1.1810 | 1.2149 | 1.1306 | 1.1368 |
| holdout / point_lookup_indexed | 0.9535 | 0.9563 | 0.9045 | 1.1598 |

Edge-batch P50 regressed again on both datasets and medium no-op regressed again. Point tails did not reproduce in this window, but holdout pooled P95 still regressed. Keep original30 and review20 samples separately and pooled50, with all five process medians in `b-field-cohort/guard-review-1/combined-review.json`. Twenty direct handle observations remain descriptive, not startup SLO evidence.

Completed engine edge-write bytes rise1,105,921→1,171,780(+5.95%) on medium and1,041,216→1,134,596(+8.97%) on holdout; these are not physical-media writes. Actual property updates rise about1.9%; no-ops write no data. Files grow1.124%/0.885% relative to A and primary-worker peak RSS0.619%/0.287%, below the5% review threshold.

Conversions take12738.77/10051.11 seconds with peak RSS77532/77428KiB. Preparation overlaps, so no conversion speedup is claimed. The16,384-entity/512-mutation oracle passes. A long-lived old pin can retain123,600,896 bytes in the private generic file. A separate test proves retirement protection, release and actual reuse of the same physical slot; it does not prove truncation of that whole generic file.

## Validation and provenance

Relevant tests total97: SCAN62/HYBRID32/ROW3. Ten strengthened cohort cases replace nine, without double counting (`b-cohort-strengthened/receipt.json`). Passing oracles:720 primary,720 original guards,12 original opens,12 untimed writes,360 focused guards and8 focused opens. No full unit suite or primary matrix rerun. Linux was executed; macOS/Windows/WASM were not. No code file exceeds3000 lines.

Original test failure, summary failure and protocol clarification remain recorded in `acceptance.json`. The measured source archive is immutable, with corrected test source archived separately. Engine/input hashes were reverified. Conversion preserves original sources, with no in-place migration or public ABI change. Keep original source files; do not assume older readers lacking experimental SCAN support can open the image.

Further Kuzu work needs a separately registered attribution of this candidate's residual CPU/page/operator cost. The remaining gap alone does not prove that64-owner groups, the tree or the entire backend must be replaced. This round does not append candidates indefinitely or equate stage completion with Kuzu attainment.
