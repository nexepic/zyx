# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | point_node_fetch_by_id | holdout | 10 | 0.24379801470786333 | 0.21552600082941353 | 0.27078989951405674 | 0.24379801470786333 | 0.4152100009378046 | 0.4152100009378046 | 0.4152100009378046 | 3692.8999264541985 | ok | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
