# Optional-cache decline, DELETE ownership, and reclamation evidence lifetimes

The full O6 preparation exposed three failures; negative evidence is retained and resource caps stay unchanged.

Public owning entity batches now fall back to admitted reads when the optional 4 MiB entity cache declines a record, reserving the independent output before its deep copy. Literal-only DELETE projections use an internal guarded reference consumer while preserving full public implicit before-images. Planning sets, retained rows, request vectors and escaping results remain charged to QUERY. The exhausted-CACHE negative test verifies private reader ownership.

Under the existing 8 MiB maintenance grant, completed uniqueness and pool-membership evidence is freed before publication assembly. Scanned retirement lists are consumed and released incrementally. Required identity, reachability and pool validation remains complete; admission failure still precedes publication.

Five new module tests cover uncached owning Node/Edge output, bounded scalar deletion, atomic rejection of oversized before-images, exhausted CACHE and result refunds. Two existing mutation test files use explicit PropertyMap construction to match the existing OwnedPropertyMap API, preserving their assertions.

The complete untimed fixture deletes 10,000 nodes with 8 KiB bodies atomically and validates every one of the 10,000 scalar results, with 10,000 unrelated Users retained. Reclamation of the 10,000 unreachable large values, reopen, integrity and exact live counts pass under the unchanged 8 MiB maintenance grant. These are correctness observations, not steady-state performance acceptance.

The next O6 window uses `MATCH (n:Churn) DELETE n RETURN 1 AS deleted` on both engines and checks every result and its multiplicity. The repaired source has a separate registration. The original `6a735df8` baseline continues only with its original frozen worker and harness, whose hashes match its registration. S0.3 and S5.5 remain pending; this checkpoint adds no accepted progress.

Final affected regression: **130 modules, 2,882 tests / 166 suites, zero failures**; 30 campaign tests plus 14 subtests pass. Release smoke covers 13 families / 886 checked operations; an additional four-client replay passes 4,000 operations. No full unit suite was run.
