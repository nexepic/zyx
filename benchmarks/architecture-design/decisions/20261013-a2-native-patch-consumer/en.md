# A2 native token property patch consumer

Date: 2026-10-13. The real backend and implicit Cypher write path are connected; 59 selected module tests passed at this checkpoint. S3.1/S3.5 remained open.

The internal combined patch entry is virtual. RelocatableStorageBackend validates the source snapshot/fence and uses the original publication lock, conflict checks and uncertain-commit protection. NativeGraphMutationExecutor reads tokenized properties in its fenced transaction, resolves SET names and looks up existing REMOVE tokens without creating tokens for missing names. Unchanged payloads move into the final token collection; created ordinals fold into creation fields. Overlapping replacements, invalid references/keys/NULL sets, deleted targets and entry limits fail before publication.

Reads follow logical identity order through one NativeReadSession. Encoded source admission, cancellation checkpoints and checked operation counts remain distinct from complete allocator budgeting. Creation field counts are admitted before copying/resolving input; persisted field counts are checked after reading. Unified A6 accounting is not complete.

Graph properties, scalar indexes, vector deltas, labels and statistics retain the existing atomic native publication. Constraint final-state views convert only relevant constraint fields; the authoritative root retains all properties. No-constraint batches avoid a second owning constraint payload copy. Created-edge overrides and properties move into final roots without redundant resolution/copying.

No persistent codec, public Driver ABI or durability protocol changed. The existing layout still encodes complete collections. This checkpoint did not claim local physical updates or complete explicit-transaction/consumer migration.

Current Linux x86_64/C++20 core and three selected suites passed 59 tests: real Cypher SET/REMOVE dispatch and output; Node/Edge/created-reference merges; unchanged-name avoidance and limits; old snapshots, a 16 KiB source, reopen and admission retry; multi-target uniqueness/companion-label atomic failure; relevant-constraint conversion excluding an unrelated 1 KiB value; graph/vector simultaneous publication; and cancellation injected at each planning checkpoint without partial graph changes. Low-level fixtures without LargeValue placement use a 1 KiB value; real backend coverage uses 16 KiB. No other-platform build, full suite, benchmark, commit or push was claimed at that checkpoint.

Commands, logs, hashes and verification/progress JSON are in `benchmarks/architecture-design/results/20261013-a2-native-patch-consumer/`. Historical acceptance remained S0 25%, S1 14.3%, S2–S6 0%, total 2/34 (5.9%). Native direct consumption did not itself close S3.1 or performance acceptance.
