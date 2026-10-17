# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | kuzu | 3.53592 |  |  |  |  | kuzu |  |  |  |  |
| aggregation_group_by | medium | kuzu | 3.37645 |  |  |  |  | kuzu |  |  |  |  |
| all_nodes_property_filter | medium | kuzu | 0.971779 |  |  |  |  | kuzu |  |  |  |  |
| label_multi_property_filter | medium | kuzu | 2.36213 |  |  |  |  | kuzu |  |  |  |  |
| label_scan_filter | medium | kuzu | 2.30468 |  |  |  |  | kuzu |  |  |  |  |
| one_hop_expand | medium | kuzu | 0.400939 |  |  |  |  | kuzu |  |  |  |  |
| relationship_property_filter | medium | kuzu | 11.5491 |  |  |  |  | kuzu |  |  |  |  |
| relationship_type_scan | medium | kuzu | 6.37906 |  |  |  |  | kuzu |  |  |  |  |
| topk_property_sort | medium | kuzu | 4.06151 |  |  |  |  | kuzu |  |  |  |  |
| two_hop_expand | medium | kuzu | 0.7884 |  |  |  |  | kuzu |  |  |  |  |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
