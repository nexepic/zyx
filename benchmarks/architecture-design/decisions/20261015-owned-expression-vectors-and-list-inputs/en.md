# Expression vectors and admitted list inputs

Component evidence; still22/34 (64.7%), with no stage acceptance.

A standard-vector wrapper preserves const vector references while pre-admitting overlapping old/new backing. Credit transfers only after the old buffer is freed. External capacities are admitted before adoption, denied adoption preserves input, and moved empty owners remain reusable. Aliased push_back arguments rebind by index after growth. Plain string/PropertyValue elements cannot pretend to self-govern their payloads.

Function arguments, dynamic-list elements and relevant clone/builder/arithmetic vectors are integrated. Static lists consume already-parsed immutable views instead of parsing again; the entire payload is admitted before construction, including two root-vector move proxies, then only trimmed. Regressions cover mixed types, embedded NUL, partial-copy cancellation, excessive reserve and aliased moves.

All701 selected tests and107 isolated cases pass. Expression allocation probes expand from six to nine shapes with a64-argument function, dynamic list and static list; the literal-list probe uses the admitted factory. No uncovered requests or retained storage/credit are observed. Long cache faults sample96 points.

Projection/aggregation containers, other workspaces and steady-state acceptance remain open. Evidence is Linux/clang19/libstdc++14; no other-platform execution, performance campaign, RSS or Kuzu benefit is claimed.
