db_src = files(
	'core/Database.cpp',
	'core/Edge.cpp',
    'core/Node.cpp',
    'core/query/QueryEngine.cpp',
    'core/Transaction.cpp',
    'storage/FileStorage.cpp',
    'storage/FileStorageSave.cpp',
    'storage/FileStorageLoad.cpp',
    'utils/ChecksumUtils.cpp',
    'utils/Serializer.cpp',
    'cli/Repl.cpp'
)

zlib_dep = dependency('zlib')

# static library
db_lib = static_library('metrix',
                              sources: db_src,
                              include_directories: db_inc,
                              dependencies: [zlib_dep],
                              install: false
)

# export library
db_dep = declare_dependency(
    include_directories: db_inc,
    link_with: db_lib
)