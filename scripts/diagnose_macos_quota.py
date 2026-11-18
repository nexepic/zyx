#!/usr/bin/env python3
"""Capture bounded native quota stacks without debugger startup or symbol lookup."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import xml.etree.ElementTree as ET

CASE = 'HybridPropertyMatureShapeCostsTest.EightBoundedGroupsKeepWholeDatabaseCostsAndEveryOwnerExact'


def diagnose(binary, case, xml_path, debugger=None, max_stacks=1, require_stack=False):
    if require_stack and debugger is not None:
        print('CI_QUOTA_UNAVAILABLE reason=require_stack_needs_native_mode', flush=True)
        return 2
    status_path = xml_path.with_name('quota-debugger.json')
    if xml_path.exists() or status_path.exists():
        print('CI_QUOTA_UNAVAILABLE reason=preexisting_evidence', flush=True)
        return 2
    command = [str(binary.resolve()), '--gtest_filter=' + case]
    if debugger is not None:
        module = Path(__file__).with_name('lldb_quota_trace.py').resolve()
        command = [debugger, '--no-lldbinit', '--batch',
                   '-o', 'settings set target.disable-aslr false',
                   '-o', 'command script import ' + shlex.quote(module.as_posix()),
                   '-o', 'run', '-o', 'script lldb_quota_trace.report_exit(lldb.debugger)',
                   '--', *command]
    environment = os.environ.copy()
    environment.pop('ZYX_CI_QUOTA_NATIVE', None)
    if debugger is None:
        environment['ZYX_CI_QUOTA_NATIVE'] = '1'
    environment['GTEST_OUTPUT'] = 'xml:' + str(xml_path)
    environment['ZYX_CI_QUOTA_STATUS_PATH'] = str(status_path)
    environment['ZYX_CI_QUOTA_STACK_LIMIT'] = str(max_stacks)
    if os.name == 'nt':
        environment['ZYX_CI_QUOTA_SYMBOL_ONLY'] = '1'
    status = subprocess.call(command, env=environment)
    print(f'CI_QUOTA_PROCESS mode={"lldb" if debugger else "native"} exit={status}', flush=True)
    if status and debugger is not None:
        return 2
    try:
        process = json.loads(status_path.read_text())
        if not isinstance(process, dict):
            raise ValueError('invalid process status')
        root = ET.parse(xml_path).getroot()
        executed = [t for t in root.iter('testcase') if t.get('status') == 'run']
        if debugger is None:
            if process.get('native') is not True or process.get('limit') != max_stacks:
                raise ValueError('native instrumentation missing')
            attempts, captured = process['attempts'], process['captured']
            if type(attempts) is not int or type(captured) is not int or attempts < 0 or captured != min(attempts, max_stacks):
                raise ValueError('incomplete native capture')
            if require_stack and captured == 0:
                print('CI_QUOTA_UNAVAILABLE reason=required_native_stack_not_captured', flush=True)
                return 2
            if process['exit_code'] != status:
                raise ValueError('process status mismatch')
        if process['exited'] is not True or type(process['exit_code']) is not int or len(executed) != 1:
            raise ValueError('incomplete test execution')
        test = executed[0]
        if test.get('classname') + '.' + test.get('name') != case or test.find('skipped') is not None:
            raise ValueError('unexpected test execution')
        if process['exit_code'] or test.find('failure') is not None:
            return 1
        return 0
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError):
        print('CI_QUOTA_UNAVAILABLE reason=missing_or_incomplete_process_or_xml', flush=True)
        return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--case', default=CASE)
    parser.add_argument('--require-stack', action='store_true', help='Require a real capture for the deterministic instrumentation probe')
    parser.add_argument('--lldb', action='store_true', help='Opt-in legacy debugger mode; CI uses native capture')
    parser.add_argument('--max-stacks', type=int, choices=(1, 2, 3), default=1)
    args = parser.parse_args()
    if args.require_stack and args.lldb:
        parser.error('--require-stack needs native mode')
    if not re.fullmatch(r'[A-Za-z0-9_./]+', args.case):
        parser.error('one exact test name is required')
    output = os.environ.get('GTEST_OUTPUT', '')
    debugger = shutil.which('lldb') if args.lldb else None
    if (args.lldb and not debugger) or not args.binary.is_file() or not output.startswith('xml:') or not output[4:]:
        print('CI_QUOTA_UNAVAILABLE reason=missing_binary_xml_or_requested_debugger', flush=True)
        return 2
    return diagnose(args.binary, args.case, Path(output[4:]).resolve(), debugger, args.max_stacks, args.require_stack)


if __name__ == '__main__':
    raise SystemExit(main())
