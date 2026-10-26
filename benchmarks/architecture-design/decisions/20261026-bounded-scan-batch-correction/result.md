# Corrective S2 result: blocked at 15%

The single allowed correction completed 24 independent processes and 960 measured samples. Every complete public result oracle passed. The frozen per-case 5x adoption gate failed on all eight dataset/case combinations. Kuzu was faster in all eight. S2–S5 remain unaccepted; S5 guards remain incomplete. No progress commit or push was made.

| Dataset | Case | Corrected B P50 ms | Speedup vs initial | Speedup vs 369 control | Kuzu P50 ms | B/Kuzu time |
|---|---|---:|---:|---:|---:|---:|
| medium | label_scan_filter | 92.002 | 3.78x | 3.09x | 3.212 | 28.64x |
| medium | all_nodes_property_filter | 198.657 | 2.31x | 2.05x | 1.177 | 168.73x |
| medium | aggregation_count_by_group | 88.642 | 3.27x | 3.21x | 6.308 | 14.05x |
| medium | multi_key_group | 148.865 | 2.56x | 2.35x | 8.364 | 17.80x |
| holdout | label_scan_filter | 83.408 | 2.68x | 2.35x | 2.664 | 31.31x |
| holdout | all_nodes_property_filter | 158.203 | 2.10x | 1.91x | 1.020 | 155.04x |
| holdout | aggregation_count_by_group | 70.624 | 2.90x | 3.84x | 4.365 | 16.18x |
| holdout | multi_key_group | 122.845 | 2.40x | 2.76x | 7.129 | 17.23x |

Protocol: unchanged public probe and complete typed/bag/order oracles; three independent processes per engine/dataset, three warmups and ten measured samples per case, one thread. Both current-machine initial and frozen 369 controls were rerun; historical AMD latency was not reused. The original failed campaign and its identity remain preserved.

The correction removed demonstrated repeated work: medium label root descents fell from 1,575 to 208, all four native field cases used the bounded directory window, multi-field grouped batches were active, and the 860,000-page insertion directory census was absent after derived-summary maintenance. Correctness and work reduction do not substitute for the end-to-end gate.

Remaining observed medium label work includes 1,563 group decodes, 491 packed BTree page decodes, 40,787 metadata cache hits and 136,571,224 bytes of decoded-metadata fixed-size borrow accounting. The latter is accounting, not measured DRAM traffic or peak memory. These counters establish residual work, not causal CPU shares; no unmeasured bottleneck claim or new timing campaign is implied.

Both real-image copies retain exact admitted bytes and whole-file savings; original image SHA256 values are unchanged. Derived refresh cost is separately recorded, never included as free query preparation. Tests: 83 scan/build cases, 64 compatible read cases and five selected default insertion cases passed; no full unit suite. The hook-dependent sixth default case was excluded from the no-hook build, not counted as passed.

The current plan allows at most two candidates and one targeted corrective recheck. Both candidates and that correction have now been evaluated. Further work needs a bounded design revision supported by these residual-cost and performance results; silently adding another candidate, lowering the gate or claiming 100% would violate the current acceptance contract. Existing implementation stays an unaccepted working draft, and no production adoption is claimed.

Evidence: `results/20261024-work-scan/bounded-correction/{freeze.json,source-files.json,selection-gate.json,selection-summary.csv,workers/}`; paired original evidence under `final-comparison/`.
