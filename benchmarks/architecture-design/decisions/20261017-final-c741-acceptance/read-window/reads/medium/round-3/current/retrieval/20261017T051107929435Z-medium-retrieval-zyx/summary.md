# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | batch_node_fetch_100 | medium | 10 | 4.31377 | 4.19958 | 4.415504 | 4.34765 | 4.90428 | 4.90428 | 4.90428 | 226.47471273947437 | ok | api |
| zyx | one_hop_fetch_neighbor_ids | medium | 10 | 3.86756 | 3.33169 | 3.5199309999999997 | 3.42009 | 3.93264 | 3.93264 | 3.93264 | 284.0964780275523 | ok | api |
| zyx | one_hop_fetch_neighbor_records | medium | 10 | 4.89197 | 4.89197 | 5.207088000000001 | 5.14014 | 5.96534 | 5.96534 | 5.96534 | 192.045918947404 | ok | api |
| zyx | point_edge_fetch_by_endpoints | medium | 10 | 7.76184 | 4.07622 | 5.622936 | 4.99445 | 7.76184 | 7.76184 | 7.76184 | 177.84303431516915 | ok | api |
| zyx | point_node_fetch_by_id | medium | 10 | 27.8647 | 26.9178 | 28.421879999999998 | 27.9087 | 29.9288 | 29.9288 | 29.9288 | 35.18416093516685 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
