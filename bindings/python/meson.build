project('zyxdb', 'cpp',
        version : run_command(
            'python3', '-c',
            'import re, pathlib; print(re.search(r"version\\s*:\\s*\'([^\']+)\'", pathlib.Path("' + meson.current_source_dir() / '..' / '..' / 'meson.build' + '").read_text()).group(1))',
            check : true).stdout().strip(),
        default_options : [
            'cpp_std=c++20',
            'warning_level=3',
            'werror=false',
            'buildtype=release',
            'default_library=static',
        ])

# Resolve the root of the main ZYX project (two levels up from bindings/python)
zyx_root = meson.current_source_dir() / '..' / '..'

cxx = meson.get_compiler('cpp')

# Windows macro conflict guards
if host_machine.system() == 'windows'
    add_project_arguments('-DNOMINMAX', '-DNOGDI', language : 'cpp')
endif

# ==============================================================================
# Dependencies (same as root meson.build)
# ==============================================================================
zlib_dep = dependency('zlib')
boost_dep = dependency('boost', modules : ['filesystem', 'system'])
cli11_dep = dependency('CLI11')

antlr4_dep = dependency('antlr4-cppruntime', required : false)
if not antlr4_dep.found()
    antlr4_dep = dependency('antlr4-runtime', method : 'cmake',
                            modules : ['antlr4_static'], required : true)
endif

# ==============================================================================
# ANTLR4 generated parser sources + implementation
# ==============================================================================
parser_dir = zyx_root / 'src' / 'query' / 'parser' / 'cypher' / 'generated'
parser_impl_dir = zyx_root / 'src' / 'query' / 'parser' / 'cypher'

_py = import('python').find_installation()

# Generated parser files
_find_parser = run_command(_py, '-c', '''
import os, sys
d = sys.argv[1]
source_dir = sys.argv[2]
for f in sorted(os.listdir(d)):
    if f.endswith(".cpp"):
        print(os.path.relpath(os.path.join(d, f), source_dir))
''', parser_dir, meson.current_source_dir(), check : true)
_parser_files = _find_parser.stdout().strip().split('\n')
parser_src = []
foreach f : _parser_files
    if f != ''
        parser_src += files(f)
    endif
endforeach

# Parser implementation files (CypherParserImpl, helpers, clauses, etc.)
_find_parser_impl = run_command(_py, '-c', '''
import os, sys
base_dir = sys.argv[1]
source_dir = sys.argv[2]
seen = set()
for line in sys.argv[3].splitlines():
    if line.strip():
        seen.add(os.path.normpath(line.strip()))
for root, dirs, files_list in os.walk(base_dir):
    for f in sorted(files_list):
        if f.endswith(".cpp"):
            rel = os.path.relpath(os.path.join(root, f), source_dir)
            if os.path.normpath(rel) not in seen:
                print(rel)
''', parser_impl_dir, meson.current_source_dir(), '\n'.join(_parser_files), check : true)
_parser_impl_files = _find_parser_impl.stdout().strip().split('\n')
foreach f : _parser_impl_files
    if f != ''
        parser_src += files(f)
    endif
endforeach

cypher_inc = include_directories(
    '../../src/query/parser/cypher/generated',
    '../../src/query/parser/cypher',
    '../../src/query/parser/cypher/helpers/internal',
    '../../src/query/parser/cypher/helpers/public',
)

cypher_dep = declare_dependency(
    sources : parser_src,
    include_directories : cypher_inc,
)

# ==============================================================================
# inputxx library (required by core/cli)
# ==============================================================================
inputxx_src_dir = zyx_root / 'lib' / 'inputxx' / 'src'
_find_inputxx = run_command(_py, '-c', '''
import os, sys
base_dir = sys.argv[1]
for root, dirs, files in os.walk(base_dir):
    for f in sorted(files):
        if f.endswith(".cpp"):
            print(os.path.relpath(os.path.join(root, f), base_dir))
''', inputxx_src_dir, check : true)
inputxx_src = []
foreach f : _find_inputxx.stdout().strip().split('\n')
    if f != ''
        inputxx_src += files(zyx_root / 'lib' / 'inputxx' / 'src' / f)
    endif
