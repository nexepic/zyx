# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | point_node_fetch_by_id | holdout | 10 | 31.5323 | 27.7672 | 29.57593 | 29.3629 | 31.5323 | 31.5323 | 31.5323 | 33.811278292855036 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
