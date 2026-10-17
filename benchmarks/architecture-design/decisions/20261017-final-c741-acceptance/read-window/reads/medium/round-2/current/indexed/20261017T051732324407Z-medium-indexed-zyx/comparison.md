# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| point_lookup_indexed | medium | zyx | 11.1389 | 11.1389 | 11.1389 | 1 | 1 |  |  | 12.1072 | 1.08693 | 1 |
| property_equality_indexed | medium | zyx | 201.759 | 201.759 | 197.896 | 1 | 1 |  |  | 215.261 | 1.06692 | 0.980853 |
| property_range_indexed | medium | zyx | 1150.79 | 1150.79 | 1189.37 | 1 | 1 |  |  | 1203.44 | 1.04575 | 1.03352 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
