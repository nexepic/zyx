# S6.4: optional O7/O8 non-adoption and final documentation snapshot

Do not adopt additional SIMD, intra-query parallelism, compression, LTO/PGO or LLVM JIT. Add no dependency, thread or code cache. Optimization section 4 explicitly allows non-adoption without supporting evidence; [decisions.json](decisions.json) records each rationale and reopening gate. This completes the optional decision/documentation delivery, without waiving mandatory A1–A6 work.

[S0.4](../20261016-s04-hotspot-and-falsification/proof.json) retains actual function samples for five cases, but collector clocks disagree with OS CPU time and establish no precise expression CPU fraction. The tested conversion-inclusive typed filter is 1.587x slower than generic across three processes; that does not disprove all SIMD/JIT designs. Four-client correctness is not parallel-operator speedup. Existing LZ4/Zstd support and Release `-O3 -DNDEBUG` remain unchanged; no additional codec, LTO or trained PGO configuration is accepted.

## Final implementation snapshot and limits

The [03436b7a source freeze/regression](../20261016-cache-decline-delete-budget/proof.json) passes 2,882 tests from 130 affected modules, 13 complete-result smoke families and 4,000 extra four-client operations. Reuse this evidence; this documentation change runs no further performance campaign or full unit suite. Linux runtime validation does not establish executed macOS/Windows coverage.

Execution retains bound/typed batches, generic dynamic-type fallback and owning public output. Existing admitted Top-K, snapshot identity and budget paths remain. Literal DELETE outputs use a private reference consumer; public full entity results remain owned. Declined optional cache entries use admitted fallback reads and are not corruption. Writes retain COW, data-before-root, A/B publication and default durable sync; stale physical pages do not overwrite newer committed versions after conflicts.

Shared QUERY/CACHE/WRITE/MAINTENANCE ownership and same-file scratch follow [S5.3](../20261016-s53-budget-and-spill-acceptance/proof.json). Sort and partitioned DISTINCT/UNION spill are supported; unsupported aggregate/join spill fails explicitly. Logical/physical read-only boundaries remain. Reclamation frees consumed evidence only after complete validation. Correctness of 10,000-node deletion/large-value reclamation does not establish steady-state performance.

Actual envelope **5** and Superblock **31** agree with source, real smoke FORMAT and the [single-file specification](../../../../docs/apps/docs/content/docs/en/zyx/architecture/native-single-file-storage-plan.mdx). Development-format rejection of 4/30 and pre-release migration/stability gates follow [A7](../20261013-a7-format-boundary/en.md). No stable-format, implemented migrator, new file format or Driver ABI change is implied.

## Open work and performance gaps

S0.3, S2.5, S3.5, S4.4, S5.5 and S6.2 remain unaccepted. The original-source full baseline still needs remaining family outcomes; repaired-source steady-state runs under its registered stopping rule. The original `6a735df8` read comparison has 20 comparable workloads and 1,200 valid samples; it is not a newest-source full performance matrix. Failed control inputs do not yield speedup ratios; unsupported Kuzu secondary indexes/dynamic semantics stay N/A.

Original-source medium median process P50s are approximately 3055.5 versus 2.142 ms for label scan, 3203.3 versus 3.629 ms for grouping, 3303.2 versus 4.016 ms for Top-K, and 65.21 versus 0.245 ms for point fetch (Native versus Kuzu). **Kuzu superiority and overall performance acceptance are not achieved.** CPU/heap observations retain their sampling limits; cumulative bytes are neither memory peaks nor physical device I/O.

Replace S5.5 with this bounded S6.4 decision in the five-delivery request; do not launch unlimited tuning to force a steady-state pass. S5.5 remains mandatory in the fixed 34 with unchanged acceptance conditions. Accepting S6.4 yields **28/34 (82.4%), request 4/5**; S0.3 remains the final requested delivery. Later source/performance/format changes must refresh this snapshot; this decision does not complete the remaining capabilities.
