# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| batch_node_fetch_100 | medium | zyx | 23.3605 | 23.3605 | 23.0618 | 1 | 1 |  |  | 24.8547 | 1.06396 | 0.987213 |
| one_hop_fetch_neighbor_ids | medium | zyx | 21.2402 | 21.2402 | 21.0793 | 1 | 1 |  |  | 22.3549 | 1.05248 | 0.992425 |
| one_hop_fetch_neighbor_records | medium | zyx | 34.085 | 34.085 | 33.9491 | 1 | 1 |  |  | 34.574 | 1.01435 | 0.996013 |
| point_edge_fetch_by_endpoints | medium | zyx | 22.0144 | 22.0144 | 22.636 | 1 | 1 |  |  | 22.9965 | 1.04461 | 1.02824 |
| point_node_fetch_by_id | medium | zyx | 25.645 | 25.645 | 25.8287 | 1 | 1 |  |  | 26.3236 | 1.02646 | 1.00716 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
