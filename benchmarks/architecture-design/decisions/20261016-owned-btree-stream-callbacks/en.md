# Allocation ownership for ordered BTree stream callbacks

Native ordered streams previously used std::function, whose nontrivial closure storage and copies bypassed the installed budget. OrderedBTreeEntrySource and OrderedBTreeEntryCursor now use OwnedFunction: small trivial targets stay inline; other targets admit the object and shared control block before construction. Copies clone mutable callable state, moves allocate nothing, and rejected copies preserve both objects. Closures retain allocation authority; copies made after the creation scope ends still charge the original owner unless the caller explicitly selects another resource scope.

This governs only closure bodies and control blocks. Captured payloads and pointees must own their allocations. A generic wrapper cannot automatically govern an external std::function's opaque captures. Invocation continues to use the calling resource scope; existing native BTree factories/cursors explicitly select their owners, preserving invocation semantics.

Eight generic tests and two native tests cover empty/inline callbacks, independent state, references/void, rejection, partial-copy rollback, owner rebinding and escaped cursor lifetime. Native cases require an actual charge increase on copy, denial before any root I/O, and release of the originating account after the final cursor dies.

All 1,762 affected tests pass. Two subsequently added native cases pass with their related selection of 24 tests, giving 1,764 distinct cases in total. The current worker passes 1,275 isolated cases and 32 crash/read-only/recovery cycles; long allocation paths sample at most 96 points. Separate resource-module revalidation is recorded in proof.json.

No full suite, new performance campaign or new stage acceptance is claimed. This complexity supplies budget enforcement, without asserting end-to-end speedup. Other workspaces remain under S5.3; the accepted S5.4 fault contract is separate. See proof.json for evidence/source hashes. Runtime evidence is Linux Clang19/libstdc++14 only, without claiming Windows/macOS execution.
