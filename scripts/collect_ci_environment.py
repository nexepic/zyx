#!/usr/bin/env python3
"""Collect a whitelist of tool/build facts, never the process environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--save-crash-symbols', action='store_true')
    args = parser.parse_args()
    record = {'platform': platform.platform(), 'architecture': platform.machine(),
              'event_sha': os.environ.get('GITHUB_SHA'), 'run_id': os.environ.get('GITHUB_RUN_ID'),
              'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT'), 'tools': {}, 'cmake': {}, 'modules': []}
    for command in [['git', 'rev-parse', 'HEAD'], ['cmake', '--version'], ['clang++', '--version'],
                    ['clang-cl', '--version'], ['llvm-symbolizer', '--version'], ['conan', '--version'],
                    ['sw_vers'], ['xcodebuild', '-version']]:
        if not shutil.which(command[0]): continue
        try:
            p = subprocess.run(command, capture_output=True, text=True, timeout=10)
            record['tools'][command[0]] = {'exit': p.returncode, 'output': (p.stdout + p.stderr)[:8192]}
        except (OSError, subprocess.TimeoutExpired) as error:
            record['tools'][command[0]] = {'error_type': type(error).__name__}
    cache = args.build_dir / 'CMakeCache.txt'
    allowed = {'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'CMAKE_CXX_COMPILER_ID', 'CMAKE_CXX_COMPILER_VERSION',
               'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_DEBUG', 'CMAKE_EXE_LINKER_FLAGS', 'ZYX_ENABLE_COVERAGE',
               'ZYX_BUILD_TESTS', 'ZYX_BUILD_APPS'}
    if cache.is_file():
        for line in cache.read_text(errors='replace').splitlines():
            key = line.split(':', 1)[0]
            if key in allowed and '=' in line: record['cmake'][key] = line.split('=', 1)[1]
    configured = record['cmake'].get('CMAKE_CXX_COMPILER')
    if configured and Path(configured).is_file():
        try:
            p = subprocess.run([configured, '--version'], capture_output=True, text=True, timeout=10)
            record['tools']['configured_cxx'] = {'exit': p.returncode, 'output': (p.stdout+p.stderr)[:8192]}
        except (OSError, subprocess.TimeoutExpired) as error:
            record['tools']['configured_cxx'] = {'error_type': type(error).__name__}
    for name in ['zyx_test_suite', 'zyx_test_suite.exe', 'zyx_test_suite.pdb']:
        p = args.build_dir / name
        if not p.is_file(): continue
        h = hashlib.sha256()
        with p.open('rb') as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
        record['modules'].append({'name': name, 'bytes': p.stat().st_size, 'sha256': h.hexdigest()})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.save_crash_symbols:
        crashed = False
        for status in list(args.output.parent.rglob('result.json'))[:256]:
            try:
                result = json.loads(status.read_text())
                raw = result.get('process', {}).get('raw_exit_code')
                crashed |= raw is not None and (raw < 0 or raw in (3, 134, 137, 139) or raw >= 0x80000000)
            except (OSError, ValueError): pass
        record['crash_symbols'] = {'crash_detected': crashed, 'copied': [], 'limit_bytes': 512*1024*1024}
        if crashed:
            symbols = [args.build_dir / 'zyx_test_suite.exe', args.build_dir / 'zyx_test_suite.pdb']
            symbols = [p for p in symbols if p.is_file()]
            total = sum(p.stat().st_size for p in symbols)
            if not symbols: record['crash_symbols']['unavailable'] = 'no Windows image/PDB at expected paths'
            elif total > 512*1024*1024: record['crash_symbols']['unavailable'] = 'combined symbol/image size exceeds bounded copy limit'
            else:
                destination = args.output.parent / 'crash-symbols';destination.mkdir(exist_ok=True)
                for p in symbols:
                    shutil.copyfile(p, destination / p.name)
                    record['crash_symbols']['copied'].append(p.name)
    args.output.write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__': main()
