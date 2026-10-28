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

# Native targets may not silently reacquire a retired implementation merely by
# adding a file to an allowlist. This guard keeps a later source-list edit from
# recreating a second execution or storage path from retired implementation
# names. Historical design notes remain in documentation only.
function(zyx_assert_no_retired_native_paths list_variable)
    foreach(path IN LISTS ${list_variable})
        if(path MATCHES "/(query/(algorithm|execution/(operators|PhysicalOperator|QueryExecutor|NodeBatchLoader|NodeCandidateSource|NodeColumnBatch|NodeColumnBatchMaterializer|NodeColumnarPredicateCounter|NodeMetadataColumnLoader|NodeMetadataFilter|NodePropertyColumnLoader|NodeScanRequirementUtils|NodeScanRequirements|PropertyColumnLoadKernel|PropertyPredicateKernel|PropertyPredicateScanKernel|RelationshipAdjacencyCursor|RelationshipCandidateSource|RelationshipColumnarCountKernel|RelationshipExpandConfig|RelationshipFrontierExecutor|RelationshipMetadataColumnLoader|RelationshipPropertyColumnLoader|ScanConfigs|StorageScalarValueAdapter|VectorizedPredicate)|optimizer)|query/planner/(Physical|NodeAccessPath|NodeAggregateScan|NodeProjectionScan|NodeTopKScan|QueryPlanner|Relationship)|storage/(data|dictionaries|indexes|state|wal|constraints)|vector/(index|quantization)|traversal|config)/" OR
           path MATCHES "/src/vector/" OR
           path MATCHES "/storage/(FileHeaderManager|FileStorage|SegmentAllocator|SegmentTracker|StorageBootstrap|StorageReader|StorageWriter)\\.(c|cc|cpp|cxx)$")
            message(FATAL_ERROR "Native source/test list contains retired implementation path: ${path}")
        endif()
    endforeach()
endfunction()

# The allowlists above prevent direct re-entry of retired sources. Follow local
# quoted includes as well so a native source cannot reintroduce retired Vector
# headers indirectly. System and third-party angle-bracket includes stay outside
# this project-boundary audit.
function(zyx_visit_native_include_closure path)
    get_property(visited GLOBAL PROPERTY ZYX_NATIVE_INCLUDE_GUARD_VISITED)
    list(FIND visited "${path}" visited_index)
    if(NOT visited_index EQUAL -1)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY ZYX_NATIVE_INCLUDE_GUARD_VISITED "${path}")

    file(READ "${path}" contents)
    string(REGEX MATCHALL "#[ \t]*include[ \t]*\\\"[^\\\"]+\\\"" include_directives "${contents}")
    get_filename_component(containing_directory "${path}" DIRECTORY)
    foreach(include_directive IN LISTS include_directives)
        string(REGEX REPLACE ".*\\\"([^\\\"]+)\\\"" "\\1" include_path "${include_directive}")
        set(include_candidates
            "${containing_directory}/${include_path}"
            "${PROJECT_SOURCE_DIR}/include/${include_path}"
            "${PROJECT_SOURCE_DIR}/src/${include_path}"
            "${PROJECT_SOURCE_DIR}/${include_path}"
        )
        foreach(include_candidate IN LISTS include_candidates)
            if(EXISTS "${include_candidate}" AND NOT IS_DIRECTORY "${include_candidate}")
                file(REAL_PATH "${include_candidate}" resolved_include)
                if(resolved_include MATCHES "/include/graph/vector/(core|index|quantization)/" OR
                   resolved_include MATCHES "/src/vector/")
                    message(FATAL_ERROR
                        "Native source/test include closure reaches retired Vector path: ${resolved_include} "
                        "(included from ${path})")
                endif()
                zyx_visit_native_include_closure("${resolved_include}")
                break()
            endif()
        endforeach()
    endforeach()
endfunction()

