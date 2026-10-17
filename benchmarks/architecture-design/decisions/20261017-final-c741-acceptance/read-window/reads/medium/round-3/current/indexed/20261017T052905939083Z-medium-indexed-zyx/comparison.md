# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| point_lookup_indexed | medium | zyx | 11.0367 | 11.0367 | 10.8855 | 1 | 1 |  |  | 12.0549 | 1.09226 | 0.9863 |
| property_equality_indexed | medium | zyx | 196.031 | 196.031 | 195.205 | 1 | 1 |  |  | 200.052 | 1.02051 | 0.995786 |
| property_range_indexed | medium | zyx | 1182.76 | 1182.76 | 1212.17 | 1 | 1 |  |  | 1216.34 | 1.02839 | 1.02487 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
