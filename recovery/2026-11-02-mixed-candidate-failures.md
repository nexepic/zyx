# Mixed candidate ordinary test failures

Candidate `64d8eb67`, incremental same-source build: 10,842 enabled completed, 10,798 passed, 44 failed, 1 skipped and 17 disabled. Duration 2,076.674 seconds; XML `/workspace/zyx-link-tmp/integrated-mixed-unit-xml/shard-0.xml`. Six stress cases were not run. Subsequent isolated fixes for seven named failures have targeted evidence only, not another full run.


* `RelocatableStorageBackendSchemaTest.NormalReopenDoesNotScanTheLargeValueDataRegion`
* `RelocatableMetadataStoreTest.InitialBootstrapMigrationDataPageShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.InitialBootstrapMigrationDataPageEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.InitialBootstrapMigrationFinalSyncFailureReopensEitherOldOrCompleteGeneration`
* `RelocatableMetadataStoreTest.RoutedMetadataPoolRelocationChurnReclaimsAndReusesMetadataCapacityAcrossReopen`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationCandidateShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationCandidateEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationFinalFenceReportsUncertainDurabilityAndReopensTheCompleteRoute`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationDataFenceFailureKeepsTheOldGenerationAndCanRetryAfterReopen`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationFinalFenceEnospcReportsUncertainDurabilityAndReopensTheCompleteRoute`
* `RelocatableMetadataStoreTest.SparseMetadataPoolEvacuationRecoversTheDurableNewRootAfterPublication`
* `RelocatableMetadataStoreTest.NativeSparseMetadataPoolEvacuationFinalFenceReportsUncertainDurabilityAndReopensTheCompleteRoot`
* `RelocatableMetadataStoreTest.RecoversAReservedTailDataSlotAndItsFollowingMetadataPoolAcrossPublicationBoundaries`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationBootstrapHeaderShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationBootstrapHeaderEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationRouteRadixShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationRouteRadixEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationSuperblockShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationSuperblockEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.RebasesASequentialWriterOnlyAfterItsLocalSnapshotsAreReleased`
* `RelocatableFormatTest.EnvelopeCodecMatchesGoldenLittleEndianBytes`
* `RelocatableProcessCrashTest.KillBeforeBootstrapMigrationPublicationRetainsOldGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringBootstrapMigrationCandidateRetainsOldGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillAfterBootstrapRouteReclaimRecoversPublishedGenerationAndRemainsWritable`
* `RelocatableProcessCrashTest.KillAfterMetadataPoolReclaimPublicationReopensWithReleasedPoolSlotReusable`
* `RelocatableProcessCrashTest.KillDuringRoutedMetadataPoolRelocationCandidateRetainsOldGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillAfterRoutedMetadataPoolRelocationPublicationReopensTheNewGenerationAndRemainsWritable`
* `RelocatableProcessCrashTest.KillDuringPostMigrationRouteCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationHeaderCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationRouteRadixCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationCatalogCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationAllocatorCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationAllocatorInternalCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationLocationInternalCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationLocationCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillAfterBootstrapMigrationPublicationRecoversNewGenerationAndRemainsWritable`
* `RelocatableProcessCrashTest.KillDuringEmergencyReserveRotationCandidateRetainsTheConsumedReserve`
* `RelocatableProcessCrashTest.KillDuringEmergencyReserveRotationCleanupRetainsPublishedRootsAndReusesTheTailOnRetry`
* `HybridPropertyColdPublicConversionTest.ForwardColdConversionPublishesItsNewWholeRootValueAndManifestTogether`
* `HybridPropertyLayoutScaleTest.PartialRetirementPublicationRetainsOldAllocationAndNativeValues`
* `HybridPropertyLayoutScaleTest.CompleteRetirementPublicationRecoversNewAllocationAndNativeValues`
* `HybridPropertyIndexedLifecycleTest.SustainedTypeChangesStayFieldLocalWithoutAutomaticRepromotion`
* `HybridPropertyMatureShapeCostsTest.CohortSavingDoesNotAuthorizeItsFirstGroupPublication`
* `FixedShapes/HybridPropertyShapeCostsTest.WholeAllocationAndEveryOwnerSurviveConversionAndReadOnlyReopen/mixed_null_string`

