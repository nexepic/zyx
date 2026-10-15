# Write-item and expression-map ownership

SET/MERGE/REMOVE action strings and backing own admission. CREATE/MERGE expression maps admit buckets/nodes/keys before cloning child expressions under the same owner. Partial cancellation frees clones and preserves sources.1006 selected tests,714 after REMOVE and105 isolated cases pass; full-plan heap probes cover10 shapes and expression probes12, with zero uncovered requests. Compiler/runtime workspaces and steady-state contracts remain open.

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no Windows/macOS runtime, RSS or Kuzu benefit claimed.
