# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | point_lookup_indexed | medium | 10 | 10.8855 | 10.8287 | 11.23133 | 11.0367 | 12.0549 | 12.0549 | 12.0549 | 89.03665015630384 | ok | api |
| zyx | property_equality_indexed | medium | 10 | 195.205 | 194.48 | 196.7912 | 196.031 | 200.052 | 200.052 | 200.052 | 5.081528035806479 | ok | api |
| zyx | property_range_indexed | medium | 10 | 1212.17 | 1149.31 | 1183.3899999999999 | 1182.76 | 1216.34 | 1216.34 | 1216.34 | 0.8450299563119513 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
