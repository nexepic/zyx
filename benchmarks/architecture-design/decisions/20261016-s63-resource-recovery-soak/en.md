# S6.3: long-duration resource, recovery and cleanup acceptance

Decision: accept S6.3. This delivery validates repeated resource refunds, space reuse, recovery from long-snapshot pressure and crash cleanup. It does not accept all S5.3 budget integration or S5.5 steady-state performance. S6.1 final-source freezing and S6.2 performance comparison remain separate.

## Predetermined work and results

| Check | Result |
|---|---|
| Isolated rounds | 256 rounds, exactly 6 contracts each, all 1,536 executions pass |
| Native reopen and escaped results | 16,384 cycles; QUERY/CACHE refunds and result validity checked each time, domain expires after the final owner |
| Concurrent readers/writers/maintenance | 16,384 write iterations; at most two allocated segments, valid old readers, zero retained-generation backlog after draining, content verified after reopen |
| Long snapshots and file pressure | Each round reaches a fixed file cap without publishing rejected writes; releasing pins and reclaiming permits retry within the cap |
| Scratch pressure and cleanup retry | Each round verifies private space reuse despite graph pins, protected MAINTENANCE cleanup under saturated CACHE, and retry of the same handle after admission denial |
| Actual process crash | 512 crash/physical-read-only/writable-recovery cycles on one database; graph value 7, reusable quota and integrity verified each cycle |
| Recovery file space | Exactly 1,572,864 bytes throughout all 512 cycles |
| Isolated duration | 1,187.93 seconds, about 19.8 minutes, ending at the predetermined count rather than retrying until success |
| Same-process supplement | After callback changes, 16 shuffled rounds pass: 96 contracts, 1,024 reopen and concurrent-write iterations |
| Same-process peak RSS | 30,584,832 bytes (about 29.2 MiB), reported separately from chargeable memory and any absolute RSS limit |

The 256 rounds and 512 crashes were fixed before execution. Existing assertions and limits remain unchanged. The current acceptance gate postvalidates every required case's execution and success across all rounds instead of trusting only the last XML. Ten gate unit tests reject missing, skipped, failed, duplicated or substituted contracts. S5.4's long allocation paths remain sampled, not exhaustive.

## Source identity, reproduction and limitations

The isolated worker/test binaries come from `05cba31b`. The post-callback same-process binary comes from `e9d18095`, whose 170 affected resource tests pass. Later `1714cd1e` changes only shortest-path private workspaces, with 16 affected procedure tests passing; none of the six soak contracts invokes that procedure, so unrelated long runs were not repeated. Binary SHA256 values, source mapping and evidence digests are in proof.json. S6.1 still owns final-source freezing.

```bash
source /workspace/zyx-cloud-env.sh
python3 scripts/test_resource_recovery_soak.py --test-binary /workspace/zyx-optimization-artifacts/frozen-workers/zyx-resource-acceptance-tests --worker /workspace/zyx-optimization-artifacts/frozen-workers/zyx-owned-catalog-keys-worker --output /tmp/zyx-s63-resource-soak.json
python3 scripts/test_resource_recovery_process_soak.py --test-binary /workspace/zyx-optimization-artifacts/frozen-workers/zyx-owned-callback-resource-tests --output /tmp/zyx-s63-streamed-hash-rss-soak.json
python3 scripts/test_resource_recovery_soak_validation.py
```

An initial RSS observation was contaminated by the supervisor reading a large binary at once and was discarded. The final run streams SHA256 and uses a fresh supervisor with exactly one test child. RSS includes runtime, mappings and instrumentation overhead, separate from allocation quotas. Runtime evidence is Linux x86_64/Clang19/libstdc++14 only; no multi-day or macOS/Windows soak is claimed. No full unit suite, new performance campaign or Kuzu comparison ran, and this does not replace O6 throughput/P95 acceptance. Raw logs and JSON/XML are in raw-evidence.tar.gz; isolated databases were automatically cleaned.
