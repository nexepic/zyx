# Benchmark Profile Breakdown

## Phase Summary

| database | workload | scale | profile | phase | samples | total_calls | avg_calls | total_value | total_value_calls | avg_value | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

## Reading Guide

- Each row aggregates one emitted profile phase across measured iterations.
- Rows are ordered by database, scale, profile, workload, and descending `p50_ms` so the slowest phases appear first within each workload.
- `first_ms` helps diagnose cold-ish first-iteration costs; `p50_ms` and `p95_ms` highlight steady-state and tail behavior.
- `total_calls` and `avg_calls` distinguish expensive phases from frequently repeated small operations.
- ZYX currently emits internal profile phases; other databases may only have top-level sample latency unless their adapters add phase events.
