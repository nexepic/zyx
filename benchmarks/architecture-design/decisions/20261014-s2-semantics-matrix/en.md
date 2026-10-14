# S2.4 focused execution semantics acceptance

The current-source focused matrix passes **707 tests**: the 591 consumer/binding/native cases selected for S2.3, seven new actual native semantics cases and 109 pattern-expression, budget, property-descriptor, scoped-slot and typed-key cases. Fresh native correctness smoke passes 916 commands across 13 families. No full suite or performance run.

| Contract | Evidence |
| --- | --- |
| Expression types, NULL, arithmetic errors | Native three-valued logic, lazy operands, missing parameters, NaN comparison compatibility, checked int64 overflow/division by zero, mixed primitive/scalar kernels and typed keys |
| Scope and property identity | Projected properties, local map/comprehension/reduce shadowing, correlated imports, schema/database/format/capability/owner mismatch, current parameter types |
| Multiplicity and ordering | Native DISTINCT/group counts/NULL ordering/LIMIT, UNION ALL versus UNION, Cartesian joins, repeated selections, parallel relationships |
| OPTIONAL and paths | Native OPTIONAL NULL outputs, named paths, self-loops, repeated vertices, relationship uniqueness; pattern EXISTS/comprehensions and bounded shortest-path materialization |
| Mutation read sides and snapshots | Fresh SET values, caller-injected old complete Nodes and property caches refreshed against the new snapshot, old pins retaining old values, procedure projections and transactions |
| Cancellation and budgets | Every scan publication checkpoint, source admission, retained Node/Edge overlap, sort/hash/UNWIND/pattern limits, cancelled native read-only execution, deterministic resident-growth guard, continued queries and reopen |
| Owning output | Large strings/nested map/list/NULL values retained after source mutation, new root publication, snapshot release and database close; charged row transfers and native output materialization |

## Discovered semantic defect and correction

The new native trail oracle expected seven paths in a three-Node/four-Edge graph with parallel relationships and a self-loop; execution returned four. Both the general variable-length traversal and its batched count optimization incorrectly rejected repeated **Nodes**, excluding valid self-loop/revisited-vertex trails. They now track used **Edge IDs** per path instead. General traversal uses a charged PMR identity set. Parallel Edges remain distinct and the same Edge cannot occur twice in one trail. Two self-loops produce two one-step and two two-step trails, with no three-step trail; both named-path output and optimized COUNT agree. Direction, endpoint, complete-source checks, depth/work/row/byte bounds and cancellation remain in place.

A real untimed Kuzu0.11.3 check independently confirms all seven ordered rows using explicit `*TRAIL 1..2`. Kuzu's default recursive pattern is WALK and returns eight rows, including reuse of the self-loop. This difference is recorded in kuzu-trails.json, not hidden as equal semantics. Future variable-length Kuzu comparisons must request TRAIL when comparing this native relationship-unique path contract. Existing fixed-hop workload preflight remains green. Persistent format and ABI are unchanged.

## Budget regression diagnosis

The shortest-path test `ShortestPathRetainsNodesWhileAdmittingEdgeBatch` also failed on the exact pre-change a487f7c7 runtime, at its fixed 9000-byte threshold: Node retention consumed the allowance before Edge admission was reached. Its required failure contract was not weakened. The test now searches a small bounded range for the actual Node-held/Edge-admission boundary, requires a shared-byte-bound error before any Edge batch copy, and retains the 16000-byte complete-result assertion. The 1200-byte Edge payload makes the 128-byte search increment smaller than the targeted interval. This is a portable phase assertion instead of an assumed container-size threshold. The pre-change failure log is retained separately.

Accept S2.4. S2.5 comparisons and the independent storage/graph/transaction/resource work remain pending. This is focused verification of the implemented execution boundary; it does not claim 95% whole-repository coverage, factorized execution, global resource enforcement or spill support.
