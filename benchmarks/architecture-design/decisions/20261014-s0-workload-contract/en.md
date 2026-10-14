# S0.2 executable workload and resource contract

Date: 2026-10-14. Supersedes the pending 2026-10-11 S0.2 contract for delivery purposes; its historical evidence is retained. This is workload/resource-contract acceptance, not S0.3 baseline, S0.4 attribution, a full-scale run, or a performance result.

## Frozen inputs and execution entries

contract.json retains the 13 families, resource limits, timing boundaries, statistics, cleanup and independent oracles. It adds exact smoke/contract dimensions, implemented variant lists and capability profiles. `runner.workload_preflight` drives the real native engine through a dedicated untimed C++ worker and the real Kuzu library through its Python API. Complete observed typed results are checked against input-derived models; row counts alone cannot pass. Maps distinguish absent keys from explicit NULL, integer/bool/string remain distinct, multiplicities are retained, and ordered results are compared in sequence. Native NULL property assignment follows existing removal semantics; scalar NULL and missing projections are both checked. Larger JSON input strings are retained once as authenticated UTF-8 blobs; full values are compared before evidence retention.

medium-manifest.json and holdout-manifest.json come from actual generated CSV files. They freeze every file's byte count/SHA-256 and schema, seed 42 /314159, training 225000 nodes/860000 edges and holdout 163000 nodes/772000 edges (73000 users,87000 posts,3000 tags,degree seven). Existing ring topology is disclosed, not mislabeled as a hub. The separate executable topology fixture includes a shared hub, parallel edges, self-loop and two hops. The holdout profile is implemented in dataset.generate with a distinct default CLI seed. No optimization tuning uses holdout results.

The preflight registration freezes source/binary hashes, dimensions, resource settings and actual envelope/Superblock identity before queries. source-format-freeze.json additionally records local codec constants from source; formats are not assumed identical across controls. Trace evidence records exact executed queries/parameters, expected values, result hashes, required order and expected errors. No user files are used: databases live in owned temporary directories, closed before cleanup; evidence remains separately owned. The frozen historical control is not replaced or rebuilt from current source.

## Executable coverage

| Family | Implemented checks and variants |
|---|---|
| Point/scan | Deterministic random IDs, scalar/full-owner paths, selectivities .003/.37/1, indexed range and DDL transitions |
| Traversal | One/two hops, complete neighbor scalar projections, hub, duplicate parallel edges, self-loop and edge property |
| Aggregate/sort | Eight groups, 90% skew, singleton-key groups, DISTINCT, full sort and K=1/100/10000, tie-breaker and NULL/absent scores |
| Parameters | 100 changing ID/country/range sets; cached/cleared logical plans and create/drop index |
| Dynamic | Interleaved 1/8/64-key owners, singleton keys, integer/string change, remove and NULL-removal, full map/absent lookup, old snapshot |
| Wide/large | 8/128 fields times 32 KiB/1 MiB bodies, single-field update, complete unrelated fields/body retained, point/full reads |
| Write | 1/100/1000 batches, create/set/remove/unchanged-set/delete, full post-state and reopen; edges also exercised by traversal/deletion |
| Mixed | 90% reads/10% writes, independent state model, four real native threads with separate engines and bounded fresh-transaction retries; deterministic four-writer conflicts also checked |
| Snapshot | Held old view across updates and endpoint detach-delete; release, reopen and integrity |
| Maintenance | Ten churn/reclaimer windows; finite native scan/retirement limits, file/RSS/read/write observations, reopen/integrity and exact empty live state |
| Budget | Sort/hash materialization failure, cancellation, logical/physical readonly over-budget failure, reopen/integrity, no partial success |

Smoke uses bounded populations while retaining full 128-field and 1 MiB bodies and write batches up to 1000. Contract mode implements 10000 dynamic owners,100000 singleton aggregate keys,1000 wide owners per variant,100000 one-client operations, four clients totaling100000 operations,10000 pinned updates and ten10000-owner churn windows. The 4 MiB contract pressure setting is separate from the 4 KiB smoke probe. Formal-size sweeps remain final-window execution work; smoke does not establish their runtime, resource plateau or performance.

Native worker commands are a correctness harness over storage-neutral QueryEngine/transaction interfaces and explicit native administration, not the public Driver ABI performance path. Four client engines share one backend but current global resource integration remains S5 work. Byte/RSS observations do not prove process resource governance. Reclaimer limits are explicit finite harness bounds, not accepted A6 production defaults. File plateau observations are retained without asserting steady-state admission.

Kuzu uses fixed-schema tables, primary ID indexes, one execution thread per connection and 64 MiB raw buffer pool. Equivalent point/scan/traversal/aggregate/sort/parameters/wide/write/mixed/snapshot/churn paths consume complete real cursor output. Writes are checked after real close/reopen. Four real Kuzu client threads and an independent old read transaction are checked. Native secondary-index DDL, conservative graph-fence errors, owner/generation reclamation and charged-query-budget semantics are separate capability cases; arbitrary dynamic missing-key identity and native budget protocols are N/A for Kuzu, not weakened equivalent cases. This x86 Linux correctness smoke yields no ZYX/Kuzu ratio or ARM64 baseline claim.

## Resource and final measurement contract

Formal comparison remains Docker Linux ARM64,6 GiB container memory/no extra swap,one CPU/one engine thread,64 MiB native query materialization budget; explicitly separate one/four-client cases. Preserve synchronous durability and time through durable acknowledgment. Restore identical immutable logical state before each measured write sample and record restore time separately. Recreate incompatible formats from the same data. Kuzu's internal query-memory control is not claimed equivalent to native charged-byte admission; enforce the same overall container envelope and classify incompatible budget cases explicitly.

Three independent processes,three warmups,ten samples; rotate control/current/Kuzu order. Median of process P50s with process ranges; ten-sample P95 descriptive only. Threshold-adjacent/variable controls add at most two100-sample processes; tail-sensitive steady-state needs1000 observations/process. Stop at30 minutes/case and retain inconclusive results. Targets/protections stay10% reproducible gain,5% stable regression bound,unchanged memory limits; never substitute mechanism counters for end-to-end gains. Planning/cache, snapshots, directory/page/validation/decoding, expressions, allocations/rows and owning output are attributed separately in S0.4. Timed observer costs must be disclosed consistently for both engines; a later untimed requery cannot validate measured output.

## Verification and limits

Native and Kuzu preflights use actual freshly built native code and Kuzu0.11.3, not mock cursors. Matching local results contain raw build/test/preflight logs, registrations, traces/input blobs, binary/source hashes and reports. Fifteen Python contract/oracle tests pass; the escaped-property-key fix discovered by real preflight has292 selected native/parser tests. Exact final preflight operation counts and identities are in acceptance records. No full suite or performance run. S0.3/S0.4, full formal-size matrix, global runtime resource adoption and final performance admission remain unaccepted.

Build/run from repository root:

```bash
cmake --build buildDir --target zyx_workload_contract_worker
PYTHONPATH=benchmarks/graphdb-comparison python3 -m runner.workload_preflight --native-worker buildDir/zyx-workload-contract-worker --output /tmp/zyx-private-preflight --kuzu
python3 benchmarks/graphdb-comparison/runner/optimization_contract.py benchmarks/architecture-design/decisions/20261014-s0-workload-contract/contract.json
```

`--kuzu` requires the real installed library. Every output directory must be fresh. Formal contract mode is available via `--profile contract` and is reserved for the final execution window under the user's current order.
