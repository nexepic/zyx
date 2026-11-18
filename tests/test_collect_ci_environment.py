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


def test_native_mac_copies_only_image_and_dsym_whitelist(tmp_path):
    module = load_collector()
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


def test_missing_dsym_is_explicitly_incomplete(tmp_path):
    module = load_collector()
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
