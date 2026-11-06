import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import pytest


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).parents[1] / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize('child_exit,failure,expected', [(0, False, 0), (1, True, 1), (3, False, 1)])
def test_debugger_success_cannot_hide_a_test_or_post_xml_process_failure(monkeypatch, tmp_path, child_exit, failure, expected):
    module = load('diagnose_macos_quota')
    xml = tmp_path / 'tests.xml'
    def run(command, env):
        assert '--no-lldbinit' in command and '--batch' in command
        assert 'settings set target.disable-aslr false' in command
        assert command[-1] == '--gtest_filter=' + module.CASE
        assert env['GTEST_OUTPUT'] == 'xml:' + str(xml)
        Path(env['ZYX_CI_QUOTA_STATUS_PATH']).write_text(json.dumps({'exited': True, 'exit_code': child_exit}))
        body = '<failure message="budget"/>' if failure else ''
        suite, case = module.CASE.split('.')
        xml.write_text(f'<testsuites><testsuite><testcase classname="{suite}" name="{case}" status="run">{body}</testcase></testsuite></testsuites>')
        return 0
    monkeypatch.setattr(module.subprocess, 'call', run)
    assert module.diagnose(tmp_path / 'suite', module.CASE, xml, 'lldb') == expected


@pytest.mark.parametrize('debugger_exit', [0, 1])
def test_missing_native_evidence_is_not_a_pass(monkeypatch, tmp_path, debugger_exit):
    module = load('diagnose_macos_quota')
    monkeypatch.setattr(module.subprocess, 'call', lambda *args, **kwargs: debugger_exit)
    assert module.diagnose(tmp_path / 'suite', module.CASE, tmp_path / 'tests.xml', 'lldb') == 2


def test_previous_evidence_is_preserved_without_launching(monkeypatch, tmp_path):
    module = load('diagnose_macos_quota')
    xml = tmp_path / 'tests.xml'
    xml.write_text('preserve previous result')
    def unexpected(*args, **kwargs):
        pytest.fail('must not launch over previous evidence')
    monkeypatch.setattr(module.subprocess, 'call', unexpected)
    assert module.diagnose(tmp_path / 'suite', module.CASE, xml, 'lldb') == 2
    assert xml.read_text() == 'preserve previous result'


def trace_module(monkeypatch):
    monkeypatch.setitem(sys.modules, 'lldb', SimpleNamespace(eStateExited=10))
    return load('lldb_quota_trace')


def test_only_one_quota_breakpoint_is_armed_and_it_auto_continues(monkeypatch):
    module = trace_module(monkeypatch)
    settings = {}
    breakpoint = SimpleNamespace(GetNumLocations=lambda: 1,
        SetOneShot=lambda value: settings.update(one_shot=value),
        SetAutoContinue=lambda value: settings.update(auto_continue=value),
        SetScriptCallbackFunction=lambda value: settings.update(callback=value))
    def create(pattern):
        assert pattern == 'MaterializedByteBudget::diagnosticRejection'
        return breakpoint
    debugger = SimpleNamespace(GetSelectedTarget=lambda: SimpleNamespace(BreakpointCreateByRegex=create))
    module.__lldb_init_module(debugger, {})
    assert settings == {'one_shot': True, 'auto_continue': True, 'callback': 'lldb_quota_trace.on_quota'}


def test_missing_quota_symbol_is_explicit(monkeypatch):
    module = trace_module(monkeypatch)
    debugger = SimpleNamespace(GetSelectedTarget=lambda: SimpleNamespace(
        BreakpointCreateByRegex=lambda pattern: SimpleNamespace(GetNumLocations=lambda: 0)))
    with pytest.raises(RuntimeError, match='symbol unavailable'):
        module.__lldb_init_module(debugger, {})


def test_stack_output_is_bounded_and_reads_no_target_variables(monkeypatch, capsys):
    module = trace_module(monkeypatch)
    calls = []
    file = SimpleNamespace(GetFilename=lambda: 'source.cpp')
    frame = SimpleNamespace(IsValid=lambda: True, GetFunctionName=lambda: 'f' * 1000,
        GetLineEntry=lambda: SimpleNamespace(GetFileSpec=lambda: file, GetLine=lambda: 42),
        GetModule=lambda: SimpleNamespace(GetFileSpec=lambda: file))
    def at(index):
        calls.append(index)
        return frame
    root = SimpleNamespace(GetThread=lambda: SimpleNamespace(GetFrameAtIndex=at))
    assert module.on_quota(root, None, {}) is False
    records = [json.loads(line.split(' ', 1)[1]) for line in capsys.readouterr().out.splitlines()
               if line.startswith('CI_QUOTA_FRAME ')]
    assert calls == list(range(32))
    assert len(records) == 32
    assert all(len(row['function']) == 512 and row['symbol_truncated'] for row in records)


def test_unfinished_owned_inferior_is_killed_and_not_reported_as_exited(monkeypatch, tmp_path):
    module = trace_module(monkeypatch)
    killed = []
    process = SimpleNamespace(IsValid=lambda: True, GetState=lambda: 1,
        Kill=lambda: (killed.append(True) or SimpleNamespace(Success=lambda: True)))
    debugger = SimpleNamespace(GetSelectedTarget=lambda: SimpleNamespace(GetProcess=lambda: process))
    path = tmp_path / 'quota-debugger.json'
    monkeypatch.setenv('ZYX_CI_QUOTA_STATUS_PATH', str(path))
    module.report_exit(debugger)
    result = json.loads(path.read_text())
    assert killed == [True]
    assert result == {'exited': False, 'exit_code': None, 'quota_stop': False, 'cleanup_succeeded': True}
