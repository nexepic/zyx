# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | holdout | zyx | 2111.51 | 2111.51 | 2026.45 | 1 | 1 |  |  | 2182.44 | 1.03359 | 0.959716 |
| label_scan_filter | holdout | zyx | 1913.44 | 1913.44 | 2053.81 | 1 | 1 |  |  | 2053.81 | 1.07336 | 1.07336 |
| topk_property_sort | holdout | zyx | 2461.54 | 2461.54 | 2543.96 | 1 | 1 |  |  | 2555.24 | 1.03807 | 1.03348 |
| two_hop_expand | holdout | zyx | 20.3255 | 20.3255 | 20.0662 | 1 | 1 |  |  | 20.5998 | 1.0135 | 0.987243 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