Subsequent targeted mixed checks passed for the two bootstrap fault fixtures, the envelope golden check, the cold conversion test with a genuinely admissible wide ROW image, the two retirement/lease tests, the field-local lifecycle test, and the mixed-null-string shape test. The latter directly verifies token 2 (boolean) and token 3 (inline string) use columns, while tokens 1 and 4 use extensions; it retains old snapshot, all-owner, reopen, integrity and cost checks. These eight targeted results are from later isolated source checkpoints and do not retroactively change the 44-failure full run. The cohort integrity-budget candidate is still under test.

A later mixed-only process-crash fixture checkpoint atomically reserves and publishes a real opaque segment at the original slot, with a matching envelope and segment-aligned physical tail, in the same original generation. The three bootstrap before-write, candidate-write and after-publication SIGKILL cases then passed 3/3 with their original old/new/retry assertions (`bootstrap-crash-mixed.xml`). Other route/rotation crash fixtures remain unresolved. Including the earlier targeted fixes and the separately validated cohort scanner-budget check, 13 of the 44 failed names have later targeted evidence; there is no later full mixed run yet.

A focused reopen diagnostic also measured 562,760 bytes of native reads while the `StorageIO.read_bytes` total was 1,087,056; 577 authenticated large-value data frames were found by independent page-header inspection and the native read ranges intersected none of their body bytes (`reopen-observe-mixed-v3.xml`). The original total-read assertion remains failing. This single fixture does not establish a general metadata-cost bound or performance equivalence.

## Latest candidate rerun

The 44 previous failed names were rerun together on the `e4f5e365` same-source incremental mixed binary: 13 passed and 31 failed, with no skips (`current-mixed-failures-rerun.xml`). This is not a new full mixed run. The remaining cases are 16 metadata-store tests, 14 process-crash tests and one reopen-read-volume test:

* `RelocatableStorageBackendSchemaTest.NormalReopenDoesNotScanTheLargeValueDataRegion`
* `RelocatableMetadataStoreTest.RoutedMetadataPoolRelocationChurnReclaimsAndReusesMetadataCapacityAcrossReopen`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationCandidateShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationCandidateEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationFinalFenceReportsUncertainDurabilityAndReopensTheCompleteRoute`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationDataFenceFailureKeepsTheOldGenerationAndCanRetryAfterReopen`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationFinalFenceEnospcReportsUncertainDurabilityAndReopensTheCompleteRoute`
* `RelocatableMetadataStoreTest.SparseMetadataPoolEvacuationRecoversTheDurableNewRootAfterPublication`
* `RelocatableMetadataStoreTest.NativeSparseMetadataPoolEvacuationFinalFenceReportsUncertainDurabilityAndReopensTheCompleteRoot`
* `RelocatableMetadataStoreTest.RecoversAReservedTailDataSlotAndItsFollowingMetadataPoolAcrossPublicationBoundaries`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationBootstrapHeaderShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationBootstrapHeaderEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationRouteRadixShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationRouteRadixEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationSuperblockShortWriteKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.NativeRoutedMetadataPoolRelocationSuperblockEnospcKeepsTheOldGenerationAndCanRetry`
* `RelocatableMetadataStoreTest.RebasesASequentialWriterOnlyAfterItsLocalSnapshotsAreReleased`
* `RelocatableProcessCrashTest.KillAfterBootstrapRouteReclaimRecoversPublishedGenerationAndRemainsWritable`
* `RelocatableProcessCrashTest.KillAfterMetadataPoolReclaimPublicationReopensWithReleasedPoolSlotReusable`
* `RelocatableProcessCrashTest.KillDuringRoutedMetadataPoolRelocationCandidateRetainsOldGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillAfterRoutedMetadataPoolRelocationPublicationReopensTheNewGenerationAndRemainsWritable`
* `RelocatableProcessCrashTest.KillDuringPostMigrationRouteCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationHeaderCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationRouteRadixCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationCatalogCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationAllocatorCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationAllocatorInternalCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationLocationInternalCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringPostMigrationLocationCandidateRetainsPublishedGenerationAndCanRetry`
* `RelocatableProcessCrashTest.KillDuringEmergencyReserveRotationCandidateRetainsTheConsumedReserve`
* `RelocatableProcessCrashTest.KillDuringEmergencyReserveRotationCleanupRetainsPublishedRootsAndReusesTheTailOnRetry`

