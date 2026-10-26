# V0 exit: scan-supply-20261026 (15%)

Current status supersedes the failed first diagnostic, whose raw data and `quality.json` remain unchanged. No candidate code has been measured or accepted. `acceptance-registration.json` freezes candidates and outcome/resource gates before V1 implementation.

## Evidence and cost model

- `baseline/`: frozen control and Kuzu, two datasets, four queries, three processes and ten formal samples each: 480 checked outputs. All eight P50 gaps remain 16.4–176.2x. Process/P95 dispersion is substantial; use same-window controls for final acceptance, not subtraction from historical times.
- `isolated/`: 50 ms leaf sampling alternates two sampled/two control invocations in each process. All 80 outputs pass the full oracle. Sampled/control medians are 0.9571 (all nodes) and 0.9700 (label), within the registered 5% observation goal. This supports coarse CPU localization only: 208/71 independent leaf samples, unknown libc/vdso leaves unassigned; no hardware counters or stacks.
- `current-work.*`: deterministic counters on the frozen source. The all-node query examines 225,000 owners in 440 batches, decodes 3,516 group/membership records and 3,437 columns, records 83,614 decoded-metadata cache hits and 10,031 file-size calls. Buffer-pool physical-read bytes are 151,552, not measured disk bytes. Source and budget checks still happen per value despite certified batched identity.
- `layer-receipt.json`: real persisted medium input, read-only, same exact predicate counts (10,076 all-node; 12,500 User/CN), ten checked observations and unchanged input hash. Whole provider/consumer P50 is 246.994/87.1589 ms; nested source plus callback P50 is 226.319/77.4556 ms. These are diagnostic intervals in one process, not subtraction against public-query timings. Predicate work remains inside the nested interval. The source issues 220,880/100,880 admission callbacks for 220,000/100,000 values over 440 batches. This establishes the per-value management structure and a large inclusive supply interval, not an exact causal share for any individual function.
- `kuzu-plans.json`: EXPLAIN records the competing scan/filter/aggregate plans. Actual skipped blocks were not measured; no unsupported claim of Kuzu pruning is used to choose A.

Model total work as candidate/visibility work + group location/authentication + field decoding + per-value administration/delivery + operator/result work. Observe counts separately from costs; never sum overlapping inclusive CPU samples and layer intervals. Keeping useful-value decoding linear is necessary; converting repeated administration to bounded runs/pages is the first architectural experiment. Single-function timing shares remain unknown. Amdahl bounds therefore cannot justify an exact predicted speedup, nor a persistent-format rewrite yet.

## Decision and preregistration

Implement A across source and consumer, preserving layout. Register at least 2x geometric-mean primary improvement and at least 2x all-node improvement on each dataset as the minimum material architectural gain, not a predicted result and not Kuzu superiority. This excludes accepting a collection of tiny improvements against a two-order gap. Review any primary/guard P50/P95, memory or space regression above 5%; resolve repeatable material regressions. Keep budgets and one thread fixed. Only consider B after measured residual evidence supports it. The initial holdout baseline is visible, but candidate tuning must use medium or separate synthetic cases; freeze choices before candidate holdout execution.

V0 is complete because the actual public path, inclusive supply interval, deterministic work, observer limits and decision gates are now recorded. V1–V4 remain unaccepted. Missing fine-grained allocation/traffic measurements are limitations, not invented zero costs; candidate work counters and resource-account tests must verify the proposed reductions.

## Structural validation already available

The metadata split preserves original test bodies; 87 module tests pass with current hook-enabled StorageIO linked before the unchanged production archive (`metadata-hook-receipt.json`). IMDb statements preserve exact order/content (`seed-split-verification.json`). Neither earns independent performance credit. Other oversized live files remain to split. Historical oversized source snapshots are archived losslessly with hashes and explicit restoration instructions.
