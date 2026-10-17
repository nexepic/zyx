# Operation Type Summary

Rows group benchmark workloads by database operation family. Unit: ms; lower p50-derived values are better.

| operation_type | database | scale | workload_count | workloads | ok_workloads | unsupported_workloads | failed_workloads | samples | geomean_p50_ms | avg_p50_ms | min_p50_ms | max_p50_ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| other | zyx | holdout | 1 | point_node_fetch_by_id | 1 | 0 | 0 | 10 | 25.8509 | 25.8509 | 25.8509 | 25.8509 |

## Operation Families

- `read scan`: scans without scalar property predicates, such as relationship type counts.
- `adjacency expand`: fixed-hop, bounded-reachability, and write-then-local-traversal workloads.
- `property filter`: node/relationship property predicates and indexed scalar lookup workloads.
- `topk/group`: sort/TopK and aggregation/grouping workloads.
- `point write`, `batch write`, `load/index build`: mutation and ingest phases kept separate from read latency.
