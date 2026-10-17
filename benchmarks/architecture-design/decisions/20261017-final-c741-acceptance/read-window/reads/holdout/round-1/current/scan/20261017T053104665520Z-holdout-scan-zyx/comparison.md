# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | holdout | zyx | 388.884 | 388.884 | 378.366 | 1 | 1 |  |  | 404.973 | 1.04137 | 0.972953 |
| label_scan_filter | holdout | zyx | 308.621 | 308.621 | 309.397 | 1 | 1 |  |  | 320.906 | 1.03981 | 1.00251 |
| topk_property_sort | holdout | zyx | 390.294 | 390.294 | 388.44 | 1 | 1 |  |  | 482.457 | 1.23614 | 0.99525 |
| two_hop_expand | holdout | zyx | 3.28386 | 3.28386 | 2.9782 | 1 | 1 |  |  | 3.63946 | 1.10829 | 0.906921 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
