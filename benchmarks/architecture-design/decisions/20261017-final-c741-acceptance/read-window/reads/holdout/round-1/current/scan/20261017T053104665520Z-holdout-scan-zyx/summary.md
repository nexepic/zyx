# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | aggregation_count_by_group | holdout | 10 | 378.366 | 376.2 | 390.7113 | 388.884 | 404.973 | 404.973 | 404.973 | 2.5594345492439046 | ok | api |
| zyx | label_scan_filter | holdout | 10 | 309.397 | 301.608 | 308.8002 | 308.621 | 320.906 | 320.906 | 320.906 | 3.238339871541534 | ok | api |
| zyx | topk_property_sort | holdout | 10 | 388.44 | 388.44 | 400.1329 | 390.294 | 482.457 | 482.457 | 482.457 | 2.499169650883494 | ok | api |
| zyx | two_hop_expand | holdout | 10 | 2.9782 | 2.9782 | 3.294941 | 3.28386 | 3.63946 | 3.63946 | 3.63946 | 303.49557093738554 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
