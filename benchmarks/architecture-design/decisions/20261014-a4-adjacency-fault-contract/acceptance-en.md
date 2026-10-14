# S1.4 design-delivery acceptance

2026-10-14: accept S1.4 design. en.md/zh.md define authoritative master topology, current key-only layout, two concrete shared candidates, atomic dual-direction publication and snapshot/overlay/retirement semantics. The fault table assigns torn-write, owner/generation, covered disagreement, and omitted-member responsibilities and discovery timing. First adoption preserves master verification; no changed public fault timing or certificate-only trust is enabled.

Selected adjacency codec/index, integrity/corruption, and native transaction atomicity/fault modules passed **126 tests in five suites**. Raw logs, link command and source hashes are in matching local results. Existing omission and wrong-endpoint tests use valid codecs; structural identity and short-write/ENOSPC cases are included. No full suite or benchmark. S4 production covering formats, lifecycle integration and performance remain pending.

This milestone: **2/5 newly accepted**. Total **8/34 (23.5%)**, S1 **6/7 (85.7%)**. current-progress.json is synchronized; the historical optimization table is unchanged. Commit/push is authorized.
