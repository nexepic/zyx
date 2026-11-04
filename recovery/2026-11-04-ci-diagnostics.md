# CI diagnostic checkpoint, 2026-11-04

Run 41591592889 tested diagnostic commit 834ba6cfca1cb7edb97182257727441f12244015. Ubuntu and macOS TSan passed. Windows and ordinary macOS failed; macOS ASan+UBSan was cancelled during its gate, so it did not establish whether the stack change works.

The two same-owner ABI ingest tests passed on Windows. The isolated PlanCache diagnostics identified EveryMetadataAllocationFailureLeavesNoUnindexedPlanAndAllowsRetry as the only one of 17 cases that exited with code 3; its precise failure point and cause remain unknown. The Windows unit run also showed materialization-budget failures.

macOS unit testing had six failures. The duplicate property-read case reported zero node-predicate splits, so that observed counter does not support the previously considered node-predicate split explanation. This checkpoint adds a separate property-prefetch split counter to distinguish a second possible path, without changing the exact-read assertion or budget. The CSV fixture failed earlier in field-vector admission than its expected map-construction admission. The shared-large case encounters the existing libc++ conservative 2*n+1 string bound. A diagnostic on the runner observed 8,200 bytes for 8,192 characters and at most 8 bytes overhead for lengths 0 through 8,192, but this is one platform/version sample and does not establish a safe bound for every supported input or library version.

The Windows PlanCache test now prints the injected failure number before each iteration, preserving all existing assertions. The added messages and split counter are diagnostics, not fixes; no tests, budgets, sanitizer gates, or single-writer restrictions were disabled.

Linux: a same-source incremental test binary built with the changed runtime and test objects passed PlanCacheTest.* and TightAndAmplePropertyPrefetchReadEachNodeOnce, 18/18, XML /tmp/zyx-ci-nov04-target.xml. This is not a clean full build, and it does not validate Windows output or macOS behavior. Windows, macOS, and ASan require further native evidence and fixes.
