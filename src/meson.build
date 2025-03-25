db_src = files(
	'core/Database.cpp',
	'core/Edge.cpp',
    'core/Node.cpp',
    'core/Transaction.cpp',
    'core/query/QueryEngine.cpp',
    'core/query/QueryExecutor.cpp',
    'core/query/QueryPlanner.cpp',
    'core/query/indexes/IndexManager.cpp',
    'core/query/indexes/LabelIndex.cpp',
    'core/query/indexes/PropertyIndex.cpp',
    'core/query/indexes/RelationshipIndex.cpp',
    'core/query/indexes/FulltextIndex.cpp',
    'storage/FileStorage.cpp',
    'storage/DataManager.cpp',
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