#!/usr/bin/env python3
"""Collect a whitelist of tool/build facts, never the process environment."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import re
import threading
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



# Explicit diagnostic cases are read before the bounded general evidence walk.
NATIVE_CASES = (
    'native-quota-stack',
    'budget-3257bf1e1a62b30e',  # Existing conversion budget replay.
    'windows-conversion-stack', 'windows-edge-cell-stack',
    'windows-nested-scope-stack', 'windows-query-account-stack',
    'macos-conversion-quota-stack',
)
PC_PATTERN = re.compile(
    r'^CI_QUOTA_NATIVE_PC stack=(\d{1,2}) index=(\d{1,2}) pc=((?:0x)?[0-9a-fA-F]{1,16}) '
    r'base=((?:0x)?[0-9a-fA-F]{1,16}) offset=(0x[0-9a-fA-F]{1,16}) module=(.{1,4096})$')


def bounded_symbolizer(command, addresses):
    """No shell, network symbol lookup or unbounded subprocess output."""
    expired = threading.Event()
    try:
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              env={**os.environ, 'DEBUGINFOD_URLS': '',
                                   '_NT_SYMBOL_PATH': '', '_NT_ALT_SYMBOL_PATH': '',
                                   'LLVM_SYMBOLIZER_OPTS': ''}) as process:
            def expire():
                expired.set()
                try:
                    process.kill()
                except OSError:
                    pass  # The process may have exited concurrently.
            timer = threading.Timer(10, expire)
            timer.start()
            try:
                process.stdin.write(addresses.encode('ascii'))
                process.stdin.close()
                output = process.stdout.read(65537)
                truncated = len(output) > 65536
                if truncated:
                    process.kill()
                code = process.wait(timeout=2)
            finally:
                timer.cancel()
                if process.poll() is None:
                    process.kill()
                    process.wait()
        return {'exit_code': code, 'timed_out': expired.is_set(),
                'output_truncated': truncated,
                'output': output[:65536].decode('utf-8', errors='replace')}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'error_type': type(error).__name__}


def read_native_frames(case, image_names=('zyx_test_suite', 'zyx_test_suite.exe')):
    item = {}
    log = safe_file(case, 'output.log')
    if not log:
        item['unavailable'] = 'missing whitelisted image or output.log'
        return item
    frames = []
    seen = set()
    try:
        with log.open('rb') as stream:
            raw = stream.read(2 * 1024 * 1024 + 1)
        item['input_truncated'] = len(raw) > 2 * 1024 * 1024
        for line in raw[:2 * 1024 * 1024].decode('utf-8', errors='replace').splitlines():
            match = PC_PATTERN.fullmatch(line)
            if not match:
                continue
            stack, index, pc, base, offset, module = match.groups()
            stack, index = int(stack), int(index)
            pc, base, offset = int(pc, 16), int(base, 16), int(offset, 16)
            # Ignore logged paths; only basename selects the fixed local image.
            if (module.replace('\\', '/').rsplit('/', 1)[-1] not in image_names
                    or not 0 <= stack < 3 or not 0 <= index < 32
                    or not 0 < base <= pc < 2**64 or pc - base != offset
                    or (stack, index) in seen):
                continue
            seen.add((stack, index))
            frames.append({'stack': stack, 'index': index, 'pc': hex(pc),
                           'base': hex(base), 'offset': hex(offset)})
    except OSError as error:
        item['error_type'] = type(error).__name__
        return item
    item['frames'] = frames
    return item


def symbolize_native(build, evidence, cases):
    """Resolve only our image while its local PDB/debug-map objects still exist.

    Tool success is not a guarantee of source-line resolution: retain raw output.
    Artifact copy completeness is reported separately.
    """
    results = []
    image = safe_file(build, 'zyx_test_suite.exe') or safe_file(build, 'zyx_test_suite')
    for case in cases[:8]:
        item = {'case': str(case.relative_to(evidence)), 'source_lines_guaranteed': False}
        results.append(item)
        item.update(read_native_frames(case, (image.name,) if image else ()))
        frames = item.get('frames', [])
        if not image:
            item['unavailable'] = 'missing whitelisted image'
            continue
        if not frames:
            item['unavailable'] = 'no valid local-image frames'
            continue
        if image.suffix == '.exe' or platform.system() == 'Linux':
            tool = shutil.which('llvm-symbolizer')
            if not tool and platform.system() == 'Linux':
                # Ubuntu CI installs llvm-20, whose tools have versioned names.
                tool = shutil.which('llvm-symbolizer-20')
            if image.suffix == '.exe' and not safe_file(build, 'zyx_test_suite.pdb'):
                item['unavailable'] = 'missing local PDB'
                continue
            command = [tool, '--relative-address', '--no-inlines', '--demangle',
                       '--obj=' + str(image.resolve())] if tool else None
            addresses = '\n'.join(frame['offset'] for frame in frames) + '\n'
        else:
            tool = shutil.which('atos') if platform.system() == 'Darwin' else None
            bases = {frame['base'] for frame in frames}
            if len(bases) != 1:
                item['unavailable'] = 'inconsistent image load bases'
                continue
            command = [tool, '-o', str(image.resolve()), '-l', frames[0]['base'],
                       *[frame['pc'] for frame in frames]] if tool else None
            addresses = ''
        if not command:
            item['unavailable'] = 'platform symbolizer unavailable'
            continue
        item['tool'] = Path(tool).name
        item['resolution'] = bounded_symbolizer(command, addresses)
    return results

def collect_symbols(build, evidence):
    record = {'crash_detected': False, 'native_stack_detected': False,
              'copied': [], 'complete': False, 'limit_bytes': SYMBOL_LIMIT_BYTES}
    # Bound traversal as well as JSON reads. Never follow artifact symlinks.
    native_cases = []
    visited = 0
    priority = [(evidence / case, [], ['quota-debugger.json']) for case in NATIVE_CASES
                if safe_file(evidence, case + '/quota-debugger.json')]
    for directory, dirs, files in itertools.chain(priority, os.walk(evidence, followlinks=False)):
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
                    native = (result.get('native') is True
                              and type(captured) is int and 0 < captured <= 3)
                    record['native_stack_detected'] |= native
                    if native and Path(directory) not in native_cases and len(native_cases) < 8:
                        native_cases.append(Path(directory))
                else:
                    process = result.get('process')
                    raw = process.get('raw_exit_code') if isinstance(process, dict) else None
                    record['crash_detected'] |= (type(raw) is int and
                        (raw < 0 or raw in (3, 134, 137, 139) or raw >= 0x80000000))
            except (OSError, ValueError):
                continue
    # A crash/timeout can leave valid PCs but no completed status JSON.
    raw_cases = []
    for name in NATIVE_CASES:
        case = evidence / name
        if case in native_cases or len(native_cases) >= 8:
            continue
        if not safe_file(evidence, name + '/output.log'):
            continue
        raw = read_native_frames(case)
        if raw.get('frames'):
            native_cases.append(case)
            raw_cases.append(name)
            record['native_stack_detected'] = True
    if raw_cases:
        record['raw_stack_without_completed_status'] = raw_cases
    if not (record['crash_detected'] or record['native_stack_detected']):
        return record
    if native_cases:
        record['local_symbolization'] = symbolize_native(build, evidence, native_cases)
        for item in record['local_symbolization']:
            item['capture_status'] = ('unconfirmed_or_incomplete' if item['case'] in raw_cases
                                      else 'reported_by_status')
    # No arbitrary modules, source trees or paths taken from stack output.
    names = ['zyx_test_suite.exe', 'zyx_test_suite.pdb']
    if not any(safe_file(build, name) for name in names):
        names = ['zyx_test_suite']
        if platform.system() != 'Linux':
            names += ['zyx_test_suite.dSYM/Contents/Info.plist',
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
