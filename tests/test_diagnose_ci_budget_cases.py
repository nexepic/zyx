import importlib.util
from pathlib import Path
import pytest


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).parents[1] / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize('platform', ['windows', 'macos'])
def test_conversion_capture_reuses_existing_budget_replay_and_deadline(platform):
    module = load('diagnose_ci_budget_cases')
    command = module.case_command(Path('suite'), module.CONVERSION_CASE, platform, True)
    assert command.count(module.CONVERSION_CASE) == 1
    assert command[command.index('--timeout') + 1] == '1200'
    assert command[command.index('--name') + 1] == 'budget-3257bf1e1a62b30e'
    target = command[command.index('--') + 1:]
    assert Path(target[1]).name == 'diagnose_macos_quota.py'
    assert '--require-stack' not in target  # Zero real rejections remains valid.
    assert module.CONVERSION_CASE in (module.WINDOWS if platform == 'windows' else module.MACOS)


def test_local_budget_replay_without_evidence_keeps_direct_process_contract():
    module = load('diagnose_ci_budget_cases')
    command = module.case_command(Path('suite'), module.CONVERSION_CASE, 'macos', False)
    assert Path(command[1]).name == 'run_test_process.py'
    assert command[command.index('--timeout') + 1] == '1200'
    assert command[-2:] == ['suite', '--gtest_filter=' + module.CONVERSION_CASE]


@pytest.mark.parametrize('platform', ['windows', 'windows-current', 'macos', 'asan'])
def test_other_cases_keep_the_original_direct_execution(platform):
    module = load('diagnose_ci_budget_cases')
    command = module.case_command(Path('suite'), 'Suite.Case', platform, True)
    assert command[-2:] == ['suite', '--gtest_filter=Suite.Case']
    assert command[command.index('--timeout') + 1] == '300'
    assert ('current-' in command[command.index('--name') + 1]) == (platform == 'windows-current')
