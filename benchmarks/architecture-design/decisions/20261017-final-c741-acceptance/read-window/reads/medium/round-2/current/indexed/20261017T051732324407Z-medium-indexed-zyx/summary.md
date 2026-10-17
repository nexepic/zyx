# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | point_lookup_indexed | medium | 10 | 11.1389 | 10.8302 | 11.178830000000001 | 11.1389 | 12.1072 | 12.1072 | 12.1072 | 89.45479983146716 | ok | api |
| zyx | property_equality_indexed | medium | 10 | 197.896 | 197.258 | 202.965 | 201.759 | 215.261 | 215.261 | 215.261 | 4.926957849875594 | ok | api |
| zyx | property_range_indexed | medium | 10 | 1189.37 | 1138.92 | 1164.44 | 1150.79 | 1203.44 | 1203.44 | 1203.44 | 0.8587819037477242 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
