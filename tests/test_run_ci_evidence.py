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
