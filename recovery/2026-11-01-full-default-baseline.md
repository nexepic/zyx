# Default full baseline and remaining failures

Source: C++ tree `79020ae7`; runner and audit HEAD `73acc3d5`. Unit and stress XML: `/workspace/zyx-link-tmp/full-default-unit-xml` and `full-default-stress-xml`. Discovery: 10,146 enabled, 17 disabled. Unit 10,113/10,140; stress 6/6. XML reconciliation: 10,146 distinct executed, no missing, extra, or duplicate; 27 failures. The older historical 10,185 discovery list is unavailable, so the difference cannot be classified as 39 missing cases.

## Classification

* Crash recovery (2): actual checksum failures after SIGKILL and reopen during the first retry; investigate allocation bitmap/write ordering and snapshots, without bypassing integrity checks.
* Format, routing identity, fences, budget, prefetch, cache, and reclamation contract changes: candidate test/implementation adjustments exist on the isolated branch, but none has been verified against an integrated build. Several failures may still reveal defects.
* The exact failing names follow; candidates must preserve their behavioral assertions and be tested with production objects from the same tree.

## Exact failing tests

Categories reflect the observed failure and working hypothesis, not a verified fix.

* `VectorizedNodeScanIntegrationTest.TopKPropertySortUsesLogicalProfileSummary` — NULL ordering / full-sort parity
* `LogicalGraphReadExecutorTest.OutputColumnStorageConsumesBudgetEvenWhenLimitReturnsNoRows` — budget exception type and message
* `LogicalPlanDirectEdgeRuntimeTest.NestedSortPrefetchesOuterPropertyAndTransfersRows` — prefetch reservation / batch-count contract
* `LogicalPlanDirectEdgeRuntimeTest.ProjectBetweenSortsTransfersProjectedRowReservations` — prefetch reservation / batch-count contract
* `LogicalPlanDirectEdgeRuntimeTest.SortPreservesProjectedFilteredRowsWithoutOuterPrefetch` — prefetch reservation / batch-count contract
* `RelocatableStorageBackendTest.NativeEdgeOverlayResolvesAnUnseenTypeUnderExhaustedCacheCredit` — cache admission fallback
* `RelocatableStorageBackendTest.RangeDedupSharesByteBudgetAcrossBatchesAndRetriesAfterRejection` — shared budget admission
* `BufferPoolTest.ScopedSequentialStorageReadsRestoreNormalAdmissionAndStayOnTheirThread` — sequential cache-scope contract
* `EntityPageStoreScanCacheTest.EdgeScansUseTypedEntityDirectoryWhilePointReadsRemainResident` — physical scan counter expectation
* `NativeGraphWritePlannerPublicContractsTest.PropertyOnlyCommitDoesNotDiscoverOrQueryUnrelatedEntityPools` — owner revision versus routing identity
* `RelocatableProcessCrashOrdinaryEdgeTest.KillBeforeOrdinaryEdgePublicationRetainsOldGraphAndCanRetry` — crash recovery / checksum
* `RelocatableProcessCrashOrdinaryEdgeTest.KillBeforeMultiEdgeDeletionPublicationRetainsBothEdgesAndAllowsRetry` — crash recovery / checksum
* `ScanPathFormatTest.VersionsAndBuildingFlagsAreIsolated` — format-version or reserved-field contract
* `RelocatableSuperblockTest.IsolatedCompactCohortRejectsPriorPrototypeSlotsWithoutMigration` — format-version or reserved-field contract
* `RelocatableMetadataPagePoolTest.DeferredPublicationRetainsFramesUntilBoundedAllocationThreshold` — allocation reconciliation contract
* `RelocatableMetadataPagePoolTest.RecoveryReclaimsDeferredUnpublishedFrames` — allocation reconciliation contract
* `HybridPropertyColdRowInspectionTest.InlineIdentityCountHotOverlapQuotaAndCancelledSourcesRefuseBeforeOutput` — inline directory and selected-value admission
* `HybridPropertyDescriptorStoreTest.DefaultFormatCannotPublishCandidateRoots` — format-version or reserved-field contract
* `PropertyPagePoolPackingTest.CombinedOwnerGrowthRelocatesOnlyOverflowAndPreservesOldPin` — owner revision versus routing identity
* `StorageFormatTest.DatabaseFactoryRejectsChecksumValidButNonCanonicalBootstraps` — format-version or reserved-field contract
* `LogicalPlanDirectEdgeRuntimeTest.FilterBetweenSortsTransfersSelectedRowReservations` — prefetch reservation / batch-count contract
* `LogicalPlanDirectEdgeRuntimeTest.ReversedSingleHopAnchorPreservesDirectionalBagsAndEarlyStop` — transparent anchor rewrite
* `RelocatableStorageBackendTest.LogicalMutationFenceEncodesEveryGraphVisibleRoot` — logical versus physical fence contract
* `EntityPageStoreScanCacheTest.NodeScansUseTypedEntityDirectoryAndReusePageReads` — physical scan counter expectation
* `HybridPropertyDescriptorStoreTest.FenceIncludesDescriptorEvenWhenOtherGraphRootsAreUnchanged` — logical versus physical fence contract
* `PropertyPagePoolPackingTest.ValueOverwriteKeepsRoutingIdentityWhileCowVersionsBytes` — owner revision versus routing identity
* `PropertyPagePoolPackingTest.StableOverwriteIdentityDoesNotAllowStalePlansOrDeleteReuseAba` — owner revision versus routing identity

