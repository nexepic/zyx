# Graph Database Comparison Benchmark

This benchmark compares ZYX against Neo4j, Memgraph, and Kuzu on the same generated graph dataset and common read workloads. The `fake` database adapter exists only for unit tests and local smoke validation; it is not a benchmark target.

## Docker-first workflow

Run comparisons from this directory with Docker Compose. The Compose workflow isolates services, Python dependencies, the ZYX comparison binary, database ports, and output files so host-installed Neo4j, Memgraph, Kuzu, Python packages, or ZYX builds do not affect benchmark runs.

```bash
cd benchmarks/graphdb-comparison

docker compose build runner
docker compose run --rm runner python -m runner.run --scale smoke --output-root /results --warmup 1 --iterations 1
```

For the default comparison set (`zyx`, `neo4j`, `memgraph`, `kuzu`) with default runner settings:

```bash
cd benchmarks/graphdb-comparison

docker compose up -d neo4j memgraph
docker compose run --rm runner python -m runner.run --scale smoke --output-root /results
docker compose down
```

If your system uses the legacy Compose command, replace `docker compose` with `docker-compose`.

## Python-only validation

Python-only tests validate dataset generation, summary output, failure handling, and adapter behavior that does not require live external services. Run them from the repository root:

```bash
PYTHONPATH=benchmarks/graphdb-comparison python3 -m pytest benchmarks/graphdb-comparison/tests -q
```

You can also run a local smoke check with the test-only `fake` adapter:

```bash
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.run \
  --database fake \
  --scale smoke \
  --output-root benchmarks/graphdb-comparison/results \
  --warmup 1 \
  --iterations 1
```

ZYX no longer has a query-result or derived-result cache path. Benchmark runs use the normal
storage/page/index/plan caches only:

```bash
ZYX_COMPARE_BENCH=$PWD/buildDir/zyx-compare-bench \
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.run \
  --database zyx \
  --scale smoke \
  --profile scan \
  --output-root benchmarks/graphdb-comparison/results \
  --execution-mode warm \
  --warmup 0 \
  --iterations 1
```

Use `--execution-mode warm` for steady-state latency after the configured query warmup. Use
`--execution-mode opened` for a database that has been loaded and opened but has no explicit query
warmup; read `first_ms` as the first operation after open and `p50_ms` as the same open handle
continues to execute measured iterations. Use `--execution-mode cold-ish` to run each measured query
from a freshly prepared adapter/database handle while excluding data load time from the measured
query latency. `cold-ish` is not an OS page cache flush; it is a repeatable first-query-after-open/setup
path for comparing optimistic warm-cache results against a less cache-amortized path.

For optimization validation, run the matrix wrapper so every round covers both cache-sensitive and
steady-state paths across small and medium data sizes:

```bash
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.matrix \
  --database zyx \
  --database kuzu \
  --database neo4j \
  --database memgraph \
  --profile scan \
  --output-root benchmarks/graphdb-comparison/results \
  --warmup 0 \
  --iterations 10
```

By default the matrix runs `small` and `medium` with `cold-ish`, `opened`, and `warm`. Override the
matrix with repeated `--scale` or `--execution-mode` flags for shorter local checks. The wrapper
writes a `*-matrix.json` manifest and updates `latest-matrix.txt` with links to every per-run
summary.

For the primary cross-database performance report, prefer the Operational Steady-State runner. It
uses one clear main dimension (`medium`, `warm` steady-state execution, p50 latency), combines the
core read/index/traversal/write-statement profiles, and writes a categorized report instead of mixing
scale or lifecycle modes in one table:

```bash
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.operational \
  --database zyx \
  --database kuzu \
  --database neo4j \
  --database memgraph \
  --scale medium \
  --output-root benchmarks/graphdb-comparison/results \
  --warmup 20 \
  --iterations 100
```

Outputs:

- `operational_steady_state.md`: categorized p50 report by database operation type.
- `operational_steady_state.csv`: machine-readable form of the same report.
- `latest-operational-steady-state.txt`: points to the latest manifest for this main report.

This main report intentionally excludes startup/first-operation and explicit durability-barrier
measurements. Use it to answer the primary question: how fast each database is during normal
steady-state operation.

Use the Thread Scaling runner when the question is whether one operation/query benefits from more
CPU execution threads. This runner passes the same explicit thread budgets into adapters that expose
single-operation thread control (for example ZYX and Kuzu), then writes a speedup report instead of
mixing those numbers into the main cross-database report:

```bash
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.thread_scaling \
  --database zyx \
  --database kuzu \
  --scale small \
  --scale medium \
  --profile scan \
  --execution-mode warm \
  --thread-count 1 \
  --thread-count 8 \
  --output-root benchmarks/graphdb-comparison/results \
  --warmup 2 \
  --iterations 5
```

