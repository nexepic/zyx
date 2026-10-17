# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| point_lookup_indexed | medium | zyx | 10.0308 | 10.0308 | 10.2759 | 1 | 1 |  |  | 10.2759 | 1.02443 | 1.02443 |
| property_equality_indexed | medium | zyx | 3129.23 | 3129.23 | 3186.1 | 1 | 1 |  |  | 3402.78 | 1.08742 | 1.01817 |
| property_range_indexed | medium | zyx | 20770 | 20770 | 20541.3 | 1 | 1 |  |  | 21636.2 | 1.0417 | 0.988989 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
