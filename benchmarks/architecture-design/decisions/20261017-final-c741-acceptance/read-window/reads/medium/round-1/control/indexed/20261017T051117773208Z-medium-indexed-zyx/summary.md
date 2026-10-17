# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| zyx | point_lookup_indexed | medium | 10 | 10.3364 | 10.3214 | 10.44697 | 10.3454 | 10.9257 | 10.9257 | 10.9257 | 95.7215345693536 | ok | api |
| zyx | property_equality_indexed | medium | 10 | 3423.75 | 3277.16 | 3447.281 | 3423.75 | 3689.15 | 3689.15 | 3689.15 | 0.2900836920459922 | ok | api |
| zyx | property_range_indexed | medium | 10 | 21899.0 | 21286.8 | 21771.55 | 21788.2 | 22066.9 | 22066.9 | 22066.9 | 0.045931502350544634 | ok | api |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
