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


def test_raw_stack_is_flushed_before_a_failing_pdb_symbol_lookup(monkeypatch, capsys):
    module = trace_module(monkeypatch)
    monkeypatch.setenv('ZYX_CI_QUOTA_SYMBOL_ONLY', '1')
    def broken_symbol():
        records = [json.loads(line.split(' ', 1)[1]) for line in capsys.readouterr().out.splitlines()
                   if line.startswith('CI_QUOTA_PC ')]
        assert records == [{'index': i, 'pc': 4096 + i, 'file_address': i, 'module': 'suite.exe'} for i in range(32)]
        raise RuntimeError('PDB symbol lookup failed')
    def frame_at(i):
        return SimpleNamespace(IsValid=lambda: True, GetPC=lambda: 4096 + i,
                               GetSymbol=broken_symbol,
                               GetPCAddress=lambda: SimpleNamespace(GetFileAddress=lambda: i),
                               GetModule=lambda: SimpleNamespace(GetFileSpec=lambda: SimpleNamespace(GetFilename=lambda: 'suite.exe')))
    root = SimpleNamespace(GetThread=lambda: SimpleNamespace(GetFrameAtIndex=frame_at))
    with pytest.raises(RuntimeError, match='PDB symbol lookup failed'):
        module.on_quota(root, None, {})


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


def test_windows_symbol_mode_never_completes_cpp_types_and_caps_rejections(monkeypatch, capsys):
    module = trace_module(monkeypatch)
    monkeypatch.setenv('ZYX_CI_QUOTA_SYMBOL_ONLY', '1')
    module.stack_limit = 3
    file = SimpleNamespace(GetFilename=lambda: 'suite.exe')
    # Deliberately no GetFunctionName/GetLineEntry/GetVariables methods.
    frame = SimpleNamespace(IsValid=lambda: True, GetPC=lambda: 1234,
        GetPCAddress=lambda: SimpleNamespace(GetFileAddress=lambda: 234),
        GetSymbol=lambda: SimpleNamespace(GetName=lambda: 'public_symbol'),
        GetModule=lambda: SimpleNamespace(GetFileSpec=lambda: file))
    root = SimpleNamespace(GetThread=lambda: SimpleNamespace(GetFrameAtIndex=lambda i: frame))
    disabled = []
    location = SimpleNamespace(GetBreakpoint=lambda: SimpleNamespace(SetEnabled=disabled.append))
    for _ in range(5):
        assert module.on_quota(root, location, {}) is False
    records = [json.loads(line.split(' ', 1)[1]) for line in capsys.readouterr().out.splitlines()
               if line.startswith('CI_QUOTA_FRAME ')]
    assert len(records) == 3 * 32
    assert disabled == [False]
    assert all(r['symbol_only'] and r['function'] == 'public_symbol' and r['pc'] == 1234 for r in records)

@pytest.mark.parametrize('attempts,captured,exit_code,expected', [
    (0, 0, 0, 0), (8, 3, 0, 0), (1, 0, 0, 2), (0, 0, 1, 1)])
def test_native_capture_requires_complete_evidence(monkeypatch, tmp_path, attempts, captured, exit_code, expected):
    module = load('diagnose_macos_quota')
    xml = tmp_path / 'test.xml'
    def run(command, env):
        assert command == [str((tmp_path / 'suite').resolve()), '--gtest_filter=' + module.CASE]
        assert env['ZYX_CI_QUOTA_STACK_LIMIT'] == '3'
        suite, name = module.CASE.split('.')
        xml.write_text(f'<testsuites><testsuite><testcase classname="{suite}" name="{name}" status="run"/></testsuite></testsuites>')
        Path(env['ZYX_CI_QUOTA_STATUS_PATH']).write_text(json.dumps(dict(
            exited=True, exit_code=exit_code, native=True, limit=3, attempts=attempts, captured=captured)))
        return exit_code
    monkeypatch.setattr(module.subprocess, 'call', run)
    assert module.diagnose(tmp_path / 'suite', module.CASE, xml, max_stacks=3) == expected

@pytest.mark.parametrize('changes', [
    {'native': False}, {'limit': 1}, {'exit_code': 1}, {'exited': False},
    {'attempts': -1}, {'attempts': True}, {'captured': 4}, {'exit_code': False}])
def test_native_invalid_completion_is_not_success(monkeypatch, tmp_path, changes):
    module = load('diagnose_macos_quota')
    xml = tmp_path / 'test.xml'
    def run(command, env):
        assert env['ZYX_CI_QUOTA_NATIVE'] == '1'
        suite, name = module.CASE.split('.')
        xml.write_text(f'<testsuites><testsuite><testcase classname="{suite}" name="{name}" status="run"/></testsuite></testsuites>')
        status = dict(exited=True, exit_code=0, native=True, limit=3, attempts=0, captured=0)
        status.update(changes)
        Path(env['ZYX_CI_QUOTA_STATUS_PATH']).write_text(json.dumps(status))
        return 0
    monkeypatch.setattr(module.subprocess, 'call', run)
    assert module.diagnose(tmp_path / 'suite', module.CASE, xml, max_stacks=3) == 2

@pytest.mark.parametrize('captured,expected', [(0, 2), (1, 0)])
def test_required_native_probe_cannot_pass_without_a_real_stack(monkeypatch, tmp_path, captured, expected):
    module = load('diagnose_macos_quota')
    xml = tmp_path / 'test.xml'
    def run(command, env):
        suite, name = module.CASE.split('.')
        xml.write_text(f'<testsuites><testsuite><testcase classname="{suite}" name="{name}" status="run"/></testsuite></testsuites>')
        Path(env['ZYX_CI_QUOTA_STATUS_PATH']).write_text(json.dumps(dict(
            exited=True, exit_code=0, native=True, limit=1, attempts=captured, captured=captured)))
        return 0
    monkeypatch.setattr(module.subprocess, 'call', run)
    assert module.diagnose(tmp_path / 'suite', module.CASE, xml, require_stack=True) == expected


def test_required_probe_rejects_legacy_debugger_without_launch(monkeypatch, tmp_path):
    module = load('diagnose_macos_quota')
    def forbidden(*args, **kwargs):
        pytest.fail('required native probe must not launch a debugger')
    monkeypatch.setattr(module.subprocess, 'call', forbidden)
    assert module.diagnose(tmp_path / 'suite', module.CASE, tmp_path / 'test.xml', 'lldb', require_stack=True) == 2
