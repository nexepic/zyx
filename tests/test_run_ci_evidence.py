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
