# S2 private BUILDING membership publication checkpoint

Acceptance remains **15%**. S2 has not passed; no performance test, commit or push accompanies this checkpoint.

The offline builder capability can now install one real primary-node group in a private BUILDING image using the existing allocation, metadata batch and A/B publication protocols. The request contains no caller-provided liveness or labels and cannot be default-constructed by ordinary callers. Every installed group is derived from all possible primary IDs and their complete label sets. Existing field roots survive publication. BUILDING remains set, and the public backend remains closed.

Complete integrity verification now compares the ordered directory with the primary node/label source under a bounded grant, checks declared counts and rejects missing, partial, orphan or semantically incorrect records, including records with valid checksums. Reverse page ownership checks cover the new native record. The full-width directory key handles terminal uint64 IDs.

Targeted validation passed: 11 isolated publication/directory cases, 24 isolated source/format/page cases and 2 default-profile cases. These are configuration counts, not distinct test counts. Tests cover stale fences, duplicates, 65-label refusal, pre-I/O quota rejection, cancellation after private writes, partial/complete superblock writes, reopen and old snapshots, wrong labels, orphan ownership and allocator admission. The default-profile test compiles the new guard and links the existing default core; it does not establish a complete default rebuild. See receipt.json and test JSON/logs for scope.

The first multi-group test exposed a real fixed-key-width bug: the ninth hybrid namespace was classified as a 16-byte owner key by modulo arithmetic. Its explicit scan namespace now uses the required 24-byte encoded entry, while non-scan profiles reject that namespace. No assertion was weakened to accommodate it.

Whole-file construction/field conversion, physical space and peak budgets, query consumers, mutation maintenance and final performance gates remain outstanding. The final performance window will compare the finished implementation with the frozen initial control and Kuzu; no speedup or superiority is claimed here.
