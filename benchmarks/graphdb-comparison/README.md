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
| `label_scan_filter` | Count users filtered by `country`. |
| `one_hop_expand` | Expand one outgoing `FOLLOWS` hop from a fixed user. |
| `two_hop_expand` | Expand two outgoing `FOLLOWS` hops from a fixed user. |
| `shortest_path_chain` | Check whether a bounded `FOLLOWS` path exists between fixed users. |

`equivalent_mode` indicates the execution interface:

- `api`: ZYX uses its native C++ comparison benchmark binary (`zyx-compare-bench`) and reports JSONL samples.
- `cypher`: Neo4j and Memgraph use Cypher over Bolt, and Kuzu uses Cypher through its Python API.

## Outputs

Each run creates a timestamped result directory below `results/` or the configured `--output-root`.

- `raw.jsonl`: environment event plus successful sample and failure events.
- `errors.jsonl`: one JSONL error event for each failed adapter or workload.
- `summary.csv`: aggregate rows with sample count, average, p50, p95, p99, ops/sec, status, and `equivalent_mode`.
- `summary.md`: Markdown summary table for quick inspection.
- `environment.json`: machine, Python, run configuration, and dataset manifest.
- `run_status.json`: current failure count for the run.
- `latest.txt`: written in the output root and points to the latest result directory name.

## Failures and exit status

The runner continues after adapter or workload failures when it can. Failures are written to `raw.jsonl`, `errors.jsonl`, `summary.csv`, and `run_status.json`. A nonzero process exit means `run_status.json` reported at least one failure; inspect `errors.jsonl` first, then `summary.md` and `raw.jsonl` for context. Service startup failures, missing Python packages, a missing `zyx-compare-bench`, Bolt connection failures, timeouts, and malformed adapter output are reported as benchmark failures rather than silently omitted.

## Limitations and fairness notes

- This is an apples-to-apples workload harness, not a universal database ranking.
- ZYX runs through the native C++ API path, while Neo4j, Memgraph, and Kuzu run through Cypher-compatible paths; compare results with `equivalent_mode` in mind.
- Docker Compose improves isolation, but host CPU, memory pressure, filesystem performance, Docker resource limits, and image versions still affect results.
- The generated graph is deterministic and simple; it does not model every production graph shape or write-heavy workload.
- Warmup and iteration counts should be increased beyond smoke settings before drawing performance conclusions.
- Neo4j and Memgraph run as services; Kuzu and ZYX run embedded in the runner container, so process topology differs.
