# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| batch_node_fetch_100 | medium | zyx | 3.97248 | 3.97248 | 3.99674 | 1 | 1 |  |  | 4.5373 | 1.14218 | 1.00611 |
| one_hop_fetch_neighbor_ids | medium | zyx | 3.17125 | 3.17125 | 3.28289 | 1 | 1 |  |  | 3.57917 | 1.12863 | 1.0352 |
| one_hop_fetch_neighbor_records | medium | zyx | 4.63224 | 4.63224 | 4.63224 | 1 | 1 |  |  | 4.81344 | 1.03912 | 1 |
| point_edge_fetch_by_endpoints | medium | zyx | 4.8617 | 4.8617 | 7.94776 | 1 | 1 |  |  | 7.94776 | 1.63477 | 1.63477 |
| point_node_fetch_by_id | medium | zyx | 25.7335 | 25.7335 | 25.6681 | 1 | 1 |  |  | 27.3377 | 1.06234 | 0.997459 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
