# S1.1 acceptance: complete A1 design delivery

Date: 2026-10-14. Decision: S1.1 design delivery accepted. S2 production migration and semantic/resource/performance acceptance remain unfinished. The complete contract is en.md and design-profile.json; components do not substitute for a complete executor.

Basis: optimization.mdx section 3 design checklist and A1 batch, graph-intermediate, and execution-state contracts. S1 permits concrete decisions, evidence, or explicit later experiments; correctness/core interfaces close before their production adoption. Flat-first, disabled physical caching, and explicit target interfaces settle design choices without claiming unimplemented compiler/arena/grouped code exists.

| Acceptance requirement | Decision/evidence |
|---|---|
| Current call chain/adoption boundary | Section 1 audits buildPlan/logical clone/optimizer/executeOnSnapshot/visitOperator/output; preserves frontend and shared evaluator with post-IR query-local binding |
| Concrete target interfaces | Sections 2/4 and JSON fix program/slot/operator/expression/pipeline, parameters, properties, batch/selection/types/dynamic owners and widths; compiled existing descriptors are distinguished from target containers |
| Compilation/semantics | Sections 3/5 settle scopes/imports/shadowing, projected-column priority, lazy errors, parameter/batch guards, dynamic fallback, NULL/missing/floating/overflow/empty input and algorithms; no second semantics implementation |
| Data/schema/cache identity | Sections 2/3 and authenticated two-directory observations/overlay view/revision; physical cache initially off; schema observation replaces neither data pins/fences nor capability/semantic/parameter profiles |
| Ownership/budget/borrows | Sections 4/7 specify fixed-column capacity/overlap, arena/explicit legacy owners, batch owner/incarnation, final owning output and destruction order; complete A6 preallocation remains an implementation dependency |
| Graph intermediates | Section 6 fixes flat-first and grouped offsets/correlation/multiplicity/NULL/path/order, consumer expansion and representative experiments; unverified factorization is not forced and Cartesian semantics are not changed |
| Execution state/lifetime | Section 7 and implemented shared/task state separate immutable program, query shared, task, blocking merge and output; single-threaded execution and O7 coordination/error/order/pin prerequisites are explicit |
| Failure/recovery/reclamation | Section 7 propagates budget/cancel/corruption/conflict without replay; in-memory only, no new format/WAL/retirement; pin reclamation and A6 file-internal spill boundaries are defined |
| Migration/removal | Section 8 inventories adapters, ownership/semantics and per-path removal gates; unmigrated operators stay incomplete, with no undeclared uncharged row path |
| Test inventory/evidence | Section 9 distinguishes existing boundaries from future compiler/arena/grouped/parallel tests; real schema/property/overlay and columns/slots/types/budget/nested execution verify implemented foundations |
| Hypotheses/falsification | Section 9 identifies compilation/dynamic copy/conversion/grouping/resource/semantic counterexamples and original final matched control/current/Kuzu gates; no iterative benchmark |

Necessary current-source validation: 127 A1 tests plus 103 schema/property/overlay tests, with 23 shared cases (11 property-table, 12 slot-registry), equals **207 distinct passes, zero failures**. Cases include new catalogs/old pins, untokenized staged fields, database/reopen/DDL/overlays, every observed allocation/cancellation point, rectangular rollback, selected/numeric/nested input and budget restoration. Logs/link commands/source hashes are in `benchmarks/architecture-design/results/20261014-a1-design-contract/`. No full suite, performance benchmark, or cross-platform build claim.

This accepts complete design choices and implemented foundations, not S2.1–S2.5/A6/O7/final performance. Dynamic arenas, complete physical containers/compiler, grouped prototypes, and full operator migration remain unimplemented. Future gate failures cannot be relabeled complete. Explicit later experiments do not presume successful results.

The denominator remains 34. S1.1 adds one acceptance: S0 1/4 (25%), S1 3/7 (42.9%), S2 0/5, S3 1/5 (20%), S4 0/4, S5 0/5, S6 0/4. Overall **5/34 (14.7%)**. Current JSON is synchronized; formal optimization.mdx retains historical text. Code, bilingual contracts and evidence commit/push under existing authorization when acceptance increases. The full optimization goal remains in progress.
