# 横向性能差距分析

## ZYX vs Best

| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| batch_node_fetch_100 | medium | zyx | 4.34765 | 4.34765 | 4.31377 | 1 | 1 |  |  | 4.90428 | 1.12803 | 0.992207 |
| one_hop_fetch_neighbor_ids | medium | zyx | 3.42009 | 3.42009 | 3.86756 | 1 | 1 |  |  | 3.93264 | 1.14986 | 1.13084 |
| one_hop_fetch_neighbor_records | medium | zyx | 5.14014 | 5.14014 | 4.89197 | 1 | 1 |  |  | 5.96534 | 1.16054 | 0.951719 |
| point_edge_fetch_by_endpoints | medium | zyx | 4.99445 | 4.99445 | 7.76184 | 1 | 1 |  |  | 7.76184 | 1.55409 | 1.55409 |
| point_node_fetch_by_id | medium | zyx | 27.9087 | 27.9087 | 27.8647 | 1 | 1 |  |  | 29.9288 | 1.07238 | 0.998423 |

## Reading Guide

- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.
- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.
- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.
- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.
- The comparison uses p50 latency and only includes successful rows with at least one sample.
