#!/usr/bin/env python3
"""Run the unit or separately gated large-scale correctness tests.

The exact stress names remain enabled and must be run by the stress gate; they
are excluded only from the unit tier. All other enabled GoogleTest cases stay
in the unit tier. Both tiers reuse the existing isolated shard launcher.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

STRESS_TESTS = (
    'RelocatableStorageBackendTest.IntegrityLargeValueEvidenceFitsTheUnchangedDefaultWorkspace',
    'RelocatableStorageBackendTest.OrdinaryIndexImagesRegisterSharedBatchFreshPoolsOnlyOnce',
    'RelocatableStorageBackendTest.NativeLogicalReadonlySortSpillsAndPreservesCompleteDescendingCollection',
    'RelocatableStorageBackendTest.ColdInsertionUsesBoundedSummaryAcrossGrowingDirectories',
)


def tier_filter(tier):
    selected = ':'.join(STRESS_TESTS)
    return selected if tier == 'stress' else '*-' + selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tier', choices=('unit', 'stress'))
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=2 if os.name != 'nt' else 1)
    parser.add_argument('--timeout', type=int, default=3600)
    parser.add_argument('--failure-log-dir', type=Path)
    parser.add_argument('--xml-dir', type=Path, help='retain per-shard GoogleTest XML results')
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error('jobs and timeout must be positive')
    if os.name == 'nt' and args.jobs != 1:
        parser.error('Windows requires one job; the shard runner isolates POSIX only')
    binary = args.binary.resolve()
    if os.name == 'nt' and not binary.exists():
        binary = binary.with_suffix('.exe')
    runner = Path(__file__).with_name('run_test_shards.py')
    command = [sys.executable, str(runner), '--jobs', str(args.jobs), '--timeout', str(args.timeout)]
    if args.failure_log_dir:
        command += ['--failure-log-dir', str(args.failure_log_dir.resolve())]
    command += ['--', str(binary), '--gtest_filter=' + tier_filter(args.tier)]
    environment = os.environ.copy()
    if args.xml_dir:
        xml_directory = args.xml_dir.resolve()
        xml_directory.mkdir(parents=True, exist_ok=True)
        environment['ZYX_TEST_XML_DIR'] = str(xml_directory)
        if args.jobs == 1:
            environment['GTEST_OUTPUT'] = f"xml:{xml_directory / 'shard-0.xml'}"
    return subprocess.call(command, env=environment)


if __name__ == '__main__':
    sys.exit(main())
