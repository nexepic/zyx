# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | zyx | 477.461 | 477.461 | 490.908 | 1 | 1 |  |  | 493.15 | 1.03286 | 1.02816 |
| aggregation_group_by | medium | zyx | 483.485 | 483.485 | 475.703 | 1 | 1 |  |  | 523.358 | 1.08247 | 0.983904 |
| all_nodes_property_filter | medium | zyx | 772.276 | 772.276 | 778.773 | 1 | 1 |  |  | 782.609 | 1.01338 | 1.00841 |
| label_multi_property_filter | medium | zyx | 466.998 | 466.998 | 481.274 | 1 | 1 |  |  | 487.902 | 1.04476 | 1.03057 |
| label_scan_filter | medium | zyx | 412.104 | 412.104 | 409.321 | 1 | 1 |  |  | 415.294 | 1.00774 | 0.993247 |
| one_hop_expand | medium | zyx | 2.4249 | 2.4249 | 2.4349 | 1 | 1 |  |  | 2.68043 | 1.10538 | 1.00412 |
| relationship_property_filter | medium | zyx | 1005.62 | 1005.62 | 986.311 | 1 | 1 |  |  | 1034.96 | 1.02918 | 0.980799 |
| relationship_type_scan | medium | zyx | 350.366 | 350.366 | 332.01 | 1 | 1 |  |  | 537.996 | 1.53553 | 0.947609 |
| topk_property_sort | medium | zyx | 504.402 | 504.402 | 504.402 | 1 | 1 |  |  | 519.571 | 1.03007 | 1 |
| two_hop_expand | medium | zyx | 3.17079 | 3.17079 | 3.16079 | 1 | 1 |  |  | 3.52188 | 1.11073 | 0.996846 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
