# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | holdout | zyx | 380.535 | 380.535 | 377.742 | 1 | 1 |  |  | 403.999 | 1.06166 | 0.99266 |
| label_scan_filter | holdout | zyx | 302.639 | 302.639 | 361.507 | 1 | 1 |  |  | 361.507 | 1.19452 | 1.19452 |
| topk_property_sort | holdout | zyx | 382.071 | 382.071 | 412.779 | 1 | 1 |  |  | 412.779 | 1.08037 | 1.08037 |
| two_hop_expand | holdout | zyx | 3.24737 | 3.24737 | 3.29332 | 1 | 1 |  |  | 4.27421 | 1.31621 | 1.01415 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
