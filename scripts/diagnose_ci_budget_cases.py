#!/usr/bin/env python3
"""Run known platform-budget cases separately so bounded traces reset per case."""
import argparse
import os
import hashlib
import subprocess
import sys
from pathlib import Path

WINDOWS = [
    *('ConsumedPrefixCompactionTest.' + name for name in (
        'ChargesOverlappingBuffersAndPreservesPayloadIdentity',
        'AllocatorRejectionLeavesSourceAndRetryIntact',
        'GeometricCompactionBoundsTotalMoves',
        'NoThrowSwapTransfersPayloadWhenMoveMayThrow',
        'FailedEmptyPreparationLeavesAllSourcePayloadsAndAllowsRetry',
        'SemanticPreparationFailurePropagatesWithoutChangingTheSource',
        'ReclaimsBackingBeforeRetainedOutputsConsumeTheTemporaryHeadroom',
        'PotentiallyThrowingMovesKeepOriginalStorage',
        'AllocationFailureFallsBackWithoutMovingAndAllowsRetry',
        'HierarchicalQuotaRejectionRefundsAndCanRetryAfterCompetingOwnerReleases',
    )),
    'BudgetMemoryResourceTest.HashNodeLayoutIncludesAlignmentWithoutArbitraryPerNodeSlack',
    'RelocatableMetadataStoreTest.LargeValueRangeReuseValidatesRawAndCompressedChunksOnce',
    'CellScratchVectorTest.SingleOrdinalAndKeyViewStayInsideTheirOwnersWithoutHeap',
    'CellScratchVectorTest.MultipleColumnsReserveOnceAndClearWithoutDroppingTheirCharge',
    'CellScratchVectorTest.UnreservedGrowthAndOverflowRejectWithoutChangingStoredValues',
    # Build 233 reached these native paths before the codec abort. Isolate
    # them too so tight source/parent quotas have their own bounded trace.
    'RelocatableStorageBackendTest.BoundCsvUriAndFieldsUseOwningResultsAndTheConfiguredImportRoot',
    'RelocatableStorageBackendTest.GroupedMixedCountDoesNotPrefetchLargeCountOnlyProperties',
    'RelocatableStorageBackendTest.NativeAndOverlayNodePredicatesAdmitBeforeDecodeAndSplitColumns',
    'RelocatableStorageBackendTest.NativeEdgeCellsKeepTightAndRetryReadsBounded',
    'RelocatableStorageBackendTest.NativeSelectedPayloadAdmissionKeepsTightAndAmpleReadsBounded',
    'RelocatableStorageBackendTest.NestedRawPropertyPayloadRemainsChargedAndBudgetDenialPreservesThePrefix',
    'RelocatableStorageBackendTest.RepeatedNativeReopenReturnsCacheAndOutputChargesToTheirOwners',
    'LogicalVectorBufferedSortTest.SelectedPropertyGatherPreservesThreeHundredRowsAcrossBoundedReads',
    *('LogicalVectorPropertyBatchTest.' + name for name in (
        'ProjectExpressionsShareRetainedProcedureOutputBudget',
        'GdsDijkstraProcedureForwardsMaterializedByteBudget',
        'SelectedColumnsShareRetainedResultBudgetAndKeepHitOrder',
        'BareNodeAliasesAdmitSourceAndIndependentCopiesBeforePublishing',
        'CompleteEntityGatherAndRowsShareBudgetAcrossBoundedBatches',
        'MetadataJointRetentionRejectsLowBudgetAndCanRetry',
        'RejectsOversizedSearchHitBufferBeforeReadingNodes',
        'MetadataSemanticErrorsPrecedeJointPressureSplitting',
        'FilterRetainsOneRowCacheAcrossSelectedProjectionUnderJointBound',
        'CompletePropertiesAndLabelsSplitJointRetentionBeforeCachePublication',
        'ExpandsDistinctTopKForFilterAfterSkip',
        'ShortestPathSelectedPropertiesRestorePathOrderWithoutCompleteReads',
        'BatchesMetadataAndSelectedPropertiesInTheSameExpression',
        'BareNodeOutputSharesRetainedResultBudgetAndPreservesHitOrder',
        'CompleteSemanticErrorsPrecedeJointPressureRetry',
    )),
    'PrepaidMemoryResourceTest.StandardResultMapStructuralBoundCoversIndependentAllocatorRequests',
    # Build #230 exposed more cases after the earlier abort was removed.
    # Keep each in its own process so traces and an abort identify one case.
    'CsvReaderTest.SharesByteAllowanceBetweenRawRecordsAndDecodedFieldSlots',
    'CsvReaderTest.BufferedAllowanceChangesOnlyAfterConsumingThePeekedRow',
    'CsvReaderTest.SharedStringAccountingIncludesSmallExternalAndRetainedBuffers',
    'CsvReaderTest.RetainedPropertyAccountingObservesCopiedContainerCapacity',
    'LogicalPlanDirectEdgeRuntimeTest.NodePredicateSplitPreservesScanOrderMissingCellsAndCancellation',
    'LogicalPlanDirectEdgeRuntimeTest.NestedSortPrefetchesOuterPropertyAndTransfersRows',
    'LogicalPlanDirectEdgeRuntimeTest.FilterBetweenSortsTransfersSelectedRowReservations',
    'LogicalPlanDirectEdgeRuntimeTest.ProjectBetweenSortsTransfersProjectedRowReservations',
    'LogicalPlanDirectEdgeRuntimeTest.SortPreservesProjectedFilteredRowsWithoutOuterPrefetch',
    'LogicalPlanDirectEdgeRuntimeTest.SortAdmitsExpandedRowsWhileUnwindRetainsItsInputCharge',
    'LogicalPlanDirectEdgeRuntimeTest.TwoHopParallelFinalEdgesAggregateWithinBudget',
    'LogicalPlanDirectEdgeRuntimeTest.TwoHopParallelFirstEdgesUseDistinctEndpointGatherWithinBudget',
    'LogicalPlanDirectEdgeRuntimeTest.PropertyPrefetchTransfersItsChargeBeforeSortAndVisitorCancellation',
    'LogicalPlanDirectEdgeRuntimeTest.CollectedTwoHopTopologyPreservesStreamingOrderAndBag',
    'LogicalPlanDirectEdgeRuntimeTest.NestedGraphScopesReadOnlyTheirSelectedEntityProperties',
    'LogicalPlanDirectEdgeRuntimeTest.PatternNamedColumnsPreserveUnknownTokensAndMissingValues',
    'LogicalPlanLoadCsvRuntimeTest.RejectsRemoteUrisAndFilesOutsideTheConfiguredRoot',
    'LogicalPlanLoadCsvRuntimeTest.ExecutionBudgetRejectsCsvSourceBuffersBeforeRowDelivery',
    'LogicalPlanLoadCsvRuntimeTest.PreflightsListAndMapSlotsBeforeConstructingWideCsvRows',
    'LogicalPlanLoadCsvRuntimeTest.OutputBatchSharesCsvSourceBudgetWhileReadingAhead',
    'LogicalPlanLoadCsvRuntimeTest.NestedSortTransfersRowsWithoutDuplicatingSharedCharges',
    'LogicalPlanLoadCsvRuntimeTest.AggregateAndCsvRetainedHeadersShareTheExecutionBudget',
    'LogicalPlanUnionRuntimeTest.SortTransfersLargeRowsToDirectLimitAndSkipBuffers',
    'LogicalPlanUnwindRuntimeTest.RangeAdmitsListGrowthBeforeAllocation',
    'LogicalPlanUnwindRuntimeTest.ProjectRetainsEarlierListWhileEvaluatingLaterRange',
    'LogicalPlanUnwindRuntimeTest.FunctionRetainsEarlierArgumentDuringLaterRange',
    'LogicalPlanUnwindRuntimeTest.BinaryExpressionRetainsLeftValueDuringRightEvaluation',
    'LogicalPlanUnwindRuntimeTest.InExpressionRetainsActualValueDuringCandidateEvaluation',
    'LogicalPlanUnwindRuntimeTest.SimpleCaseRetainsComparisonValueDuringWhenEvaluation',
    'LogicalPlanReadRuntimeTest.PaginationFlushesUnderBytePressureWithoutChangingTheWindow',
    'LogicalPlanReadRuntimeTest.SortPaginationAndSortTransferRowsWithinTheSharedBudget',
    'LogicalPlanVarLengthRuntimeTest.ChargesInitialCountFrontierCapacityToMaterializedByteBound',
    'PrepaidMemoryResourceTest.OwningAllocatorCopiesAndAssignmentsPreserveActualAllocationOwnership',
    'PrepaidMemoryResourceTest.ScopedConstructionAndCopyRebindingRestoreThePreviousOwner',
    'PrepaidMemoryResourceTest.MovedFromOwningVectorKeepsAReusableAllocatorAfterDestinationDestruction',
    'LogicalGdsResourceOwnershipTest.ProjectionRejectsBeforeScanningAndRefundsItsPartialConstruction',
    'LogicalGdsResourceOwnershipTest.EscapedLazyCsrUsesTheOriginatingAuthorityAndOutlivesTheProjection',
    'LogicalGdsResourceOwnershipTest.DeniedLazyCsrPublishesNothingAndRefundsBeforeTheSameProjectionRetries',
    'LogicalGdsResourceOwnershipTest.DeniedProjectionCopyPreservesTheSourceAndItsCompleteAdjacency',
    'LogicalGdsResourceOwnershipTest.LegacyProjectionDoesNotBorrowAnUnrelatedDefaultPmrResource',
    'LogicalGdsResourceOwnershipTest.CatalogCommitAndLazyCsrKeepTheirCacheOwnerUnderAForeignCaller',
    'LogicalGdsResourceOwnershipTest.AbandonedCatalogPreparationRefundsCandidateMetadataAndRevisionState',
    'LogicalGdsResourceOwnershipTest.EveryPreparationAdmissionFailureRestoresTheEmptyCatalogAndRefundsBeforeRetry',
    'LogicalGdsResourceOwnershipTest.EveryAlgorithmWorkspaceRejectsQueryPressureWithoutChargingTheProjectionCache',
    'LogicalGdsResourceOwnershipTest.OwnedAlgorithmResultsRetainOnlyTheirEscapedQueryArrays',
    'LogicalGdsResourceOwnershipTest.AllAlgorithmAdmissionPointsRefundBeforeRetryAndLeaveTheCsrReusable',
    'NativeReadSessionTokenBudgetTest.TinyFifosDoNotReserveWholePagesForTheirFirstEntry',
    'NativeReadSessionTokenBudgetTest.FailedConstructionRefundsEveryPartialContainer',
    'NativeReadSessionTokenBudgetTest.EveryOwnedPropertyInsertionFailurePreservesItsOldEntryAndRefunds',
    'NativeReadSessionTokenBudgetTest.FailedNamedTokenInsertionLeavesNoPhantomHitAndCanRetry',
    'NativeReadSessionTokenBudgetTest.SeparatePrivateBookkeepingKeepsLabelReplacementAllocatorsAndPayloadRefundsCorrect',
    'LogicalVectorPropertyBatchTest.CountsRetainedQueryResultRowsAcrossProcedureBatches',
    'LogicalVectorPropertyBatchTest.TopKResultPublicationSharesHeapRowsAndRetainedResultBudget',
    'RelocatableStorageBackendTest.CoordinatedPropertyPrefetchReleasesSelectedAndRejectedRowsBeforeClose',
    'RelocatableStorageBackendTest.WritableReopenUsesProtectedMaintenanceScratchUnderExhaustedCache',
    'RelocatableStorageBackendTest.StreamedBulkEdgeRebasedManifestCountDivergenceRetainsExternalGraphAndAllowsRetry',
    'RelocatableStorageBackendTest.StreamedBulkEdgeRebasedRunOrderingDivergenceRetainsExternalGraphAndAllowsRetry',
    'RelocatableStorageBackendTest.StreamedBulkEdgeRebasesAcrossTransactionsWithMaintenanceEnabled',
    'RelocatableStorageBackendTest.StreamedBulkEdgeRebasePreservesAnOperatorPausedMaintenanceService',
    'RelocatableStorageBackendTest.TokenCountsFollowStreamedImportForKnownNewAndUntypedEdges',
    'RelocatableStorageBackendTest.TokenCountsRebaseStreamedWritesAndRollBackCatalogFailure',
    'RelocatableStorageBackendSchemaTest.AppliesDurableQueryMemoryPolicyToTheEngineDefaultGuard',
    'BuildingScanMembershipTest.MetadataMaintenanceReleasesAtMostOnePoolAndPreservesPinnedRoots',
    'RelocatableMetadataStoreTest.PrivateSequentialChunkWorkspaceDoesNotAccumulateReleasedChunkBuffers',
    'RelocatableProcessCrashBulkEdgeStagingTest.LiveCrossProcessBulkSessionRejectsAnotherSessionAndCrashReleasesTheLeaseForRetry',
    'RelocatableBTreePageCodecTest.RebindingReleasesSourceKeysBeforeValidatingTheDestination',
    'HybridPropertyPoolTailReuseTest.ConversionFillsPartialNativePoolBeforeSpanningAnotherPool',
    'HybridPropertyReadContextTest.FieldDemotionPreparesEveryFinalDirectoryAndPreservesUnrelatedBytes',
    'HybridPropertyReadContextTest.FieldDemotionRefusesPartialOwnerDeletionAndRefundsQuotaAndCancellation',
    'HybridPropertyReadContextTest.MixedGroupMutationsCloneSharedSourcesOnceAndPreserveAllFinalChanges',
    'HybridPropertyReadContextTest.WholeOwnerDeletionCoordinatesColumnDemotionExistingExtensionsAndLiveOnce',
    'HybridPropertyMatureShapeCostsTest.EightBoundedGroupsKeepWholeDatabaseCostsAndEveryOwnerExact',
    'PropertySubblockMapMutationTest.RejectedNewPrefixKeepsPreviousFinalStateAndRefundsPlanningNodes',
    'RelocatableVectorCodebookStoreTest.EmptyCacheNeedsNoGrantAndFirstReadCanRetryAfterDenial',
    'RelocatableVectorCodebookStoreTest.RollsBackEveryMetadataAllocationFailureAndRetries',
    'StorageIOTest.TypedPageRangePreservesShortAndFailedReadSemantics',
    'ProcedurePayloadAdmissionTest.PreparedHashBucketsAreChargedOnceAndNeverShrunkByInsertion',
    'ProcedurePayloadAdmissionTest.MissingHashCapacityStillRequiresAdmissionBeforeRehash',
    'ProcedurePayloadAdmissionTest.ReusingBucketsDoesNotBypassNodeAdmissionAndAllowsRetryAfterRefund',
    'RelocatableBTreePageCodecTest.RestartSearchBorrowsProbeKeysButReturnsAnIndependentOwner',
]
MACOS = [
    'LogicalVectorBufferedSortTest.SelectedPropertyGatherPreservesThreeHundredRowsAcrossBoundedReads',
    'LogicalPlanDirectEdgeRuntimeTest.FilterSelectedColumnsReleaseWideRejectedBatchesAfterFailure',
    'RelocatableStorageBackendTest.CoordinatedPropertyPrefetchReleasesSelectedAndRejectedRowsBeforeClose',
    'HybridPropertyMatureShapeCostsTest.EightBoundedGroupsKeepWholeDatabaseCostsAndEveryOwnerExact',
    'LogicalPlanDirectEdgeRuntimeTest.TightAndAmplePropertyPrefetchReadEachNodeOnce',
    'LogicalPlanLoadCsvRuntimeTest.ChargesHeaderMapLinksAndBucketsBeforeConstructingRows',
    '*HybridPropertyShapeCostsTest.WholeAllocationAndEveryOwnerSurviveConversionAndReadOnlyReopen/shared_large',
    'QueryEngineLogicalBackendBoundaryTest.AdministrationPipelineRespectsParentQuotaBeforeMutation',
    'LogicalVectorPropertyBatchTest.SelectedColumnsShareRetainedResultBudgetAndKeepHitOrder',
    'LogicalVectorPropertyBatchTest.GdsDijkstraPipelineAccountsOwnedResultsAfterPathRelease',
    'LogicalVectorPropertyBatchTest.UsesBoundedTopKWhenFilterReadsAProjectedAliasBeforeWindow',
    'LogicalPlanLoadCsvRuntimeTest.PropertyPrefetchAndLiveCsvHeadersShareTheExecutionBudget',
]


