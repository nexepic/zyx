# Operation Type Summary

Rows group benchmark workloads by database operation family. Unit: ms; lower p50-derived values are better.

| operation_type | database | scale | workload_count | workloads | ok_workloads | unsupported_workloads | failed_workloads | samples | geomean_p50_ms | avg_p50_ms | min_p50_ms | max_p50_ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| adjacency expand | zyx | holdout | 1 | two_hop_expand | 1 | 0 | 0 | 10 | 3.2568 | 3.2568 | 3.2568 | 3.2568 |
| property filter | zyx | holdout | 1 | label_scan_filter | 1 | 0 | 0 | 10 | 305.055 | 305.055 | 305.055 | 305.055 |
| topk/group | zyx | holdout | 2 | aggregation_count_by_group;topk_property_sort | 2 | 0 | 0 | 20 | 377.319 | 377.332 | 374.247 | 380.417 |

## Operation Families

- `read scan`: scans without scalar property predicates, such as relationship type counts.
- `adjacency expand`: fixed-hop, bounded-reachability, and write-then-local-traversal workloads.
- `property filter`: node/relationship property predicates and indexed scalar lookup workloads.
- `topk/group`: sort/TopK and aggregation/grouping workloads.
- `point write`, `batch write`, `load/index build`: mutation and ingest phases kept separate from read latency.