After that rerun, a valid allocated source fixture was used for four post-migration route/header/Catalog/allocator candidate SIGKILL cases and the allocator-internal radix candidate case. Each later passed on the mixed configuration (`bootstrap-post-crash-mixed.xml`, 4/4; `bootstrap-radix-crash-mixed.xml`, 1/1). These five results do not alter the earlier `e4f5e365` rerun: its 31 failures remain recorded there; 26 of those names still lack a later passing result. The remaining radix/location fixtures include synthetic page or extent routes and need valid durable targets, not a blanket reuse of the simpler source fixture.

The candidate-source mixed `HybridPropertyMatureShapeCostsTest.EightBoundedGroupsKeepWholeDatabaseCostsAndEveryOwnerExact` passed 1/1 in 282.463 seconds (`mixed-eight-groups.xml`, `mixed-eight-groups.log`). This single test exercises all eight bounded groups and every owner, using the candidate production objects and separately compiled test object. It was allowed to finish without a 180-second cutoff; the result is distinct from historical timings and does not establish a performance comparison.

The later isolated checkpoint `3d543491` supplies two location-radix SIGKILL fixtures with allocated, fully initialized and checksummed target segments, and corrects each extent to the current scanner's full 31-page extent contract. The two location cases plus the three previously repaired bootstrap crash cases passed 5/5 in mixed and 5/5 in SCAN (`bootstrap-extent-crash-mixed.xml`, `bootstrap-extent-crash-scan.xml`). Their kill markers, prior published generation, target and retry checks remain. These are low-level metadata cases, not completed-SCAN public transaction tests. The checkpoint is not yet in the candidate branch while its clean build is running; the earlier 31-failure rerun remains unchanged. With these two additional cases, 24 of those 31 names have no later passing evidence.

Further source inspection of `RebasesASequentialWriterOnlyAfterItsLocalSnapshotsAreReleased` shows that it holds two independently opened writable low-level metadata stores concurrently, then refreshes the second after publication by the first. This is outside the accepted single-writable-owner product contract and is not evidence that general multi-writer rebase must be restored. It also seeds synthetic Catalog entries, so its mixed failure cannot by itself establish a production recovery defect. The test has not been deleted or rewritten: any removal must retain supported same-owner staged-transaction, snapshot, reopen, and lease coverage, and its particular failure must be checked against a valid fixture before a final disposition.

Several same-format physical relocation tests call `prepareRoutedMetadataPoolForEvacuation`, which first seeds a large set of synthetic Catalog slots and then invokes the older bootstrap migration helper before publishing further synthetic Catalog and extent references. Those tests include actual relocation, old-pin, failure and retry assertions that remain valuable. A safe restoration must replace the setup with a valid current-format routed pool and valid allocated targets; disabling native recovery or deleting the entire tests would not demonstrate that contract.

After the `b0e4b16d` clean default exact-once run, the extent fixture was cherry-picked into the candidate as `f6b8c453` and its changed test object was rebuilt by the standard Ninja build. The first link failed because the invocation omitted the sysroot `LIBRARY_PATH` (`ld: cannot find -lxxhash`); retrying the link with `/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu` succeeded. The five bootstrap/location SIGKILL names passed 5/5 on this candidate's default binary (`clean-extent-f6.xml`), in addition to the earlier isolated mixed and SCAN 5/5 evidence. The 10,159-test clean exact-once result remains tied to the parent `b0e4b16d`, not this later test-only checkpoint.

