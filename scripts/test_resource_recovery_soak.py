#!/usr/bin/env python3
"""Repeat selected resource-lifetime contracts and real-process crash recovery.

This is a correctness/resource soak, not a throughput or latency benchmark.
Existing test assertions enforce budget refunds and bounded physical space.
"""
import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import subprocess
import tempfile
import sys
import time
import xml.etree.ElementTree as ET

CASES = (
    'RelocatableStorageBackendTest.RepeatedNativeReopenReturnsCacheAndOutputChargesToTheirOwners',
    'RelocatableStorageBackendTest.NativePinnedSnapshotFilePressureFailsWithoutPublishingAndReusesSpaceAfterRelease',
    'RelocatableStorageBackendTest.QueryScratchFilePressureReleasesHandlesAndCanReuseQuotaWithGraphPinStillHeld',
    'RelocatableStorageBackendTest.QueryScratchCleanupUsesProtectedMaintenanceWhenCacheCreditIsExhausted',
    'RelocatableStorageBackendTest.QueryScratchCleanupAdmissionDenialPrecedesPublicationAndCanRetryTheSameHandle',
    'RelocatableMaintenanceServiceTest.BoundsSpaceDuringConcurrentReadersWritersAndMaintenance',
)


def file_sha256(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def verify_selected_xml(path):
    root = ET.parse(path).getroot()
    entries = list(root.iter('testcase'))
    cases = {case.attrib['classname'] + '.' + case.attrib['name']: case for case in entries}
    if len(entries) != len(CASES) or set(cases) != set(CASES):
        raise RuntimeError('soak must execute exactly the selected resource contracts')
    for name, case in cases.items():
        if (case.attrib.get('status') != 'run' or case.attrib.get('result') != 'completed'
                or case.find('failure') is not None or case.find('skipped') is not None):
            raise RuntimeError('resource contract did not pass: ' + name)
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-binary', required=True, type=pathlib.Path)
    parser.add_argument('--worker', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    parser.add_argument('--rounds', type=int, default=256)
    parser.add_argument('--crash-cycles', type=int, default=512)
    args = parser.parse_args()
    if args.rounds < 1 or args.crash_cycles < 1:
        parser.error('rounds and crash cycles must be positive')
    binary, worker = args.test_binary.resolve(strict=True), args.worker.resolve(strict=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    prefix = args.output.with_suffix('')
    crash_report = pathlib.Path(str(prefix) + '-crashes.json')
    test_log = pathlib.Path(str(prefix) + '-tests.log')
    crash_log = pathlib.Path(str(prefix) + '-crashes.log')
    source_root = pathlib.Path(__file__).resolve().parents[1]
    started = time.monotonic()
    hashes = {str(path): file_sha256(path) for path in (binary, worker)}

    def crash_lane():
        with crash_log.open('w') as log:
            subprocess.run([
                sys.executable, str(source_root / 'scripts/test_transaction_resource_faults.py'),
                '--worker', str(worker), '--output', str(crash_report),
                '--crash-recovery', '--crash-cycles', str(args.crash_cycles),
                '--progress-every', '64'], stdout=log, stderr=log, check=True)
        report = json.loads(crash_report.read_text())
        cycles = report['crash_recovery']['cycles']
        if report['failures'] or len(cycles) != args.crash_cycles:
            raise RuntimeError('incomplete crash recovery lane')
        for cycle in cycles:
            if (not cycle['passed'] or cycle['crash_exit'] != 87
                    or cycle['readonly_before_sha256'] != cycle['readonly_after_sha256']
                    or not cycle['recovered']['passed']):
                raise RuntimeError('crash/readonly/recovery contract failed')
        sizes = [cycle['file_bytes'] for cycle in cycles]
        if any(size != sizes[0] for size in sizes):
            raise RuntimeError('repeated fixed scratch recovery grew the database file')
        return {'cycles': len(cycles), 'fixed_file_bytes': sizes[0]}

    completed_rounds = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        crashed = executor.submit(crash_lane)
        with tempfile.TemporaryDirectory(prefix='zyx-resource-soak-') as temporary, test_log.open('w') as log:
            xml = pathlib.Path(temporary) / 'selected.xml'
            for index in range(args.rounds):
                log.write('Resource round ' + str(index + 1) + '\n')
                log.flush()
                subprocess.run([str(binary), '--gtest_filter=' + ':'.join(CASES),
                                '--gtest_output=xml:' + str(xml)],
                               stdout=log, stderr=log, check=True, timeout=180)
                verify_selected_xml(xml)
                completed_rounds += 1
                if completed_rounds % 16 == 0:
                    print(f'resource soak: {completed_rounds}/{args.rounds} rounds passed', flush=True)
                # Surface crash-lane failures without hiding them behind the soak.
                if crashed.done():
                    crashed.result()
            recovery = crashed.result()
    if any(file_sha256(path) != digest
           for path, digest in hashes.items()):
        raise RuntimeError('a selected executable changed during the soak')
    report = {'passed': True, 'full_suite': False, 'performance_benchmark': False,
              'completed_rounds': completed_rounds, 'contracts_per_round': len(CASES),
              'contract_executions': completed_rounds * len(CASES),
              'native_reopen_cycles': completed_rounds * 64,
              'concurrent_write_iterations': completed_rounds * 64,
              'crash_recovery': recovery, 'elapsed_seconds': time.monotonic() - started,
              'binary_sha256': hashes, 'cases': CASES}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