Outputs:

- `thread_scaling.md`: p50 report comparing baseline and target thread counts by workload.
- `thread_scaling.csv`: long-form machine-readable p50 and speedup rows for every thread count.
- `latest-thread-scaling.txt`: points to the latest manifest for the thread-scaling report.

`--thread-count 0` means adapter/engine auto-detected thread count when supported. Neo4j and
Memgraph adapter-level per-query thread control is intentionally not modeled here; use service-level
configuration if you need a separate database-specific study.

If an adapter cannot execute a workload with the advertised semantics, the row is reported as
`unsupported` instead of emitting a fallback latency. For example, the Kuzu adapter probes scalar
secondary property index support before reporting the indexed `User(country)` / `User(age)` workloads;
when the active Kuzu version rejects those `CREATE INDEX` statements, the related indexed rows are
marked unsupported rather than reporting unindexed scan latency as indexed performance.

Use `--profile write` when evaluating incremental update behavior that `load_nodes_edges` cannot
represent. The write profile keeps the normal load baseline, then measures point node/edge creation,
point property updates, create-then-delete, and write-then-read visibility. This profile is a
statement-latency view: it measures when the adapter call returns and does not force an additional
checkpoint unless the engine does so as part of normal statement execution.

```bash
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.matrix \
  --database zyx \
  --database kuzu \
  --profile write \
  --output-root benchmarks/graphdb-comparison/results \
  --warmup 1 \
  --iterations 20
```

Use `--profile write_durable` when comparing write latency with an explicit durability barrier after
each measured write. For Kuzu the barrier is `CHECKPOINT`; for ZYX the normal auto-commit write path
fsyncs the WAL commit record and may defer the main database checkpoint until the WAL threshold or a
clean close. This profile is intentionally separate from `write` so statement latency and
durable-after-each-write latency are not mixed into one number.

## Datasets

Datasets are deterministic CSV files generated per run under `<result>/dataset` using `--scale` and `--seed`.

| Scale | Users | Posts | Tags | FOLLOWS/user | Tags/post |
| --- | ---: | ---: | ---: | ---: | ---: |
| `smoke` | 100 | 120 | 30 | 3 | 2 |
| `small` | 10,000 | 12,000 | 1,000 | 5 | 2 |
| `medium` | 100,000 | 120,000 | 5,000 | 5 | 2 |

Generated files:

- `users.csv`: `User` nodes with `id`, `age`, `country`, `score`.
- `posts.csv`: `Post` nodes with `id`, `created_at`, `score`.
- `tags.csv`: `Tag` nodes with `id`, `rank`.
- `follows.csv`: `FOLLOWS` edges from users to users.
- `authored.csv`: `AUTHORED` edges from users to posts.
- `has_tag.csv`: `HAS_TAG` edges from posts to tags.
- `manifest.json`: scale, seed, and row counts for all generated files.

## Workloads and equivalence mode

The benchmark records one row per database/workload and labels how equivalent work was executed.

| Workload | Description |
| --- | --- |
| `load_nodes_edges` | Create/load graph schema, indexes or constraints, nodes, and edges from CSV files in a fresh adapter/database instance for each warmup and measured sample. |
| `point_lookup_indexed` | Lookup one `User` by primary key-like `id`. |
| `index_seek_then_one_hop_expand` | `operational_dynamic` profile workload that uses a secondary `User(country)` predicate to select seed users and then counts one-hop `FOLLOWS` expansions. Databases without equivalent scalar secondary-index semantics report `unsupported` instead of scan fallback latency. |
| `index_seek_then_two_hop_expand` | `operational_dynamic` profile workload that uses the same secondary-index seed selection and then counts two-hop `FOLLOWS` expansions. |
| `label_scan_filter` | Count users filtered by `country`. |
| `one_hop_expand` | Count outgoing `FOLLOWS` neighbors from the anchored seed `(:User {id: 'user-000001'})`. |
| `two_hop_expand` | Count two-step outgoing `FOLLOWS` paths from the same anchored seed. |
| `shortest_path_chain` | Check whether a bounded `FOLLOWS` path exists between fixed users. |
| `reachable_within_6` / `12` / `24` / `30` | `multihop` profile workloads that use bounded path existence (`MATCH p = ...[:FOLLOWS*1..N]... RETURN 1 LIMIT 1`) and scale-aware targets chosen to require roughly the configured hop depth in the generated `FOLLOWS` graph. |
| `aggregation_group_by` | Count distinct user countries. |
| `aggregation_count_by_group` | Group users by country and count rows per group. |
| `topk_property_sort` | Return top users by score with `ORDER BY ... LIMIT`. |
| `point_create_node` | `write` profile workload that creates one `User` node with a unique benchmark id. |
| `point_create_edge` | `write` profile workload that creates one `FOLLOWS` edge between existing users. |
| `point_update_node_property` | `write` profile workload that updates one existing `User` property and returns the affected count. |
| `point_update_edge_property` | `write` profile workload that updates one existing `FOLLOWS` edge property and returns the affected count. |
| `point_create_delete_edge` | `write` profile workload that creates one transient `FOLLOWS` edge and deletes the same edge. |
| `write_then_read_edge` | `write` profile workload that creates one `FOLLOWS` edge and immediately verifies it through a read query. |
| `batch_create_edges_100` / `1000` / `10000` | `operational_dynamic` profile workloads that append a measured batch of `FOLLOWS` edges between existing users. |
| `batch_create_edges_100_then_one_hop_expand` / `10000_then_one_hop_expand` | `operational_dynamic` profile workloads that append a batch of `FOLLOWS` edges from one source and immediately count that source's outgoing neighbors. |
| `*_durable` | `write_durable` profile variants that run the matching write workload and then an adapter-defined durability barrier before stopping the timer. For ZYX this barrier is the WAL commit fsync; clean close still checkpoints the main file and removes the WAL. |

