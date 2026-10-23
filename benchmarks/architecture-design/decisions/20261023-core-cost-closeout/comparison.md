# Final measured comparison / 最终实测对比

All times are milliseconds. Lower is faster. Speedup is old ZYX/new ZYX; ZYX/Kuzu above1 means ZYX is slower. All99 registered workers are retained. Cold-layout adoption remains disabled; these are the actual default-format production paths.

| Dataset | Workload | Before P50 | Final P50 | Kuzu equivalent P50 | Speedup | ZYX/Kuzu |
|---|---|---:|---:|---:|---:|---:|
| holdout | edge_batch_1000 | 379.906 | 30.644 | 4.868 | 12.397 | 6.295 |
| holdout | edge_create_delete | 316.901 | 23.174 | 4.741 | 13.675 | 4.888 |
| holdout | fresh_import | 213737.781 | 33692.129 | 31062.727 | 6.344 | 1.085 |
| holdout | property_noop | 2.326 | 2.274 | 1.300 | 1.023 | 1.749 |
| holdout | property_set | 60.304 | 4.660 | 1.245 | 12.941 | 3.744 |
| holdout | graph_full_neighbors | 1.380 | 1.287 | 0.863 | 1.072 | 1.492 |
| holdout | graph_full_point | 0.842 | 0.794 | 0.209 | 1.060 | 3.793 |
| holdout | point_lookup_indexed | 0.645 | 0.600 | 0.174 | 1.075 | 3.442 |
| holdout | aggregation_count_by_group | 89.787 | 74.522 | 1.878 | 1.205 | 39.676 |
| holdout | all_nodes_property_filter | 161.142 | 120.862 | 0.394 | 1.333 | 307.025 |
| holdout | label_scan_filter | 89.077 | 74.050 | 1.161 | 1.203 | 63.779 |
| holdout | multi_key_group | 113.617 | 94.055 | 2.692 | 1.208 | 34.939 |
| holdout | one_hop_expand | 0.920 | 0.861 | 0.517 | 1.068 | 1.666 |
| holdout | two_hop_expand | 1.088 | 1.015 | 1.048 | 1.072 | 0.969 |
| large | large_patch | 4.511 | 3.758 | N/A | 1.201 | N/A |
| large | large_full | 1.279 | 1.179 | N/A | 1.084 | N/A |
| large | large_selected | 1.086 | 1.010 | N/A | 1.075 | N/A |
| medium | edge_batch_1000 | 397.313 | 32.649 | 4.836 | 12.169 | 6.751 |
| medium | edge_create_delete | 344.080 | 24.302 | 4.595 | 14.159 | 5.288 |
| medium | fresh_import | 276364.335 | 41195.026 | 37527.360 | 6.709 | 1.098 |
| medium | property_noop | 2.377 | 2.241 | 1.251 | 1.061 | 1.791 |
| medium | property_set | 68.031 | 4.845 | 1.260 | 14.040 | 3.846 |
| medium | graph_full_neighbors | 1.327 | 1.257 | 0.646 | 1.055 | 1.947 |
| medium | graph_full_point | 0.863 | 0.798 | 0.212 | 1.082 | 3.770 |
| medium | point_lookup_indexed | 0.644 | 0.601 | 0.176 | 1.071 | 3.405 |
| medium | aggregation_count_by_group | 122.571 | 100.570 | 2.441 | 1.219 | 41.199 |
| medium | all_nodes_property_filter | 213.944 | 164.512 | 0.411 | 1.300 | 399.798 |
| medium | label_scan_filter | 124.777 | 100.149 | 1.406 | 1.246 | 71.254 |
| medium | multi_key_group | 154.346 | 127.540 | 3.358 | 1.210 | 37.977 |
| medium | one_hop_expand | 0.917 | 0.850 | 0.536 | 1.079 | 1.586 |
| medium | two_hop_expand | 1.065 | 0.991 | 1.067 | 1.074 | 0.929 |
| skew | skew_both | 0.668 | 0.639 | N/A | 1.046 | N/A |
| skew | skew_out | 0.573 | 0.553 | N/A | 1.035 | N/A |
| skew | skew_twohop | 0.952 | 0.932 | N/A | 1.021 | N/A |

## Kuzu production COPY (different transaction boundary)

| Dataset | ZYX production P50 | Kuzu COPY P50 | ZYX/Kuzu |
|---|---:|---:|---:|
| medium | 41195.026 | 887.417 | 46.421 |
| holdout | 33692.129 | 784.591 | 42.942 |

- Queries/writes:30 observations from three independent processes, three warmups and ten measurements each. See per-process.csv; no slow samples removed.
- Imports:three independent imports. Their maximum is not a reliable P95. The equivalent import control reopens Kuzu at the same1000-row transaction boundary; COPY is displayed separately.
- comparison.csv includes baseline/current P95, sample counts and both old/current Kuzu controls. resources.csv records all process CPU/RSS/OS I/O and observed file peaks. Parent polling can miss short-lived file peaks; do not interpret it as an exact allocation census.
- Large/skew fixtures have no Kuzu-equivalent result and remain N/A; current/prospective support must not be invented.
- Baseline was frozen before these changes; absolute before/after comparisons can include shared-host variation. The controls were rerun with the final candidate and the unchanged registered ordering/limits.
- Linux only. No three-platform or full-unit-suite claim. Historical CPU percentages are not a new profile.
- P7/Kuzu objective remains unmet. Acceptance stays90%; report completion does not earn the last10% or authorize commit/push.
