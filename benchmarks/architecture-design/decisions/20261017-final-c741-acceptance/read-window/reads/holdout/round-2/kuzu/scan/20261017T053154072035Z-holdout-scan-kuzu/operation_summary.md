# Operation Type Summary

Rows group benchmark workloads by database operation family. Unit: ms; lower p50-derived values are better.

| operation_type | database | scale | workload_count | workloads | ok_workloads | unsupported_workloads | failed_workloads | samples | geomean_p50_ms | avg_p50_ms | min_p50_ms | max_p50_ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| adjacency expand | kuzu | holdout | 1 | two_hop_expand | 1 | 0 | 0 | 10 | 0.760398 | 0.760398 | 0.760398 | 0.760398 |
| property filter | kuzu | holdout | 1 | label_scan_filter | 1 | 0 | 0 | 10 | 1.5514 | 1.5514 | 1.5514 | 1.5514 |
| topk/group | kuzu | holdout | 2 | aggregation_count_by_group;topk_property_sort | 2 | 0 | 0 | 20 | 2.69568 | 2.70333 | 2.50005 | 2.9066 |

## Operation Families

- `read scan`: scans without scalar property predicates, such as relationship type counts.
- `adjacency expand`: fixed-hop, bounded-reachability, and write-then-local-traversal workloads.
- `property filter`: node/relationship property predicates and indexed scalar lookup workloads.
- `topk/group`: sort/TopK and aggregation/grouping workloads.
- `point write`, `batch write`, `load/index build`: mutation and ingest phases kept separate from read latency.
