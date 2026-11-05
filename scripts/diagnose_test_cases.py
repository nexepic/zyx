#!/usr/bin/env python3
"""Run named TEST cases one process at a time to isolate process termination."""

import argparse
import os
import hashlib
import re
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--suite', required=True)
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error('timeout must be positive')
    pattern = r'\bTEST\s*\(\s*' + re.escape(args.suite) + r'\s*,\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)'
    names = re.findall(pattern, args.source.read_text(encoding='utf-8'))
    if not names or len(names) != len(set(names)):
        raise RuntimeError('test source has no unique matching TEST cases')
    runner = Path(__file__).with_name('run_test_process.py')
    failures = []
    for name in names:
        case = f'{args.suite}.{name}'
        if name.startswith('DISABLED_'):
            print(f'SKIP disabled {case}', flush=True)
            continue
        print(f'RUN {case}', flush=True)
        command = [sys.executable, str(runner), '--timeout', str(args.timeout), '--',
                   str(args.binary), '--gtest_filter=' + case]
        if os.environ.get('ZYX_CI_EVIDENCE_DIR'):
            token = hashlib.sha256(case.encode()).hexdigest()[:16]
            command = [sys.executable, str(Path(__file__).with_name('run_ci_evidence.py')),
                       '--name', 'plan-' + token, '--timeout', str(args.timeout), '--gtest', '--',
                       str(args.binary), '--gtest_filter=' + case]
        status = subprocess.call(command)
        print(f'RESULT {case} exit={status}', flush=True)
        if status:
            failures.append(case)
    print(f'Isolated {len(names)} cases; failures: {len(failures)}', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
