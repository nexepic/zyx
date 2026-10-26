# Roadmap delivery and measured limits

The implementation roadmap is complete under the user's revised completion criterion. Kuzu remains faster in all eight primary read comparisons. 33 of 42 guard combinations exceed the 5% review threshold in at least one statistic; this is a review flag rather than proof that each excess is a stable regression; this report does not label those guards as passes. The new format remains explicit and experimental, with the default ROW path unchanged.

## Read comparison (original final window)

All figures are P50 milliseconds from the same original window; write-only corrections were not represented as new read timings.

| Dataset | Case | Initial ZYX | Control ZYX | Selected scan path | Kuzu |
|---|---|---:|---:|---:|---:|
| medium | label_scan_filter | 324.512 | 292.368 | 104.337 | 3.264 |
| medium | all_nodes_property_filter | 496.636 | 417.886 | 232.544 | 1.153 |
| medium | aggregation_count_by_group | 346.321 | 272.749 | 104.685 | 5.612 |
| medium | multi_key_group | 423.322 | 370.494 | 149.524 | 8.488 |
| holdout | label_scan_filter | 201.752 | 211.291 | 75.363 | 2.890 |
| holdout | all_nodes_property_filter | 290.209 | 342.440 | 162.873 | 1.118 |
| holdout | aggregation_count_by_group | 183.950 | 240.021 | 72.635 | 4.406 |
| holdout | multi_key_group | 243.302 | 281.998 | 109.854 | 7.219 |

## Corrected write comparison

| Dataset | Case | Scan path P50 / P95 ms | Same-window control P50 / P95 ms |
|---|---|---:|---:|
| medium | edge_batch_1000 | 134.304 / 329.920 | 131.318 / 181.680 |
| medium | edge_create_delete | 91.532 / 138.709 | 104.502 / 184.447 |
| holdout | edge_batch_1000 | 135.876 / 246.002 | 145.324 / 200.963 |
| holdout | edge_create_delete | 104.128 / 633.848 | 93.055 / 374.308 |

## Validation and scope

The original 171-process window retained nine failed Skew workers. Independent graph inspection proved that their input contained 32 nodes and no edges. A fresh, versioned copy restores the preregistered 512 edges; the oracle is unchanged. The 33-process correction window replaces those nine workers and 24 affected write workers. The two windows execute 204 processes in total, retaining nine original failures and 3900 validated sample records including superseded write samples. Reconciled evidence selects 171 successful process records and 3660 validated samples, with explicit per-record provenance and the original failures retained.

The final three engine edits affect write placement/publication only. The original read timings identify their archived binaries and c8b37bc3 source; the correction identifies its own production binaries, source freeze and input hashes. No entire performance matrix or full unit suite was repeated.

Corrected write comparisons use only the contemporaneous frozen control. Historical initial timings remain visible but are not treated as same-window write comparators. `process-costs.csv` reports process wall/CPU, RSS, OS byte counters, observed file peaks and measured-query sums separately; their difference is not an attribution of startup or I/O time.

See `report.json` and `summary.csv` for P50/P95, guard ratios, frozen identities, and adoption limits. See the sibling S4 validation directory for module results, complete public oracles, old-pin and 4 KiB-cache generality checks, and before/after regression tests.

## Adoption costs and future work

Steady whole-file savings are 17.25% on medium and 14.57% on holdout, including the admitted images' retained structures. Original conversion costs remain about 207 and 136 minutes respectively; conversion is not free. Historical file polling is an observed lower-bound peak, and process RSS high-water marks are not exact allocator peaks. No DRAM-traffic or hardware-stall claims are inferred from sizeof counters or nested timers.

Initial conversion savings are not a promise after updates. The preregistered 512-batch generality case with a long-lived old pin grows from 5767168 to 128712704 bytes; snapshot retention and mutation amplification are material adoption costs.

Known point/property overheads and the substantial Kuzu gap remain future optimization work. Use the scan format for a measured scan-heavy workload that amortizes conversion and accepts the documented point/write costs; retain ordinary ROW for workloads that do not. Large/Skew native conversion refusals remain explicit N/A. Linux was executed; macOS and Windows were not executed in this environment.
