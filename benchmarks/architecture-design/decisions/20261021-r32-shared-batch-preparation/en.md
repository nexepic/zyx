
### 2026-10-21 R3.2: Separate private preparation from final metadata publication

Evidence: `benchmarks/architecture-design/decisions/20261021-r32-shared-batch-preparation/`. The native field entry still creates read stores before acquiring its writer and retains an authenticated source pin. Its private preparation function now accepts an existing MetadataWriteBatch and placement, checks the shared source and unpublished descriptor, and prepares fields, directories, indexes, counts and retirement without final publication. The outer entry retains source and placement until its single publish outcome, then records validated reuse hints. No-ops still do not publish.

This internal structure is needed for subsequent same-batch Vector coordination. Actual Vector delta/bootstrap integration and ordinary native topology writers remain unfinished. Refactored field/index/deletion/map/LargeValue/journal and same-executor affected groups: default32 passed; candidate128 passed. Prior57-module and Vector-defect evidence remains separate, not counted as a fresh broad run. No full repository suite or timing benchmark. **Total14/24 (58.3%), accepted growth0; no commit or push.**
