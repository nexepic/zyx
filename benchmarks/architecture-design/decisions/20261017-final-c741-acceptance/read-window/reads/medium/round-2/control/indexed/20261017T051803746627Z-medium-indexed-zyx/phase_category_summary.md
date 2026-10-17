# Measurement Phase Summary

Rows group raw profile timers into benchmark semantics such as statement latency, storage sync, close checkpoint, and load/import.

| database | workload | scale | profile | semantic_phase | raw_phases | samples | total_calls | avg_calls | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

## Semantic Phases

- `statement_latency`: user-visible operation latency measured by the workload sample timer.
- `durable_storage_sync`: commit/sync work emitted by storage profile timers.
- `close_checkpoint`: clean close, checkpoint, and deferred database flush work.
- `load_import`: data loading, index build, and batch storage ingest work.
- `statement_commit`: commit wrapper work that is not precise enough to classify as storage-sync-only.
