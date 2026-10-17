# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| batch_node_fetch_100 | medium | zyx | 3.94926 | 3.94926 | 4.06068 | 1 | 1 |  |  | 4.15578 | 1.05229 | 1.02821 |
| one_hop_fetch_neighbor_ids | medium | zyx | 3.13429 | 3.13429 | 3.62625 | 1 | 1 |  |  | 3.62625 | 1.15696 | 1.15696 |
| one_hop_fetch_neighbor_records | medium | zyx | 4.6089 | 4.6089 | 4.57907 | 1 | 1 |  |  | 4.88344 | 1.05957 | 0.993528 |
| point_edge_fetch_by_endpoints | medium | zyx | 4.41314 | 4.41314 | 7.35515 | 1 | 1 |  |  | 7.35515 | 1.66665 | 1.66665 |
| point_node_fetch_by_id | medium | zyx | 25.7091 | 25.7091 | 25.6024 | 1 | 1 |  |  | 26.3721 | 1.02579 | 0.99585 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
