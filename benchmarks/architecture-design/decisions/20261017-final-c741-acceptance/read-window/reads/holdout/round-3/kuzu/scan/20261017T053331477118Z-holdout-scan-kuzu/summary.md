# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | aggregation_count_by_group | holdout | 10 | 2.640491002239287 | 2.496429020538926 | 2.5528360944008455 | 2.543608978157863 | 2.640491002239287 | 2.640491002239287 | 2.640491002239287 | 391.72119283071385 | ok | cypher |
| kuzu | label_scan_filter | holdout | 10 | 1.6930479905568063 | 1.5784899878781289 | 1.6909353027585894 | 1.6546620172448456 | 1.9263520080130547 | 1.9263520080130547 | 1.9263520080130547 | 591.3886819729894 | ok | cypher |
| kuzu | topk_property_sort | holdout | 10 | 2.9107779846526682 | 2.9107779846526682 | 3.159139200579375 | 3.141016000881791 | 3.385465999599546 | 3.385465999599546 | 3.385465999599546 | 316.5419237672729 | ok | cypher |
| kuzu | two_hop_expand | holdout | 10 | 0.7984930125530809 | 0.7473780133295804 | 0.8479220065055415 | 0.8528130128979683 | 0.9709570149425417 | 0.9709570149425417 | 0.9709570149425417 | 1179.353752264554 | ok | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
