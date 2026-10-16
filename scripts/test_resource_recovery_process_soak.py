#!/usr/bin/env python3
"""Repeat resource contracts in one process; record RSS separately from quotas."""
import argparse
import collections
import json
import pathlib
import subprocess
import sys
import time
import test_resource_recovery_soak as contracts


def verify_repetitions(log, rounds):
    expected = collections.Counter({name: rounds for name in contracts.CASES})
    for prefix in ('[ RUN      ] ', '[       OK ] '):
        actual = collections.Counter(line[len(prefix):].split(' ', 1)[0]
                                     for line in log.splitlines() if line.startswith(prefix))
        if actual != expected:
            raise RuntimeError('incomplete, duplicated or failed same-process resource contracts')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-binary', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    parser.add_argument('--rounds', type=int, default=16)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('rounds must be positive')
    binary = args.test_binary.resolve(strict=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    log = args.output.with_suffix('.log')
    xml = args.output.with_suffix('.xml')
    digest = contracts.file_sha256(binary)
    started = time.monotonic()
    with log.open('w') as destination:
        subprocess.run([str(binary), '--gtest_filter=' + ':'.join(contracts.CASES),
                        '--gtest_repeat=' + str(args.rounds), '--gtest_shuffle',
                        '--gtest_random_seed=42', '--gtest_output=xml:' + str(xml)],
                       stdout=destination, stderr=subprocess.STDOUT, check=True, timeout=3600)
    verify_repetitions(log.read_text(), args.rounds)
    contracts.verify_selected_xml(xml)
    if contracts.file_sha256(binary) != digest:
        raise RuntimeError('selected executable changed during same-process soak')
    # Fresh supervisor: the selected executable is its only child. Linux uses
    # KiB for ru_maxrss; Darwin uses bytes. Windows has no resource module.
    peak_rss_bytes = None
    if sys.platform in ('linux', 'darwin'):
        import resource
        peak_rss_bytes = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        if sys.platform == 'linux':
            peak_rss_bytes *= 1024
    report = {'passed': True, 'same_process': True, 'rounds': args.rounds,
              'contracts_per_round': len(contracts.CASES),
              'contract_executions': args.rounds * len(contracts.CASES),
              'native_reopen_cycles': args.rounds * 64,
              'concurrent_write_iterations': args.rounds * 64,
              'shuffle_seed': 42, 'elapsed_seconds': time.monotonic() - started,
              'peak_rss_bytes': peak_rss_bytes, 'binary_sha256': digest,
              'full_suite': False, 'performance_benchmark': False}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
