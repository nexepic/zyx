# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | point_node_fetch_by_id | holdout | 10 | 0.2476239751558751 | 0.2152860106434673 | 0.2431744011119008 | 0.2225270145572722 | 0.392896996345371 | 0.392896996345371 | 0.392896996345371 | 4112.274957510158 | ok | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
