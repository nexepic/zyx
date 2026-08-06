function(zyx_normalize_source_list out_var)
    set(paths ${ARGN})
    list(REMOVE_DUPLICATES paths)
    list(SORT paths)
    set(${out_var} ${paths} PARENT_SCOPE)
endfunction()

function(zyx_filter_out_regex in_var regex)
    set(filtered)
    foreach(path IN LISTS ${in_var})
        if(NOT path MATCHES "${regex}")
            list(APPEND filtered "${path}")
        endif()
    endforeach()
    set(${in_var} ${filtered} PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE ZYX_INPUTXX_SOURCES_RAW CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/lib/inputxx/src/*.cpp"
)
zyx_normalize_source_list(ZYX_INPUTXX_SOURCES ${ZYX_INPUTXX_SOURCES_RAW})

set(ZYX_CYPHER_GENERATED_SOURCES
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherLexer.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherParser.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherParserBaseVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/generated/CypherParserVisitor.cpp"
)
set(ZYX_CYPHER_IMPLEMENTATION_SOURCES
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/CypherParserImpl.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/CypherToPlanVisitor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/ir/CypherASTBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/internal/AstExtractor.cpp"
    "${PROJECT_SOURCE_DIR}/src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp"
)
zyx_normalize_source_list(ZYX_CYPHER_SOURCES ${ZYX_CYPHER_GENERATED_SOURCES} ${ZYX_CYPHER_IMPLEMENTATION_SOURCES})

set(ZYX_SOURCE_ROOT "${PROJECT_SOURCE_DIR}/src")

# This is intentionally an allowlist, not a recursive source glob. A legacy
# implementation remaining in the checkout must never become a production
# dependency merely because it has a .cpp extension. Additions require an
# explicit architectural decision and a matching native-format test.
set(ZYX_NATIVE_API_SOURCES
    "${ZYX_SOURCE_ROOT}/api/DatabaseImpl.cpp"
    "${ZYX_SOURCE_ROOT}/api/DriverAbi.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiDatabase.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiErrors.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiIngest.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiParams.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiResults.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiTransactions.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiValueRefs.cpp"
    "${ZYX_SOURCE_ROOT}/api/driver_abi/DriverAbiValues.cpp"
)

set(ZYX_NATIVE_CORE_SOURCES
    "${ZYX_SOURCE_ROOT}/core/Database.cpp"
    "${ZYX_SOURCE_ROOT}/core/Edge.cpp"
    "${ZYX_SOURCE_ROOT}/core/Node.cpp"
    "${ZYX_SOURCE_ROOT}/core/PropertyTypes.cpp"
    "${ZYX_SOURCE_ROOT}/core/TemporalTypes.cpp"
    "${ZYX_SOURCE_ROOT}/core/Transaction.cpp"
    "${ZYX_SOURCE_ROOT}/debug/PerfTrace.cpp"
)

set(ZYX_NATIVE_QUERY_SOURCES
    "${ZYX_SOURCE_ROOT}/query/QueryContext.cpp"
    "${ZYX_SOURCE_ROOT}/query/QueryErrorMessages.cpp"
    "${ZYX_SOURCE_ROOT}/query/QueryGuard.cpp"
    "${ZYX_SOURCE_ROOT}/query/api/QueryEngine.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalGraphAdjacencyCursor.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalGraphNodeCursor.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalGraphReadExecutor.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalPlanMutationRuntime.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalPlanProcedureRuntime.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalPlanReadRuntime.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalPlanSchemaRuntime.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/LogicalPropertyPredicate.cpp"
    "${ZYX_SOURCE_ROOT}/query/execution/Record.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/EntityIntrospectionFunction.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/EvaluationContext.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ExistsExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/Expression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ExpressionUtils.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/FunctionRegistry.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/IsNullExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ListComprehensionExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ListFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ListLiteralExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ListSliceExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/MathFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/PatternComprehensionExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/PredicateFunction.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/QuantifierFunctionExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ReduceExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/StringFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/TemporalFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/TypeConversionFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/UtilityFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/ir/LogicalPlanBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/query/ir/SemanticAnalyzer.cpp"
    "${ZYX_SOURCE_ROOT}/query/logical/LogicalOperator.cpp"
    "${ZYX_SOURCE_ROOT}/query/planner/PipelineValidator.cpp"
    "${ZYX_SOURCE_ROOT}/query/planner/ProcedureRegistry.cpp"
)

set(ZYX_NATIVE_STORAGE_SOURCES
    "${ZYX_SOURCE_ROOT}/storage/LogicalGraphTransactionOverlay.cpp"
    "${ZYX_SOURCE_ROOT}/storage/StorageBackendFactory.cpp"
    "${ZYX_SOURCE_ROOT}/storage/StorageFormat.cpp"
    "${ZYX_SOURCE_ROOT}/storage/StorageIO.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/cache/BufferPool.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/AllocatorDirectory.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/AllocatorPage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/PhysicalSegmentAllocator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/SharedPagePool.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/SharedPagePoolAllocator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/SlottedPage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/allocation/SlottedPagePool.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/backend/RelocatableLogicalGraphRead.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/backend/RelocatableStorageBackend.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/EntityLabelSet.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/EntityLabelSetStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/EntityPage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/EntityPageStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphMutationExecutor.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphWritePlanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeScalarIndexMutationPlanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeVectorMutationPlanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/PageBackedConstraintMutationAdapter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/PageBackedGraphReadAdapter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/Format.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/Superblock.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/SuperblockFile.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/SuperblockPair.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/adjacency/AdjacencyKeyCodec.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/adjacency/GlobalAdjacencyIndex.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BTreeIndex.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BTreePage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BTreePageStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BulkBTreeBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/ExternalBTreeEntrySorter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/IndexKeyCodec.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/InitialBTreeBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/OfflineScalarIndexMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/OfflineVectorBaseRebuild.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/OfflineVectorAdjacencyQuality.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/OfflineVectorPqRebuild.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/RelocatableCompactionCoordinator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/RelocatableIntegrityScanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/RelocatableMaintenanceLoop.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/RelocatableMaintenanceService.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/RelocatablePageCompactionScheduler.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/RelocatableSegmentRelocationScheduler.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/VectorCodebookReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/VectorIndexStateReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/VectorPagePoolReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/VectorRawReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/Catalog.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogPage.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogRetireQueueReclaimer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogSegmentRelocationCoordinator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogSegmentRetirementCoordinator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataDirectoryEvacuation.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPagePool.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPoolOwnership.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataReservationCoordinator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataRetirement.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataStoreLifecycle.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogRadix.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogTree.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/LocationDirectory.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPageFile.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataSnapshotRegistry.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataWriteBatch.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/StateRecord.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/StateRecordStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/LargeValue.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/LargeValueStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/PropertyCollectionRoot.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/PropertyCollectionRootStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/PropertyValueCodec.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/ConstraintDefinition.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/ConstraintMutationValidator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/ConstraintSupportTree.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/NativeConstraintSupportTreeBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/NativeConstraintDefinitionWriter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/IndexDefinition.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/SystemCatalog.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/TokenDefinition.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/DeterministicProductQuantizer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/RelocatableVectorSearch.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/RelocatableVectorSemanticScanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorAdjacency.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorAdjacencyMapping.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorAdjacencyPageStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorCodebook.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorCodebookExtent.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorCodebookStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorDeltaMapping.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorManifestPage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorManifestStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorNodeMapping.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorPage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorPageStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorPqMapping.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/vector/VectorSemanticCodec.cpp"
)

set(ZYX_NATIVE_UTILITY_SOURCES
    "${ZYX_SOURCE_ROOT}/utils/ChecksumUtils.cpp"
    "${ZYX_SOURCE_ROOT}/utils/CompressUtils.cpp"
    "${ZYX_SOURCE_ROOT}/utils/Sha256.cpp"
)

set(ZYX_NATIVE_CLI_SOURCES
    "${ZYX_SOURCE_ROOT}/cli/CommandLineInterface.cpp"
    "${ZYX_SOURCE_ROOT}/cli/CypherCompleter.cpp"
    "${ZYX_SOURCE_ROOT}/cli/DebugCommandHandler.cpp"
    "${ZYX_SOURCE_ROOT}/cli/ImportCommand.cpp"
    "${ZYX_SOURCE_ROOT}/cli/Repl.cpp"
)

set(ZYX_CORE_SOURCES
    ${ZYX_NATIVE_API_SOURCES}
    ${ZYX_NATIVE_CORE_SOURCES}
    ${ZYX_NATIVE_QUERY_SOURCES}
    ${ZYX_NATIVE_STORAGE_SOURCES}
    ${ZYX_NATIVE_UTILITY_SOURCES}
    ${ZYX_NATIVE_CLI_SOURCES}
)
if(ZYX_WASM OR NOT ZYX_BUILD_APPS)
    zyx_filter_out_regex(ZYX_CORE_SOURCES "/src/cli/")
endif()
zyx_normalize_source_list(ZYX_CORE_SOURCES ${ZYX_CORE_SOURCES})

# Tests are isolated in the same way as production sources. In particular,
# tests for FileStorage, DataManager, and physical operators must not make
# discarded implementation details part of the native binary through a broad
# tests/**/*.cpp glob.
set(ZYX_NATIVE_TEST_SOURCES
    "${PROJECT_SOURCE_DIR}/tests/src/test_main.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_ErrorHandling.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_GraphValues.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Ingest.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Lifecycle.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_NestedValues.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_ResultCursor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Transactions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_ValueBuilders.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Bytes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiDatabaseImpl_Bytes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_Edge.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_Node.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_PropertyTypes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_PropertyTypes_CompositeAndHash.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_PropertyTypes_Bytes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_TemporalTypes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/debug/test_PerfTrace.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryEngineLogicalBackend.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryResult.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphCursors.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphReadExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphReadExecutor_Validation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanLoadCsvRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanMutationRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanPatternExpressionRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanSchemaRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanUnionRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanUnwindRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanVarLengthRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_Record.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/ir/test_LogicalPlanBuilder_Coverage.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/ir/test_SemanticAnalyzer.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherLogicalOnlyParser.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/planner/test_PipelineValidator.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/planner/test_NativeProcedureRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/test_QueryGuard.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphReadContract.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphTransactionOverlay.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_StorageFormat.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_StorageIO.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/utils/test_ChecksumUtils.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/utils/test_CompressUtils.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/utils/test_Sha256.cpp"
)

# The relocatable directory is the native storage implementation's bounded
# test module.
file(GLOB_RECURSE ZYX_RELOCATABLE_TEST_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tests/src/storage/relocatable/test_*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/relocatable/**/test_*.cpp"
)
file(GLOB_RECURSE ZYX_INPUTXX_TEST_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/lib/inputxx/tests/test_*.cpp"
    "${PROJECT_SOURCE_DIR}/lib/inputxx/tests/**/test_*.cpp"
)

set(ZYX_TEST_SOURCES
    ${ZYX_NATIVE_TEST_SOURCES}
    ${ZYX_RELOCATABLE_TEST_SOURCES}
    ${ZYX_INPUTXX_TEST_SOURCES}
)
zyx_normalize_source_list(ZYX_TEST_SOURCES ${ZYX_TEST_SOURCES})
