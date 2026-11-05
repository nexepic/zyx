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
