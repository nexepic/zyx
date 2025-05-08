db_src = files(
	'core/Database.cpp',
	'core/Edge.cpp',
	'core/Node.cpp',
	'core/Property.cpp',
	'core/Blob.cpp',
	'core/Transaction.cpp',
	'core/PropertyValue.cpp',
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
	'storage/DeletionManager.cpp',
	'storage/SegmentTracker.cpp',
	'storage/SpaceManager.cpp',
	'storage/FileHeaderManager.cpp',
	'storage/DatabaseInspector.cpp',
	'storage/IDAllocator.cpp',
	'utils/ChecksumUtils.cpp',
	'utils/CompressUtils.cpp',
	'utils/Serializer.cpp',
	'cli/Repl.cpp'
)

cc = meson.get_compiler('cpp')

zlib_dep = dependency('zlib')
boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)
if not boost_dep.found()
    boost_dep = cc.find_library('boost_filesystem', required: true)
endif

# static library
db_lib = static_library('metrix',
						sources : db_src,
						include_directories : db_inc,
						dependencies : [zlib_dep, boost_dep],
						install : false
)

# export library
db_dep = declare_dependency(
	include_directories : db_inc,
	link_with : db_lib
)