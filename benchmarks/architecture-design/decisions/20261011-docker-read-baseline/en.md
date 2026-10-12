# Docker read performance baseline (2026-10-11)

Status: three read assessment rounds complete; S0.1 accepted. Optimization percentages are maintained in optimization.mdx.

The user requested Docker/Colima isolation. The environment is an Apple M4 Pro host with Linux ARM64 containers, GCC 13.3.0, C++20 Release and Kuzu 0.11.3. Formal runs use an 8 GiB Colima VM, a 6 GiB/single-core container, one execution thread, seed 42, medium scale (225000 nodes, 860000 edges), warm execution, 3 warmups and 10 measurements in each of 3 independent process rounds. ZYX runs first in odd rounds and Kuzu first in even rounds. ZYX diagnostics are disabled. Import, copying and open/close are outside query latency.

The original 2 GiB VM/1800 MiB container triggered kernel global OOM during ZYX import. The Python runner also retained its generated graph; it now releases that graph after CSV serialization, verified using a weak-reference test. Formal rounds use a separate resource contract. Increased VM memory is not a database optimization benefit. Query materialization retains the source default of 64 MiB.

EntityPage.hpp needed an explicit functional standard header to compile on Linux; this changes neither format nor query behavior. Hashes of 567 compiled source files match the working tree. The benchmark binary SHA-256 is 41b5c9f3f26d94a579a421feeffea34c9112c217efd7674e1216be1b8593cf08. Source, CMake/Ninja configuration, dependency versions, runtime libraries and dataset hashes are recorded with the raw evidence.

The 18 scenarios cover scan filters, relationship filters, one/two hops, DISTINCT, real group-by, Top-K, point lookup, secondary indexes and projections. Two scalar secondary-index scenarios unsupported by Kuzu are N/A. Scalar results and projection row counts are independently checked against CSV data. These checks do not replace validation of projected values, types, ordering, duplicates or complete query semantics.

Aggregation uses the median of independent process P50s, preserving their range and raw samples. A process P95 based on 10 samples is descriptive, not a precise tail estimate. Resource high-water marks include import, benchmark inputs, caches and validation, not query-exclusive memory. Historical macOS aggregates appear in optimization.mdx; original samples are absent from this worktree, so there is no strict historical regression claim.

Raw evidence: benchmarks/architecture-design/results/20261011-docker-baseline/. Reproduction entry points: runner.baseline and runner.baseline_report. All 1020 valid timing samples and result observations are complete; formal scalar/count checks passed.


| Scale | Workload | ZYX P50 | Kuzu P50 | ZYX/Kuzu |
|---|---|---:|---:|---:|
| medium | aggregation_count_by_group | 301.455000 | 2.165042 | 139.237484 |
| medium | aggregation_group_by | 299.879000 | 1.990833 | 150.629912 |
| medium | all_nodes_property_filter | 355.153000 | 0.430791 | 824.420659 |
| medium | batch_node_fetch_100 | 1.746420 | 0.240416 | 7.264159 |
| medium | label_multi_property_filter | 233.057000 | 1.359917 | 171.375900 |
| medium | label_scan_filter | 208.899000 | 1.293667 | 161.478186 |
| medium | one_hop_expand | 1.458960 | 0.228875 | 6.374484 |
| medium | one_hop_fetch_neighbor_ids | 1.649420 | 0.304583 | 5.415338 |
| medium | one_hop_fetch_neighbor_records | 2.106960 | 0.373375 | 5.643013 |
| medium | point_edge_fetch_by_endpoints | 1.953920 | 0.164208 | 11.899055 |
| medium | point_lookup_indexed | 1.084670 | 0.101667 | 10.668850 |
| medium | point_node_fetch_by_id | 1.751210 | 0.089208 | 19.630638 |
| medium | property_equality_indexed | 133.227000 | N/A | N/A |
| medium | property_range_indexed | 828.054000 | N/A | N/A |
| medium | relationship_property_filter | 630.077000 | 5.273500 | 119.479852 |
| medium | relationship_type_scan | 206.960000 | 2.988792 | 69.245367 |
| medium | topk_property_sort | 495.441000 | 1.955875 | 253.309133 |
| medium | two_hop_expand | 1.702670 | 0.477834 | 3.563309 |

89 relevant module tests passed; no full unit suite, commit or push. Docker images, containers, volumes and build cache are all zero. Generated datasets/databases were removed. Colima is restored to 2 CPUs/2 GiB and temporary DNS/proxy configuration is gone. About 11.4 MiB of reports and compressed controls are retained. TRIM reported 7.6 GiB, not a measured host disk reduction. Scan, aggregation and sorting are optimization candidates; ZYX C++ and Kuzu Python/Cypher entry points differ. Use runner.baseline --help and runner.baseline_report to reproduce three rounds in a new output directory under the recorded contract.

- `baseline-summary.json` SHA-256: `b7020734a2a9aaa0839e222868268f2f87cf82777edfe051dee571aa341437c0`

- `control/source.tar.gz` SHA-256: `391fec9aca8f36c667ea23f02265b357fb2eb5f64ec12b6f9eaaa75e95282a41`

- `control/zyx-compare-bench.gz` SHA-256: `2c6defa925fd99f0853f9a31281cbb36866243cf7b58de8d0081d4df27cf3954`
