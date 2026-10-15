# Property predicate/container ownership

Node-scan property/range/parameter/composite predicates, traversal filters, create/merge property maps, UNWIND and procedure argument arrays pre-admit copies. Raw maps/vectors normalize by copying; rejection preserves old values. Owned map moves swap an admitted empty structure to avoid deep copying; storage dies before refund.710 selected tests,441 after the move adjustment and105 isolated cases pass, including96 sampled cache faults and6 full-plan heap probes with no uncovered requests. Other compiler/runtime workspaces and steady-state contracts remain open.

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no Windows/macOS runtime, RSS, new timing campaign or Kuzu improvement claimed.
