"""Run the real Linux dependency workflow body with an isolated fake sudo."""
import json
import os
from pathlib import Path
import subprocess
import sys
import textwrap

import pytest

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(os.name == 'nt', reason='Linux dependency shell contract')


def invoke(tmp_path, fail_update):
    text = (ROOT / '.github/workflows/build.yml').read_text()
    block = text.split('      - name: Install System Deps (Linux)\n', 1)[1]
    block = block.split('      - name:', 1)[0].split('        run: |\n', 1)[1]
    script = textwrap.dedent(block)
    directory = tmp_path / 'bin'; directory.mkdir()
    sudo = directory / 'sudo'
    sudo.write_text(f'#!{sys.executable}\n' + '''import json,os,sys
from pathlib import Path
args=sys.argv[1:]
with Path(os.environ['APT_TEST_LOG']).open('a') as f:f.write(json.dumps(args)+'\\n')
if args[0]=='apt-get' and 'update' in args and os.environ['APT_TEST_FAIL']=='1':
    # APT normally warns and succeeds for transient update errors unless strict.
    sys.exit(100 if 'APT::Update::Error-Mode=any' in args else 0)
sys.exit(0)
''')
    sudo.chmod(0o755)
    env = os.environ.copy(); env.pop('ZYX_CI_DEADLINE_UNIX', None)
    env.update(PATH=str(directory) + os.pathsep + env['PATH'],
               APT_TEST_LOG=str(tmp_path / 'commands.jsonl'), APT_TEST_FAIL=str(int(fail_update)),
               ZYX_CI_EVIDENCE_DIR=str(tmp_path / 'evidence'))
    result = subprocess.run(['bash', '-e', '-o', 'pipefail', '-c', script], cwd=ROOT,
                            env=env, capture_output=True, timeout=30)
    commands = [json.loads(line) for line in (tmp_path / 'commands.jsonl').read_text().splitlines()]
    return result, commands


def test_update_dns_failure_stops_before_install_and_preserves_failure(tmp_path):
    result, commands = invoke(tmp_path, True)
    assert result.returncode == 100
    assert len(commands) == 2 and commands[-1][-1] == 'update'
    assert '--no-update' in commands[0]
    evidence = json.loads((tmp_path / 'evidence/apt-update/result.json').read_text())
    assert evidence['exit_code'] == 100
    assert not (tmp_path / 'evidence/apt-install').exists()


def test_success_keeps_required_packages_and_passes_bounded_retry_configuration(tmp_path):
    result, commands = invoke(tmp_path, False)
    assert result.returncode == 0 and len(commands) == 3
    assert all('Acquire::Retries=3' in command for command in commands[1:])
    assert commands[-1][-4:] == ['clang-20', 'llvm-20', 'libc++-20-dev', 'libc++abi-20-dev']
    assert json.loads((tmp_path / 'evidence/apt-install/result.json').read_text())['exit_code'] == 0
