# S0.3: same-envelope versioned baseline and complete family outcomes

This delivers the baseline records and outcomes of the registered full scenarios. **It is not an all-scenarios pass, S6.2 overall performance acceptance or Kuzu superiority.** Native `current` means frozen source `6a735df8`. Repaired production source `03436b7a` has separate correctness evidence and canceled O6 warmups; original-source numbers are never labeled newest-source performance.

## Provenance, resources and timing scopes

Original registration, immutable worker/harness hashes, preparation failures and continuation policy are retained. Only an externally interrupted aggregate case restarts; successful read samples are not rerun. Per-case actual formats and worker/runner hashes are in the compressed evidence. Prepared input prefix/suffix lineage stays explicit: no fresh final-version import or import-time claim is inferred from continued fixtures.

Same host, one CPU affinity, one query thread, 6 GiB per-process address-space and 2 GiB per-file limits; Native driver/worker have separate limits, not a combined 6 GiB cgroup. Report their RSS separately rather than claiming the sum of noncoincident peaks is a measured concurrent peak; Kuzu uses a 64 MiB pool / 2 GiB mapping, Native QUERY is 64 MiB with frozen domain/class limits. Internal private-write-grant changes remain disclosed by prior repair evidence. This is not a 6 GiB RSS/cgroup-equivalence experiment; actual host cgroup, CPU, compiler and dependencies remain registered. Native statements retain their default 30-second deadline, while the Kuzu contract helper uses the remaining family budget (up to 30 minutes). This difference is explicit: timeout/failure outcomes do not establish equal-deadline performance superiority. Default sync/acknowledgement stays enabled. Warm OS cache is not cold-cache evidence.

Formal reads contain 20 comparable workloads, 24 independent profile processes, 120 process-workload distributions and 1,200 valid owning-output samples. Each query/process has three warmups and ten samples. Do not call the 120 distributions independent processes. Aggregate the median of independent process P50s and retain process ranges; ten-sample P95 is descriptive, not a tail-protection pass.

Other full contract cases follow the preregistered isolated-family policy with complete values/types/NULL/missing/order/multiplicity checks and atomic/failure observations. Except maintenance windows, these are full-scale semantic/operational observations, not repeated warm-query distributions or protection passes; no missing warmups/repetitions are invented. Native IPC includes serialization/decoding/validation and trace retention; Kuzu Python cursor timing includes execute/drain/validation/close and ends before trace retention. Observer overheads are therefore not identical. Native C++ read and Kuzu Cypher entry costs differ; timers from these scopes are not summed or pooled.

## Complete contract coverage and failures

117 registered slots cover 13 families, three engines and three rounds; executed processes are distinguished from skipped/unsupported slots. Only complete oracle success is `completed`. Failed original preparation, deterministic failures and cutoffs remain N/A/inconclusive, never zero latency or inferred speedup. Later unchanged failed-family rounds reference the original failure under the preregistered rule, not fictitious independent execution.

| Family | control | frozen current6a | Kuzu |
|---|---|---|---|
| aggregate | failed=1, unmeasurable_previous_failure=2 | completed=3 | completed=3 |
| budget | completed=3 | failed=1, unmeasurable_previous_failure=2 | unsupported_NA=3 |
| dynamic | inconclusive_timeout=1, unmeasurable_previous_failure=2 | inconclusive_timeout=1, unmeasurable_previous_failure=2 | unsupported_NA=3 |
| maintenance | failed=1, unmeasurable_previous_failure=2 | failed=1, unmeasurable_previous_failure=2 | completed=3 |
| mixed | failed=1, unmeasurable_previous_failure=2 | completed=3 | failed=1, unmeasurable_previous_failure=2 |
| parameters | completed=3 | completed=3 | completed=3 |
| point | completed=3 | completed=3 | completed=3 |
| scan | completed=3 | completed=3 | completed=3 |
| snapshot | completed=3 | completed=3 | failed=1, unmeasurable_previous_failure=2 |
| sort | completed=3 | completed=3 | completed=3 |
| traversal | completed=3 | completed=3 | completed=3 |
| wide_large | inconclusive_timeout=1, unmeasurable_previous_failure=2 | failed=1, unmeasurable_previous_failure=2 | failed=1, unmeasurable_previous_failure=2 |
| write | completed=3 | completed=3 | completed=3 |

Executed independent contract processes: 87. Outcome counts: {"failed": 9, "completed": 75, "unmeasurable_previous_failure": 24, "inconclusive_timeout": 3, "unsupported_NA": 6}.

Frozen-current wide/large-value writes report Input/output error at offset 764542976; the cause is unresolved and is not relabeled a file-cap or format failure. Kuzu reports buffer-pool allocation failure at 64 MiB; control hits the 30-minute case cutoff. Retain each original error and do not repeat to obtain success. Both Native dynamic cases also hit the registered cutoff, without a complete semantic/performance pass.

