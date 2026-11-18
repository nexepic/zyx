import importlib.util
import json
from pathlib import Path
import pytest


def load():
    spec = importlib.util.spec_from_file_location('verify_release', Path(__file__).parents[1] / 'scripts/verify_windows_release.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def setup_build(tmp_path, flags='/O2 /DNDEBUG -MD /Z7'):
    (tmp_path / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\nZYX_ENABLE_COVERAGE:BOOL=OFF\nCMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=/DEBUG /OPT:REF /OPT:ICF\n')
    (tmp_path / 'zyx_test_suite.pdb').write_bytes(b'fixture-pdb')
    commands = [{'file': source, 'command': '"C:\\Program Files\\LLVM\\bin\\clang-cl.exe" ' + flags + ' /c ' + source}
                for source in ['D:/a/zyx/zyx/src/query/execution/LogicalPlanReadRuntime.cpp', 'D:/a/zyx/zyx/tests/src/test_main.cpp']]
    (tmp_path / 'compile_commands.json').write_text(json.dumps(commands))
    return commands


def test_real_windows_command_quoting_and_mixed_flag_prefixes(tmp_path):
    setup_build(tmp_path)
    result = load().verify(tmp_path)
    assert result['test_compile_commands'] == result['production_compile_commands'] == 1
    assert result['build_type'] == 'Release' and result['ndebug'] and not result['coverage']


@pytest.mark.parametrize('flags', ['/Od /DNDEBUG -MD', '/O2 -MD', '/O2 /DNDEBUG /MDd',
    '/O2 /DNDEBUG -MD /Od', '/O2 /DNDEBUG -MD /MT', '/O2 /DNDEBUG -MD /D_DEBUG', '/O2 /DNDEBUG -MD /O1',
    '/O2 /DNDEBUG -MD /D_DEBUG=1', '/O2 /DNDEBUG -MD /U NDEBUG',
    '/O2 /DNDEBUG -MD "@flags.rsp"', '/O2 /DNDEBUG -MD /UNDEBUG',
    '/O2 /DNDEBUG -MD -D_ITERATOR_DEBUG_LEVEL=2',
    '/O2 /DNDEBUG -MD -fprofile-instr-generate', '/O2 /DNDEBUG -MD -fcoverage-mapping'])
def test_rejects_debug_or_partial_release_flags(tmp_path, flags):
    setup_build(tmp_path, flags + ' /Z7')
    with pytest.raises(ValueError): load().verify(tmp_path)


@pytest.mark.parametrize('cache', ['CMAKE_BUILD_TYPE:STRING=Debug\nZYX_ENABLE_COVERAGE:BOOL=OFF',
    'CMAKE_BUILD_TYPE:STRING=Release\nZYX_ENABLE_COVERAGE:BOOL=ON', ''])
def test_cache_identity_is_mandatory(tmp_path, cache):
    setup_build(tmp_path)
    (tmp_path / 'CMakeCache.txt').write_text(cache)
    with pytest.raises(ValueError): load().verify(tmp_path)


@pytest.mark.parametrize('selection', [[], [0], [1]])
def test_both_production_and_test_commands_are_mandatory(tmp_path, selection):
    commands = setup_build(tmp_path)
    (tmp_path / 'compile_commands.json').write_text(json.dumps([commands[i] for i in selection]))
    with pytest.raises(ValueError): load().verify(tmp_path)


def test_missing_pdb_or_symbols_fail_closed(tmp_path):
    setup_build(tmp_path, '/O2 /DNDEBUG -MD')
    with pytest.raises(ValueError, match='debug symbols'): load().verify(tmp_path)
    setup_build(tmp_path)
    (tmp_path / 'zyx_test_suite.pdb').unlink()
    with pytest.raises(ValueError, match='PDB'): load().verify(tmp_path)


def test_response_file_and_missing_test_main_fail_closed(tmp_path):
    commands = setup_build(tmp_path)
    commands[1]['command'] += ' @flags.rsp'
    (tmp_path / 'compile_commands.json').write_text(json.dumps(commands))
    with pytest.raises(ValueError, match='response file'): load().verify(tmp_path)
    commands = setup_build(tmp_path)
    commands[1]['file'] = 'D:/a/zyx/zyx/tests/src/not_main.cpp'
    (tmp_path / 'compile_commands.json').write_text(json.dumps(commands))
    with pytest.raises(ValueError, match='missing'): load().verify(tmp_path)


def test_pdb_link_flags_cannot_disable_release_link_optimization(tmp_path):
    setup_build(tmp_path)
    cache = tmp_path / 'CMakeCache.txt'
    cache.write_text(cache.read_text().replace('/OPT:REF /OPT:ICF', ''))
    with pytest.raises(ValueError, match='REF/ICF'): load().verify(tmp_path)
