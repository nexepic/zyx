# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | kuzu | 3.41162 |  |  |  |  | kuzu |  |  |  |  |
| aggregation_group_by | medium | kuzu | 3.32701 |  |  |  |  | kuzu |  |  |  |  |
| all_nodes_property_filter | medium | kuzu | 0.79571 |  |  |  |  | kuzu |  |  |  |  |
| label_multi_property_filter | medium | kuzu | 2.16767 |  |  |  |  | kuzu |  |  |  |  |
| label_scan_filter | medium | kuzu | 2.09495 |  |  |  |  | kuzu |  |  |  |  |
| one_hop_expand | medium | kuzu | 0.355743 |  |  |  |  | kuzu |  |  |  |  |
| relationship_property_filter | medium | kuzu | 10.5248 |  |  |  |  | kuzu |  |  |  |  |
| relationship_type_scan | medium | kuzu | 6.04734 |  |  |  |  | kuzu |  |  |  |  |
| topk_property_sort | medium | kuzu | 3.75089 |  |  |  |  | kuzu |  |  |  |  |
| two_hop_expand | medium | kuzu | 0.811173 |  |  |  |  | kuzu |  |  |  |  |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
