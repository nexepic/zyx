# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| point_lookup_indexed | medium | zyx | 11.6981 | 11.6981 | 11.5968 | 1 | 1 |  |  | 13.6188 | 1.16419 | 0.99134 |
| property_equality_indexed | medium | zyx | 200.015 | 200.015 | 196.388 | 1 | 1 |  |  | 257.088 | 1.28534 | 0.981866 |
| property_range_indexed | medium | zyx | 1175.62 | 1175.62 | 1174.36 | 1 | 1 |  |  | 1223.91 | 1.04108 | 0.998928 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
