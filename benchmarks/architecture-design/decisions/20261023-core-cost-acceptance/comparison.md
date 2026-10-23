# Final measured comparison / 最终实测对比

All times are milliseconds. Lower is faster. Speedup is old ZYX/new ZYX; ZYX/Kuzu above1 means ZYX is slower. All99 registered workers are retained. Cold-layout adoption remains disabled; these are the actual default-format production paths.

| Dataset | Workload | Before P50 | Final P50 | Kuzu equivalent P50 | Speedup | ZYX/Kuzu |
|---|---|---:|---:|---:|---:|---:|
| holdout | edge_batch_1000 | 379.906 | 31.363 | 4.841 | 12.113 | 6.479 |
| holdout | edge_create_delete | 316.901 | 23.476 | 4.574 | 13.499 | 5.133 |
| holdout | fresh_import | 213737.781 | 33776.498 | 31436.480 | 6.328 | 1.074 |
| holdout | property_noop | 2.326 | 2.290 | 1.240 | 1.016 | 1.847 |
| holdout | property_set | 60.304 | 4.774 | 1.189 | 12.632 | 4.014 |
| holdout | graph_full_neighbors | 1.380 | 1.332 | 0.795 | 1.036 | 1.676 |
| holdout | graph_full_point | 0.842 | 0.792 | 0.188 | 1.063 | 4.205 |
| holdout | point_lookup_indexed | 0.645 | 0.595 | 0.159 | 1.084 | 3.748 |
| holdout | aggregation_count_by_group | 89.787 | 76.277 | 1.880 | 1.177 | 40.563 |
| holdout | all_nodes_property_filter | 161.142 | 119.399 | 0.391 | 1.350 | 305.610 |
| holdout | label_scan_filter | 89.077 | 74.647 | 1.139 | 1.193 | 65.549 |
| holdout | multi_key_group | 113.617 | 96.326 | 2.746 | 1.179 | 35.077 |
| holdout | one_hop_expand | 0.920 | 0.874 | 0.539 | 1.052 | 1.621 |
| holdout | two_hop_expand | 1.088 | 1.011 | 1.059 | 1.076 | 0.955 |
| large | large_patch | 4.511 | 3.794 | N/A | 1.189 | N/A |
| large | large_full | 1.279 | 1.189 | N/A | 1.075 | N/A |
| large | large_selected | 1.086 | 1.055 | N/A | 1.029 | N/A |
| medium | edge_batch_1000 | 397.313 | 34.434 | 5.047 | 11.539 | 6.823 |
| medium | edge_create_delete | 344.080 | 24.766 | 4.598 | 13.893 | 5.387 |
| medium | fresh_import | 276364.335 | 41266.549 | 39028.662 | 6.697 | 1.057 |
| medium | property_noop | 2.377 | 2.289 | 1.307 | 1.038 | 1.752 |
| medium | property_set | 68.031 | 4.788 | 1.287 | 14.207 | 3.721 |
| medium | graph_full_neighbors | 1.327 | 1.255 | 0.903 | 1.057 | 1.390 |
| medium | graph_full_point | 0.863 | 0.826 | 0.206 | 1.044 | 4.019 |
| medium | point_lookup_indexed | 0.644 | 0.616 | 0.182 | 1.044 | 3.386 |
| medium | aggregation_count_by_group | 122.571 | 102.146 | 2.471 | 1.200 | 41.345 |
| medium | all_nodes_property_filter | 213.944 | 164.038 | 0.426 | 1.304 | 385.349 |
| medium | label_scan_filter | 124.777 | 101.542 | 1.452 | 1.229 | 69.943 |
| medium | multi_key_group | 154.346 | 126.940 | 3.471 | 1.216 | 36.576 |
| medium | one_hop_expand | 0.917 | 0.854 | 0.542 | 1.074 | 1.576 |
| medium | two_hop_expand | 1.065 | 1.015 | 1.079 | 1.050 | 0.940 |
| skew | skew_both | 0.668 | 0.643 | N/A | 1.039 | N/A |
| skew | skew_out | 0.573 | 0.569 | N/A | 1.005 | N/A |
| skew | skew_twohop | 0.952 | 0.942 | N/A | 1.010 | N/A |

## Kuzu production COPY (different transaction boundary)

| Dataset | ZYX production P50 | Kuzu COPY P50 | ZYX/Kuzu |
|---|---:|---:|---:|
| medium | 41266.549 | 928.086 | 44.464 |
| holdout | 33776.498 | 797.391 | 42.359 |

- Queries/writes:30 observations from three independent processes, three warmups and ten measurements each. See per-process.csv; no slow samples removed.
- Imports:three independent imports. Their maximum is not a reliable P95. The equivalent import control reopens Kuzu at the same1000-row transaction boundary; COPY is displayed separately.
- comparison.csv includes baseline/current P95, sample counts and both old/current Kuzu controls. resources.csv records all process CPU/RSS/OS I/O and observed file peaks. Parent polling can miss short-lived file peaks; do not interpret it as an exact allocation census.
- Large/skew fixtures have no Kuzu-equivalent result and remain N/A; current/prospective support must not be invented.
- Baseline was frozen before these changes; absolute before/after comparisons can include shared-host variation. The controls were rerun with the final candidate and the unchanged registered ordering/limits.
- Linux only. No three-platform or full-unit-suite claim. Historical CPU percentages are not a new profile.
- Implementation acceptance is100% under the updated user instruction; this does not assert5x benefits or Kuzu superiority. Public cold conversion is available in the explicit experimental format; formal timings use default-format production paths without automatic adoption.
