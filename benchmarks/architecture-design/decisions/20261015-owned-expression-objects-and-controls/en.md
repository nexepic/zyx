# Expression object and shared-control ownership

Component evidence; still22/34 (64.7%), with no stage acceptance.

Expression unique objects retain resource prefixes and scalar strings pre-admit copies. Shared control blocks in AST/semantic/plan building and logical cloning use owning allocators. In-place factories use allocate_shared, covering allocations bypassing class new. Failed control-block admission destroys a consumed unique object exactly once; failed construction refunds storage. Legacy factories preserve global-allocation behavior without using unrelated ambient PMR resources.

String construction trims conservative admission to observed capacity without post-allocation growth; self-assignment does not copy. Query-header transitive include/namespace compatibility remains intact. All686 selected module tests pass, including scope lifetimes, constructor/control failure, cross-resource clones and expression clone/visitor regressions.

All106 isolated cases pass, including96 sampled cache-miss allocation points and six fixed-child-storage expression clone/control-block heap shapes with no uncovered allocation requests or retained credit/storage. Existing string/codec/partial-cancellation probes pass. Rendering is a separate caller-owned operation excluded from this clone probe. Dynamic argument vectors and nested PropertyValue literals remain open; six shapes do not establish whole-expression heap accounting.

Runtime evidence is Linux/clang19/libstdc++14. No performance campaign, RSS/Kuzu improvement or Windows/macOS execution is claimed.
