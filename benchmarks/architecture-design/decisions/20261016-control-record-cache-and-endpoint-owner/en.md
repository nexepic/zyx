# Final control-record coherence and endpoint ownership

The publication validator correctly rejected a newly assigned frame whose cached pool header still marked it free. A deterministic read-before-pwrite interleaving reproduced the cache refill gap. The buffer pool now holds its existing mutex across eviction and the exact control-record write; partial failure preserves the surviving bytes without a retained dirty image. Authentication remains mandatory.

Full medium preparation separately exposed endpoint checks before mutation staging with no explicit allocation scope. Transaction snapshot existence probes now default to their WRITE owner and preserve an explicit caller owner. Exhausted-CACHE negative control fails before the fix and passes after it; no resource cap changes.

The two focused modules passed 117 cache/I/O tests and 58 backend resource tests. Final regression passed 2,182 tests in 102 explicitly selected modules and 142 suites. No full suite ran. Bounded threaded replay, renewed source freeze and fresh full-size preparations are tracked separately; no performance acceptance is claimed here.
