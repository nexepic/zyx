# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| batch_node_fetch_100 | medium | zyx | 24.6898 | 24.6898 | 25.3634 | 1 | 1 |  |  | 29.5903 | 1.19848 | 1.02728 |
| one_hop_fetch_neighbor_ids | medium | zyx | 22.0736 | 22.0736 | 21.611 | 1 | 1 |  |  | 25.6785 | 1.16331 | 0.979043 |
| one_hop_fetch_neighbor_records | medium | zyx | 34.1418 | 34.1418 | 32.8051 | 1 | 1 |  |  | 36.7003 | 1.07494 | 0.960849 |
| point_edge_fetch_by_endpoints | medium | zyx | 22.7736 | 22.7736 | 23.3365 | 1 | 1 |  |  | 24.7239 | 1.08564 | 1.02472 |
| point_node_fetch_by_id | medium | zyx | 26.5288 | 26.5288 | 27.3138 | 1 | 1 |  |  | 29.2022 | 1.10077 | 1.02959 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