ASAN = [
    'RelocatableStorageBackendTest.NativeEdgeCellsKeepTightAndRetryReadsBounded',
    'RelocatableStorageBackendTest.NativeSelectedPayloadAdmissionKeepsTightAndAmpleReadsBounded',
    'RelocatableStorageBackendTest.CoordinatedPropertyPrefetchReleasesSelectedAndRejectedRowsBeforeClose',
    'ResultPropertyMapAdmissionTest.*',
    'ScopedScalarReadValueTest.*',
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--platform', choices=('windows', 'macos', 'asan'), required=True)
    args = parser.parse_args()
    cases = {'windows': WINDOWS, 'macos': MACOS, 'asan': ASAN}[args.platform]
    runner = Path(__file__).with_name('run_test_process.py')
    failures = []
    child_env = os.environ.copy()
    child_env.setdefault('ZYX_CI_CASE_TRACE', '1')
    for case in cases:
        print(f'CI_BUDGET_CASE BEGIN {case}', flush=True)
        listing = subprocess.run([str(args.binary), '--gtest_list_tests', '--gtest_filter=' + case],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if listing.returncode or not any(line.startswith('  ') for line in listing.stdout.splitlines()):
            print(f'CI_BUDGET_CASE missing or undiscoverable {case} exit={listing.returncode}', flush=True)
            failures.append(case)
            continue
        timeout = '1200' if 'EightBoundedGroups' in case else '300'
        command = [sys.executable, str(runner), '--timeout', timeout, '--',
                   str(args.binary), '--gtest_filter=' + case]
        if os.environ.get('ZYX_CI_EVIDENCE_DIR'):
            token = hashlib.sha256(case.encode()).hexdigest()[:16]
            command = [sys.executable, str(Path(__file__).with_name('run_ci_evidence.py')),
                       '--name', 'budget-' + token, '--timeout', timeout, '--gtest', '--',
                       str(args.binary), '--gtest_filter=' + case]
        status = subprocess.call(command, env=child_env)
        print(f'CI_BUDGET_CASE END {case} exit={status}', flush=True)
        if status:
            failures.append(case)
    print(f'CI_BUDGET_CASE total={len(cases)} failed={len(failures)}', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
