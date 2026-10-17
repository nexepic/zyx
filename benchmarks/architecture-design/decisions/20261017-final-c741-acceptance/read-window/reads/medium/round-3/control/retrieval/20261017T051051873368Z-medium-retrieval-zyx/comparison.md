# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| batch_node_fetch_100 | medium | zyx | 23.5466 | 23.5466 | 23.9232 | 1 | 1 |  |  | 24.3358 | 1.03352 | 1.01599 |
| one_hop_fetch_neighbor_ids | medium | zyx | 21.6089 | 21.6089 | 21.9338 | 1 | 1 |  |  | 22.8761 | 1.05864 | 1.01504 |
| one_hop_fetch_neighbor_records | medium | zyx | 34.0407 | 34.0407 | 34.2979 | 1 | 1 |  |  | 35.135 | 1.03215 | 1.00756 |
| point_edge_fetch_by_endpoints | medium | zyx | 21.4054 | 21.4054 | 21.3924 | 1 | 1 |  |  | 27.4574 | 1.28273 | 0.999393 |
| point_node_fetch_by_id | medium | zyx | 24.8382 | 24.8382 | 26.7915 | 1 | 1 |  |  | 26.7915 | 1.07864 | 1.07864 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
