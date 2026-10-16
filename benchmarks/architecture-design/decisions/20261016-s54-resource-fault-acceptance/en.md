# S5.4: resource fault and cleanup acceptance

Decision: accept S5.4. This delivery checks low-memory, cancellation, disk-full, read-only and crash-cleanup behavior. It does not accept S5.3, S5.5, S6.1 or S6.3, or count component changes as separate stages. The plan permits work across stages according to dependencies.

The source baseline is `05cba31b046cafc26775ecb1cec6dd80367b112f`. This change adds acceptance evidence without changing runtime paths. The unused OwnedFunction prototype is outside this delivery; this decision does not claim that every native allocation is governed.

## Acceptance matrix

| Condition | Evidence |
|---|---|
| Low budgets, concurrent totals and refunds | Query/database/process admission, alias sharing, rejection before publication, retained results and destruction refunds |
| Cancellation and early stop | Cancellation after sealed sort runs and before first publication, stopped output, rollback at DDL checkpoints |
| Disk full and uncertain I/O | Actual ENOSPC/platform-equivalent injection, short data/root writes and final-sync failures; readable old graph, fenced writes and reopen recovery |
| Logical and physical read-only | Actual sorting spill in a logical read-only query on a writable file; physical read-only refuses scratch and never writes during orphan inspection |
| Single-file scratch interleaved with graph commits | No lost graph roots or changed graph fence, isolated private handles, reclamation despite long graph pins |
| Corruption | Corrupt payload/catalog cannot reclaim user pages or refund incorrectly; no partial successful result |
| Partitioned hash results | DISTINCT/UNION preserve NULL, duplicates and first-occurrence order; output rejection cleans spill |
| Actual process crash recovery | 32 `_Exit(87)` cycles, unchanged file SHA256 during physical read-only inspection, preserved graph value 7, reusable quota and successful integrity checks after writable recovery |

`required-cases.json` maps these conditions to 19 cases required to execute and pass. All 170 tests in 12 affected modules pass on the final source, without disabled, skipped or failed tests. XML and logs are preserved here. The unchanged source/worker also has 1,275 isolated cases across 19 allocation paths, retained in adjacent `20261016-owned-catalog-keys-and-schema-encoding/zyx-owned-catalog-keys-all-faults.json`. Long paths sample at most 96 points; short paths are exhaustive. This is not exhaustive coverage of every possible fault.

The fresh 32-cycle crash sequence retained exactly 1,572,864 file bytes throughout. That demonstrates reuse for this fixed recovery sequence; it does not replace O6 throughput/P95/backlog measurements or S6.3 long-duration resource testing. No new performance campaign ran.

## Reproduction and limitations

In the existing Linux Clang19 coverage Ninja environment, source `/workspace/zyx-cloud-env.sh` and run this directory's `run_selected_tests.py`. It builds only the listed 12 test translation units and core, then links a separate executable without building/running the full test target. Crash reproduction:

```bash
python3 scripts/test_transaction_resource_faults.py --worker /workspace/zyx-optimization-artifacts/frozen-workers/zyx-owned-catalog-keys-worker --output /tmp/zyx-s54-crash-recovery.json --crash-recovery --crash-cycles 32 --progress-every 8
```

`proof.json` records worker, evidence and source-manifest hashes. Runtime evidence is Linux x86_64/Clang19/libstdc++14 only; the Windows injection branch is present in source but no macOS/Windows run is claimed. Limits and assertions were unchanged. No full suite or new performance conclusion is claimed. Preserve evidence and the necessary worker; isolated runner databases are automatically removed.
