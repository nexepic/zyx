# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | zyx | 2910.79 | 2910.79 | 2867.62 | 1 | 1 |  |  | 3095.01 | 1.06329 | 0.985169 |
| aggregation_group_by | medium | zyx | 2878.46 | 2878.46 | 2992.49 | 1 | 1 |  |  | 3039 | 1.05577 | 1.03961 |
| all_nodes_property_filter | medium | zyx | 5490.81 | 5490.81 | 5783.86 | 1 | 1 |  |  | 5783.86 | 1.05337 | 1.05337 |
| label_multi_property_filter | medium | zyx | 2815.06 | 2815.06 | 2954.92 | 1 | 1 |  |  | 3005.1 | 1.06751 | 1.04968 |
| label_scan_filter | medium | zyx | 2789.81 | 2789.81 | 2714.24 | 1 | 1 |  |  | 2918.14 | 1.046 | 0.972912 |
| one_hop_expand | medium | zyx | 16.0083 | 16.0083 | 16.0083 | 1 | 1 |  |  | 17.0726 | 1.06648 | 1 |
| relationship_property_filter | medium | zyx | 6425.36 | 6425.36 | 6761.79 | 1 | 1 |  |  | 6898.05 | 1.07357 | 1.05236 |
| relationship_type_scan | medium | zyx | 594.774 | 594.774 | 613.295 | 1 | 1 |  |  | 639.29 | 1.07485 | 1.03114 |
| topk_property_sort | medium | zyx | 3579.33 | 3579.33 | 3449.06 | 1 | 1 |  |  | 3701.14 | 1.03403 | 0.963605 |
| two_hop_expand | medium | zyx | 22.6093 | 22.6093 | 20.8966 | 1 | 1 |  |  | 35.2221 | 1.55786 | 0.924248 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
