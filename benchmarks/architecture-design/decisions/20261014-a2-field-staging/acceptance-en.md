# S3.1 acceptance: A2 and legacy-layout regressions

Date: 2026-10-14. Decision: S3.1 accepted. This covers A2 implementation and regressions on the current layout; it does not accept A3, unified resource accounting, the later complete fault matrix, or performance improvements.

Basis: A2 and S3.1 in formal optimization.mdx and the A2 design contract. The concrete carrier refinement is documented in en.md and preserves all migration objectives.

| Requirement | Implementation and direct evidence |
|---|---|
| Separate keyed SET/REMOVE and full replacement | Native combined patches and transaction raw staging use explicit entry points. Complete batches contain no raw fields that consumers could ignore. Default capability is disabled/raw entry rejects explicitly; legacy full-state adapters pass |
| Existing four Cypher property semantics | Runtime introduces no second semantics path. Mutation/atomicity/validation, FOREACH/map/aliases, and real Cypher transaction modules pass |
| Full references and snapshot identity | Entity kind/reference kind/ID, source fences, and overlay revisions are checked. Equal Node/Edge IDs, creation/provisional references, and external-writer conflicts pass |
| Statement visibility and final field folding | Values remain correct across 32 real Node/Edge raw statements. Retained complete targets 0, raw targets 2, entries 2; last SET/REMOVE/types, restoration, full-map resets, and stored NULL pass |
| Creation updates/deletion | Creation folding and subsequent provisional references pass. Existing creation/deletion canonicalization and DETACH regressions pass; earlier raw fields do not rewrite deleted targets |
| Bounded validation, cancellation, atomic failure | Positive bounds, source admission, invalid/duplicate/conflicting keys, overflow, read-only, and ingest mode checks pass. Cancellation at every observed checkpoint in three real staging states preserves prior statements and successful commit |
| Atomic derived-state publication | Native tokens, scalar/vector/constraint/statistics retain one native publication. Unique/NOT NULL, types/collisions, unchanged indexes, and atomic companion-label rejection regressions pass |
| Pins and old-layout lifetimes | Owning fields retain no page borrow. 16 KiB source/old snapshots and LargeValue/property-root/store regressions pass, as do reopen and integrity. Codecs are unchanged |
| Failure/recovery | Graph-fence conflicts have an explicit state. Real explicit Cypher final-sync failure reports INDETERMINATE and reopen sees a complete old/new state. Existing writer failure/reopen modules pass |

Validation: 507 distinct selected module tests passed with zero failures, plus supplemental reopen integrity verification for the wide-property case. Source/logs and module link commands are in the results directory identified by en.md. No full suite or benchmark ran; macOS/Windows builds are not claimed.

A2 explicitly permits the existing layout. Complete root encoding, complete visibility entities, and full-replacement adapters therefore remain valid for this item; they do not justify skipping A3. A6 query-wide capacity/accounting and S3.5 resource/performance thresholds remain unaccepted. Work entries do not substitute for those metrics. S3.2–S3.5 remain unfinished.

The fixed denominator remains 34. S3.1 adds one acceptance: S0 1/4 (25%), S1 2/7 (28.6%), S2 0/5, S3 1/5 (20%), S4 0/4, S5 0/5, S6 0/4. Overall 4/34 (11.8%); authoritative metadata is `../current-progress.json`. Formal optimization.mdx retains its historical state. Code, bilingual decisions, and evidence are committed/pushed under the existing authorization when acceptance increases. The complete optimization goal remains unfinished.
