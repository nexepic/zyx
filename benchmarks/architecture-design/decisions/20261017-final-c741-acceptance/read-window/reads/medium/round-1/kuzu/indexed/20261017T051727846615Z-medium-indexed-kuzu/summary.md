# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | point_lookup_indexed | medium | 10 | 0.3640649956651032 | 0.19877197337336838 | 0.23815209860913455 | 0.20555200171656907 | 0.3640649956651032 | 0.3640649956651032 | 0.3640649956651032 | 4198.997220012925 | ok | cypher |
| kuzu | property_equality_indexed | medium | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | unsupported | cypher |
| kuzu | property_range_indexed | medium | 0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | unsupported | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
