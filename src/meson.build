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
	'core/query/indexes/FullTextIndex.cpp',
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

# Static library - using dependencies declared in root meson.build
db_lib = static_library('metrix',
						sources : db_src,
						include_directories : inc_dirs,
						dependencies : [zlib_dep, boost_dep],
						install : false
)

# Export library dependency for other parts of the project
db_dep = declare_dependency(
	include_directories : inc_dirs,
	link_with : db_lib
)