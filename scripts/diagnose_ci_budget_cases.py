#!/usr/bin/env python3
"""Run known platform-budget cases separately so bounded traces reset per case."""
import argparse
import os
import hashlib
import subprocess
import sys
from pathlib import Path

WINDOWS = [
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
]
MACOS = [
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
        status = subprocess.call(command)
        print(f'CI_BUDGET_CASE END {case} exit={status}', flush=True)
        if status:
            failures.append(case)
    print(f'CI_BUDGET_CASE total={len(cases)} failed={len(failures)}', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