`equivalent_mode` indicates the execution interface:

- `api`: ZYX uses its native C++ comparison benchmark binary (`zyx-compare-bench`) and reports JSONL samples.
- `cypher`: Neo4j and Memgraph use Cypher over Bolt, and Kuzu uses Cypher through its Python API.

## Outputs

Each run creates a timestamped result directory below `results/` or the configured `--output-root`.

- `raw.jsonl`: environment event plus successful sample and failure events.
- `errors.jsonl`: one JSONL error event for each failed adapter or workload.
- `summary.csv`: aggregate rows with sample count, first/min/average/p50/p95/p99/max latency, ops/sec, status, and `equivalent_mode`.
- `summary.md`: Markdown summary table for quick inspection.
- `comparison.csv`: per-workload p50 ranking, ZYX-vs-best ratios, first-sample ratio, and tail-volatility metrics.
- `comparison.md`: Markdown comparison report with ZYX rank, p50 ratio, first/p50 ratio, and p95/p50 volatility.
- `zyx_profiles.jsonl`: raw ZYX phase events emitted by `zyx-compare-bench --emit-profile`.
- `profile_summary.csv`: per-workload/per-phase aggregate timings with calls, first/min/average/p50/p95/p99/max latency.
- `profile_summary.md`: Markdown phase breakdown to identify whether load, open/close, query execution, storage, or planner phases dominate.
- `environment.json`: machine, Python, run configuration, dataset manifest, execution-mode semantics, and write durability contract.
- `run_status.json`: current failure count for the run.
- `latest.txt`: written in the output root and points to the latest result directory name.

ZYX and Kuzu database files are treated as reproducible benchmark artifacts and are removed by
default after each adapter finishes. Summaries, raw samples, profiles, comparison reports, and CSV
datasets remain available for analysis. Use `--keep-db-artifacts` only when debugging a storage file;
medium-scale database artifacts can consume hundreds of MiB per run.

## Failures and exit status

The runner continues after adapter or workload failures when it can. Failures are written to `raw.jsonl`, `errors.jsonl`, `summary.csv`, and `run_status.json`. A nonzero process exit means `run_status.json` reported at least one failure; inspect `errors.jsonl` first, then `summary.md` and `raw.jsonl` for context. Service startup failures, missing Python packages, a missing `zyx-compare-bench`, Bolt connection failures, timeouts, and malformed adapter output are reported as benchmark failures rather than silently omitted.

## Limitations and fairness notes

- This is an apples-to-apples workload harness, not a universal database ranking.
- ZYX runs through the native C++ API path, while Neo4j, Memgraph, and Kuzu run through Cypher-compatible paths; compare results with `equivalent_mode` in mind.
- Docker Compose improves isolation, but host CPU, memory pressure, filesystem performance, Docker resource limits, and image versions still affect results.
- The generated graph is deterministic and simple; it does not model every production graph shape or write-heavy workload.
- Warmup and iteration counts should be increased beyond smoke settings before drawing performance conclusions.
- Report `execution_mode`, warmup, and iteration settings with results. `opened` skips explicit query warmup, while `cold-ish` avoids repeated-query optimism but does not clear OS or database service page caches.
- Use `write` for statement-return latency and `write_durable` for durable-after-each-write latency; do not compare them as the same durability contract.
- Neo4j and Memgraph run as services; Kuzu and ZYX run embedded in the runner container, so process topology differs.
- Use `runner.thread_scaling` rather than each engine's implicit defaults when comparing single-operation CPU parallelism.
