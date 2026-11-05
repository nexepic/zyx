#!/usr/bin/env python3
"""Preserve bounded CI command output, status and optional GoogleTest XML.

Only workflow-supplied test/build commands belong here; never pass credentials.
The existing process runner remains responsible for timeout/tree cleanup.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

LOG_LIMIT = 8 * 1024 * 1024
TAIL_LIMIT = 1024 * 1024
CONSOLE_LIMIT = 1024 * 1024


def write_json(path, record):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def run_evidence(command, directory, timeout, *, gtest=False):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault('ZYX_TEST_HEARTBEAT_SECONDS', '30')
    if gtest:
        env['GTEST_OUTPUT'] = 'xml:' + str(directory / 'tests.xml')
    actual_sha = subprocess.run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True, check=False)
    record = {'command': list(command), 'checkout_sha': actual_sha.stdout.strip(),
              'event_sha': env.get('GITHUB_SHA'), 'started_at': time.time(), 'state': 'running',
              'timeout_seconds': timeout, 'exit_code': None,
              'diagnostic_options': {k: env[k] for k in ('ASAN_OPTIONS','UBSAN_OPTIONS','TSAN_OPTIONS',
                  'ZYX_CI_BUDGET_TRACE','ZYX_CI_RSS_TRACE','ZYX_CI_DEADLINE_UNIX') if k in env}}
    write_json(directory / 'result.json', record)
    deadline = float(env.get('ZYX_CI_DEADLINE_UNIX', 'inf'))
    remaining = deadline - time.time()
    effective_timeout = min(timeout, remaining)
    record['effective_timeout_seconds'] = max(0, effective_timeout)
    if effective_timeout <= 0:
        record.update(state='evidence_deadline_exhausted', exit_code=124, completed_at=time.time())
        write_json(directory / 'result.json', record)
        return 124
    runner = Path(__file__).with_name('run_test_process.py')
    try:
        process = subprocess.Popen([sys.executable, str(runner), '--timeout', str(effective_timeout), '--result-file', str(directory / 'process.json'), '--', *command],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as error:
        record.update(state='launch_failed', exit_code=127, error_type=type(error).__name__, completed_at=time.time())
        write_json(directory / 'result.json', record)
        return 127
    interrupted = False
    def forward(signum, _frame):
        nonlocal interrupted
        interrupted = True
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
    handlers = {s: signal.signal(s, forward) for s in (signal.SIGINT, signal.SIGTERM)}
    kept = total = console = 0
    tail = bytearray()
    console_tail = bytearray()
    last_tail = 0.0
    try:
        with (directory / 'output.log').open('wb') as log:
            while True:
                chunk = process.stdout.read1(16384)
                if not chunk:
                    break
                total += len(chunk)
                console_tail.extend(chunk)
                if len(console_tail)>65536: del console_tail[:-65536]
                first = chunk[:max(0, LOG_LIMIT - kept)]
                log.write(first); log.flush(); kept += len(first)
                if len(first) != len(chunk):
                    tail.extend(chunk[len(first):])
                    if len(tail) > TAIL_LIMIT:
                        del tail[:-TAIL_LIMIT]
                    if time.monotonic() - last_tail > 1:
                        (directory / 'output-tail.log').write_bytes(tail)
                        last_tail = time.monotonic()
                visible = chunk[:max(0, CONSOLE_LIMIT - console)]
                if visible:
                    sys.stdout.buffer.write(visible); sys.stdout.buffer.flush(); console += len(visible)
                if console == CONSOLE_LIMIT:
                    print('\nCI_EVIDENCE console capped; inspect output artifact.', flush=True)
                    console += 1
            code = process.wait()
    finally:
        for s, handler in handlers.items(): signal.signal(s, handler)
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        if tail: (directory / 'output-tail.log').write_bytes(tail)
        if total>CONSOLE_LIMIT:
            sys.stdout.buffer.write(b'\nCI_EVIDENCE retained console tail:\n'+console_tail)
            sys.stdout.buffer.flush()
        code = process.returncode
        if interrupted and code == 0: code = 130
        record.update(state='interrupted' if interrupted else 'completed', completed_at=time.time(),
                      exit_code=code, output_bytes=total, retained_prefix_bytes=kept,
                      retained_tail_bytes=len(tail), output_truncated=total > LOG_LIMIT,
                      console_truncated=total > CONSOLE_LIMIT,
                      xml_present=(directory / 'tests.xml').is_file())
        documents = []
        for xml in list(directory.rglob('*.xml'))[:128]:
            item = {'path': str(xml.relative_to(directory)), 'bytes': xml.stat().st_size}
            if item['bytes'] > 16*1024*1024:
                item['parse_state'] = 'not_parsed_size_limit'
            else:
                try:
                    tree = ET.parse(xml).getroot()
                    item.update(parse_state='complete', summary=dict(tree.attrib))
                except (ET.ParseError, OSError): item['parse_state'] = 'incomplete_or_unreadable'
            documents.append(item)
        record['xml_documents'] = documents
        record['xml_present'] = bool(documents)
        if (directory / 'process.json').is_file():
            record['process'] = json.loads((directory / 'process.json').read_text())
        write_json(directory / 'result.json', record)
        print(f'CI_EVIDENCE directory={directory.name} exit={code} output_bytes={total} truncated={total > LOG_LIMIT}', flush=True)
    return code if code >= 0 else 128 - code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=os.environ.get('ZYX_CI_EVIDENCE_DIR'))
    parser.add_argument('--name', required=True)
    parser.add_argument('--timeout', type=float, required=True)
    parser.add_argument('--gtest', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.root or not re.fullmatch(r'[A-Za-z0-9_.-]+', args.name) or args.timeout <= 0:
        parser.error('a root, safe unique name and positive timeout are required')
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command: parser.error('command is required')
    return run_evidence(command, args.root / args.name, args.timeout, gtest=args.gtest)


if __name__ == '__main__':
    sys.exit(main())
