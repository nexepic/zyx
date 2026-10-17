# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | batch_node_fetch_100 | medium | 10 | 23.0618 | 22.5931 | 23.48169 | 23.3605 | 24.8547 | 24.8547 | 24.8547 | 42.586372616281025 | ok | api |
| zyx | one_hop_fetch_neighbor_ids | medium | 10 | 21.0793 | 20.7768 | 21.354779999999998 | 21.2402 | 22.3549 | 22.3549 | 22.3549 | 46.8279233033541 | ok | api |
| zyx | one_hop_fetch_neighbor_records | medium | 10 | 33.9491 | 33.1928 | 34.027570000000004 | 34.085 | 34.574 | 34.574 | 34.574 | 29.38793454836769 | ok | api |
| zyx | point_edge_fetch_by_endpoints | medium | 10 | 22.636 | 21.8142 | 22.23678 | 22.0144 | 22.9965 | 22.9965 | 22.9965 | 44.97053979937743 | ok | api |
| zyx | point_node_fetch_by_id | medium | 10 | 25.8287 | 24.902 | 25.703660000000003 | 25.645 | 26.3236 | 26.3236 | 26.3236 | 38.90496528509947 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
