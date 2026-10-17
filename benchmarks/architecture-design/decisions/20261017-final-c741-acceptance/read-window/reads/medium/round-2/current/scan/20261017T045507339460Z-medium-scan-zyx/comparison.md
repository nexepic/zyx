# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | medium | zyx | 503.764 | 503.764 | 503.817 | 1 | 1 |  |  | 529.609 | 1.0513 | 1.00011 |
| aggregation_group_by | medium | zyx | 475.915 | 475.915 | 463.267 | 1 | 1 |  |  | 516.13 | 1.0845 | 0.973424 |
| all_nodes_property_filter | medium | zyx | 781.502 | 781.502 | 784.559 | 1 | 1 |  |  | 792.562 | 1.01415 | 1.00391 |
| label_multi_property_filter | medium | zyx | 479.117 | 479.117 | 479.117 | 1 | 1 |  |  | 484.52 | 1.01128 | 1 |
| label_scan_filter | medium | zyx | 418.397 | 418.397 | 422.474 | 1 | 1 |  |  | 467.314 | 1.11692 | 1.00974 |
| one_hop_expand | medium | zyx | 2.40895 | 2.40895 | 2.42212 | 1 | 1 |  |  | 2.67813 | 1.11174 | 1.00547 |
| relationship_property_filter | medium | zyx | 966.955 | 966.955 | 967.688 | 1 | 1 |  |  | 1015.37 | 1.05007 | 1.00076 |
| relationship_type_scan | medium | zyx | 351.724 | 351.724 | 330.23 | 1 | 1 |  |  | 397.652 | 1.13058 | 0.93889 |
| topk_property_sort | medium | zyx | 520.802 | 520.802 | 539.579 | 1 | 1 |  |  | 565.582 | 1.08598 | 1.03605 |
| two_hop_expand | medium | zyx | 2.99008 | 2.99008 | 2.86692 | 1 | 1 |  |  | 3.30359 | 1.10485 | 0.95881 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
