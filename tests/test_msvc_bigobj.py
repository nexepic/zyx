"""Configure-only compiler branch contracts; no Windows compiler emulation."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize('compiler,msvc', [('MSVC', True), ('Clang', True),
                                         ('GNU', False), ('AppleClang', False)])
def test_common_options_bigobj_is_shared_and_msvc_only(tmp_path, compiler, msvc):
    cmake = shutil.which('cmake')
    if not cmake:
        pytest.skip('CMake required for configure-only contract')
    (tmp_path / 'dummy.cpp').write_text('int answer() { return 42; }\n')
    # Detect the host compiler normally, then exercise the selected CMake branch.
    # We generate flags only: these are not native MSVC compilation tests.
    (tmp_path / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
project(options_contract LANGUAGES CXX)
set(CMAKE_CXX_COMPILER_ID "{compiler}")
set(MSVC {"TRUE" if msvc else "FALSE"})
include("{(ROOT / 'cmake/CompilerOptions.cmake').as_posix()}")
add_library(zyx_project_options INTERFACE)
zyx_apply_common_options(zyx_project_options)
foreach(target zyx zyx_core)
  add_library(${{target}} STATIC dummy.cpp)
  target_link_libraries(${{target}} PUBLIC zyx_project_options)
  file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/${{target}}-options.txt"
       CONTENT "$<TARGET_PROPERTY:${{target}},COMPILE_OPTIONS>")
endforeach()
''')
    build = tmp_path / 'build'
    subprocess.run([cmake, '-S', str(tmp_path), '-B', str(build)],
                   check=True, capture_output=True, text=True, timeout=60)
    for target in ('zyx', 'zyx_core'):
        flags = (build / f'{target}-options.txt').read_text().split(';')
        assert flags.count('/bigobj') == int(compiler == 'MSVC')
        assert ('/W3' in flags) == msvc
