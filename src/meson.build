db_src = files(
	'core/Database.cpp',
	'core/Edge.cpp',
	'core/Node.cpp',
	'core/Property.cpp',
	'core/Blob.cpp',
	'core/BlobChainManager.cpp',
	'core/Transaction.cpp',
	'core/PropertyValue.cpp',
	'query/QueryEngine.cpp',
	'query/QueryExecutor.cpp',
	'query/QueryPlanner.cpp',
	'query/indexes/IndexManager.cpp',
	'query/indexes/LabelIndex.cpp',
	'query/indexes/PropertyIndex.cpp',
	'query/indexes/RelationshipIndex.cpp',
	'query/indexes/FullTextIndex.cpp',
	'query/strategies/TraversalQuery.cpp',
	'storage/FileStorage.cpp',
	'storage/DataManager.cpp',
	'storage/DeletionManager.cpp',
	'storage/SegmentTracker.cpp',
	'storage/SpaceManager.cpp',
	'storage/FileHeaderManager.cpp',
	'storage/DatabaseInspector.cpp',
	'storage/IDAllocator.cpp',
	'storage/EntityReferenceUpdater.cpp',
	'storage/SegmentIndexManager.cpp',
	'traversal/RelationshipTraversal.cpp',
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