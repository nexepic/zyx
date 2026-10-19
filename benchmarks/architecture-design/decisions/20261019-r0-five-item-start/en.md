# R0 starting point, baseline, and five-item scope

Plan `cow-performance-20261019`. This round delivers only R0.1/R0.2/R0.3/R1.1/R1.2. It does not adopt hybrid formats, adjacency candidates, WAL, multi-writer execution, or new spill algorithms.

## R0.1: Source and consumer audit

Remote work and the local starting commit agree. Documentation cleanup is isolated in `02380c0d`; production source equals frozen `c28794a7`. source-manifest.json hashes 720 actual source files. Existing interfaces and formats remain.

| Current path | Replacement boundary | Retained adapters |
|---|---|---|
| Authenticated selected-property cursor → owner×token entries → optional columns → named columns | Add a synchronous selected-cell sink using the same authentication/page-order implementation without a full matrix | Existing matrix APIs and cache semantics remain |
| BoundNodeScanBatch → PrimitiveNumericColumns | A budgeted direct owner receives primitive arrays and string offsets/bytes | Snapshots lacking the new capability retain the explicit matrix path |
| NodeColumnReceiver → ungrouped non-DISTINCT numeric aggregation | Direct single/multiple-key grouping and DISTINCT without LogicalPlanReadRow/property_cache | Complex/correlated/side-effect expressions and unsupported operators retain bounded row adapters |
| Sort/DISTINCT scratch, resource accounts, retained results | Protection contracts | Do not rebuild them or enable scratch by default |

Ordinary graph fences, bounded blind rebase, key-only adjacency, and COW/A-B are unchanged. First authentication, old snapshots, and error propagation are protected.

## R0.2: Baseline and final sampling registration

Reuse the completed source-identical c287 read/write/resource baseline under the main plan's reuse rule rather than repeat long matrices. proof.json checks unchanged production/harness source, identical platform/architecture/allowed CPU set, frozen benchmark/worker hashes, and medium/holdout file hashes. Reuse registered Release options, data manifests/schema, one thread, three independent processes, three warm-ups/ten measurements, timing/result/cache/durability contracts. Harness Git metadata in a report is not binary production provenance.

Primary goals are numeric/string filtering/count, actual single/multiple-key aggregation, and COUNT DISTINCT. Protect point/gather, full entities, complex types/NULLs, sort/DISTINCT spill, writes, snapshots, and resources. Retain 10% primary P50 and 5% regression gates and explicit admission for resource growth above 5%. Preserve process distributions, failed/N/A comparators, admitted 7–11% preparation RSS cost, and tail uncertainty. Formal timing belongs to R7 using retained before binaries, without mixing older after-source samples.

Before implementation, classify R1.1/R1.2 as foundational interfaces/capabilities admitted on semantics, lifetimes, and necessary module checks. R1.3/R7 assesses speedups. There are no new performance samples or Kuzu-superiority claims.

## R0.3: Mechanisms and pre-production decisions

Existing short diagnostics show User.country filtering handles 100000 owners, 2917 metadata-page reads, and 17019 file-size calls. Actual grouping produces 100000 logical rows and 42700000 cumulative materialization bytes. All-node numeric filtering already has 430 typed batches. These are mechanism observations, not CPU attribution, memory peaks, or device reads. Original local record: `/workspace/zyx-architecture-analysis-20261019/summary.json`; formal acceptance retains committed earlier evidence.

| Decision | Contract | Task |
|---|---|---|
| Native producer | Optional synchronous capability; strictly increasing nonzero source IDs; preserve owner/column ordinals independently of page order | R1.1 |
| Gather | Sort/deduplicate only for source access; preserve duplicate inputs/original ordinals and missing-vs-NULL; scans reject duplicate IDs | R1.1 |
| Payload | Existing codec/owner/generation checks precede per-owner delivery; transient scalar decoding is not a full PropertyValue matrix; no matrix-to-typed conversion for the direct owner | R1.1 |
| Lifetime | Query-batch owner charges arrays/string capacity; borrows validate owner/incarnation; seal before consumption; failed construction publishes no partial aggregation/output | R1.1 |
| Dynamic | Keep per-cell mixed types and a charged complex-value owner without permanent schema inference | R1.1 |
| Filter | Reuse bound predicates and selection; compare borrowed string bytes without per-row property maps; retain NULL/missing behavior | R1.2 |
| Aggregate | Complete tuple keys reuse type-sensitive DISTINCT encoding; preserve first-occurrence order, physical floating accumulation order, NULL/count/empty-input behavior and existing state/output budgets | R1.2 |
| Failure | Only pre-delivery pressure permits splitting/fallback; corruption/cancellation/I/O never replay; consumer failures do not restart the source | R1.1/R1.2 |
| Deferred | Physical hybrid parameters, trusted adjacency covering, general group/hash spill, and conflict relaxation are outside these five tasks | R3–R6 |

Necessary tests cover missing/NULL/mixed types, strings/bools/numerics, duplicate/out-of-order gather, aliases, single/multiple-key grouping, DISTINCT, filters, empty input, errors, budgets, cancellation, old snapshots, and stale borrows. Run affected modules only, not the full unit suite.
