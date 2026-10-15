# Native plan-object and cache ownership

Component evidence; still22/34 (64.7%), with no stage acceptance.

Logical operator objects retain their allocation resource in a prefix, including failed construction and escaped polymorphic unique_ptr lifetimes. Public buildPlan uses a bounded QUERY account. execute binds its caller account before compilation; private source/cache-hit operator objects and execution share that account. Node credit is released before returning a result, while escaped plans retain it until destruction.

Native cache state, query text/index and cloned operator objects share the backend CACHE owner. Rebinding prepares replacement state before discarding old entries; failure preserves them. clear frees retained container capacity while preserving admission authority for subsequent insertions. Explicit engine lifecycle reset also releases the owner; Database.close already calls this reset. Four previous tests assumed backend closure alone destroyed the still-live engine cache; they now check retained CACHE credit and preserve zero-credit assertions after reset.

The index borrows stable text owned by LRU entries and erases borrowed keys before eviction. An8192-byte query needs one text-sized PMR request instead of two; tests cover rehash, caller mutation, updates and eviction. The previous >2000-byte retention assertion depended on duplicate1000-byte text; it now checks >1000 bytes plus an independent strict one-text-allocation probe.

Parser wrapping preserves admission denial, local quota denial and bad_alloc. All590 selected module tests plus12 Database lifecycle tests pass. The final index version passes100 isolated cases, including96 sampled points among25092 normal miss-path allocations. The earlier index version passes868 isolated cases and32 crash/readonly/recovery cycles; its production-source difference is retained in pre-index-borrow-PlanCache.hpp and this campaign is not described as a final-source rerun.

No performance campaign, RSS result, Kuzu win or macOS/Windows execution is claimed. Standard internal IR strings/containers, expressions and other private workspaces remain open; object accounting is not whole-plan heap accounting.
