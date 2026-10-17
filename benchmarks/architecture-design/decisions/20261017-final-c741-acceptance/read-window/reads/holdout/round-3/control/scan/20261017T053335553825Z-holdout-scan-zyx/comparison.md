# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | holdout | zyx | 2087.15 | 2087.15 | 1975.11 | 1 | 1 |  |  | 2216.81 | 1.06212 | 0.946319 |
| label_scan_filter | holdout | zyx | 1922.82 | 1922.82 | 1982.88 | 1 | 1 |  |  | 2031.78 | 1.05667 | 1.03124 |
| topk_property_sort | holdout | zyx | 2626.05 | 2626.05 | 2629.79 | 1 | 1 |  |  | 2924.22 | 1.11354 | 1.00142 |
| two_hop_expand | holdout | zyx | 19.0057 | 19.0057 | 18.885 | 1 | 1 |  |  | 19.0822 | 1.00403 | 0.993649 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
