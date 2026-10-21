# Bounded native large-field inspection

Tasks: R3.2/R3.3, implementation checkpoint only. No newly accepted roadmap item. The working tree is based on remote `work` at `9cc2821c`; changed/new C++ identities are in `source-manifest.json`. No commit or push is performed because accepted progress has not increased.

`LargeValueStore::verifyContent` reuses the existing authenticated manifest/chunk traversal and computes the complete SHA-256 without allocating a value-sized output. It retains only bounded decoded chunks and the manifest frontier, with the existing range-read admission. Cancellation and I/O/corruption errors propagate; only a complete verified root returns.

The canonical shape inspector now accepts immutable range callbacks through the same validation implementation used by borrowed buffers. Child limits, flags, scalar widths, nesting/count bounds and bytewise canonical map ordering remain checked. Map keys are compared in 256-byte blocks without allocating full keys. This API requires the caller to authenticate the complete immutable source; skipped string/byte bodies are not independently content authentication.

Native integrity, non-NULL inspection and encoded-output pre-admission share `HybridPropertyLargeFieldInspection`. It authenticates the entire value, checks the existing single-field envelope/token and keeps at most one authenticated raw chunk for range reuse. Manifest, temporary output, cached chunk and directory credits remain live through their backing storage. Non-NULL checks preserve header-only structural semantics after content authentication. Owning value decoding keeps its existing output/allocation contract.

Scoped checks cover bit mutations/truncations, long and unsigned map keys, reader errors/cancellation, hierarchical manifests, compression, valid chunk checksums with incorrect complete hashes, quota denial/retry and the existing range/snapshot/reopen/fault module. A native 512 KiB field passes integrity, non-NULL and pre-admission within 128 KiB without decoding a PropertyValue. The hierarchy test also rejects an owning read under the same budget. These are resource/correctness checks, not performance results.

Latest per-case results are 86 default and 124 candidate passes, accumulated across the scoped runs in `proof.json`; these are not single all-pass campaign counts. The first candidate run had one new-fixture failure: the large source consumed its fixed logical PageIds. The fixture now chooses unused IDs while preserving the historical small case and assertions; the affected module then passes. Original failure evidence is retained. No full unit suite ran. Only Linux x86_64 was verified.

## User-requested starting comparison

Before code edits, the actual `9cc2821c` Release comparison binary was rebuilt. The small scan baseline ran three independent rounds, three warmups and ten measurements per workload, one thread and no PROFILE. Scalar and complete aggregate/Top-K result checks pass in all rounds. The binary and source manifest remain under `benchmarks/architecture-design/results/20261020-work-resume/start/`; committed compressed samples, registration and source hashes are in `raw/`.

Median of process P50s (ms): label filter 11.4647 versus Kuzu 0.276455; actual group-by 11.5349 versus 0.409294; one-hop 0.772040 versus 0.231327; two-hop 0.955275 versus 0.522976. All ten measured workloads are slower than Kuzu. Full distributions are in the compressed comparison JSON.

This small starting observation is not R7 admission: native C++/IPC and Kuzu Python differ, native benchmark budgets remain at defaults and Kuzu's buffer pool is explicitly 64 MiB. Page-cache and query budgets are distinct; this is not a strict equal-resource comparison. It does not replace the final medium/holdout, write or steady-state resource window.

Remaining: native physical data/manifest/content-index/directory/descriptor publication, authoritative writes and bidirectional layout transitions, global recovery/integrity/reclamation/relocation, complete costs, R5 candidates and final R7 admission. Nonempty durable layout guards remain and production properties stay ROW.