On unified production `e973d286`, a new test-only current-format fixture at `db033c08` creates 30 genuinely allocated, initialized typed pools using the reservation preparer, pins an old snapshot, and forces metadata route evacuation. Both publication failpoints passed 2/2 (`unified-real-evac-db.xml`): BEFORE preserves the old route on reopen and allows a successful retry; AFTER reopens the advanced generation and distinct route. Both check all Catalog/allocation identities and integrity. This replaces no historical test and does not establish native short-write, ENOSPC, final-sync, process-kill or power-loss behavior. The earlier 15 old-fixture failures remain recorded and require individual disposition.

The new valid current-format fixture was extended through test-only `de151989`: 5/5 passed on the unchanged `e973d286` production archive (`unified-real-native-de.xml`). In addition to BEFORE/AFTER publication failpoints, the suite verifies native Catalog-leaf short write and ENOSPC hooks actually fire, retain the old root, reopen and retry successfully with integrity; it also fails the second native sync and accepts only a complete old or new route after reopen, with Catalog/allocation/integrity checks and old-state retry. The final-sync result is not proof of behavior under power loss. The original 15 failing names were not changed or silently reclassified, and process-kill coverage remains separate.

The valid current-format metadata pool fixture was expanded to 13/13 passing tests at test-only `22b39edf` against the same `e973d286` production archive (`unified-real-native-22.xml`). It covers actual hooked short-write and ENOSPC at Catalog leaf, bootstrap header, route radix and superblock; data-sync failure; final-sync EIO and ENOSPC; and BEFORE/AFTER inactive-slot publication failpoints. Each injected case checks that the intended hook fired, keeps old snapshots and all 30 real allocated pool identities valid, and checks old-root/retry or whole-old/whole-new final-sync recovery plus integrity. These tests provide current-format behavioral replacements for several categories but do not retroactively make the original 15 names pass or establish SIGKILL/power-loss behavior.

The five bootstrap/extent process-termination cases were also run on the unified `e973d286` production archive and its same-tree Ninja crash test objects: 5/5 (`unified-extent-crash-e973.xml`). The focused binary uses a saved small main (`unified-crash-main.cpp`) with the original crash dispatcher and includes the same-tree ABI child object; the first link omitted that ABI object and failed, then succeeded after adding it. These cases still call the old bootstrap migration entrypoint. They prove the executed process-kill behavior on this tree, not completion of latest-only cleanup or power-loss behavior.

The same 14 previously failing `RelocatableProcessCrashTest` names were rerun together on the unified `e973d286` crash binary: seven passed and seven failed (`unified-crash-prior14.xml`). Six failing cases reported a truncated segment envelope and the post-migration route-radix case reported an invalid current pool route. The seven successes include the separately restored post-migration Catalog, allocator and extent targets. These are precise targeted results, not a full crash suite; each failure still needs a valid current-format setup or a separately established product root cause.

The former post-migration route-radix crash failure has a later passing current-format replacement at test-only `21f4a674` (`unified-radix-real-21.xml`): direct complete initialization, 169 sparse PageIds in distinct leaf ranges, six allocated typed pools with distinct valid EntityPage frames and aligned segment tails, actual candidate-write marker followed by process termination, old generation and first/last route verification, retry to a new physical root, reopen and integrity scan. The operation republishes the same valid page location: the earlier attempt to change an Entity page's `free_bytes` from 0 to 1 was invalid and failed integrity. Before that, an incomplete last segment prevented the marker from being reached. Both failures and their fixes are preserved (`unified-radix-real-a42.xml`, `unified-radix-real-4c.xml`). This later test no longer calls the legacy migration function, although its historical test name remains; the original 14-case rerun result is unchanged.
