# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | zyx | 2863.76 | 2863.76 | 2841.79 | 1 | 1 |  |  | 3008.89 | 1.05068 | 0.992328 |
| aggregation_group_by | medium | zyx | 2871.31 | 2871.31 | 2890.39 | 1 | 1 |  |  | 3003.86 | 1.04616 | 1.00665 |
| all_nodes_property_filter | medium | zyx | 5384.85 | 5384.85 | 5438.83 | 1 | 1 |  |  | 5664.93 | 1.05201 | 1.01002 |
| label_multi_property_filter | medium | zyx | 2831.57 | 2831.57 | 2831.57 | 1 | 1 |  |  | 2952.97 | 1.04287 | 1 |
| label_scan_filter | medium | zyx | 2757.28 | 2757.28 | 2710.68 | 1 | 1 |  |  | 2875.86 | 1.04301 | 0.983099 |
| one_hop_expand | medium | zyx | 15.8609 | 15.8609 | 15.5491 | 1 | 1 |  |  | 16.5817 | 1.04545 | 0.980342 |
| relationship_property_filter | medium | zyx | 6332.1 | 6332.1 | 6335.54 | 1 | 1 |  |  | 6559.93 | 1.03598 | 1.00054 |
| relationship_type_scan | medium | zyx | 572.783 | 572.783 | 580.189 | 1 | 1 |  |  | 587.744 | 1.02612 | 1.01293 |
| topk_property_sort | medium | zyx | 3605.52 | 3605.52 | 3568.37 | 1 | 1 |  |  | 3798.63 | 1.05356 | 0.989696 |
| two_hop_expand | medium | zyx | 20.4291 | 20.4291 | 20.759 | 1 | 1 |  |  | 21.1682 | 1.03618 | 1.01615 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
