# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | aggregation_count_by_group | holdout | 10 | 2.9026880220044404 | 2.6413630112074316 | 3.0442794028203934 | 2.939060010248795 | 3.466296009719372 | 3.466296009719372 | 3.466296009719372 | 328.48496070155164 | ok | cypher |
| kuzu | label_scan_filter | holdout | 10 | 2.0077319932170212 | 1.5618460020050406 | 1.7345008003758267 | 1.72404499608092 | 2.0077319932170212 | 2.0077319932170212 | 2.0077319932170212 | 576.5347584638317 | ok | cypher |
| kuzu | topk_property_sort | holdout | 10 | 3.379486995982006 | 3.22467100340873 | 3.5294962988700718 | 3.379486995982006 | 4.01078601134941 | 4.01078601134941 | 4.01078601134941 | 283.326547281021 | ok | cypher |
| kuzu | two_hop_expand | holdout | 10 | 1.061279996065423 | 0.7721649890299886 | 0.9013587899971753 | 0.8534339722245932 | 1.061279996065423 | 1.061279996065423 | 1.061279996065423 | 1109.4361214396476 | ok | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
