# Source responsibility split verification

Structure work earns no independent performance progress and changes neither persistent formats nor public APIs. Candidate acceptance belongs to scan-supply-v1.

- Write planning has real entity/vector/adjacency/commit/lifecycle translation units; metadata separates page mutation, reclamation and lookup. Method bodies are preserved in storage-splits.json. Two batched directory methods require the private cursor definition and remain in its original unit without exposing internals.
- Query runtimes, transaction overlays and large storage flows use private responsibility-based .inc phases, preserving translation units, unnamed namespaces and RAII lifetimes. Expanded hashes are recorded in inline-splits.json.
- ANTLR generation now deterministically splits rule families. WASM generation extracts a filesystem module. Regeneration preserves the 3000-line constraint, rather than only editing checked-in outputs.
- IMDb seed statement content and order are preserved; see scan-supply-v0/seed-split-verification.json.
- Twelve historical oversized source snapshots are losslessly archived under ../../archive/20261026-oversized-source-evidence/. Restore them as documented before replaying old experiments; old scripts require that preparation.

Verification: current scan and online-format cores compiled. All 277 selected module cases passed; see scan-supply-v1/final-module-receipt.json, including its explicit current-source overlay scope for ROW metadata. WASM tests use the existing checked-in binary: real Driver ABI open, transactions, queries and filesystem isolation between two instances passed, recorded in wasm-runtime-test.log. No new WASM binary, macOS/Windows run or complete browser build is claimed. scripts/check_source_size.py checks source sizes.
