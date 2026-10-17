# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | zyx | 479.716 | 479.716 | 477.334 | 1 | 1 |  |  | 489.513 | 1.02042 | 0.995035 |
| aggregation_group_by | medium | zyx | 488.133 | 488.133 | 501.113 | 1 | 1 |  |  | 501.113 | 1.02659 | 1.02659 |
| all_nodes_property_filter | medium | zyx | 767.968 | 767.968 | 761.529 | 1 | 1 |  |  | 800.839 | 1.0428 | 0.991616 |
| label_multi_property_filter | medium | zyx | 462.195 | 462.195 | 461.191 | 1 | 1 |  |  | 499.253 | 1.08018 | 0.997828 |
| label_scan_filter | medium | zyx | 446.383 | 446.383 | 411.968 | 1 | 1 |  |  | 454.644 | 1.01851 | 0.922903 |
| one_hop_expand | medium | zyx | 2.565 | 2.565 | 2.74709 | 1 | 1 |  |  | 2.79687 | 1.0904 | 1.07099 |
| relationship_property_filter | medium | zyx | 1003.7 | 1003.7 | 966.589 | 1 | 1 |  |  | 1085.77 | 1.08177 | 0.963026 |
| relationship_type_scan | medium | zyx | 344.346 | 344.346 | 335.026 | 1 | 1 |  |  | 350.652 | 1.01831 | 0.972934 |
| topk_property_sort | medium | zyx | 513.011 | 513.011 | 766.706 | 1 | 1 |  |  | 766.706 | 1.49452 | 1.49452 |
| two_hop_expand | medium | zyx | 3.03845 | 3.03845 | 3.27335 | 1 | 1 |  |  | 3.44703 | 1.13447 | 1.07731 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
