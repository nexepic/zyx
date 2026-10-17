# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| aggregation_count_by_group | holdout | zyx | 374.247 | 374.247 | 377.92 | 1 | 1 |  |  | 387.806 | 1.03623 | 1.00981 |
| label_scan_filter | holdout | zyx | 305.055 | 305.055 | 303.493 | 1 | 1 |  |  | 325.874 | 1.06825 | 0.99488 |
| topk_property_sort | holdout | zyx | 380.417 | 380.417 | 389.237 | 1 | 1 |  |  | 425.507 | 1.11853 | 1.02319 |
| two_hop_expand | holdout | zyx | 3.2568 | 3.2568 | 3.34687 | 1 | 1 |  |  | 3.61307 | 1.10939 | 1.02766 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
