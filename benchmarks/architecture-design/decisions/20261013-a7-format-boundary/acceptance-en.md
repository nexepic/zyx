# S1.7 A7 design acceptance

Date: 2026-10-13. S1.7 is accepted as a design deliverable. S1.1/S1.3–S1.6 and S2–S6 remain unchanged.

The review matches optimization.mdx requirements to zh.md/en.md: the factory/probe/bootstrap/local-reader chain (§1); root registration, interfaces and ownership (§3/5); the 42 sourced version constants, independent namespaces, snapshot/route identity and migration DatabaseId/EntityId rules (§1–3/5); actual codec/probe/select rejection layers, split brain, corruption discovery and failure/crash/indeterminate states (§2/5); current 4/30/local-version and flag matrices plus future field registration (§1–3 and version-profile.json); COW reachability, pins, retirement, integrity and protected-source migration (§3/5); replacement boundaries, stable-format triggers and separation from ABI/library versions (§3–5); 103 passing selected format/codec/factory tests, including checksum-valid old/future A/B rejection without changing file bytes or length (§6/results); and resource assumptions/falsification without a runtime registry or speedup claim (§7).

The plan requires stable-release prerequisites and version boundaries for this item, not implementation of a migrator. Future A3/A4 codecs/adoption, A6 resources and actual migration fault/cross-platform tests remain mandatory in their original stages and are not counted as implemented capabilities.

The added acceptance ID is S1.7. The fixed denominator is 34: S0 1/4 (25%), S1 2/7 (28.6%), S2–S6 0%; total **3/34 (8.8%)**. Native property consumers, explicit transactions and staging coalescing are implementation progress within S3.1; they do not close its entire migration/acceptance. The formal optimization.mdx table remains unchanged; this record and its JSON substantiate the current acceptance count.

The latest user instruction authorizes checkpoint commit/push after percentage growth. Actual execution status is recorded separately; this document alone does not prove a push. No full test suite or benchmark was run.
