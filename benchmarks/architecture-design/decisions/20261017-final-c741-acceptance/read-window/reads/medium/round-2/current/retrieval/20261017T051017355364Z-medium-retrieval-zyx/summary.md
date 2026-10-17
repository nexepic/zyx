# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | batch_node_fetch_100 | medium | 10 | 4.06068 | 3.93124 | 3.9958929999999997 | 3.94926 | 4.15578 | 4.15578 | 4.15578 | 250.25695132477273 | ok | api |
| zyx | one_hop_fetch_neighbor_ids | medium | 10 | 3.62625 | 3.03306 | 3.2378 | 3.13429 | 3.62625 | 3.62625 | 3.62625 | 308.8516894187411 | ok | api |
| zyx | one_hop_fetch_neighbor_records | medium | 10 | 4.57907 | 4.55902 | 4.678783 | 4.6089 | 4.88344 | 4.88344 | 4.88344 | 213.73079281514018 | ok | api |
| zyx | point_edge_fetch_by_endpoints | medium | 10 | 7.35515 | 4.0015 | 5.361933 | 4.41314 | 7.35515 | 7.35515 | 7.35515 | 186.49990591079748 | ok | api |
| zyx | point_node_fetch_by_id | medium | 10 | 25.6024 | 25.3465 | 25.820299999999996 | 25.7091 | 26.3721 | 26.3721 | 26.3721 | 38.72921693396282 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
