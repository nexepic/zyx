import importlib.util
import json
from pathlib import Path
import sys


def test_environment_manifest_whitelists_cache_and_preserves_crash_identity(tmp_path, monkeypatch):
    source=Path(__file__).resolve().parents[1]/'scripts/collect_ci_environment.py'
    spec=importlib.util.spec_from_file_location('ci_environment',source)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    build=tmp_path/'build';build.mkdir()
    (build/'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug\nCMAKE_CXX_FLAGS:STRING=-O0\nPRIVATE_TOKEN:STRING=do-not-include\n')
    (build/'zyx_test_suite.exe').write_bytes(b'fake-image')
    (build/'zyx_test_suite.pdb').write_bytes(b'fake-symbols')
    out=tmp_path/'evidence';(out/'case').mkdir(parents=True)
    (out/'case/result.json').write_text(json.dumps({'process':{'raw_exit_code':3}}))
    monkeypatch.setattr(module.shutil,'which',lambda _:None)
    monkeypatch.setattr(sys,'argv',['collector','--output',str(out/'environment.json'),'--build-dir',str(build),'--save-crash-symbols'])
    module.main()
    text=(out/'environment.json').read_text();r=json.loads(text)
    assert 'do-not-include' not in text and 'PRIVATE_TOKEN' not in text
    assert r['cmake']=={'CMAKE_BUILD_TYPE':'Debug','CMAKE_CXX_FLAGS':'-O0'}
    assert r['crash_symbols']['copied']==['zyx_test_suite.exe','zyx_test_suite.pdb']
    assert all(len(x['sha256'])==64 for x in r['modules'])


def test_normal_test_failure_does_not_copy_symbol_images(tmp_path, monkeypatch):
    source=Path(__file__).resolve().parents[1]/'scripts/collect_ci_environment.py'
    spec=importlib.util.spec_from_file_location('ci_environment_normal',source)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    build=tmp_path/'build';build.mkdir();(build/'zyx_test_suite.exe').write_bytes(b'image')
    out=tmp_path/'evidence';(out/'case').mkdir(parents=True)
    (out/'case/result.json').write_text(json.dumps({'process':{'raw_exit_code':1}}))
    monkeypatch.setattr(module.shutil,'which',lambda _:None)
    monkeypatch.setattr(sys,'argv',['collector','--output',str(out/'environment.json'),'--build-dir',str(build),'--save-crash-symbols'])
    module.main();r=json.loads((out/'environment.json').read_text())
    assert not r['crash_symbols']['crash_detected'] and not (out/'crash-symbols').exists()


def test_locked_symbols_do_not_destroy_other_evidence(tmp_path, monkeypatch):
    source=Path(__file__).resolve().parents[1]/'scripts/collect_ci_environment.py'
    spec=importlib.util.spec_from_file_location('ci_environment_locked',source)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    build=tmp_path/'build';build.mkdir();(build/'zyx_test_suite.exe').write_bytes(b'image')
    out=tmp_path/'evidence';(out/'case').mkdir(parents=True)
    (out/'case/result.json').write_text(json.dumps({'process':{'raw_exit_code':3}}))
    monkeypatch.setattr(module.shutil,'which',lambda _:None)
    def denied(*_):raise PermissionError('simulated locked symbol')
    monkeypatch.setattr(module.shutil,'copyfile',denied)
    monkeypatch.setattr(sys,'argv',['collector','--output',str(out/'environment.json'),'--build-dir',str(build),'--save-crash-symbols'])
    module.main();r=json.loads((out/'environment.json').read_text())
    assert r['modules'][0]['sha256']
    assert r['crash_symbols']['copied']==[]
    assert r['crash_symbols']['copy_errors'][0]['error_type']=='PermissionError'


