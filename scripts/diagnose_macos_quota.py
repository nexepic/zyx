#!/usr/bin/env python3
"""Capture one live quota-rejection stack without changing allocator behavior."""
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


def diagnose(binary, case, xml_path, debugger):
    status_path = xml_path.with_name('quota-debugger.json')
    if xml_path.exists() or status_path.exists():
        print('CI_QUOTA_UNAVAILABLE reason=preexisting_evidence', flush=True)
        return 2
    module = Path(__file__).with_name('lldb_quota_trace.py').resolve()
    command = [debugger, '--no-lldbinit', '--batch',
               '-o', 'settings set target.disable-aslr false',
               '-o', 'command script import ' + shlex.quote(module.as_posix()),
               '-o', 'run', '-o', 'script lldb_quota_trace.report_exit(lldb.debugger)',
               '--', str(binary.resolve()), '--gtest_filter=' + case]
    environment = os.environ.copy()
    environment['GTEST_OUTPUT'] = 'xml:' + str(xml_path)
    environment['ZYX_CI_QUOTA_STATUS_PATH'] = str(status_path)
    status = subprocess.call(command, env=environment)
    print(f'CI_QUOTA_DEBUGGER exit={status}', flush=True)
    if status:
        return 2
    try:
        process = json.loads(status_path.read_text())
        root = ET.parse(xml_path).getroot()
        executed = [t for t in root.iter('testcase') if t.get('status') == 'run']
        if not process['exited'] or len(executed) != 1:
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
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_./]+', args.case):
        parser.error('one exact test name is required')
    output = os.environ.get('GTEST_OUTPUT', '')
    debugger = shutil.which('lldb')
    if not debugger or not args.binary.is_file() or not output.startswith('xml:') or not output[4:]:
        print('CI_QUOTA_UNAVAILABLE reason=missing_lldb_binary_or_xml_destination', flush=True)
        return 2
    return diagnose(args.binary, args.case, Path(output[4:]).resolve(), debugger)


if __name__ == '__main__':
    raise SystemExit(main())
