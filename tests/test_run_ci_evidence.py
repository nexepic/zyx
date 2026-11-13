"""Exercise real evidence, failure, timeout and interrupted-process boundaries."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / 'scripts/run_ci_evidence.py'


def invoke(tmp_path, program, timeout='5', extra_env=None):
    env = os.environ.copy();env.pop('ZYX_CI_DEADLINE_UNIX', None)
    if extra_env: env.update(extra_env)
    p = subprocess.run([sys.executable, str(WRAPPER), '--root', str(tmp_path), '--name', 'case',
                        '--timeout', timeout, '--gtest', '--', sys.executable, '-c', program],
                       capture_output=True, env=env, timeout=20)
    return p, json.loads((tmp_path / 'case/result.json').read_text())


def test_success_retains_xml_sha_and_output(tmp_path):
    p, r = invoke(tmp_path, "import os;from pathlib import Path;Path(os.environ['GTEST_OUTPUT'][4:]).write_text('<testsuites/>');print('success-marker')")
    assert p.returncode == r['exit_code'] == 0
    assert r['xml_present'] and len(r['checkout_sha']) == 40
    assert 'success-marker' in (tmp_path / 'case/output.log').read_text()


def test_failure_status_is_not_hidden(tmp_path):
    p, r = invoke(tmp_path, "import sys;print('before-failure',flush=True);sys.exit(7)")
    assert p.returncode == r['exit_code'] == r['process']['raw_exit_code'] == 7
    assert not r['xml_present']


def test_timeout_preserves_partial_output_and_status(tmp_path):
    p, r = invoke(tmp_path, "import time;print('before-timeout',flush=True);time.sleep(60)", '3' if os.name == 'nt' else '.5')
    assert p.returncode == r['exit_code'] == 124
    assert 'before-timeout' in (tmp_path / 'case/output.log').read_text()


def test_timeout_journals_case_identity_without_complete_xml(tmp_path, monkeypatch):
    monkeypatch.delenv('ZYX_CI_CASE_TRACE', raising=False)
    program = ("import os,time;assert os.getenv('ZYX_CI_CASE_TRACE') == '1';print('CI_CASE_BEGIN Suite.Failed',flush=True);"
               "print('CI_CASE_END Suite.Failed failed=1',flush=True);"
               "print('CI_CASE_BEGIN Suite.Unfinished',flush=True);time.sleep(60)")
    p, r = invoke(tmp_path, program, '3' if os.name == 'nt' else '.5')
    assert p.returncode == 124
    journal = r['case_journal']
    assert journal['started'] == 2 and journal['completed'] == journal['failed'] == 1
    assert journal['last_started'] == 'Suite.Unfinished'
    assert journal['last_completed'] == 'Suite.Failed'
    assert journal['failed_names'] == ['Suite.Failed']
    assert json.loads((tmp_path / 'case/case-journal.json').read_text()) == journal
    assert b'CI_CASE_JOURNAL ' in p.stdout


def test_journal_survives_console_cap_and_bounds_failure_names(tmp_path):
    program = ("import sys;sys.stdout.write('x'*(2*1024*1024)+'\\n');"
               "[print(f'CI_CASE_BEGIN Suite.C{i}\\nCI_CASE_END Suite.C{i} failed=1') for i in range(260)];"
               "print('private-assertion-payload');sys.exit(1)")
    p, r = invoke(tmp_path, program)
    journal = r['case_journal']
    assert p.returncode == 1 and r['console_truncated']
    assert journal['started'] == journal['completed'] == journal['failed'] == 260
    assert len(journal['failed_names']) == 256 and journal['failed_names_truncated']
    assert journal['oversized_lines'] == 1
    assert 'private-assertion-payload' not in json.dumps(journal)
    assert b'CI_CASE_JOURNAL ' in p.stdout


def test_journal_handles_split_trace_lines_without_interpreting_payloads():
    import importlib.util
    spec = importlib.util.spec_from_file_location('ci_evidence', WRAPPER)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    journal = module.CaseJournal()
    for chunk in (b'CI_CA', b'SE_BEGIN Suite.Case\r\nCI_CASE_END Suite.', b'Case failed=0\r\n'):
        journal.feed(chunk)
    assert journal.snapshot()['started'] == journal.snapshot()['completed'] == 1
    assert not journal.snapshot()['failed_names']


def test_optional_journal_write_failure_preserves_test_exit_and_console_evidence(tmp_path):
    program = f'''import importlib.util,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location("evidence", {str(WRAPPER)!r})
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
original=m.write_json
def write(path,record):
    if path.name=="case-journal.json":raise PermissionError("private-error-detail")
    original(path,record)
m.write_json=write
sys.exit(m.run_evidence([sys.executable,"-c","print('CI_CASE_BEGIN Suite.Case');print('CI_CASE_END Suite.Case failed=1');raise SystemExit(7)"],Path({str(tmp_path / 'case')!r}),10))
'''
    env=os.environ.copy();env.pop('ZYX_CI_DEADLINE_UNIX',None)
    p=subprocess.run([sys.executable,'-c',program],capture_output=True,env=env,cwd=ROOT,timeout=20)
    r=json.loads((tmp_path/'case/result.json').read_text())
    assert p.returncode==r['exit_code']==7
    assert r['case_journal_write_errors']>0 and r['case_journal']['failed']==1
    assert b'CI_CASE_JOURNAL ' in p.stdout and b'CI_CASE_JOURNAL_WRITE_ERROR' in p.stdout
    assert b'private-error-detail' not in p.stdout+p.stderr


def test_shared_deadline_fails_without_starting_command(tmp_path):
    p, r = invoke(tmp_path, "raise AssertionError('must not execute')", extra_env={'ZYX_CI_DEADLINE_UNIX': '1'})
    assert p.returncode == r['exit_code'] == 124
    assert r['state'] == 'evidence_deadline_exhausted'
    assert not (tmp_path / 'case/output.log').exists()


def test_output_is_bounded_and_keeps_tail(tmp_path):
    p, r = invoke(tmp_path, "import sys;sys.stdout.write('a'*(10*1024*1024)+'last-marker')")
    assert p.returncode == 0 and r['output_truncated'] and r['console_truncated']
    assert (tmp_path / 'case/output.log').stat().st_size == 8*1024*1024
    assert (tmp_path / 'case/output-tail.log').stat().st_size <= 1024*1024
    assert (tmp_path / 'case/output-tail.log').read_text().endswith('last-marker')


def test_failed_xml_identities_survive_console_cap_without_assertion_payload(tmp_path):
    xml = '<testsuites><testsuite><testcase classname="Suite" name="Case" file="test.cpp" line="42"><failure message="D:\\work\\test.cpp:99&#10;private-assertion-payload"/></testcase></testsuite></testsuites>'
    program = ("import os,sys;from pathlib import Path;sys.stdout.write('a'*(2*1024*1024));"
               f"Path(os.environ['GTEST_OUTPUT'][4:]).write_text({xml!r});sys.exit(1)")
    p, r = invoke(tmp_path, program)
    assert p.returncode == 1 and r['console_truncated']
    assert r['failed_case_count'] == 1 and not r['failed_cases_truncated']
    assert r['failed_cases'][0]['name'] == 'Case'
    assert r['failed_cases'][0]['assertion_locations'] == [{'file': 'test.cpp', 'line': '99'}]
    assert b'CI_FAILED_CASE ' in p.stdout and b'"line": "42"' in p.stdout
    assert b'private-assertion-payload' not in p.stdout


def test_failed_xml_summary_is_bounded_and_explicit(tmp_path):
    xml = '<testsuites><testsuite>' + ''.join(
        f'<testcase classname="Suite" name="Case{i}"><failure/></testcase>' for i in range(260)) + '</testsuite></testsuites>'
    p, r = invoke(tmp_path, "import os;from pathlib import Path;"
                  f"Path(os.environ['GTEST_OUTPUT'][4:]).write_text({xml!r})")
    assert p.returncode == 0  # Reporting never changes the command's exit status.
    assert r['failed_case_count'] == 260 and len(r['failed_cases']) == 256
    assert r['failed_cases_truncated']
    assert b'CI_FAILED_CASE_SUMMARY total=260 emitted=256 truncated=True' in p.stdout


def test_failed_xml_exports_only_scalar_observations_and_fixed_error_categories(tmp_path):
    import xml.etree.ElementTree as ET
    root = ET.Element('testsuites'); suite = ET.SubElement(root, 'testsuite')
    case = ET.SubElement(suite, 'testcase', classname='Suite', name='Case')
    ET.SubElement(case, 'failure', message=(
        'test.cpp:12\nWhich is: 16\nActual: 2 vs 1\n'
        'Actual: it throws nothing.\nshared materialization exceeded its byte bound\n'
        'Which is: private-secret-payload\nWhich is: 0x12345\nWhich is: { 1, 2 }'))
    xml = ET.tostring(root, encoding='unicode')
    program = ("import os,sys;from pathlib import Path;sys.stdout.write('a'*(2*1024*1024));"
               f"Path(os.environ['GTEST_OUTPUT'][4:]).write_text({xml!r});sys.exit(7)")
    p, r = invoke(tmp_path, program)
    assert p.returncode == r['exit_code'] == 7
    observation = r['failed_cases'][0]['observations']
    assert observation == {'kinds': ['byte_bound', 'unexpected_success'], 'scalars': [
        {'label': 'Which is', 'value': '16'}, {'label': 'Actual', 'value': '2', 'versus': '1'}], 'truncated': False}
    assert b'private-secret-payload' not in p.stdout and b'0x12345' not in p.stdout


def test_failure_observation_scan_and_scalar_count_are_bounded():
    import importlib.util
    import xml.etree.ElementTree as ET
    spec = importlib.util.spec_from_file_location('ci_evidence', WRAPPER)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    failures = [ET.Element('failure', message=('Which is: 1\n' * 20) + 'x' * 20000)] * 6
    observed = module.failure_observations(failures)
    assert len(observed['scalars']) == 8 and observed['truncated']


def test_successful_numeric_metrics_survive_console_cap_without_payloads(tmp_path):
    xml = ('<testsuites><testsuite><testcase classname="Suite" name="Case"><properties>'
           '<property name="escaped_result_account_bytes" value="251464"/>'
           '<property name="growth_success_bound" value="8192"/>'
           '<property name="growth_result_capacity" value="33"/>'
           '<property name="retained_result_bytes" value="private-secret"/>'
           '<property name="private-metric" value="123"/>'
           '</properties></testcase></testsuite></testsuites>')
    program = ("import os,sys;from pathlib import Path;sys.stdout.write('a'*(2*1024*1024));"
               f"Path(os.environ['GTEST_OUTPUT'][4:]).write_text({xml!r})")
    p, r = invoke(tmp_path, program)
    assert p.returncode == 0 and r['console_truncated']
    assert r['numeric_case_metrics'] == [{'class': 'Suite', 'name': 'Case',
        'metric': 'escaped_result_account_bytes', 'value': '251464'},
        {'class': 'Suite', 'name': 'Case', 'metric': 'growth_success_bound', 'value': '8192'},
        {'class': 'Suite', 'name': 'Case', 'metric': 'growth_result_capacity', 'value': '33'}]
    assert b'CI_CASE_METRIC ' in p.stdout and b'251464' in p.stdout
    assert b'private-secret' not in p.stdout and b'private-metric' not in p.stdout


def test_numeric_metric_count_is_bounded_and_reports_truncation(tmp_path):
    xml = '<testsuites><testsuite>' + ''.join(
        f'<testcase classname="Suite" name="Case{i}"><properties><property name="retained_result_bytes" value="{i}"/></properties></testcase>'
        for i in range(80)) + '</testsuite></testsuites>'
    p, r = invoke(tmp_path, "import os;from pathlib import Path;"
                  f"Path(os.environ['GTEST_OUTPUT'][4:]).write_text({xml!r})")
    assert p.returncode == 0 and len(r['numeric_case_metrics']) == 64
    assert r['numeric_case_metrics_truncated']
    assert b'CI_CASE_METRIC_TRUNCATED process_limit=64' in p.stdout


@pytest.mark.skipif(os.name == 'nt', reason='POSIX signal identity')
def test_crash_keeps_raw_signal(tmp_path):
    p, r = invoke(tmp_path, "import os,signal;os.kill(os.getpid(),signal.SIGTERM)")
    assert p.returncode != 0 and r['process']['raw_exit_code'] == -signal.SIGTERM
    assert r['process']['posix_signal'] == signal.SIGTERM


@pytest.mark.skipif(os.name == 'nt', reason='POSIX supervisor interruption')
def test_interrupt_keeps_partial_evidence(tmp_path):
    env=os.environ.copy();env.pop('ZYX_CI_DEADLINE_UNIX',None)
    p=subprocess.Popen([sys.executable,str(WRAPPER),'--root',str(tmp_path),'--name','case','--timeout','60','--',
                        sys.executable,'-c',"import time;print('ready',flush=True);time.sleep(60)"],
                       stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT,env=env)
    try:
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            log=tmp_path/'case/output.log'
            if log.exists() and 'ready' in log.read_text():break
            time.sleep(.02)
        else:pytest.fail('child never reached readiness')
        p.send_signal(signal.SIGTERM);p.wait(timeout=15)
        r=json.loads((tmp_path/'case/result.json').read_text())
        assert p.returncode == 130 and r['state']=='interrupted'
        assert 'ready' in log.read_text()
    finally:
        if p.poll() is None:p.kill();p.wait()

@pytest.mark.skipif(os.name == 'nt', reason='two-shard workspace isolation is POSIX-only')
@pytest.mark.parametrize('hang', [False, True])
def test_tier_evidence_survives_failure_or_timeout(tmp_path, hang):
    fake=tmp_path/'fake-gtest'
    fake.write_text('#!'+sys.executable+'\n'+
        "import os,time\nfrom pathlib import Path\nprint('tier-partial-marker',flush=True)\n"+
        ("time.sleep(60)\n" if hang else
         "Path(os.environ['GTEST_OUTPUT'][4:]).write_text('<testsuites tests=\"1\" failures=\"0\"/>')\nraise SystemExit(9 if os.environ['GTEST_SHARD_INDEX']=='1' else 0)\n"))
    fake.chmod(0o700)
    directory=tmp_path/'evidence/unit'
    command=[sys.executable,str(ROOT/'scripts/run_test_tier.py'),'unit','--binary',str(fake),
             '--jobs','2','--timeout','2' if hang else '10','--xml-dir',str(directory/'xml'),'--failure-log-dir',str(directory/'shards')]
    env=os.environ.copy();env.pop('ZYX_CI_DEADLINE_UNIX',None)
    env['ZYX_CI_EVIDENCE_DIR']=str(tmp_path/'evidence')
    result=subprocess.run([sys.executable,str(WRAPPER),'--root',str(tmp_path/'evidence'),
                           '--name','unit','--timeout','30','--',*command],
                          capture_output=True,env=env,cwd=ROOT,timeout=40)
    record=json.loads((directory/'result.json').read_text())
    assert result.returncode==record['exit_code']==(124 if hang else 1)
    statuses=json.loads((directory/'shards/status.json').read_text())
    assert statuses['exit_code']==result.returncode
    assert all('tier-partial-marker' in (directory/f'shards/shard-{n}.log').read_text() for n in (0,1))
    assert (directory/'shards/bounded/shard-0/output.log').exists()
    if hang:
        assert not record['xml_present']
    else:
        assert len(record['xml_documents'])==2
        assert all(x['parse_state']=='complete' for x in record['xml_documents'])
        assert any(x['raw_exit_code']==9 for x in statuses['shards'])


def test_diagnostic_profile_output_is_outside_source_and_artifacts(tmp_path):
    program="import os;from pathlib import Path;p=Path(os.environ['LLVM_PROFILE_FILE'].replace('%p',str(os.getpid())));p.write_bytes(b'profile')"
    env={'RUNNER_TEMP':str(tmp_path/'runner-temp')}
    # The child inherits no caller profile setting in this dedicated process.
    original=os.environ.pop('LLVM_PROFILE_FILE',None)
    try:p,r=invoke(tmp_path,program,extra_env=env)
    finally:
        if original is not None:os.environ['LLVM_PROFILE_FILE']=original
    assert p.returncode==0
    assert list((tmp_path/'runner-temp/zyx-ci-diagnostic-profiles').glob('*.profraw'))
    assert not list((tmp_path/'case').rglob('*.profraw'))
