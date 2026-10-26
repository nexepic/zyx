# V1 candidate implementation and exit record

Plan scan-supply-20261026 has accepted V1 on correctness and work evidence at 45% progress. This directory is not formal performance evidence.

The candidate retains the persistent format: admit bounded typed runs before output, retain at most three budgeted field frames per scan window, and consume primitive columns directly while preserving mixed fallback and NULL/missing/NaN semantics. Cancellation remains bounded and exceptions cannot replay delivered prefixes. Freeze exact source with the final candidate.

## Verified work and results

- The scan-format Release core compiled. check-work.py executes four untimed read-only public queries on persistent medium input with complete passing oracles. Commands and binary/input hashes are in work-receipt.json.
- work-counter.cpp uses V0 certified supply and consumer semantics once per case without timing. All-node still examines 225,000 candidates, delivers 220,000 values in 440 batches and returns exactly 10,076; total admission callbacks fall from 220,880 to 4,349. Labels retain the same examined population, deliver 100,000 values and return 12,500; admissions fall from 100,880 to 2,474. Reductions are 50.79/40.78x, **not query speedups**.
- Public typed-run admission counters are 3,437/1,562. These differ from total admissions and must not be conflated.
- work-comparison.json shows unchanged field-page reuse, column decodes, checksum bytes and BufferPool read bytes on this input. Three-frame retention is not yet a demonstrated benefit; resource costs and other access patterns determine adoption.
- Existing certified identities and batch operators are publicly adopted. Online-format modules and lifecycle acceptance remain required; historical passes are not inherited.

## Failures and configuration boundaries

The first eight-module run incorrectly linked the SCAN=1 offline format while testing online initialization. Retain module-tests.json and logs as failures, never passes. Online modules now use HYBRID=1/SCAN=0; their core is rebuilding. The test build overlays current fault-hook StorageIO on the production archive; it is not a fully hook-enabled core build.

preliminary-old-probe-* predates completed relinking; preliminary-overlapped-* has overlapping output paths. Neither is candidate acceptance. The serialized check-work.py work-receipt.json and candidate-work.* supersede them. Preliminary online tests linked the old archive and require relinking and verification once the new core finishes.

Preliminary online modules passed 10 field-mutation, 8 reuse and 61 public lifecycle tests. ROW metadata fault fixtures are being checked separately with current metadata/directory/tree source over the ROW dependency archive. One typed-run assertion found a redundant final-page copy: production now skips that cursor image after all run borrows are consumed, retaining non-run caching. Final relink validation is pending; assertions are unchanged.

## Remaining

Longer pressure/fault review, long update/pinned-snapshot reclamation, V2 layout selection, and final paired control/candidate/Kuzu comparison. No formal candidate P50/P95, 2x gain or Kuzu attainment is claimed. Run affected modules only, never the full suite; progress and milestone commits require stage exits.

All 277 final module cases passed; see final-module-receipt.json. Recompiled online archive and source hashes match final test linkage. Relinking scan modules against the final archive preserves the test binary byte-for-byte, avoiding an unnecessary repeat. ROW metadata validation uses the explicitly listed current source overlay, not a fully rebuilt ROW core. The page-copy assertion passes against the new production archive.