endforeach
inputxx_inc = include_directories('../../lib/inputxx/include')
inputxx_lib = static_library('inputxx',
    sources : inputxx_src,
    include_directories : inputxx_inc,
    install : false,
)
inputxx_dep = declare_dependency(
    include_directories : inputxx_inc,
    link_with : inputxx_lib,
)

# ==============================================================================
# Core library sources (same discovery as src/meson.build)
# ==============================================================================
src_dir = zyx_root / 'src'
_find_src = run_command(_py, '-c', '''
import os, sys
base_dir = sys.argv[1]
source_dir = sys.argv[2]
exclude = ["query/parser", "cli"]
for root, dirs, files_list in os.walk(base_dir):
    rel_root = os.path.relpath(root, base_dir)
    skip = False
    for ex in exclude:
        if rel_root == ex or rel_root.startswith(ex + os.sep):
            skip = True
            break
    if skip:
        continue
    for f in sorted(files_list):
        if f.endswith(".cpp"):
            print(os.path.relpath(os.path.join(root, f), source_dir))
''', src_dir, meson.current_source_dir(), check : true)
_src_files = _find_src.stdout().strip().split('\n')
db_src = []
foreach f : _src_files
    if f != ''
        db_src += files(f)
    endif
endforeach

inc_dirs = include_directories(
    '../../include',
    '../../src',
    '../..',
    '../../src/query/parser/cypher',
)

# Generate ProjectConfig.hpp from template
project_config = configuration_data()
project_config.set('PROJECT_NAME', 'zyx')
project_config.set('PROJECT_DISPLAY', 'ZYX')
project_config.set('PROJECT_VERSION', meson.project_version())
configure_file(
    input: zyx_root / 'include' / 'graph' / 'core' / 'ProjectConfig.hpp.in',
    output: 'ProjectConfig.hpp',
    configuration: project_config,
    install: false,
)

# Python's Token.h defines macros (SEMI, COMMA, etc.) that conflict with
# ANTLR4 parser method names. Force-include an undef header to neutralize them.
_undef_header = meson.current_source_dir() / 'undef_python_tokens.h'
if cxx.get_argument_syntax() == 'msvc'
    _force_include = ['/FI' + _undef_header]
else
    _force_include = ['-include', _undef_header]
endif

db_lib = static_library('zyx_core',
    sources : db_src,
    include_directories : inc_dirs,
    dependencies : [zlib_dep, boost_dep, cli11_dep, antlr4_dep, cypher_dep, inputxx_dep],
    cpp_args : ['-DCOVERAGE_SKIP_ZLIB_ERRORS'] + _force_include,
    install : false,
)

db_dep = declare_dependency(
    include_directories : [inc_dirs, cypher_inc],
    link_with : db_lib,
    dependencies : [zlib_dep, boost_dep, cli11_dep, antlr4_dep, inputxx_dep],
)

# ==============================================================================
# Python extension module
# ==============================================================================
py = import('python').find_installation(pure: false)

pybind11_dep = dependency('pybind11', required: true)

zyxdb_core = py.extension_module(
    '_core',
    sources: ['src/zyxdb/_core.cpp'],
    dependencies: [pybind11_dep, db_dep],
    include_directories: inc_dirs,
    install: true,
    subdir: 'zyxdb',
)

py.install_sources(
    'src/zyxdb/__init__.py',
    'src/zyxdb/database.py',
    'src/zyxdb/result.py',
    'src/zyxdb/transaction.py',
    'src/zyxdb/types.py',
    'src/zyxdb/py.typed',
    subdir: 'zyxdb',
)
