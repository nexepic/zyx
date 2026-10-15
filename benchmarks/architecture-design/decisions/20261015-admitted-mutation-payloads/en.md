# Admitted native mutation payloads

Component implementation and verification; accepted progress remains 22/34 (64.7%).

Native ordinary staging now admits a private normalized input before copying payloads or reserving new creation identities. Input sizing follows supported standard-container copy behavior, rejects excessive depth, and checks cancellation during sizing and between outer-copy chunks. Private request slots remain PMR-owned. Retained strings, lists, bytes and maps have a separate WRITE lease, transferred without refund/re-admission and destroyed after the buffers it covers. A new statement pre-admits its old-prefix clone and reserves the complete outer-slot bound before adoption.

Property folds borrow keys/values in PMR maps and admit the entire result-copy workspace before construction. Consumed incoming inputs are freed before that workspace is refunded. A counterfactual that retained those inputs exposed 190,971 uncovered requested bytes; the fixed independent allocation observer found zero uncovered bytes in five native stages, including small-to-large growth and successive patch merging. Retained private requests peaked at 357,601 observed requested bytes. These are allocator requests, not malloc overhead/RSS, and the observer excludes caller-owned by-value inputs by starting at the first native checkpoint.

Canonical batches pre-admit copied nested payloads, standard outer vectors, durable receipts and creation-plan vectors. Canonical deduplication borrows retained requests until its final reserved copy. Canonical patch slots use the WRITE allocator; normalized delta payloads and temporary replacement slots are admitted before copying. Undo/overlay nested payloads, snapshot-source materialization and other publication/compiler workspaces still require ownership work. This is not comprehensive heap accounting.

Final verification: 63 relevant module tests pass. An earlier broader 1,982-test selection passed 1,979; three old assertions were corrected after identifying earlier private-copy rejection and the resulting absence of ID consumption. The final module selection covers those assertions and the consumed-input lifecycle fix; this does not claim the whole earlier selection was rerun on final source.

The frozen final worker passed 583 isolated cases: 576 allocation points (short paths exhaustive, long paths sampled at 96 each), four terminal cases, two heap-accounting cases (22 standard/input shapes and five native stages), and one flow containing 32 real crash/read-only/recovery cycles. Closed native transactions retain 1,856 requested bytes until destruction, below the unchanged 16 KiB bound. Exact per-phase sample counts are in proof.json and raw logs/source in module-evidence.tar.gz.

No full suite or new performance campaign ran. No whole-stage acceptance, Windows/macOS execution, universal OOM guarantee or Kuzu performance win is claimed.
