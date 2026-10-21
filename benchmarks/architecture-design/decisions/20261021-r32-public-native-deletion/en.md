
### 2026-10-21 R3.2: Public homogeneous native owner deletion

Evidence: `benchmarks/architecture-design/decisions/20261021-r32-public-native-deletion/`. Public isolated-Node and Edge deletion now selects authenticated native owners and reuses the existing field/entity/adjacency/scalar/constraint/count/Vector lifecycle writer in one publication. Homogeneous native batches retain64-owner and1MiB limits. Mixed ROW/native deletion and detach remain guarded; incident Nodes refuse before publication.

Five related modules: candidate189 and default111 passed. New public cases cover old pins and reopen, isolated Node deletion, Edge adjacency/counts, Vector membership removal with old embedding preserved, complete64-Edge group deletion and integrity/refunds, and rejection of incident Nodes/unsupported detach without changed fields. Source snapshots precede subsequent patch routing. Native creation, patches and general mixed/detach lifecycle, production space/resource gates and R5/R7 remain unfinished. No full suite or timing benchmark. **Total14/24 (58.3%), accepted growth0; no commit or push.**
