# Coordinated Property-prefetch allocation ownership

This checkpoint accepts no stage. Progress remains 22/34 (64.7%).

Coordinated Property-prefetch now uses BoundedBatch's shared PMR capacity and payload reservation together. Cache enrichment pre-admits into that same reservation, then reconciles the estimator; split/rebatch transfers embedded row ownership without refunding and re-admitting payloads. Selected rows retain an independent charge while moving to the downstream receiver. Rejected rows are destroyed in place before their charge is refunded, avoiding both an untracked discard window and unnecessary independent-row admission. Legacy uncoordinated logical-byte semantics remain unchanged.

For native nonthrowing ownership moves, appendCharged keeps the incoming value after its reservation and destroys a rejected insertion before rolling back the batch's charge. This fixes validation/allocation failure destruction order rather than merely asserting counters after unwinding. Generic throwing-move types retain their existing fallback behavior; native row/record ownership moves are nonthrowing. New tests observe the budget during actual payload destruction.

A real native Property filter runs matching, fully rejected and matching inputs again under a 32 KiB account; each result's retained charge matches the account and releasing it returns query ownership to zero. Final source passes all 681 selected tests in 28 suites. No full suite, performance campaign, commit or push occurred; no fixed delivery has newly met its full acceptance criteria. Evidence is archived with source hashes in proof.json. Plan/write/maintenance, temporary workspace audits, backlog admission and partitioned hash/aggregate remain required before S5.3/S5.4 can close.
