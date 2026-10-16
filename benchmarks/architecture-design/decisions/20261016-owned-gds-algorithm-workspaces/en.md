# GDS algorithm allocation ownership

PageRank, WCC, betweenness, closeness, Leiden and Dijkstra admit private workspace allocations through the active query resource. Workspace allocators borrow the authority retained by the enclosing call. Native returned arrays retain their authority independently, including after the calling scope ends. Legacy vector-returning entry points remain caller-owned compatibility boundaries; native procedures use owning results without slicing them.

Governed Leiden currently computes degrees on the caller: pool task metadata and concurrent budget access have not been admitted for this path. The legacy ungoverned parallel path remains available. No throughput improvement is claimed.

The final selected GDS modules pass 26/26 tests. Three new cases exercise tiny-budget rejection before workspace allocation, escaped output lifetime and all 191 allocation admission points with refund, retry and reusable cached CSR. The earlier expanded selection passed 233/242, retaining the same nine vector-budget failures documented in the preceding decision; those are unresolved and this change does not accept S5.3 or S6.1. No full suite or performance campaign was run. Accepted progress remains 24/34 (70.6%).
