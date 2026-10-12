# S0 workload, resource and attribution contract

Date: 2026-10-11. Tasks: S0.2/S0.4. Status: preregistered contract; complete runners and semantic observations remain pending. Not accepted. Percentages belong exclusively to optimization.mdx.

## Decision

The adjacent contract.json defines 13 workload families with inputs, operations, independent oracles, timing boundaries and metrics. `existing` means a partial benchmark entry exists, not that all specified variants or observers are implemented. `pending` requires a new entry. Contract validation proves neither performance nor semantic acceptance.

Formal comparisons retain Docker Linux ARM64, a 6 GiB/one-CPU container, one engine thread and a 64 MiB query materialization budget. Measurement uses Colima 8 GiB/two CPUs, restoring its original configuration afterward. Budget-pressure cases use 4 MiB; concurrency cases explicitly use one/four clients and are not pooled with single-query results. Preserve synchronous durability. Restore identical logical state before each write sample, report restoration separately, and time through durable acknowledgement. Recreate changed formats from identical data and record actual format identities rather than assuming cross-format readability.

Training uses medium/seed 42. Holdout uses seed 314159, 73000 users, 87000 posts, 3000 tags, degree seven and untuned selectivities. Holdout generation, heterogeneous properties, wide/large values, snapshots and steady-state runners still need implementation. The existing five-neighbor ring is not evidence for hubs or heterogeneous shapes.

Primary performance experiments are scans, aggregation and sorting. Protect point access, traversal, wide/large entities, small writes and mixed workloads. Retain the 10% reproducible end-to-end improvement gate, 5% stable regression limit and unchanged memory cap. Report 2x/10x structural milestones and residual Kuzu gaps as observations, not promises or substitutes for admission.

## Statistics and correctness

Use three independent processes with three warmups and ten measurements each, interleaving control/current/Kuzu. Aggregate the median of process P50s and retain process ranges. Ten-sample P95 is descriptive only. Near gates or with excessive control variance, add at most two processes with 100 samples each; tail-sensitive steady-state experiments require at least 1000 observations/process. Stop after 30 minutes/case and retain inconclusive findings rather than sampling until success.

Check IDs, types, null versus missing, multiplicity and required ordering. Compare each aggregate key/count and valid Top-K boundaries; do not impose unspecified tie order. Consume all output inside timing, compare independent expected results afterward. Existing scalar/count observations do not satisfy full semantics. Unsupported Kuzu semantics are N/A rather than weakened substitutes.

## Attribution and falsification

Measure planning/cache, snapshot setup, directory resolution, page access/validation, property decoding, expression work, row/container allocation, aggregation/sorting and owning output. Pair these with pages/bytes, owners, edge backlookups, input/output rows, allocations and memory. CPU sampling and instrumented runs are separate from formal timing; nested timers are not summed.

Compare storage-only selected-property reads with full queries, then generic/typed execution over identical storage input to separate A1/A3 effects. Compare bounded Top-K and full sorting with identical semantics. Adjacency experiments require an accepted consistency protocol first; skipping mandatory checks is not a gain. Mechanism improvements without conclusive end-to-end gains do not justify a speedup claim.

## Verification and remaining deliverables

Run from the repository root:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/runner/optimization_contract.py benchmarks/architecture-design/decisions/20261011-s0-workload-contract/contract.json
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/tests/test_optimization_contract.py
```

Contract validation passed; nine module tests cover missing families, duplicate IDs, invalid resource limits, reduced sampling, missing oracles/timing/entries and malformed containers. No host dependencies, databases or Docker resources were created; no full test suite, commit or push.

S0.2 still requires all data/operation variants, full semantic observations, preflight execution and frozen actual queries/parameters/indexes/manifests/format identities. S0.3 requires the complete control/current/Kuzu matrix. S0.4 requires attribution counters/CPU samples and falsification experiments with uncertainty. Cleanup is restricted to owned artifacts, retaining bounded evidence and controls.

## Full-result validation integration

`runner.semantic_oracle` now checks actual group keys/integer counts and Top-K IDs/order. It derives scores independently from CSV, allows arbitrary equal-score ordering and valid boundary ties, and rejects duplicate/unknown IDs, wrong selections and ordering. This version requires unique IDs and finite scores; it does not cover null sorting or heterogeneous values.

`runner.baseline --require-full-results` requires complete rows for each measured iteration and writes `full-result-correctness.json`; absent or invalid rows fail the run. The default retains the previous read timing contract and does not relabel old samples. ZYX/Kuzu row observers are now wired behind explicit observation mode; strict Docker preflight with a freshly built binary remains pending. A separate untimed query cannot substitute for measured output. Retaining output changes timing/memory costs and must be disclosed in a new contract consistently for both engines.

Eleven oracle tests and three baseline integration tests passed. Integration verifies that correct row counts with wrong values still fail, evidence export/temporary input cleanup, and invalid sampling rejection before output/engine creation. Tests use standard-library dependencies and small generated data, without host installation or Docker resources.

```bash
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/tests/test_semantic_oracle.py
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/graphdb-comparison/tests/test_baseline_semantics.py
```


## Dual-engine row observers

The native benchmark accepts `--emit-result-rows` for real group-by and Top-K; row values are retained during output consumption and serialized after stopping the timer. Kuzu captures actual cursor rows under the same explicit mode. `runner.baseline --require-full-results` enables both observers through the runner. Default mode retains count-only behavior. Every measured operation clears warmup/previous output; native serializer preserves strings/integers and rejects unsupported cells before emitting partial JSON. The runner records the observation contract, and comparison provenance rejects mixed count-only/full-row modes.

Verification: C++20 syntax check passed; an isolated executable compiled from current benchmark source and current PerfTrace.cpp ran the three `CompareBenchmarkObserverTest.*` GoogleTests using existing cached GoogleTest dependencies. Six adapter/provenance tests, eleven oracle tests and three baseline integration tests passed (23 targeted tests). This does not execute the actual native database query path or Kuzu library: cursor tests use deterministic stand-ins. A newly built Docker binary and real dual-engine strict smoke preflight are next. No full suite, commit or push; temporary check executables/source removed. Logs: `benchmarks/architecture-design/results/20261011-result-observers/tests.json`.


## Updated execution order

The user deferred performance benchmarks until optimization implementation is complete. The 2026-10-11 strict-preflight image build was cancelled before any performance run; Docker resources are zero, Colima restored to two CPUs/2 GiB, and temporary VM DNS/proxy removed. Continue design, implementation and module correctness tests now. Timed dual-engine preflight, attribution and admission comparisons remain in the final window; their absence does not block code work or count as accepted performance evidence. The contract checker still passes nine tests. Cancellation evidence: `benchmarks/architecture-design/results/20261011-strict-preflight/cancellation.json`.
