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


SYMBOL_LIMIT_BYTES = 512 * 1024 * 1024
STATUS_LIMIT_BYTES = 64 * 1024


def safe_file(root, relative):
    """Accept only regular whitelist files, with no symlink path components."""
    candidate = root / relative
    for part in [candidate, *candidate.parents]:
        if part == root.parent:
            break
        if part.is_symlink():
            return None
    return candidate if candidate.is_file() else None


def collect_symbols(build, evidence):
    record = {'crash_detected': False, 'native_stack_detected': False,
              'copied': [], 'complete': False, 'limit_bytes': SYMBOL_LIMIT_BYTES}
    # Bound traversal as well as JSON reads. Never follow artifact symlinks.
    visited = 0
    for directory, dirs, files in os.walk(evidence, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d != 'crash-symbols'
                         and not (Path(directory) / d).is_symlink())
        visited += 1
        if visited > 256:
            record['scan_truncated'] = True
            break
        for name in ('result.json', 'quota-debugger.json'):
            if name not in files:
                continue
            status = Path(directory) / name
            try:
                if status.is_symlink() or status.stat().st_size > STATUS_LIMIT_BYTES:
                    continue
                result = json.loads(status.read_text())
                if not isinstance(result, dict):
                    continue
                if name == 'quota-debugger.json':
                    captured = result.get('captured')
                    record['native_stack_detected'] |= (result.get('native') is True
                        and type(captured) is int and 0 < captured <= 3)
                else:
                    process = result.get('process')
                    raw = process.get('raw_exit_code') if isinstance(process, dict) else None
                    record['crash_detected'] |= (type(raw) is int and
                        (raw < 0 or raw in (3, 134, 137, 139) or raw >= 0x80000000))
            except (OSError, ValueError):
                continue
    if not (record['crash_detected'] or record['native_stack_detected']):
        return record
    # No arbitrary modules, source trees or paths taken from stack output.
    names = ['zyx_test_suite.exe', 'zyx_test_suite.pdb']
    if not any(safe_file(build, name) for name in names):
        names = ['zyx_test_suite', 'zyx_test_suite.dSYM/Contents/Info.plist',
                 'zyx_test_suite.dSYM/Contents/Resources/DWARF/zyx_test_suite']
    symbols = [(name, safe_file(build, name)) for name in names]
    record['missing'] = [name for name, path in symbols if path is None]
    symbols = [(name, path) for name, path in symbols if path is not None]
    if not symbols:
        record['unavailable'] = 'no image/symbol files at whitelisted paths'
        return record
    try:
        total = sum(path.stat().st_size for _, path in symbols)
        record['required_bytes'] = total
        if total > SYMBOL_LIMIT_BYTES:
            record['unavailable'] = 'combined symbol/image size exceeds bounded copy limit'
            return record
        destination = evidence / 'crash-symbols'
        if destination.is_symlink():
            raise OSError('unsafe symbol destination')
        destination.mkdir(exist_ok=True)
        for name, path in symbols:
            target = destination / name
            if any(parent.is_symlink() for parent in [target, *target.parents]):
                raise OSError('unsafe symbol destination')
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + '.partial')
            try:
                if temporary.is_symlink():
                    raise OSError('unsafe temporary destination')
                shutil.copyfile(path, temporary)
                temporary.replace(target)
                record['copied'].append(name)
                digest = hashlib.sha256()
                with target.open('rb') as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(chunk)
                record.setdefault('identities', []).append(
                    {'name': name, 'bytes': target.stat().st_size, 'sha256': digest.hexdigest()})
            except OSError as error:
                record.setdefault('copy_errors', []).append(
                    {'name': name, 'error_type': type(error).__name__})
                try: temporary.unlink(missing_ok=True)
                except OSError: pass
    except OSError as error:
        record['collection_error_type'] = type(error).__name__
    record['complete'] = not record['missing'] and len(record['copied']) == len(names)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--save-crash-symbols', action='store_true')
    args = parser.parse_args()
    record = {'platform': platform.platform(), 'architecture': platform.machine(),
              'event_sha': os.environ.get('GITHUB_SHA'), 'run_id': os.environ.get('GITHUB_RUN_ID'),
              'run_attempt': os.environ.get('GITHUB_RUN_ATTEMPT'), 'tools': {}, 'cmake': {}, 'modules': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
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
            try:
                with p.open('rb') as f:
                    for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
                record['modules'].append({'name': name, 'bytes': p.stat().st_size, 'sha256': h.hexdigest()})
            except OSError as error:
                record['modules'].append({'name': name, 'error_type': type(error).__name__})
    except (OSError, ValueError) as error:
        record['collection_error_type'] = type(error).__name__
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.save_crash_symbols:
        record['crash_symbols'] = collect_symbols(args.build_dir, args.output.parent)
    args.output.write_text(json.dumps(record, indent=2) + '\n')
    return 1 if 'collection_error_type' in record else 0


if __name__ == '__main__': raise SystemExit(main())
