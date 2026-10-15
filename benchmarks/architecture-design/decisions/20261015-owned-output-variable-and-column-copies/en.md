# Output-variable and result-column ownership

36 native operators expose admitted output variables while retaining the public legacy API. Read-result columns are generated under QUERY, their standard-vector copy is admitted first, and existing credit detaches into retained output ownership. Empty Windows debug-vector proxies are admitted before result construction; source review is not Windows execution. Public borrowed column arrays pre-admit copies and self assignment is a no-op.1063 selected tests and106 isolated cases pass; after the final empty-proxy adjustment,57 tests and10 isolated cases pass. Rejected large column copies allocate payload zero times; accepted copies once. Other mutation/procedure output, compiler/runtime workspaces, unnamed aggregate rendering fallback and steady-state acceptance remain open.

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no macOS/Windows runtime, RSS, new performance campaign or Kuzu benefit claimed.