function(zyx_assert_no_retired_native_include_closure list_variable)
    set_property(GLOBAL PROPERTY ZYX_NATIVE_INCLUDE_GUARD_VISITED "")
    foreach(path IN LISTS ${list_variable})
        if(EXISTS "${path}" AND NOT IS_DIRECTORY "${path}")
            file(REAL_PATH "${path}" resolved_path)
            zyx_visit_native_include_closure("${resolved_path}")
        endif()
    endforeach()
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
    "${ZYX_SOURCE_ROOT}/api/PreparedQuery.cpp"
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
	"${ZYX_SOURCE_ROOT}/concurrent/ParallelOperatorExecutor.cpp"
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
	"${ZYX_SOURCE_ROOT}/query/NativeConfigurationService.cpp"
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
    "${ZYX_SOURCE_ROOT}/query/gds/LogicalGraphProjection.cpp"
	"${ZYX_SOURCE_ROOT}/query/gds/LogicalGraphAlgorithms.cpp"
    "${ZYX_SOURCE_ROOT}/query/gds/LogicalProjectionCatalog.cpp"
	"${ZYX_SOURCE_ROOT}/query/gds/LogicalProjectionSpecCodec.cpp"
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
    "${ZYX_SOURCE_ROOT}/query/expressions/QuantifierFunctionExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/ReduceExpression.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/StringFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/TemporalFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/TypeConversionFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/expressions/UtilityFunctions.cpp"
    "${ZYX_SOURCE_ROOT}/query/ir/LogicalPlanBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/query/ir/SemanticAnalyzer.cpp"
    "${ZYX_SOURCE_ROOT}/query/logical/LogicalOperator.cpp"
	"${ZYX_SOURCE_ROOT}/query/optimization/LogicalPlanOptimizer.cpp"
    "${ZYX_SOURCE_ROOT}/query/planner/PipelineValidator.cpp"
    "${ZYX_SOURCE_ROOT}/query/planner/ProcedureRegistry.cpp"
)

