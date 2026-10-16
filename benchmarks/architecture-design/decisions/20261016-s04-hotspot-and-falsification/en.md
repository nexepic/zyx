# S0.4 hotspot and falsification report

The diagnostic report is delivered; this is not a performance pass. Progress is 27/34 (79.4%), 3/5 this request. S0.3 full-family baseline and S5.5 steady-state acceptance remain pending.

The formal read comparison has 20 comparable workloads, 24 independent profile processes and 1200 valid samples with exact successful-result oracles. Three indexed workload rows retain unmeasurable outcomes. Medium scan/aggregate/Top-K median independent-process P50s are 3055.525/3203.310/3303.220 ms versus Kuzu 2.142/3.629/4.016 ms. The Kuzu goal is not met. Frozen control input preparation failed, so no 10% control gain can be established.

Complete single-query native counters are independent of CPU/heap collection. Bytes are cumulative. Buffer-pool read bytes are not physical device I/O, and cumulative materialization is not peak residency.

| Case | Checksum bytes | File-size calls | Metadata page reads | Buffer-pool read bytes |
|---|---:|---:|---:|---:|
| label_scan_filter | 3616675584 | 870558 | 856414 | 37621760 |
| aggregation_count_by_group | 3606618368 | 868136 | 854008 | 37621760 |
| topk_property_sort | 3689754304 | 888210 | 873874 | 38023168 |
| point_node_fetch_by_id | 55211328 | 13264 | 13164 | 73728 |
| two_hop_expand | 45252864 | 10866 | 10778 | 233472 |

Repeated checksum, directory resolution and temporary allocation are concrete cost candidates: label filtering checksums about 3.62 GB while reading about 37.6 MB through the buffer pool and issuing about 870k file-size calls. A single point read checksums about 55 MB. Actual CPU samples/allocation stacks repeatedly contain crc32_z, route-leaf decoding, CatalogRadix validation and MetadataPageReadContext/BTree containers. These are observations, not a complete causal percentage or permission to remove first-use authentication.

CPU, bounded heap and native counters run separately. Combined heap/CPU collection reached the unchanged 30s query timeout and is retained as failed. Five final cases pass result validation. Whole-process CPU sampling includes open/clone/warmups. Collector-reported CPU is roughly 1% of isolated OS user+system time; its seconds are not actual CPU time and PC proportions are low-confidence qualitative evidence only. OS usage is recorded separately; no complete-coverage or precise query-share claim. Nested timers are not summed.

Heap stacks/instances/bytes cover only an approximately 200ms wall window and are not extrapolated to whole queries or reported as RSS/peak query memory. Linux process I/O has a distinct scope; unavailable gprofng I/O collection is not replaced by zero.

Three independent mechanism processes, three warmups and ten samples each yield 150 fully checked samples. Selected native reads of 4096 values take 3.624ms. Over identical already materialized source values, conversion-inclusive typed filtering takes 0.06333ms versus generic 0.03990ms (1.587x slower). This falsifies a gain for that conversion-inclusive shape; it does not authorize broader adoption. Existing generic fallback remains and production performance admission stays with S2.5.

Full sort returns 4096 rows (17.052ms); Top-K returns 128 (11.816ms). Different output cardinalities prohibit a strict end-to-end gain, 2x or 10x claim. Mechanism results never replace formal owning-output latency.

The probe's zero-valued required limits were corrected before successful sampling, with the old failure preserved. Only diagnostic setup changed: no core/format/durability change or materialization-cap increase. Production source still matches the freeze verified by 2628 selected tests. S0.4 report acceptance does not accept S2.5/S3.5/S4.4/S5.5/S6.2 performance gates.