Native 100,000-group owning output remains N/A at the fixed admission cap; partitioned value checks do not make the rejected full query succeed. Kuzu unsupported secondary scalar indexes retain the real capability witness. Its erroneous unlimited-mapping probe was corrected before valid samples for the affected case; failures and the single targeted retry remain. Dynamic missing-key semantics and Native fence/reclaimer APIs have no fabricated Kuzu implementation.

Original source6a maintenance has unequal implicit full-result Native versus no-return Kuzu semantics and a Native optional-cache failure; it yields no comparable steady-state latency. Repaired-source scalar-output O6 was externally canceled after two warmups for the five-delivery scope, with no accepted measurement or steady-state pass. Frozen-current budget fails its post-reopen INTEGRITY step with hierarchical resource admission denied; expected earlier quota/cancellation failures do not turn this family into a pass. Frozen-current mixed completes all three processes, while control publication-authentication and Kuzu pool-full failures remain; no strict speed ratio follows. Multi-client conservative-fence versus Kuzu transaction differences stay explicit. Unavailable global backlog, older Native engine-peak RSS and physical-device I/O are not zero. Older elapsed files lacking rss_scope are driver RSS only.

## Formal read comparison

Median independent-process P50s in ms; Native/Kuzu above one means Native is slower. Full process ranges, P95s, samples and capability/failure outcomes are in [read-comparison.json](read-comparison.json). Both control input preparations failed, so all control-improvement fields stay null: no 10% primary improvement or protection pass is inferred.

| Scale | Workload | Native ms | Kuzu ms | Native/Kuzu |
|---|---|---:|---:|---:|
| holdout | point_node_fetch_by_id | 63.756 | 0.230 | 277.672 |
| holdout | aggregation_count_by_group | 2316.490 | 2.622 | 883.401 |
| holdout | label_scan_filter | 2210.235 | 1.668 | 1325.086 |
| holdout | topk_property_sort | 2383.020 | 3.314 | 719.174 |
| holdout | two_hop_expand | 32.913 | 0.826 | 39.863 |
| medium | point_lookup_indexed | N/A | N/A | N/A |
| medium | property_equality_indexed | N/A | N/A | N/A |
| medium | property_range_indexed | N/A | N/A | N/A |
| medium | batch_node_fetch_100 | 41.792 | 0.485 | 86.226 |
| medium | one_hop_fetch_neighbor_ids | 36.685 | 0.621 | 59.088 |
| medium | one_hop_fetch_neighbor_records | 57.733 | 0.753 | 76.693 |
| medium | point_edge_fetch_by_endpoints | 42.844 | 0.449 | 95.317 |
| medium | point_node_fetch_by_id | 65.210 | 0.245 | 266.267 |
| medium | aggregation_count_by_group | 3203.310 | 3.629 | 882.585 |
| medium | aggregation_group_by | 3216.990 | 3.344 | 962.032 |
| medium | all_nodes_property_filter | 5961.120 | 0.900 | 6624.954 |
| medium | label_multi_property_filter | 3106.350 | 2.355 | 1319.273 |
| medium | label_scan_filter | 3055.525 | 2.142 | 1426.383 |
| medium | one_hop_expand | 21.136 | 0.389 | 54.317 |
| medium | relationship_property_filter | 7039.810 | 11.241 | 626.264 |
| medium | relationship_type_scan | 659.060 | 6.249 | 105.460 |
| medium | topk_property_sort | 3303.220 | 4.016 | 822.560 |
| medium | two_hop_expand | 27.739 | 0.874 | 31.746 |

Kuzu superiority is not achieved. S2.5, S3.5, S4.4, S5.5 and S6.2 production/resource/end-to-end admission remain independent. This baseline-data delivery does not replace them or establish a same-version newest-source whole-matrix comparison. CPU clock disagreement and bounded heap windows remain in [S0.4](../20261016-s04-hotspot-and-falsification/proof.json); cumulative bytes are neither RSS peaks nor physical device I/O.

Evidence: [family outcomes](family-outcomes.json), [compressed evidence/file hashes](full-family-files.json), [formal read raw evidence](../20261016-final-read-results/proof.json), [repaired-source correctness](../20261016-cache-decline-delete-budget/proof.json), and [optional decisions/documentation](../20261016-s64-optional-decisions-and-docs/proof.json). Every archived file was verified after decompression. Source/input/result versions remain separate. This delivery adds no algorithm and changes no budget or durability promise.

S0.3 baseline-data delivery accepted: **29/34 (85.3%), request 5/5**. Acceptance retains every version/failure/timing limitation above; outstanding performance/resource deliveries and the unmet Kuzu goal are unchanged.
