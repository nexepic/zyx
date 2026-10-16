# Resource checkpoint for GDS projections, catalogs and procedure results

Projection node/adjacency arrays, adjacency hash tables, lazy CSR arrays and control blocks now retain allocation authority. Escaped CSR construction uses the originating owner; failure publishes no cache and permits retry. Construction borrows still-live topology-page entries, removing two relationship-type string copies. Public const std::vector node/adjacency reference interfaces remain unchanged, and legacy projections do not borrow temporary default PMR resources.

Projection specs, descriptors and catalog keys own string/array capacity credit. Both catalog maps start cold; state, buckets, nodes and control blocks admit before allocation, and clear frees the entire state without allocating. Deferred commit uses the preparation owner. Failed preparation restores revision references, including failures after constructor arguments have moved and released the lock. Prepared objects and callback boxes are admitted too. Native listOwned preserves array authority; legacy list remains a caller-owned compatibility boundary.

Autocommit and transaction procedures receive CACHE authority from the original backend, preventing projections/CSR from retaining their creating query. Procedures install QUERY scope and construct results with retained credit, preserving data/charges after close until destruction. OwnedStdString adds admitted append/+= with alias safety, funded spare-capacity reuse and unchanged source on rejection.

This does not complete all GDS/procedure governance or accept S5.3. Algorithm arrays, other procedure buffers and compiler/CSV/binding boundaries remain open. Final result charging does not prove admission before every temporary allocation.

## Validation and unresolved failures

The explicit 239-test selection has 230 passes. All 16 new tests pass, covering projection/CSR ownership, catalog/callback rollback, appending and actual backend CACHE/QUERY lifetimes. Every one of 32 catalog-preparation admission points rejects without publication and refunds completely.

Nine vector budget/batching cases fail. The control compiles the old procedure source from 882e7971 against current headers and other dependencies, linking that translation unit before current core. Its 75 vector cases fail at the identical nine IDs. This isolates the procedure implementation; it is not a complete old-source rebuild. Raw XML, logs, old-unit compile command and failure IDs are retained. Limits/assertions were not weakened and failures were not skipped to claim green. S6.1 still must resolve them.

run_selected_modules.py builds core and explicitly listed test objects in the existing Linux Clang19 coverage Ninja environment, then links separately. It honestly exits nonzero for the existing failures. Run only one Ninja build with at most four compilers and freeze C++/headers while building. No full unit suite or new performance campaign ran. Runtime evidence is Linux x86_64/Clang19/libstdc++14 only.
