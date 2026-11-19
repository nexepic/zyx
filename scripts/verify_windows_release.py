#!/usr/bin/env python3
"""Fail closed if the Windows Release job compiled Debug/coverage commands."""
import argparse
import json
from pathlib import Path
import re
import shlex


def verify_link_flags(flags, source):
    flags = {flag.upper() for flag in flags.split()}
    if (not {'/DEBUG', '/OPT:REF', '/OPT:ICF'} <= flags
            or {'/OPT:NOREF', '/OPT:NOICF', '/DEBUG:NONE'} & flags):
        raise ValueError(f'{source}: Release PDB linking must preserve REF/ICF optimization')


def verify(build, *, preflight=False):
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        match = re.fullmatch(r'([^:#]+):[^=]+=(.*)', line)
        if match:
            cache[match[1]] = match[2]
    for key, expected in [('CMAKE_BUILD_TYPE', 'Release'), ('ZYX_ENABLE_COVERAGE', 'OFF')]:
        if cache.get(key) != expected:
            raise ValueError(f'{key} must be {expected}, got {cache.get(key)!r}')
    verify_link_flags(cache.get('CMAKE_EXE_LINKER_FLAGS_RELEASE', ''), 'CMake cache')
    ninja = (build / 'build.ninja').read_text(encoding='utf-8')
    # A single-config Ninja build emits the real executable and its variables
    # in one stanza. Check that stanza as well as the CMake cache.
    matches = re.findall(r'^build zyx_test_suite\.exe: CXX_EXECUTABLE_LINKER[^\n]*\n((?: +[^\n]*\n)*)',
                         ninja, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError('expected one generated zyx_test_suite.exe linker rule')
    link_flags = re.search(r'^ +LINK_FLAGS = (.+)$', matches[0], re.MULTILINE)
    if link_flags is None:
        raise ValueError('missing generated zyx_test_suite.exe LINK_FLAGS')
    verify_link_flags(link_flags[1], 'Ninja linker rule')
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
    if not preflight and not (build / 'zyx_test_suite.pdb').is_file():
        raise ValueError('missing built Release PDB')
    return {'phase': 'preflight' if preflight else 'post-build', 'build_type': 'Release', 'coverage': False, 'crt': 'MD',
            'optimization': 'O2', 'ndebug': True,
            'test_compile_commands': tests, 'production_compile_commands': production}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--preflight', action='store_true', help='Audit generated configuration before building the PDB')
    args = parser.parse_args()
    try:
        print('CI_WINDOWS_RELEASE ' + json.dumps(verify(args.build_dir, preflight=args.preflight), sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('CI_WINDOWS_RELEASE_INVALID ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
