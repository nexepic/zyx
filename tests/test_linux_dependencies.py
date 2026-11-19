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


def invoke(tmp_path, fail_update, workflow='build', occurrence=1):
    text = (ROOT / f'.github/workflows/{workflow}.yml').read_text()
    name = 'Install System Deps (Linux)' if workflow == 'build' else 'Install system deps (Linux)'
    block = text.split(f'      - name: {name}\n')[occurrence]
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


@pytest.mark.parametrize('workflow,occurrence', [
    ('nodejs-bindings', 1), ('nodejs-bindings', 2), ('rust-bindings', 1),
])
@pytest.mark.parametrize('fail_update', [False, True])
def test_bindings_native_universe_install_contract(tmp_path, workflow, occurrence, fail_update):
    result, commands = invoke(tmp_path, fail_update, workflow, occurrence)
    assert result.returncode == (100 if fail_update else 0)
    assert commands[0] == ['add-apt-repository', '--no-update', '-y', 'universe']
    assert commands[1] == ['apt-get', '-o', 'Acquire::Retries=3', '-o',
                           'APT::Update::Error-Mode=any', 'update']
    if fail_update:
        assert len(commands) == 2
    else:
        assert commands[2] == ['apt-get', '-o', 'Acquire::Retries=3', 'install', '-y',
                               'clang-20', 'llvm-20', 'libc++-20-dev', 'libc++abi-20-dev']
        assert len(commands) == 3
