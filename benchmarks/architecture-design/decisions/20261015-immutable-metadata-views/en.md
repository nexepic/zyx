# Immutable decoded metadata views

Component implementation only: acceptance remains 22/34 (64.7%).

Read-only routing, Catalog and location lookups/visits now share immutable decoded cache pages. Retained location range cursors can retain the immutable view. Existing mutable/COW page APIs still return independent copies. A cache miss retains its decoded page inline in a move-only view, so skipped optional caching does not require another heap control object. Reference generation, visibility, directory type, radix membership, IO identity/epoch and codec checks retain their existing responsibilities.

Tests fill the entire arena and verify hits require no payload clone or additional PMR allocation. Views survive cache/context eviction, cache invalidation preserves earlier immutable bytes, mutable copies cannot change a borrowed page, wrong page types do not alias, invalid hints/generations still fail, and zero-retention cache misses need only the decoded payload allocation.

Validation: 126 focused tests and the final 1,213 selected tests / 51 suites pass; four transaction OOM/retention workers pass (closed wrappers1,736 requested bytes). No full suite or performance campaign ran. Two expensive native sort integrations remain excluded from this selected run. No speedup relative to Kuzu is claimed.

For reproduction, the evidence archive adds the complete tracked working-tree patch against the recorded base commit plus every untracked task source/header/script. Source hashes and exact module recipe/XML are included. The patch predates this documentation note. Earlier reservation/cursor checkpoint describes all resource changes and remaining work; no requested delivery has reached acceptance.
