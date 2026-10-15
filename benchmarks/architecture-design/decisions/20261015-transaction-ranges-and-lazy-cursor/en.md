# Owned transaction ranges and lazy directory contexts

Component implementation only: 22/34 accepted (64.7%); no delivery acceptance or performance campaign.

Ordinary transaction creation-range and preview-range buffers now use the same WRITE account as wrapper/issued-ID storage. Owned reservation copies retain their allocator explicitly. A failed preview cannot publish an empty or half-initialized preview record; failed staging preserves the prior preview and remains retryable. If range admission fails after durable ID acquisition, the acquired capability is abandoned. New span overloads abandon already-owned ranges without allocating conversion vectors, preserving rollback under allocator failure. Terminal reset retains the original allocator and cannot capture an unrelated ambient owner. Streamed reservation buffers and the store's outstanding lease maps are still outside this component.

A diagnostic fault exposed lazy Property directory decoding after its creation read scope had ended. The raw trace identifies MetadataPageRouteLeafCodec::decode allocating through the ambient resource. Scope ownership can now be captured strongly, nested/failed/thread-local bindings restore correctly, and each lazy cursor advance reinstalls its snapshot read context. Cursor objects and route vectors bind the same cache arena. Unowned object prefixes use permanent new/delete storage rather than borrowing a temporary default resource. Full Property and topology reads are tested while the ambient PMR resource refuses all allocations.

The corrected directory context also reuses decoded routing pages: repeated count reads need neither disk access nor an extra Buffer Pool hit. The regression now requires unchanged hits, rather than requiring an unnecessary lookup. No test budget was raised.

Validation: 1,209 selected tests / 51 suites pass; four isolated transaction OOM/retention workers pass with 1,736 requested bytes retained in each closed wrapper. No full suite ran. Two expensive native sort integration cases remain excluded from this selection. The archived negative diagnostic belongs to the earlier cursor implementation; final XML and worker evidence have zero failures.

Remaining: native plan IR/parser/compilation ownership, full mutation/Property/undo/overlay/COW and commit workspaces, outstanding lease metadata, maintenance/recovery classes and protected progress under pinned cache pressure, partitioned hash/aggregate spill, Driver ABI buffers, complete fault/long-run acceptance and final Kuzu comparison. Current requested delivery IDs are still unaccepted.