## Recovery checkpoints

* `59d3c875`: owner reclamation/admission, default 67/67, mixed 67/67 and SCAN 36/36, in owner-admission.xml, mixed-admission.xml and scan-admission.xml. `OwnerReclamationMakesNodeAndEdgeProgressDuringSustainedNodeGrowth` uses budget 64 and adds 65 real owners each round; it failed in reclaim-strong-baseline.xml and passed in the new tree. Post-publication reconciliation failure remained open there; isolated 8e956260/ba64d308 are unvalidated candidates.
* `d95d2adb`: projected-value budget handoff, default 86/86 in prefetch-handoff.xml including `TwoLiveProjectedPayloadsShareOneMaterializationBudget` and 25 BoundedBatch cases; this does not establish all fallback paths or historical sibling coverage.
* Main `73acc3d5` and isolated `865fbacf` share `16a7422f`; isolated commits have not been integrated or tested with main production objects.

## Process-kill write-order investigation

At baseline, a `strace` reproduction showed a shared pool control record written at offset 524416, marking a second frame while the frame at 532480 remained zero-filled. The child was killed before a later candidate write; after reopen, the retry read that frame and failed the owner-header checksum. The write-control call stack resolves to `prepareAppendPagesUnderWrite`: the preceding typed page write can remain dirty in BufferPool while the control-record write goes through to the OS. This establishes the process-kill sequence, not power-loss durability.

Candidate `d4da7de4` on `recovery/zyx-crash-order` writes affected owner pages through before writing control records and orders prepared suffix pages before their control record. With all other production and test objects reused from the identical baseline source and the changed allocator object rebuilt, its separately linked binary passed 83/83: allocator 54, allocator faults 16, ordinary-edge process crash 8, shared-pool process crash 5 (`/workspace/zyx-link-tmp/crash-order-expanded.xml`). This is incremental evidence, not a clean full rebuild or mixed/SCAN acceptance. The candidate is not yet in the main recovery branch.

## Integrated candidate checkpoint

The isolated `recovery/zyx-crash-order` branch at `9816cd50` combines the crash-order candidate with the previously unvalidated test, cache admission and owner-publication candidates. Only source files differing from baseline were compiled; unchanged objects from the same source baseline were reused. The resulting default binary ran 120/120 selected tests, including 24 of the 27 original failed names and all 13 selected process-kill cases (`/workspace/zyx-link-tmp/integrated-candidates.xml`). Three old names are replaced by tests of the revised contracts: `DeferredPublicationRetainsFramesUntilBoundedAllocationThreshold`, `LogicalMutationFenceEncodesEveryGraphVisibleRoot`, and `IsolatedCompactCohortRejectsPriorPrototypeSlotsWithoutMigration`. The replacement tests passed in this selection. This is targeted incremental evidence, not full-suite, clean-build, mixed or SCAN acceptance. The integrated candidate branch has not been merged or pushed.