def load_collector():
    source = Path(__file__).resolve().parents[1] / 'scripts/collect_ci_environment.py'
    spec = importlib.util.spec_from_file_location('ci_symbols', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def native_evidence(tmp_path, captured=1):
    evidence = tmp_path / 'evidence'
    evidence.mkdir()
    (evidence / 'quota-debugger.json').write_text(json.dumps(
        {'native': True, 'captured': captured, 'limit': 3, 'exited': True, 'exit_code': 0}))
    return evidence


def test_successful_native_capture_preserves_windows_pair(tmp_path):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    for name in ['zyx_test_suite.exe', 'zyx_test_suite.pdb']:
        (build / name).write_bytes(b'symbol')
    (build / 'secret.txt').write_text('do-not-copy')
    record = module.collect_symbols(build, native_evidence(tmp_path))
    assert record['native_stack_detected'] and not record['crash_detected']
    assert record['complete']
    assert record['copied'] == ['zyx_test_suite.exe', 'zyx_test_suite.pdb']


def test_native_mac_copies_only_image_and_dsym_whitelist(tmp_path, monkeypatch):
    module = load_collector()
    monkeypatch.setattr(module.platform, 'system', lambda: 'Darwin')
    build = tmp_path / 'build'; build.mkdir()
    names = ['zyx_test_suite', 'zyx_test_suite.dSYM/Contents/Info.plist',
             'zyx_test_suite.dSYM/Contents/Resources/DWARF/zyx_test_suite']
    for name in names:
        path = build / name; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'debug')
    (build / 'zyx_test_suite.dSYM/secret').write_text('private')
    evidence = native_evidence(tmp_path)
    record = module.collect_symbols(build, evidence)
    assert record['complete'] and record['copied'] == names
    assert not (evidence / 'crash-symbols/zyx_test_suite.dSYM/secret').exists()


def test_missing_dsym_is_explicitly_incomplete(tmp_path, monkeypatch):
    module = load_collector()
    monkeypatch.setattr(module.platform, 'system', lambda: 'Darwin')
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite').write_bytes(b'image')
    record = module.collect_symbols(build, native_evidence(tmp_path))
    assert not record['complete'] and len(record['missing']) == 2


def test_symbol_total_bound_copies_nothing(tmp_path, monkeypatch):
    module = load_collector()
    monkeypatch.setattr(module, 'SYMBOL_LIMIT_BYTES', 5)
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite.exe').write_bytes(b'large-image')
    evidence = native_evidence(tmp_path)
    record = module.collect_symbols(build, evidence)
    assert 'exceeds' in record['unavailable'] and record['copied'] == []
    assert not (evidence / 'crash-symbols').exists()


def test_zero_capture_needs_no_symbols(tmp_path):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    record = module.collect_symbols(build, native_evidence(tmp_path, 0))
    assert not record['native_stack_detected'] and record['copied'] == []


