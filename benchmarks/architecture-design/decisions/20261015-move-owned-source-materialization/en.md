# Move owned scalar source materializations

Component verification; accepted progress remains 22/34 (64.7%).

Scalar full-property Node/Edge reads now move their locally owned raw records into logical materialization. Borrowed const-record APIs preserve their copy behavior. Cached label-name returns move the owned optional string. Node duplicate checks borrow stable output key/label views in PMR hash sets, preserving the hash algorithm and avoiding retained duplicate name copies. Metadata chooses an explicit owned scope or the snapshot allocation context; legacy context-free reads use new_delete_resource. Explicit temporary admission callbacks remain the sole admission authority for their resource, avoiding duplicate accounting. Temporary resources retain their upstream owner until destruction.

All 151 affected-module tests pass. The ambient-default allocator denial case now verifies both full Node and Edge rereads; it exposed and corrected an early metadata implementation that accidentally consulted the unrelated default resource. No test was weakened to allow that failure.

An independent allocation probe reads a 65,536-byte string property through complete native Node/Edge source paths. Each now requests the 65,537-byte string buffer once. A counterfactual restoring the const-copy branch requests it twice for each entity and fails the unchanged assertion. This measures allocation requests, not latency or a Kuzu comparison.

The final worker passes 103 isolated cases, including 96 sampled points out of 955 normal complete-source-read allocations, four terminal cases and three codec/copy probes. After a bad_alloc, the same snapshot retries complete Node/Edge reads, the user graph fence remains unchanged, integrity passes, and all coordinated resource credit is zero after closing. Successful payload checks are outside exception capture. The admitted-decoder heap and no-allocation inspection probes remain enabled.

Decoded source lifetimes/metadata, normalized/API payload copies and remaining publication/compiler workspaces still need ownership integration. No whole-stage acceptance, exhaustive 955-point sweep, full suite, new crash campaign, performance campaign or cross-platform runtime result is claimed.
