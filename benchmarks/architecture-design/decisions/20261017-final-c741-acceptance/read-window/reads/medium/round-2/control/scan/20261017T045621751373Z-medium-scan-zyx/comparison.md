# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | zyx | 2856.7 | 2856.7 | 2856.7 | 1 | 1 |  |  | 2964.46 | 1.03772 | 1 |
| aggregation_group_by | medium | zyx | 2848.65 | 2848.65 | 2886.41 | 1 | 1 |  |  | 2973.22 | 1.04373 | 1.01326 |
| all_nodes_property_filter | medium | zyx | 5752.36 | 5752.36 | 5649.97 | 1 | 1 |  |  | 5982.1 | 1.03994 | 0.9822 |
| label_multi_property_filter | medium | zyx | 2844.57 | 2844.57 | 2862.75 | 1 | 1 |  |  | 2926.41 | 1.02877 | 1.00639 |
| label_scan_filter | medium | zyx | 2831.26 | 2831.26 | 2793.12 | 1 | 1 |  |  | 2972.83 | 1.05 | 0.986529 |
| one_hop_expand | medium | zyx | 15.4112 | 15.4112 | 15.474 | 1 | 1 |  |  | 15.7348 | 1.021 | 1.00407 |
| relationship_property_filter | medium | zyx | 6411.19 | 6411.19 | 6451.29 | 1 | 1 |  |  | 6727.32 | 1.04931 | 1.00625 |
| relationship_type_scan | medium | zyx | 581.053 | 581.053 | 562.634 | 1 | 1 |  |  | 599.829 | 1.03231 | 0.968301 |
| topk_property_sort | medium | zyx | 3570.12 | 3570.12 | 3569.97 | 1 | 1 |  |  | 3681.93 | 1.03132 | 0.999958 |
| two_hop_expand | medium | zyx | 19.701 | 19.701 | 19.6698 | 1 | 1 |  |  | 22.7434 | 1.15443 | 0.998416 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
