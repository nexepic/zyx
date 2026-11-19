#!/usr/bin/env python3
"""Fail before Conan if the selected native Windows compiler/STL pair is unusable."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

VERSION = '22.1.8'
TOOLS = ('clang-cl', 'lld-link', 'llvm-symbolizer', 'llvm-cov', 'llvm-profdata')
SOURCE = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>
__declspec(noinline) int zyx_toolchain_probe() {
    std::pmr::monotonic_buffer_resource memory;
    std::pmr::vector<std::string> values(&memory);
    values.emplace_back("native-stl-ok");
    void* frames[8];
    return values.front() == "native-stl-ok" && CaptureStackBackTrace(0, 8, frames, nullptr) > 0 ? 0 : 1;
}
int main() {
    int result = zyx_toolchain_probe();
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto pc = reinterpret_cast<std::uintptr_t>(&zyx_toolchain_probe);
    std::printf("STL=%ld MSVC=%d RVA=0x%llx\n", long(_MSVC_STL_UPDATE), _MSC_VER,
                static_cast<unsigned long long>(pc - base));
    return result;
}
'''


def run(command, **kwargs):
    print('CI_WINDOWS_TOOLCHAIN_COMMAND ' + json.dumps([str(x) for x in command]), flush=True)
    result = subprocess.run(command, shell=False, check=True, timeout=60,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors='replace', **kwargs)
    print(result.stdout, flush=True)
    return result.stdout


def verify_tools(llvm_bin):
    tools = {}
    for name in TOOLS:
        path = llvm_bin / (name + '.exe')
        selected = shutil.which(name)
        # File identity tolerates Windows junctions, short paths and case aliases.
        same_file = bool(selected and path.is_file() and Path(selected).is_file()
                         and os.path.samefile(selected, path))
        if not same_file:
            raise ValueError(f'{name} is missing or PATH selects another installation: {selected}')
        version = run([str(path), '--version'])
        if not re.search(r'(?<![\d.])' + re.escape(VERSION) + r'(?![\d.])', version):
            raise ValueError(f'{name} must be LLVM {VERSION}')
        tools[name] = str(path)
    return tools


def compile_command(compiler, source, executable, build_type):
    flags = ['/MDd', '/Od'] if build_type == 'Debug' else ['/MD', '/O2', '/DNDEBUG']
    return [compiler, '/nologo', '/std:c++20', '/EHsc', '/Z7', '-fuse-ld=lld', *flags,
            str(source), '/Fe:' + str(executable), '/link', '/DEBUG',
            '/PDB:' + str(executable.with_suffix('.pdb'))]


def verify(llvm_bin, build_type):
    tools = verify_tools(llvm_bin)
    print('CI_WINDOWS_NATIVE_LINKER ' + json.dumps({'link': shutil.which('link.exe'),
          'cl': shutil.which('cl.exe'), 'lldb_optional': shutil.which('lldb.exe')}), flush=True)
    print('CI_WINDOWS_TOOLCHAIN_ENV ' + json.dumps({key: os.environ.get(key) for key in
          ('ImageOS', 'ImageVersion', 'VCToolsVersion', 'WindowsSDKVersion', 'VCINSTALLDIR')}), flush=True)
    # Temporary working directory avoids objects/PDBs leaking into the repository.
    with tempfile.TemporaryDirectory(prefix='zyx-toolchain-') as temporary:
        directory = Path(temporary)
        source, executable = directory / 'probe.cpp', directory / 'probe.exe'
        source.write_text(SOURCE, encoding='utf-8')
        run(compile_command(tools['clang-cl'], source, executable, build_type), cwd=directory)
        if not executable.with_suffix('.pdb').is_file():
            raise ValueError('native probe did not generate a PDB')
        output = run([str(executable)], cwd=directory)
        match = re.search(r'STL=(\d+) MSVC=(\d+) RVA=(0x[0-9a-fA-F]+)', output)
        if not match:
            raise ValueError('native probe did not report STL identity and RVA')
        symbols = run([tools['llvm-symbolizer'], '--relative-address', '--no-inlines',
                       '--obj=' + str(executable)], input=match[3] + '\n', cwd=directory,
                      env={**os.environ, '_NT_SYMBOL_PATH': '', '_NT_ALT_SYMBOL_PATH': '',
                           'DEBUGINFOD_URLS': '', 'LLVM_SYMBOLIZER_OPTS': ''})
        if 'zyx_toolchain_probe' not in symbols or not re.search(r'probe\.cpp:[1-9]\d*', symbols):
            raise ValueError('native PDB probe lacks its function/source line')
        print('CI_WINDOWS_TOOLCHAIN_OK ' + json.dumps({'llvm': VERSION, 'build_type': build_type,
              'stl_update': match[1], 'msvc': match[2], 'native_stack_capture': True,
              'pdb_symbolization': True}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-bin', type=Path, required=True)
    parser.add_argument('--build-type', choices=('Debug', 'Release'), required=True)
    args = parser.parse_args()
    try:
        verify(args.llvm_bin, args.build_type)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stdout or '', flush=True)
        print('CI_WINDOWS_TOOLCHAIN_INVALID ' + str(error), flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
