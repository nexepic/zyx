# Allocation admission for shortest-path temporary state

The BFS queue, depth/parent hash tables and returned path previously had local logical-size limits without shared admission for their actual container requests. The negative control made three topology reads under a one-byte caller budget before failing.

The queue and both tables now use allocators retaining their authority, charging actual deque blocks, bucket arrays and nodes. The returned path owns its allocation credit until procedure output materialization completes. Local algorithm limits remain separate, avoiding duplicate global charges for logical estimates. Endpoint identities use a stack array and deduplicated view.

Two new cases check rejection before the first topology read and complete partial-state release/retry after a post-read container request is denied. The latter rejects at a read checkpoint instead of assuming identical libc++/libstdc++/MSVC deque block sizes. All six procedure identity/resource tests and ten Cypher algorithm tests pass, without weakening assertions to preserve the old bug.

This does not change accepted stage progress or claim completion of other GDS/compiler/runtime workspaces. No full suite or new performance campaign ran. Negative/final logs, XML and source hashes are retained here; runtime evidence is Linux Clang19/libstdc++14 only.