set(ZYX_NATIVE_STORAGE_SOURCES
    "${ZYX_SOURCE_ROOT}/storage/LogicalGraphTransactionOverlay.cpp"
    "${ZYX_SOURCE_ROOT}/storage/LogicalGraphPropertyPatch.cpp"
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
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeRecord.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeRun.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeRunStore.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeEntityPageBatcher.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeAdjacencyRunBuilder.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeTokenRunBuilder.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgePublicationPreflight.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgeRunReducer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgePrivateRunMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgePrivateFamilyMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgePrivateRunSet.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeBulkEdgePrivateTokenResolver.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphPublicationShape.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphPageRanges.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphCandidateLayout.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphPrivatePreparation.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphEntityPageMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphPropertyPageMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphLargeValuePlanner.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphLargeValuePrivateMaterializer.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphLargeValueContentIndexPlanner.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphCatalogIndexPlanner.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphAdjacencyIndexPlanner.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphPublicationIndexPlanner.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeStreamedGraphMembershipIndexPlanner.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphMutationExecutor.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphWritePlanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphWriteLifecycle.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphWriteCommit.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphWriteAdjacencyStaging.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeGraphWriteVectorStaging.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeScalarIndexMutationPlanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/NativeVectorMutationPlanner.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/PageBackedConstraintMutationAdapter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/PageBackedGraphReadAdapter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/ScanReadProof.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/Format.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/Superblock.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/SuperblockFile.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/format/SuperblockPair.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/adjacency/AdjacencyKeyCodec.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/adjacency/GlobalAdjacencyIndex.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/adjacency/GlobalAdjacencyMutationMerger.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BTreeIndex.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BTreePage.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BTreePageStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/BulkBTreeBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/ExternalBTreeEntrySorter.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/IndexKeyCodec.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/InitialBTreeBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/NativeBulkRun.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/LargeValueMaintenanceJournal.cpp"
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
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPagePoolAvailability.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPagePoolRetirement.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPoolOwnership.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataReservationCoordinator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataRetirement.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataStoreLifecycle.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogRadix.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/CatalogTree.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/LocationDirectory.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/LocationDirectoryTypedAccess.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPageFile.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPageRoute.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataPageRouteDirectory.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/PageDirectoryBootstrap.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataSnapshotRegistry.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataWriteBatch.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreIdentity.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreGraphMutation.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreStreamedAdjacency.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreScanMembership.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreScanMutation.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreScanMaintenance.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreNativeJournal.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreNativePoolRecovery.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/maintenance/NativeStorageAllocationCensus.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreVector.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStoreVectorRebuild.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/RelocatableMetadataStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataStoreLookup.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataStoreReclamation.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/MetadataStorePageMutation.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/StateRecord.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/metadata/StateRecordStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/LargeValue.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/LargeValueStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/PropertyCollectionRoot.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/HybridPropertyColdRowBlock.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/HybridPropertyColdRowInspection.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/PropertyCollectionRootStore.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/property/PropertyValueCodec.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/ConstraintDefinition.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/ConstraintMutationValidator.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/ConstraintSupportTree.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/NativeConstraintSupportTreeBuilder.cpp"
    "${ZYX_SOURCE_ROOT}/storage/relocatable/schema/NativeConstraintDefinitionWriter.cpp"
	"${ZYX_SOURCE_ROOT}/storage/relocatable/schema/NativeBulkEdgeCatalogTokenMaterializer.cpp"
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

# Every expression/logical query test in the scanned scope is explicitly
# classified. This keeps retired physical tests out of the native binary
# without silently dropping current logical tests.
set(ZYX_NATIVE_QUERY_TEST_SOURCES
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_EvaluationContext.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_ExpressionAcceptNonConst.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_ExpressionClone.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_EntityIntrospectionFunction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_ExpressionUtils.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_ExpressionUtils_Coverage.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_FunctionRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_FunctionRegistry_ListFunctions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_FunctionRegistry_MathFunctions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_FunctionRegistry_StringFunctions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_FunctionRegistry_TemporalFunctions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_FunctionRegistry_TypeConversion.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_HeterogeneousLists.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_MathFunctions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_MathFunctions_NullHandling.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_ShortestPathExpression.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/expressions/test_TemporalFunctions_Coverage.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/operators/test_LogicalTransactionControl.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalAdminOperators.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalLimitSkip_NullChild.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalNamedPath.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalOpTypeToString.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalReadOperator_NullGuards.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalReadOperators.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalSingleRowAndPlanPrinter.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalSubqueryOperators.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalTraversalOperators.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/logical/test_LogicalWriteOperators.cpp"
)

# Legacy query tests that remain outside the native source set are classified
# explicitly here. The former evaluator family is fully retired.
set(ZYX_HOLD_FOR_PORT_QUERY_TEST_SOURCES
)

# Tests are isolated in the same way as production sources. In particular,
# retired storage and physical-operator tests must not make discarded
# implementation details part of the native binary through a broad glob.
set(ZYX_NATIVE_TEST_SOURCES
    "${PROJECT_SOURCE_DIR}/tests/src/apps/test_CompareBenchmarkObservation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundSlotRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundPropertyBindings.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundColumnLayout.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_ResourceCoordinator.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_CoordinatedMaterializedBudget.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_ProcessResourceDomains.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_PrepaidMemoryResource.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedHeapReservation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedStdString.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedFunction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedSharedPtr.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedPropertyValue.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedStdVector.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedStringVector.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedOutputVariables.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedExpressionMap.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedMapProjectionItems.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedPropertyContainers.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedStringSet.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_QuerySpillRowCodec.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_ExternalQuerySort.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_ExternalQueryKeySet.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_MaterializedReservationEmbedded.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_DynamicValueArena.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundProjectionReferences.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundExecutionProgram.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundExpressionInputs.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BatchEnvelope.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundNodeScanBatch.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_PrimitiveColumnFilter.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundNodeColumnPredicates.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_DirectPrimitiveColumnFilter.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_StreamingPredicateCount.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanNodeColumnAggregate.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanCertifiedBatchHandoff.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_PrimitiveNumericColumns.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_FixedWidthBatch.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_FixedWidthColumn.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/test_main.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_BugVerification.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationAggregateProjection.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCrud.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherAdvancedFeatures.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherClauses.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherCombinations.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherExpressionDepth.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherExpressions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherFiltering.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherNewFeatures.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherPatterns.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationDatabase.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationConstraints.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationCypherWithPipeline.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationHeterogeneousLists.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationPersistence.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationQuery.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationTransaction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_IntegrationSubqueryFeatures.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_ReadOnlyTransaction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_RelationshipExpandCountScan.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_VectorizedNodeScan.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_ErrorHandling.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbiAgentMemorySigsegvRepro.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_GraphValues.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Ingest.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_IngestSchemaValidation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_IngestRebaseCorruption.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Lifecycle.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_NestedValues.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_ResultCursor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Transactions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_ValueBuilders.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_DriverAbi_Bytes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiAlgorithm.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiConcurrentExecution.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiCrud.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiDatabaseImpl_Bytes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiDatabaseImpl_ConversionAndTxn.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiDatabaseImpl_ErrorPaths.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiParameterizedQueries.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiPersistence.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiQueryClassification.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiResult.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiSequentialExecution.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiTransaction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiTransactionEdgeCases.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_ApiValueConversion.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/api/test_PreparedQuery.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_Edge.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_Node.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/core/test_NativeDatabaseLifecycle.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_NativeTransaction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_PropertyTypes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_PropertyTypes_CompositeAndHash.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_PropertyTypes_Bytes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/core/test_TemporalTypes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ParallelExecutionTypes.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ParallelOperatorExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ParallelScanExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ThreadPool.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ThreadPool_ExceptionAndParallel.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ThreadPool_Stopped.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ThreadPool_SubmitAndParallelFor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/concurrent/test_ThreadPool_WorkerLoopDirect.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/apps/test_CompareBenchmark.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/cli/test_CommandLineInterface.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/cli/test_CypherCompleter.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/cli/test_DebugCommandHandler.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/cli/test_ImportCommand.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/cli/test_Repl.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/debug/test_PerfTrace.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/debug/test_PerfTraceScopedObserver.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/log/test_Log.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryEngineAnchorPlanning.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryEngineTypePlanning.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryEngineLogicalBackend.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryEngineCacheIdentity.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_LogicalVectorProcedureBatch.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_LogicalVectorProcedureBatch_TopK.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_LogicalVectorProcedureBatch_Materialization.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_LogicalProcedureIdentity.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_LogicalVectorBufferedSort.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryEnginePublicContracts.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryResult.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/api/test_QueryResultResources.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_CsvReader.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphCursors.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphReadExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphReadExecutor_RuntimeContracts.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphReadExecutor_IndexLookupContracts.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalGraphReadExecutor_Validation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPropertyPredicate.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/gds/test_LogicalGraphProjection.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/gds/test_LogicalGdsResourceOwnership.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_OwnedLazyUnorderedMap.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/gds/test_LogicalProjectionSpecCodec.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/gds/test_LogicalGdsProcedures.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalAggregateAccumulator.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BoundedBatch.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanLoadCsvRuntime.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanDirectEdgeRuntime.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanDirectEdgeRuntime_Batching.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanFilterSelectedColumns.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanDirectEdgeRuntime_PatternBatching.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanDirectEdgeRuntime_Aggregate.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanTraversalTopologyPrefilter.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanMutationRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanMutationRuntime_Atomicity.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanMutationRuntime_Validation.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanPatternExpressionRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanPatternExpressionRuntime_Budget.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_GuardedIdGather.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_BudgetMemoryResource.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanDistinctAllocator.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_GuardedPropertyCopy.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanSchemaRuntime.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_SelectionVector.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanUnionRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanUnwindRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanSortGrowth.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanBoundedTopK.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanUnwindRuntime_Budget.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_LogicalPlanVarLengthRuntime.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_Record.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/execution/test_TypedValueKey.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/ir/test_LogicalPlanBuilder_Coverage.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/optimization/test_LogicalPlanOptimizer.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/cache/test_PlanCache.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/planner/test_LogicalPlanBuilderFlags.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/ir/test_SemanticAnalyzer.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/helpers/test_AstExtractor.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/helpers/test_ExpressionBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/helpers/test_ExpressionBuilder_Parsing.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherASTBuilder_ASTExtraction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherAdmin.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherAdvanced.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherAlgo.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherAggregateContracts.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherBasic.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherDataModification.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherGds.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherFunctionContracts.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherListLiterals.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherStructuralExpressions.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherLogicalOnlyParser.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherParserImpl_Reuse.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherTransaction.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherVector.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/ir/test_CypherASTBuilder_Clauses.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherProjectionSemantics.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherWithSemantics.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/test_CypherWithAggregationSemantics.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/syntax/test_SyntaxAdmin.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/syntax/test_SyntaxAdvanced.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/syntax/test_SyntaxBasic.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/parser/cypher/syntax/test_SyntaxVector.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/planner/test_PipelineValidator.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/planner/test_NativeProcedureRegistry.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/planner/test_ScopeStack.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/query/test_NativeConfigurationService.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/test_ExecMode.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/query/test_QueryGuard.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphReadContract.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphTransactionOverlay.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphTransactionOverlayUndo.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphPropertyPatch.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphTransactionOverlayExactCandidates.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_LogicalGraphTransactionOverlayVector.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/storage/test_CancellationCheckpoint.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_StorageFormat.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_StorageIO.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_StorageIOFileBudget.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_PreadHelper.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/storage/test_PwriteHelper.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/utils/test_ChecksumUtils.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/utils/test_CompressUtils.cpp"
    "${PROJECT_SOURCE_DIR}/tests/src/utils/test_Sha256.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/utils/test_Serializer.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/utils/test_TemporalSerialization.cpp"
	"${PROJECT_SOURCE_DIR}/tests/src/common/test_VectorMath.cpp"
    ${ZYX_NATIVE_QUERY_TEST_SOURCES}
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

# Every repository test source is classified at configure time. Sources in the
# native allowlist are executed by zyx_test_suite; explicitly listed migration
# specifications may remain outside that executable. An unclassified source is
# a configuration error so retired tests cannot quietly accumulate in checkout.
file(GLOB_RECURSE ZYX_PROJECT_TEST_CANDIDATES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/tests/src/test_*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/integration/test_*.cpp"
)
set(ZYX_HOLD_FOR_PORT_TEST_SOURCES ${ZYX_HOLD_FOR_PORT_QUERY_TEST_SOURCES})
set(ZYX_CLASSIFIED_TEST_SOURCES ${ZYX_TEST_SOURCES} ${ZYX_HOLD_FOR_PORT_TEST_SOURCES})
foreach(project_test_source IN LISTS ZYX_PROJECT_TEST_CANDIDATES)
    if(NOT project_test_source IN_LIST ZYX_CLASSIFIED_TEST_SOURCES)
        message(FATAL_ERROR
            "Project test source is unclassified; add it to ZYX_NATIVE_TEST_SOURCES, "
            "a relocatable/inputxx source set, or an explicit HOLD_FOR_PORT list: ${project_test_source}")
    endif()
endforeach()
zyx_normalize_source_list(ZYX_HOLD_FOR_PORT_TEST_SOURCES ${ZYX_HOLD_FOR_PORT_TEST_SOURCES})
zyx_normalize_source_list(ZYX_CLASSIFIED_TEST_SOURCES ${ZYX_CLASSIFIED_TEST_SOURCES})
zyx_assert_no_retired_native_paths(ZYX_CORE_SOURCES)
zyx_assert_no_retired_native_paths(ZYX_TEST_SOURCES)
set(ZYX_NATIVE_INCLUDE_GUARD_SOURCES
    ${ZYX_CORE_SOURCES}
    ${ZYX_TEST_SOURCES}
    "${PROJECT_SOURCE_DIR}/apps/vector_calibration/main.cpp"
    "${PROJECT_SOURCE_DIR}/apps/vector_build_calibration/main.cpp"
)
zyx_assert_no_retired_native_include_closure(ZYX_NATIVE_INCLUDE_GUARD_SOURCES)
