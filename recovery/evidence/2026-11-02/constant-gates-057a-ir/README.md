# Fixed-format condition cleanup: LLVM IR comparison

The comparison uses the 15263106 build configuration and compares seven production translation units from that source with candidate 057a8563. It removes `-g` and emits optimized (`-O1`) LLVM IR, normalizing the source path and module identifiers. Six units match byte for byte. `RelocatableMetadataStoreGraphMutation.cpp` differs: inspection found renumbered string constants and TBAA metadata, and `llvm-diff` also reports differing instructions and block correspondences. This evidence does not establish semantic equivalence for that unit. Preserve version, authority, snapshot and failure-path checks and validate the candidate using a coherent build and tests.

The initial run retained a later `-g` flag and reported differences in all seven units; it is not treated as a valid semantic comparison. This test does not replace full build, test discovery, fault tests, or performance validation.
