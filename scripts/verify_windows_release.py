#!/usr/bin/env python3
"""Fail closed if the Windows Release job compiled Debug/coverage commands."""
import argparse
import json
from pathlib import Path
import re
import shlex


def verify(build):
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        match = re.fullmatch(r'([^:#]+):[^=]+=(.*)', line)
        if match:
            cache[match[1]] = match[2]
    for key, expected in [('CMAKE_BUILD_TYPE', 'Release'), ('ZYX_ENABLE_COVERAGE', 'OFF')]:
        if cache.get(key) != expected:
            raise ValueError(f'{key} must be {expected}, got {cache.get(key)!r}')
    link_flags = cache.get('CMAKE_EXE_LINKER_FLAGS_RELEASE', '').split()
    if (not {'/DEBUG', '/OPT:REF', '/OPT:ICF'} <= set(link_flags)
            or {'/OPT:NOREF', '/OPT:NOICF', '/DEBUG:NONE'} & set(link_flags)):
        raise ValueError('Release PDB linking must preserve REF/ICF optimization')
    if not (build / 'zyx_test_suite.pdb').is_file():
        raise ValueError('missing built Release PDB')
    commands = json.loads((build / 'compile_commands.json').read_text())
    production = tests = 0
    test_main = core_runtime = False
    for entry in commands:
        source = entry['file'].replace('\\', '/')
        # Verify both the test binary and production library, not a probe alone.
        is_test = '/tests/src/' in source
        is_production = '/src/' in source and '/tests/' not in source
        if not (is_test or is_production):
            continue
        args = entry.get('arguments')
        if args is None:
            args = shlex.split(entry['command'], posix=False)
        args = [arg.strip('"') for arg in args]
        if any(arg.startswith('@') for arg in args):
            raise ValueError(f'unexpanded response file: {source}')
        normalized = []
        index = 1
        while index < len(args):
            arg = args[index]
            if arg in ('/D', '-D', '/U', '-U'):
                index += 1
                if index == len(args):
                    raise ValueError(f'incomplete define/undefine: {source}')
                arg += args[index]
            normalized.append(arg)
            index += 1
        flags = set(normalized)
        if not any(flag in flags for flag in ('/Z7', '-Z7', '/Zi', '-Zi')):
            raise ValueError(f'missing Release debug symbols: {source}')
        if not any(flag in flags for flag in ('/O2', '-O2')):
            raise ValueError(f'missing Release optimization: {source}')
        if not any(flag in flags for flag in ('/DNDEBUG', '-DNDEBUG', '/DNDEBUG=1', '-DNDEBUG=1')):
            raise ValueError(f'missing NDEBUG: {source}')
        if not any(flag in flags for flag in ('/MD', '-MD')):
            raise ValueError(f'missing Release DLL CRT: {source}')
        forbidden = {'/MT', '-MT', '/O1', '-O1', '/Ox', '-Ox', '-O3', '/Od', '-Od', '-O0', '/MDd', '-MDd', '/MTd', '-MTd',
                     '/UNDEBUG', '-UNDEBUG', '/D_ITERATOR_DEBUG_LEVEL=1',
                     '-D_ITERATOR_DEBUG_LEVEL=1', '/D_ITERATOR_DEBUG_LEVEL=2',
                     '-D_ITERATOR_DEBUG_LEVEL=2'}
        debug_macro = any(re.fullmatch(r'[-/]D_DEBUG(?:=.*)?', f) for f in flags)
        if (flags & forbidden or debug_macro
                or any('fprofile-instr-generate' in f or 'fcoverage-mapping' in f for f in flags)):
            raise ValueError(f'Debug/coverage flags in Release command: {source}')
        test_main |= source.endswith('/tests/src/test_main.cpp')
        core_runtime |= source.endswith('/src/query/execution/LogicalPlanReadRuntime.cpp')
        tests += is_test
        production += is_production
    if not tests or not production or not test_main or not core_runtime:
        raise ValueError('missing test or production compile commands')
    return {'build_type': 'Release', 'coverage': False, 'crt': 'MD',
            'optimization': 'O2', 'ndebug': True,
            'test_compile_commands': tests, 'production_compile_commands': production}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        print('CI_WINDOWS_RELEASE ' + json.dumps(verify(args.build_dir), sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('CI_WINDOWS_RELEASE_INVALID ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
