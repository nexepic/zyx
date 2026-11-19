import importlib.util
from pathlib import Path
import subprocess
import pytest


def load():
    spec = importlib.util.spec_from_file_location('windows_toolchain', Path(__file__).parents[1] / 'scripts/verify_windows_toolchain.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize('kind,required,forbidden', [('Debug', ['/MDd', '/Od'], ['/MD', '/O2', '/DNDEBUG']),
                                                   ('Release', ['/MD', '/O2', '/DNDEBUG'], ['/MDd', '/Od'])])
def test_native_arguments_preserve_crt_optimization_and_symbols(tmp_path, kind, required, forbidden):
    command = load().compile_command('C:/Program Files/LLVM/bin/clang-cl.exe', tmp_path / 'probe.cpp', tmp_path / 'probe.exe', kind)
    assert all(flag in command for flag in required + ['/Z7', '/DEBUG', '/std:c++20', '-fuse-ld=lld'])
    assert all(flag not in command for flag in forbidden)
    assert command[0] == 'C:/Program Files/LLVM/bin/clang-cl.exe'
    assert not any('Git/' in arg for arg in command)


def tools_fixture(module, tmp_path, monkeypatch, version='22.1.8'):
    for name in module.TOOLS:
        (tmp_path / (name + '.exe')).write_bytes(b'fixture')
    monkeypatch.setattr(module.shutil, 'which', lambda name: str(tmp_path / (name + '.exe')))
    monkeypatch.setattr(module, 'run', lambda command, **kwargs: 'LLVM version ' + version)


def test_matching_toolset(tmp_path, monkeypatch):
    module = load()
    tools_fixture(module, tmp_path, monkeypatch)
    assert set(module.verify_tools(tmp_path)) == set(module.TOOLS)


@pytest.mark.parametrize('version', ['20.1.4', '22.1.80', '122.1.8', '23.1.2', 'unknown'])
def test_wrong_tool_version_rejected(tmp_path, monkeypatch, version):
    module = load()
    tools_fixture(module, tmp_path, monkeypatch, version)
    with pytest.raises(ValueError, match='must be LLVM'):
        module.verify_tools(tmp_path)


def test_shadowed_tool_rejected(tmp_path, monkeypatch):
    module = load()
    tools_fixture(module, tmp_path, monkeypatch)
    monkeypatch.setattr(module.shutil, 'which', lambda name: str(tmp_path / 'other' / (name + '.exe')))
    with pytest.raises(ValueError, match='another installation'):
        module.verify_tools(tmp_path)


def test_missing_tool_rejected(tmp_path, monkeypatch):
    module = load()
    tools_fixture(module, tmp_path, monkeypatch)
    (tmp_path / 'llvm-symbolizer.exe').unlink()
    with pytest.raises(ValueError, match='missing'):
        module.verify_tools(tmp_path)


def test_run_is_native_bounded_and_checked(monkeypatch):
    module = load()
    observed = {}
    def fake(command, **kwargs):
        observed.update(kwargs)
        return subprocess.CompletedProcess(command, 0, 'ok')
    monkeypatch.setattr(module.subprocess, 'run', fake)
    assert module.run(['clang-cl', '/O2']) == 'ok'
    assert observed['shell'] is False and observed['check'] is True and observed['timeout'] == 60


@pytest.mark.parametrize('failure', ['compile', 'run', 'pdb', 'identity', 'symbol', None])
def test_full_probe_sequence_and_negative_results(tmp_path, monkeypatch, failure):
    module = load()
    monkeypatch.setattr(module, 'verify_tools', lambda root: {name: name for name in module.TOOLS})
    commands = []
    def fake(command, **kwargs):
        commands.append(command)
        if command[0] == 'clang-cl':
            if failure == 'compile':
                raise subprocess.CalledProcessError(1, command, 'STL1000')
            if failure != 'pdb':
                Path(kwargs['cwd'], 'probe.pdb').write_bytes(b'pdb')
            return ''
        if command[0] == 'llvm-symbolizer':
            assert kwargs['input'] == '0x1234\n'
            assert kwargs['env']['_NT_SYMBOL_PATH'] == ''
            return '??\n??:0:0' if failure == 'symbol' else 'zyx_toolchain_probe\nC:/probe.cpp:12:0'
        if failure == 'run':
            raise subprocess.CalledProcessError(1, command, 'native stack unavailable')
        return '' if failure == 'identity' else 'STL=202611 MSVC=1952 RVA=0x1234'
    monkeypatch.setattr(module, 'run', fake)
    if failure:
        with pytest.raises((ValueError, subprocess.CalledProcessError)):
            module.verify(tmp_path, 'Release')
    else:
        module.verify(tmp_path, 'Release')
        assert len(commands) == 3


def test_workflow_common_pin_and_preconan_native_gate():
    root = Path(__file__).parents[1]
    workflow = (root / '.github/workflows/build.yml').read_text()
    powershell = (root / 'scripts/install_conan_windows.ps1').read_text()
    assert 'choco install llvm --version=22.1.8 --force' in workflow
    assert workflow.count('tests/test_verify_windows_toolchain.py') == 3
    assert powershell.index('vcvarsEnvironment |') < powershell.index('verify_windows_toolchain.py') < powershell.index('conan install .')
    assert 'throw "Windows compiler/STL/PDB compatibility preflight failed."' in powershell
    assert 'compiler=msvc\ncompiler.version=$msvcAbiVersion' in powershell
    assert '_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH' not in powershell


def test_path_alias_to_same_file_is_accepted(tmp_path, monkeypatch):
    module = load()
    tools_fixture(module, tmp_path, monkeypatch)
    aliases = tmp_path / 'alias'
    aliases.mkdir()
    for name in module.TOOLS:
        (aliases / (name + '.exe')).hardlink_to(tmp_path / (name + '.exe'))
    monkeypatch.setattr(module.shutil, 'which', lambda name: str(aliases / (name + '.exe')))
    assert len(module.verify_tools(tmp_path)) == len(module.TOOLS)


def test_pinned_contract_is_opt_in_only_for_build_windows_conan():
    root = Path(__file__).parents[1]
    script = (root / 'scripts/install_conan_windows.ps1').read_text()
    assert '$verifyPinnedToolchain = $env:ZYX_VERIFY_WINDOWS_TOOLCHAIN -eq "1"' in script
    assert 'if ($verifyPinnedToolchain -or $env:PATH -notlike "*$llvmBin*") {' in script
    assert 'if ($verifyPinnedToolchain) {\n    python "$PSScriptRoot/run_ci_evidence.py" --name windows-toolchain' in script
    workflows = root / '.github/workflows'
    opted_in = [p.name for p in workflows.glob('*.yml') if 'ZYX_VERIFY_WINDOWS_TOOLCHAIN:' in p.read_text()]
    assert opted_in == ['build.yml']
    workflow = (workflows / 'build.yml').read_text()
    assert workflow.count('ZYX_VERIFY_WINDOWS_TOOLCHAIN:') == 1
    step = workflow.split('      - name: Install Project Dependencies (Conan) (Windows)\n', 1)[1].split('      - name:', 1)[0]
    assert 'ZYX_VERIFY_WINDOWS_TOOLCHAIN: "1"' in step
    assert "if: runner.os == 'Windows'" in step
