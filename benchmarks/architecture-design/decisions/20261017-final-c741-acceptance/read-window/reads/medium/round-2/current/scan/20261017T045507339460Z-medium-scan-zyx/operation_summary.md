# Operation Type Summary

Rows group benchmark workloads by database operation family. Unit: ms; lower p50-derived values are better.

| operation_type | database | scale | workload_count | workloads | ok_workloads | unsupported_workloads | failed_workloads | samples | geomean_p50_ms | avg_p50_ms | min_p50_ms | max_p50_ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| adjacency expand | zyx | medium | 2 | one_hop_expand;two_hop_expand | 2 | 0 | 0 | 20 | 2.68383 | 2.69951 | 2.40895 | 2.99008 |
| property filter | zyx | medium | 4 | all_nodes_property_filter;label_multi_property_filter;label_scan_filter;relationship_property_filter | 4 | 0 | 0 | 40 | 623.866 | 661.493 | 418.397 | 966.955 |
| read scan | zyx | medium | 1 | relationship_type_scan | 1 | 0 | 0 | 10 | 351.724 | 351.724 | 351.724 | 351.724 |
| topk/group | zyx | medium | 3 | aggregation_count_by_group;aggregation_group_by;topk_property_sort | 3 | 0 | 0 | 30 | 499.816 | 500.16 | 475.915 | 520.802 |

## Operation Families

- `read scan`: scans without scalar property predicates, such as relationship type counts.
- `adjacency expand`: fixed-hop, bounded-reachability, and write-then-local-traversal workloads.
- `property filter`: node/relationship property predicates and indexed scalar lookup workloads.
- `topk/group`: sort/TopK and aggregation/grouping workloads.
- `point write`, `batch write`, `load/index build`: mutation and ingest phases kept separate from read latency.