def test_symbol_symlink_cannot_exfiltrate_file(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    candidate = build / 'zyx_test_suite.exe'
    candidate.write_text('secret')
    # Model the unsafe file without requiring Windows symlink privileges.
    original_is_symlink = Path.is_symlink
    monkeypatch.setattr(Path, 'is_symlink',
                        lambda path: path == candidate or original_is_symlink(path))
    record = module.collect_symbols(build, native_evidence(tmp_path))
    assert record['copied'] == [] and 'unavailable' in record


def test_malformed_status_is_ignored(tmp_path):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    evidence = native_evidence(tmp_path, True)
    (evidence / 'result.json').write_text('{"process": []}')
    record = module.collect_symbols(build, evidence)
    assert not record['native_stack_detected'] and not record['crash_detected']


def test_priority_native_case_survives_walk_limit_and_copy_limit(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite.exe').write_bytes(b'image')
    (build / 'zyx_test_suite.pdb').write_bytes(b'symbols')
    evidence = tmp_path / 'evidence'; evidence.mkdir()
    for index in range(270):
        (evidence / f'budget-{index:03}').mkdir()
    case = evidence / 'windows-edge-cell-stack'; case.mkdir()
    (case / 'quota-debugger.json').write_text('{"native":true,"captured":3}')
    (case / 'output.log').write_text(
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=00007FF75178CB80 base=00007FF74C440000 '
        'offset=0x534cb80 module=D:\\runner\\zyx_test_suite.exe\n')
    monkeypatch.setattr(module, 'SYMBOL_LIMIT_BYTES', 1)
    monkeypatch.setattr(module.shutil, 'which', lambda _: '/fixed/llvm-symbolizer')
    calls = []
    def resolve(command, addresses):
        calls.append((command, addresses))
        return {'exit_code': 0, 'output': 'function\nsource.cpp:10\n'}
    monkeypatch.setattr(module, 'bounded_symbolizer', resolve)
    result = module.collect_symbols(build, evidence)
    assert result['native_stack_detected'] and result['scan_truncated']
    assert not result['complete'] and result['copied'] == []
    assert result['local_symbolization'][0]['resolution']['exit_code'] == 0
    assert len(calls) == 1 and calls[0][1] == '0x534cb80\n'
    assert '--relative-address' in calls[0][0]
    assert '--obj=' + str((build / 'zyx_test_suite.exe').resolve()) in calls[0][0]


def test_symbolizer_rejects_foreign_and_invalid_frames(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite.exe').write_bytes(b'image')
    evidence = tmp_path / 'evidence'; evidence.mkdir()
    (evidence / 'output.log').write_text('\n'.join([
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=0x1100 base=0x1000 offset=0x100 module=evil.exe',
        'CI_QUOTA_NATIVE_PC stack=3 index=0 pc=0x1100 base=0x1000 offset=0x100 module=zyx_test_suite.exe',
        'CI_QUOTA_NATIVE_PC stack=0 index=32 pc=0x1100 base=0x1000 offset=0x100 module=zyx_test_suite.exe',
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=0x1100 base=0x1000 offset=0x999 module=zyx_test_suite.exe',
    ]))
    result = module.symbolize_native(build, evidence, [evidence])[0]
    assert result['frames'] == [] and 'unavailable' in result


def test_macos_symbolizer_uses_fixed_image_and_runtime_base(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite').write_bytes(b'image')
    evidence = tmp_path / 'evidence'; evidence.mkdir()
    (evidence / 'output.log').write_text(
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=0x1100 base=0x1000 '
        'offset=0x100 module=/untrusted/zyx_test_suite\n')
    monkeypatch.setattr(module.platform, 'system', lambda: 'Darwin')
    monkeypatch.setattr(module.shutil, 'which', lambda _: '/usr/bin/atos')
    calls = []
    monkeypatch.setattr(module, 'bounded_symbolizer', lambda cmd, data: calls.append((cmd, data)) or {})
    result = module.symbolize_native(build, evidence, [evidence])[0]
    assert calls == [(['/usr/bin/atos', '-o', str((build / 'zyx_test_suite').resolve()),
                      '-l', '0x1000', '0x1100'], '')]
    assert result['source_lines_guaranteed'] is False


def test_symbolizer_output_is_bounded():
    module = load_collector()
    result = module.bounded_symbolizer([sys.executable, '-c', 'print("x" * 100000)'], '')
    assert result['output_truncated'] and len(result['output']) == 65536


def test_symbolizer_timeout_is_reported(monkeypatch):
    module = load_collector()
    original_timer = module.threading.Timer
    monkeypatch.setattr(module.threading, 'Timer', lambda _, callback: original_timer(.1, callback))
    result = module.bounded_symbolizer([sys.executable, '-c', 'import time; time.sleep(30)'], '')
    assert result['timed_out']


def test_linux_symbolizer_does_not_require_pdb(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite').write_bytes(b'image')
    evidence = tmp_path / 'evidence'; evidence.mkdir()
    (evidence / 'output.log').write_text(
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=0x1100 base=0x1000 '
        'offset=0x100 module=/runner/zyx_test_suite\n')
    monkeypatch.setattr(module.platform, 'system', lambda: 'Linux')
    monkeypatch.setattr(module.shutil, 'which', lambda _: '/usr/bin/llvm-symbolizer')
    calls = []
    monkeypatch.setattr(module, 'bounded_symbolizer', lambda cmd, data: calls.append((cmd, data)) or {})
    result = module.symbolize_native(build, evidence, [evidence])[0]
    assert 'unavailable' not in result
    assert calls[0][1] == '0x100\n'
    assert '--relative-address' in calls[0][0]


def test_raw_native_stack_survives_crash_or_timeout_without_status(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite.exe').write_bytes(b'image')
    (build / 'zyx_test_suite.pdb').write_bytes(b'symbols')
    monkeypatch.setattr(module, 'SYMBOL_LIMIT_BYTES', 1)
    monkeypatch.setattr(module.shutil, 'which', lambda _: '/fixed/llvm-symbolizer')
    calls = []
    monkeypatch.setattr(module, 'bounded_symbolizer',
                        lambda cmd, data: calls.append(data) or {'exit_code': 0, 'output': 'function'})
    for failure, process in [('crash', {'raw_exit_code': 139}),
                             ('timeout', {'raw_exit_code': None, 'timed_out': True})]:
        evidence = tmp_path / failure; evidence.mkdir()
        case = evidence / 'native-quota-stack'; case.mkdir()
        (case / 'result.json').write_text(json.dumps({'process': process}))
        (case / 'output.log').write_text(
            'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=00007FF75178CB80 base=00007FF74C440000 '
            'offset=0x534cb80 module=D:\\a\\zyx\\zyx\\buildDir\\zyx_test_suite.exe\n')
        result = module.collect_symbols(build, evidence)
        assert result['native_stack_detected']
        assert result['raw_stack_without_completed_status'] == ['native-quota-stack']
        resolved = result['local_symbolization'][0]
        assert resolved['capture_status'] == 'unconfirmed_or_incomplete'
        assert resolved['resolution']['output'] == 'function'
        assert not result['complete'] and result['copied'] == []
    assert calls == ['0x534cb80\n', '0x534cb80\n']


def test_raw_stack_fallback_ignores_unrecognized_directories(tmp_path):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    evidence = tmp_path / 'evidence'; evidence.mkdir()
    case = evidence / 'arbitrary'; case.mkdir()
    (case / 'output.log').write_text(
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=0x1100 base=0x1000 '
        'offset=0x100 module=/runner/zyx_test_suite\n')
    result = module.collect_symbols(build, evidence)
    assert not result['native_stack_detected']
    assert 'local_symbolization' not in result


def test_linux_symbol_copy_does_not_require_dsym(tmp_path, monkeypatch):
    module = load_collector()
    monkeypatch.setattr(module.platform, 'system', lambda: 'Linux')
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite').write_bytes(b'elf-with-debug-info')
    record = module.collect_symbols(build, native_evidence(tmp_path))
    assert record['copied'] == ['zyx_test_suite']
    assert record['missing'] == [] and record['complete']


def test_linux_versioned_symbolizer_fallback_and_precedence(tmp_path, monkeypatch):
    module = load_collector()
    build = tmp_path / 'build'; build.mkdir()
    (build / 'zyx_test_suite').write_bytes(b'elf')
    evidence = tmp_path / 'evidence'; evidence.mkdir()
    (evidence / 'output.log').write_text(
        'CI_QUOTA_NATIVE_PC stack=0 index=0 pc=0x1100 base=0x1000 '
        'offset=0x100 module=/runner/zyx_test_suite\n')
    monkeypatch.setattr(module.platform, 'system', lambda: 'Linux')
    commands = []
    monkeypatch.setattr(module, 'bounded_symbolizer',
                        lambda cmd, data: commands.append(cmd) or {'exit_code': 0})
    for available, expected in [
            ({'llvm-symbolizer', 'llvm-symbolizer-20'}, 'llvm-symbolizer'),
            ({'llvm-symbolizer-20'}, 'llvm-symbolizer-20')]:
        monkeypatch.setattr(module.shutil, 'which',
                            lambda name: '/usr/bin/' + name if name in available else None)
        result = module.symbolize_native(build, evidence, [evidence])[0]
        assert result['tool'] == expected
        assert commands[-1][0] == '/usr/bin/' + expected
        assert '--relative-address' in commands[-1]
    monkeypatch.setattr(module.shutil, 'which', lambda _: None)
    result = module.symbolize_native(build, evidence, [evidence])[0]
    assert result['unavailable'] == 'platform symbolizer unavailable'
    assert len(commands) == 2
