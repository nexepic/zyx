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
import tempfile
import xml.etree.ElementTree as ET

LOG_LIMIT = 8 * 1024 * 1024
TAIL_LIMIT = 1024 * 1024
CONSOLE_LIMIT = 1024 * 1024
NUMERIC_CASE_METRICS = frozenset((
    'largest_local_budget_peak_bytes', 'escaped_result_account_bytes',
    'retained_object_lower_bound_bytes', 'retained_result_bytes',
    'restart_key_allocations',
    'growth_success_bound', 'growth_result_capacity',
    'complete_result_storage_requests',
    'sort_run_array_large_requests_short', 'sort_run_array_large_requests_long',
))


class CaseJournal:
    """Bounded identity-only progress, usable even when GoogleTest never closes XML."""
    def __init__(self):
        self.pending = b''
        self.discard_line = False
        self.started = self.completed = self.failed = self.oversized_lines = 0
        self.passed = self.skipped = self.unclassified_completed = 0
        self.last_started = self.last_completed = None
        self.epoch = time.monotonic()
        self.last_started_seconds = self.last_completed_seconds = None
        self.failed_names = []
        self.skipped_names = []

    def feed(self, chunk):
        lines = (self.pending + chunk).split(b'\n')
        self.pending = lines.pop()
        for line in lines:
            if not self.discard_line and len(line) <= 4096:
                match = re.search(rb'CI_CASE_(BEGIN|END) ([A-Za-z0-9_./-]{1,512})(?: failed=([0-9]+))?(?: skipped=([0-9]+))?(?:\r|$)', line)
                if match:
                    kind, name, failed, skipped = match.groups()
                    name = name.decode('ascii')
                    if kind == b'BEGIN':
                        self.started += 1; self.last_started = name
                        self.last_started_seconds = time.monotonic() - self.epoch
                    elif failed is not None:
                        self.completed += 1; self.last_completed = name
                        self.last_completed_seconds = time.monotonic() - self.epoch
                        if int(failed):
                            self.failed += 1
                            if len(self.failed_names) < 256: self.failed_names.append(name)
                        elif skipped is None:
                            self.unclassified_completed += 1
                        elif int(skipped):
                            self.skipped += 1
                            if len(self.skipped_names) < 256: self.skipped_names.append(name)
                        else:
                            self.passed += 1
            elif not self.discard_line:
                self.oversized_lines += 1
            self.discard_line = False
        if len(self.pending) > 4096:
            if not self.discard_line: self.oversized_lines += 1
            self.pending = b''
            self.discard_line = True

    def snapshot(self):
        return dict(started=self.started, completed=self.completed, failed=self.failed,
                    passed=self.passed, skipped=self.skipped, unclassified_completed=self.unclassified_completed,
                    last_started=self.last_started, last_completed=self.last_completed,
                    last_started_observed_seconds=self.last_started_seconds,
                    last_completed_observed_seconds=self.last_completed_seconds,
                    failed_names=self.failed_names, failed_names_truncated=self.failed > len(self.failed_names),
                    skipped_names=self.skipped_names, skipped_names_truncated=self.skipped > len(self.skipped_names),
                    oversized_lines=self.oversized_lines, counts_are_observed_not_discovery=True)


def write_json(path, record):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def failure_observations(failures):
    """Bounded scalar evidence; never echo arbitrary assertion strings/payloads."""
    kinds, numbers = set(), []
    truncated = len(failures) > 4
    for failure in failures[:4]:
        message = failure.get('message') or failure.text or ''
        truncated |= len(message) > 16384
        message = message[:16384]
        lowered = message.lower()
        for kind, markers in (
            ('byte_bound', ('exceeded its byte bound', 'materialized byte bound')),
            ('resource_admission', ('hierarchical resource admission denied',)),
            ('rss_guard', ('query exceeded memory limit',)),
            ('storage_io', ('storageio::',)),
            ('file_lock', ('locked a portion', 'lock violation',)),
            ('unexpected_success', ('actual: it throws nothing',)),
        ):
            if any(marker in lowered for marker in markers): kinds.add(kind)
        for line in message.splitlines():
            # Keep only scalar numeric/boolean assertions, never user strings,
            # container dumps, expression source, addresses or exception text.
            match = re.fullmatch(r'\s*(Which is:|Actual:)\s*(-?\d+(?:\.\d+)?|true|false)(?:\s+vs\s+(-?\d+(?:\.\d+)?|true|false))?\s*', line)
            if match:
                if len(numbers) < 8 and len(match[2]) <= 32 and (not match[3] or len(match[3]) <= 32):
                    numbers.append({'label': match[1][:-1], 'value': match[2], **({'versus': match[3]} if match[3] else {})})
                else: truncated = True
    return {'kinds': sorted(kinds), 'scalars': numbers, 'truncated': truncated}