The integrated candidate `9816cd50` subsequently completed the default ordinary tier: 10,153 enabled tests passed across two shards, with 17 disabled (`integrated-default-unit-xml`). This used the incremental same-source binary described above. Six stress cases and exact-once reconciliation are separate and were still running when this note was recorded. Mixed and SCAN baseline binaries are not candidate-tree results.

Candidate `9816cd50` then completed all six default stress tests successfully. Reconciliation of the four ordinary/stress XML files against that binary's discovery found 10,159 enabled and 17 disabled, with 10,159 distinct executions, zero missing, extra, duplicates or failures. Evidence: `integrated-default-unit-xml`, `integrated-default-stress-xml`, and `/workspace/zyx-link-tmp/verify_exact_once.py`. The result remains a same-source incremental-link validation, not a clean rebuild or a mixed/SCAN result.

On the isolated candidate branch, `64d8eb67` follows `9816cd50` and changes only two hybrid-only descriptor tests. An initial mixed targeted run at the parent had two failures (a stale hardcoded superblock version and a test that treated an empty descriptor's physical publication as a logical graph change). The revised test checks unchanged graph/schema revisions, a changed physical root, successful staged node commit and integrity. The updated mixed incremental same-source binary passed 130/130 selected tests (`integrated-mixed-targeted-new.xml`); complete mixed, SCAN and clean-build validation remain outstanding.

The same `64d8eb67` candidate's SCAN incremental binary passed the 85 exact test names selected in the earlier current-tree `latest-scan.xml` (`integrated-scan-targeted.xml`). These are not the historical final set of 85. The SCAN baseline ordinary run remains in progress and has thousands of failures; many fixtures try to create a SCAN image directly instead of performing the required offline construction and whole-file admission, and others lack a completed descriptor. No admission rule was weakened. Separately, the mixed baseline ordinary run has numerous migration and recovery failures, many involving a synthetic Catalog entry whose referenced segment envelope is truncated; these must be classified and repaired with valid fixtures or actual code changes rather than ignored. The mixed baseline reopen read-volume test also exceeded its bound and needs independent attribution.

Two mixed bootstrap fault fixtures were separately reconstructed with a real matching shared-pool control page, allocated Catalog slot and a segment-aligned physical tail. With native positioned I/O and truncate available, short-write and ENOSPC hooks actually fired, the old root remained, and retry succeeded: 2/2 in `bootstrap-fixture-mixed-v3.xml`. The initial fixture-only attempt exposed an unallocated slot; the second attempt exposed an unaligned physical tail that poisoned cleanup. These changes are candidate commits on the isolated branch, not a fix for the remaining fixture failures.

For the mixed `NormalReopenDoesNotScanTheLargeValueDataRegion` failure, a separate read-hook diagnostic retained the failing total-read assertion: 1,087,056 delivered bytes versus the 1,048,576 bound. It observed 1,154 reads starting at authenticated large-value data-owner headers, with zero body bytes in those particular reads (`reopen-observe-mixed.xml`). Reads starting at other offsets or spanning multiple pages were not classified, so this is partial evidence, not proof that no payload bytes were scanned and not a basis for relaxing the bound.

The `64d8eb67` candidate's SCAN incremental binary also passed 97/97 selected tests in 639.259 seconds (`integrated-scan-broad.xml`), using the deduplicated names from the earlier `scan-admission.xml` and `scan-broad-current.xml`. This includes completed-image read proof, vector and owner-maintenance coverage; it does not make the incompatible generic SCAN fixture suite pass.

A later independent diagnostic identified 577 checksum-valid large-value data-owner frames in the 2 MiB fixture (including any stale frames) and intersected all native reads during reopen against their bodies. The observed intersection was zero bytes (`reopen-observe-mixed-v2.xml`); the original total logical-read bound still failed at 1,087,056 bytes. This is evidence for one fixture, not a general performance equivalence result or justification to drop the bound.

The mixed `ForwardColdConversionPublishesItsNewWholeRootValueAndManifestTogether` failure was traced to the unchanged whole-file admission: the original ROW image was 2,228,224 bytes and the candidate was 2,097,152 bytes, only 5.88% smaller, so conversion correctly remained unpublished. The isolated candidate rebuilds this test with wider real ROW properties, retaining the 8,193-byte external value and all old-pin, reclamation, integrity and budget assertions. It passed 1/1 with 2,752,512 source bytes and 2,097,152 candidate bytes (23.81% saving), admission explicitly accepted (`cold-admission-mixed-v2.xml`). This is a targeted incremental test, not full mixed acceptance.

The original SCAN baseline ordinary tier finished with 10,953 enabled cases completed, 7,861 passed and 3,092 failed, plus 3 skipped and 20 disabled (`full-scan-unit-xml/shard-0.xml`); stress was not run, so there is no full exact-once claim. Of the failures, 2,396 messages mention direct SCAN creation and 146 mention a missing persistent descriptor; the remaining 550 need individual classification. This run is baseline source, not the candidate tree. In the separate candidate mixed targeted retirement test, an ID lease correctly preserved graph/schema revisions while physically relocating the descriptor; comparing decoded descriptor semantics and retaining the original partial/complete publication, old-pin, reopen and integrity checks passed 2/2 (`retirement-lease-mixed-v2.xml`).

The original mixed baseline ordinary tier completed: 10,829 enabled tests completed, 10,757 passed and 72 failed, plus 1 skipped and 17 disabled; duration 2,099.357 seconds (`full-mixed-unit-xml/shard-0.xml`). A coarse first-failure exception classification finds 19 truncated-envelope, two owner-header checksum, one admission, one invalid-route and 49 assertion/other cases. This is not the integrated candidate; the mixed baseline stress tier was not run.

The current-format envelope golden test was separately compiled and passed in default, mixed and SCAN using each configuration's format library (`golden-coherent.xml`, `golden-mixed.xml`, `golden-scan.xml`). It preserves independent fixed CRC values for the three currently selectable formats and deliberately requires a new golden value when the reconstructed unified format is implemented. No format selector has yet been removed.

Two retirement-publication tests also failed because their old fixture expected a pure logical-page-ID lease to be rejected with a hybrid descriptor. The lease is allowed and can relocate the descriptor's physical root while retaining its decoded semantics. The revised isolated tests assert unchanged graph/schema revisions and equal decoded descriptors, then retain the active-pin, partial/complete fault injection, reopen and integrity checks; mixed targeted validation passed 2/2 (`retirement-lease-mixed-v2.xml`).

The envelope golden test and two bootstrap fault fixture changes were independently compiled against each configuration's same-source production objects and passed respectively 1/1 and 2/2 in default, mixed and SCAN. These local results do not resolve the other mixed migration/process-crash fixture failures or the generic SCAN applicability gap.

For the `64d8eb67` mixed candidate, the separate stress tier passed 6/6. Its ordinary XML has 10,842 completed cases (10,798 passed, 44 failed), one explicit skip and 17 disabled. Combined discovery found 10,849 enabled names, 10,848 completed, one skipped, no extras or duplicates and 44 failures. The one skipped case is `HybridPropertyDescriptorStoreTest.DefaultFormatCannotPublishCandidateRoots`, which is intentionally applicable to the default configuration. Earlier counts that included it as a pass were incorrect.

The retained-sibling prefetch test originally passed the resource account on success but accidentally omitted it on the tight-budget rejection path. Candidate `b0e4b16d` passes the same `QueryContext` there too. The test then passed 1/1 in default, mixed and SCAN (`sibling-budget-{coherent,mixed,scan}.xml`): two live 3,000-byte projected values, tight-budget rejection before delivery with account refund, wide-budget delivery and zero residual charge, and a trace of both bound property reads. These are targeted checks; the earlier full default binary predates this test-only correction.

The later `e4f5e365` candidate was rebuilt with exact changed objects and reran the complete default ordinary and stress tiers: 10,159 enabled names each completed exactly once, 17 disabled, no missing, extra, duplicate or failed cases (`current-default-unit-xml`, `current-default-stress-xml`). This is an incremental same-source result; candidate `b0e4b16d` subsequently changed only crash-fixture and sibling-account test source. A standard clean default build for `b0e4b16d` was configured and started separately; no clean-build result is implied by this record.
