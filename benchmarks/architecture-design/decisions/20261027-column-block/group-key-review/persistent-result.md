# Borrowed group-key persistent comparison: do not adopt

The frozen protocol is `persistent-protocol.json`. All ten deterministic persisted SCAN fixtures were generated and hashed in `fixture-receipt.json`. The source writer enforces limits on owners and changed fields: attempts at 512 and 64 owners failed; 16-owner write batches completed. Batching preserves the same independently generated properties; the full result oracle passed for both arms in every timed invocation and in each separate work run. The two failure logs remain `/tmp/zyx-borrowed-fixture-run.log` and `/tmp/zyx-borrowed-fixture2-run.log`.

Each scenario used three paired processes, three warmups and eight measured samples per arm per process, alternating order. Every returned group and count was compared to an independently generated oracle, and reported time includes query execution plus complete result traversal and oracle-map construction. Release runtime objects used `-O3 -DNDEBUG`, coverage disabled, and test hooks removed from the timed query runtime. They differ only in `ZYX_EXPERIMENTAL_BORROWED_GROUP_KEY`; the same core archive was linked to both and that archive was built with test hooks, so this is not a fully hook-free library build. Build hashes and scope are in `build-receipt.json`; every sample is in `persistent-receipt.json`; analysis is reproducible with `analyze-persistent.py`. A separate untimed observer confirmed the public plan `Aggregate -> NodeScan` and 512 direct input rows for the first fixture. Separately instrumented work runs confirmed the direct path and full oracle for all ten fixtures (`persistent-work.json`).

| Rows | Scenario | Control median ms | Candidate median ms | Control/candidate | Process ratio range |
| ---: | --- | ---: | ---: | ---: | ---: |
| 512 | repeated, short | 3.374 | 3.563 | 0.947 | 0.613–2.402 |
| 512 | unique, short | 6.443 | 7.043 | 0.915 | 0.804–1.150 |
| 512 | repeated, long | 7.663 | 9.447 | 0.811 | 0.504–1.297 |
| 512 | unique, long | 17.954 | 18.933 | 0.948 | 0.822–1.037 |
| 512 | mixed/fallback | 3.276 | 3.052 | 1.073 | 0.836–1.398 |
| 1536 | repeated, short | 7.500 | 9.065 | 0.827 | 0.680–0.931 |
| 1536 | unique, short | 23.849 | 22.646 | 1.053 | 0.736–1.778 |
| 1536 | repeated, long | 28.057 | 41.350 | 0.679 | 0.719–1.016 |
| 1536 | unique, long | 68.290 | 57.853 | 1.180 | 1.023–1.219 |
| 1536 | mixed/fallback | 10.281 | 12.799 | 0.803 | 0.688–0.951 |

A ratio greater than one favors the candidate. The three-process ratios show substantial instability and are not a P95 estimate; no timing ratio is interpreted as a causal attribution. OS peak RSS was reported as 31360 KiB in every process arm, which is insufficient to resolve query-level differences; query allocation counts were unavailable. The measured time includes constructing the oracle map, so it can dilute a query-only difference. No cold-cache, concurrency, write, broader workload or Kuzu conclusion follows.

The untimed counters show actual work and cost, rather than inferring them from timing. At 1536 rows and six repeated short keys, encoded rows fell 1536 to 6 and encoded bytes 39936 to 156, with 1530 borrowed hits and 296 requested index bytes retained at the observed peak. For long repeated keys, encoded bytes fell 1600512 to 6252. At 1536 unique keys, there were no hits or serialization reductions; the added index requested 68008 bytes. Mixed/fallback produced 1304 hits, 232 serialized rows, and 488 requested index bytes. Requested allocator bytes are not RSS and do not include keys or the whole aggregate state.

Decision: keep the candidate OFF and stop this candidate route for now. The bounded comparison does not establish stable net benefit; several scenarios regress, cross-process variability is large, and unique keys add index memory with no construction reduction. Do not introduce observed-data thresholds to hide those costs. Preserve the candidate, protocol, negative result and raw samples. This does not demonstrate that a different design is impossible, and does not complete R1 or R2.