def run_evidence(command, directory, timeout, *, gtest=False):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault('ZYX_TEST_HEARTBEAT_SECONDS', '30')
    if gtest:
        env['GTEST_OUTPUT'] = 'xml:' + str(directory / 'tests.xml')
        # A killed process cannot finish its XML. Keep the active case and
        # completed identities observable for every supervised native gate.
        env.setdefault('ZYX_CI_CASE_TRACE', '1')
        if 'LLVM_PROFILE_FILE' not in env:
            # Coverage intermediates are neither diagnostic artifacts nor source files.
            profiles = Path(env.get('RUNNER_TEMP') or tempfile.gettempdir()) / 'zyx-ci-diagnostic-profiles'
            profiles.mkdir(parents=True, exist_ok=True)
            env['LLVM_PROFILE_FILE'] = str(profiles / 'code-%p.profraw')
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
    journal = CaseJournal()
    last_journal = 0.0
    journal_write_errors = 0
    def persist_journal(snapshot):
        nonlocal journal_write_errors
        try: write_json(directory / 'case-journal.json', snapshot)
        except OSError:
            # Optional progress evidence must not kill a real test because an
            # antivirus/file lock temporarily denies replacing this file.
            journal_write_errors += 1
    try:
        with (directory / 'output.log').open('wb') as log:
            while True:
                chunk = process.stdout.read1(16384)
                if not chunk:
                    break
                total += len(chunk)
                journal.feed(chunk)
                if time.monotonic() - last_journal >= 5.0:
                    persist_journal(journal.snapshot())
                    last_journal = time.monotonic()
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
        journal.feed(b'\n')
        record['case_journal'] = journal.snapshot()
        persist_journal(record['case_journal'])
        record['case_journal_write_errors'] = journal_write_errors
        if interrupted and code == 0: code = 130
        record.update(state='interrupted' if interrupted else 'completed', completed_at=time.time(),
                      exit_code=code, output_bytes=total, retained_prefix_bytes=kept,
                      retained_tail_bytes=len(tail), output_truncated=total > LOG_LIMIT,
                      console_truncated=total > CONSOLE_LIMIT,
                      xml_present=(directory / 'tests.xml').is_file())
        documents = []
        failed_cases = []
        failed_case_count = 0
        numeric_metrics = []
        numeric_metrics_truncated = False
        for xml in list(directory.rglob('*.xml'))[:128]:
            item = {'path': str(xml.relative_to(directory)), 'bytes': xml.stat().st_size}
            if item['bytes'] > 16*1024*1024:
                item['parse_state'] = 'not_parsed_size_limit'
            else:
                try:
                    tree = ET.parse(xml).getroot()
                    item.update(parse_state='complete', summary=dict(tree.attrib))
                    for case in tree.iter('testcase'):
                        for prop in case.findall('properties/property'):
                            name, value = prop.get('name', ''), prop.get('value', '')
                            if name in NUMERIC_CASE_METRICS and re.fullmatch(r'\d{1,20}', value):
                                if len(numeric_metrics) < 64:
                                    numeric_metrics.append({'class': case.get('classname', '')[:256],
                                        'name': case.get('name', '')[:256], 'metric': name, 'value': value})
                                else: numeric_metrics_truncated = True
                        failures = list(case.findall('failure')) + list(case.findall('error'))
                        if not failures:
                            continue
                        failed_case_count += 1
                        if len(failed_cases) < 256:
                            locations = []
                            for failure in failures[:4]:
                                message = failure.get('message') or failure.text or ''
                                first = message.splitlines()[0] if message else ''
                                location = re.search(r'(?:^|[/\\])([A-Za-z0-9_.-]+\.(?:cpp|hpp|inc)):(\d+):?$', first)
                                if location:
                                    locations.append({'file': location[1][:256], 'line': location[2][:32]})
                            # Keep identities, locations and bounded numeric evidence;
                            # arbitrary assertion strings stay in the artifact.
                            failed_cases.append({'xml': item['path'],
                                'class': case.get('classname', '')[:256],
                                'name': case.get('name', '')[:256],
                                'file': case.get('file', '')[:256],
                                'line': case.get('line', '')[:32],
                                'identity_truncated': any(len(case.get(key, '')) > 256
                                    for key in ('classname', 'name', 'file')),
                                'assertion_locations': locations,
                                'observations': failure_observations(failures),
                                'failures': len(failures)})
                except (ET.ParseError, OSError): item['parse_state'] = 'incomplete_or_unreadable'
            documents.append(item)
        record['xml_documents'] = documents
        record['failed_case_count'] = failed_case_count
        record['failed_cases'] = failed_cases
        record['numeric_case_metrics'] = numeric_metrics
        record['numeric_case_metrics_truncated'] = numeric_metrics_truncated
        for metric in numeric_metrics:
            print('CI_CASE_METRIC ' + json.dumps(metric, ensure_ascii=True), flush=True)
        if numeric_metrics_truncated:
            print('CI_CASE_METRIC_TRUNCATED process_limit=64', flush=True)
        record['failed_cases_truncated'] = failed_case_count > len(failed_cases)
        record['xml_present'] = bool(documents)
        if (directory / 'process.json').is_file():
            record['process'] = json.loads((directory / 'process.json').read_text())
        write_json(directory / 'result.json', record)
        # Print after the bounded console tail so long successful case tracing
        # cannot hide the identities of failures in the middle of a full run.
        for case in failed_cases:
            print('CI_FAILED_CASE ' + json.dumps(case, ensure_ascii=True), flush=True)
        if failed_case_count:
            print(f'CI_FAILED_CASE_SUMMARY total={failed_case_count} emitted={len(failed_cases)} '
                  f'truncated={failed_case_count > len(failed_cases)}', flush=True)
        if journal.started or journal.completed:
            print('CI_CASE_JOURNAL ' + json.dumps(record['case_journal']), flush=True)
        if journal_write_errors:
            print(f'CI_CASE_JOURNAL_WRITE_ERROR count={journal_write_errors}', flush=True)
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
