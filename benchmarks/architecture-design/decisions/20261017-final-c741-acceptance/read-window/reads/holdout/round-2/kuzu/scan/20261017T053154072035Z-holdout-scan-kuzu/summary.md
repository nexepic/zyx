# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | aggregation_count_by_group | holdout | 10 | 2.490782004315406 | 2.480266004567966 | 2.5598291016649455 | 2.500054979464039 | 2.733548986725509 | 2.733548986725509 | 2.733548986725509 | 390.65107875740114 | ok | cypher |
| kuzu | label_scan_filter | holdout | 10 | 1.5962659963406622 | 1.5308310103137046 | 1.567009705468081 | 1.5514009865000844 | 1.6448980022687465 | 1.6448980022687465 | 1.6448980022687465 | 638.1581406359512 | ok | cypher |
| kuzu | topk_property_sort | holdout | 10 | 2.8984609816689044 | 2.8762780129909515 | 3.013474799809046 | 2.9066019924357533 | 3.4349290071986616 | 3.4349290071986616 | 3.4349290071986616 | 331.8428281077269 | ok | cypher |
| kuzu | two_hop_expand | holdout | 10 | 0.8190740190912038 | 0.729482009774074 | 0.7739318010862917 | 0.7603979902341962 | 0.8729339751880616 | 0.8729339751880616 | 0.8729339751880616 | 1292.1035142843318 | ok | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
