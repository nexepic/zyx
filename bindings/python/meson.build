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
    'src/zyxdb/transaction.py',
    'src/zyxdb/types.py',
    subdir: 'zyxdb',
)
