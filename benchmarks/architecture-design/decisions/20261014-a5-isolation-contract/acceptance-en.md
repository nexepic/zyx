# S1.5 design-delivery acceptance

2026-10-14: accept S1.5 only. en.md/zh.md define pinned-snapshot/conservative-write isolation, allowed and forbidden histories, current seven-root fence and limitations, target dependency keys, publication validation, bounded latest-page rebuild, conservative fallback, costs, retry and indeterminate-commit boundaries. Public transaction documents are synchronized without weakening production fences.

All five required interleavings have native tests: unique keys, endpoint deletion, write skew, range reads, and disjoint entities sharing an actual property page. The selected isolation, transaction-validation, and transaction-atomicity modules passed **57 tests**, including five new cases; raw logs, link command and source hashes are in matching local results. No full suite or performance run. Fine-grained runtime validation, concurrent-owner support and S5 acceptance remain pending.

Newly accepted in the user's five-item milestone: **1/5**. Total **7/34 (20.6%)**, S1 **5/7 (71.4%)**. current-progress.json is authoritative; the historical optimization table is unchanged. Commit/push is authorized by the standing progress-checkpoint instruction.
