#include "query/execution/OrderByComparison.hpp"
/**
 * @file LogicalPlanProcedureRuntime.cpp
 * @brief Implementation of bounded logical stored procedures.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/StringBufferBytes.hpp"
#include "query/execution/ProcedurePayloadAdmission.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/OwnedStdVector.hpp"
#include "graph/query/NativeProcedureManifest.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/gds/LogicalGraphAlgorithms.hpp"
#include "graph/query/gds/LogicalProjectionSpecCodec.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"

namespace graph::query::execution {

	namespace {
#include "procedure_runtime/ExpressionsAndBindings.inc"
#include "procedure_runtime/BufferedRows.inc"
#include "procedure_runtime/PrefetchAndResults.inc"
#include "procedure_runtime/VectorOutputAndTopK.inc"
	} // namespace

	bool LogicalPlanProcedureRuntime::supportsNativeProcedure(const std::string_view name) noexcept {
		const NativeProcedureManifestEntry *entry = findNativeProcedureManifestEntry(name);
		if (entry == nullptr) {
			return false;
		}
		switch (entry->kind) {
			case NativeProcedureKind::NPK_SHORTEST_PATH:
			case NativeProcedureKind::NPK_VECTOR_QUERY_NODES:
			case NativeProcedureKind::NPK_VECTOR_TRAIN:
			case NativeProcedureKind::NPK_GDS_GRAPH_PROJECT:
			case NativeProcedureKind::NPK_GDS_GRAPH_DROP:
			case NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL:
			case NativeProcedureKind::NPK_GDS_GRAPH_EXISTS:
			case NativeProcedureKind::NPK_GDS_GRAPH_LIST:
			case NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA:
			case NativeProcedureKind::NPK_GDS_PAGE_RANK:
			case NativeProcedureKind::NPK_GDS_WCC:
			case NativeProcedureKind::NPK_GDS_BETWEENNESS:
			case NativeProcedureKind::NPK_GDS_CLOSENESS:
			case NativeProcedureKind::NPK_GDS_LEIDEN:
			case NativeProcedureKind::NPK_GDS_DIJKSTRA:
			case NativeProcedureKind::NPK_DBMS_LIST_CONFIG:
			case NativeProcedureKind::NPK_DBMS_GET_CONFIG:
			case NativeProcedureKind::NPK_DBMS_SET_CONFIG:
			case NativeProcedureKind::NPK_DBMS_SHOW_STATS:
			case NativeProcedureKind::NPK_DBMS_RESET_STATS:
			case NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES:
			case NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES:
				return true;
		}
		return false;
	}

	QueryResult LogicalPlanProcedureRuntime::execute(
			const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
			size_t maximumVectorMappingEntries, size_t maximumRawVectorReads, size_t vectorCandidateSearchBudget,
			size_t vectorCandidateRefinementBudget, size_t maximumVectorTrainingRecords,
			size_t maximumVectorTrainingPages, const size_t maximumGdsAlgorithmEdges,
			const size_t maximumGdsAlgorithmIterations, const size_t maximumGdsAlgorithmWorkUnits,
			const QueryContext &context, const LogicalProcedureExecutionScope scope) const {
        context.checkGuardNow();
        detail::ProcedureQueryCheckpointScope procedureCheckpoints(context);
        std::optional<utils::OwnedAllocationScope> allocation;
        if (context.resource_account) {
            const auto domain = read_backend_.queryResourceDomain();
            if (context.resource_account->resourceClass() != detail::ResourceClass::RCK_QUERY ||
                (domain && !context.resource_account->belongsTo(*domain)))
                throw std::invalid_argument("procedure execution requires QUERY ownership within its database");
            allocation.emplace(std::make_shared<MaterializedByteBudget>(readBounds.maximum_materialized_bytes,
                context.resource_account, std::pmr::new_delete_resource()));
        }
        const logical::LogicalOperator *current = &root;
        utils::OwnedStdVector<const logical::LogicalOperator *> operations;
		while (current->getType() == logical::LogicalOpType::LOP_PROJECT ||
			   current->getType() == logical::LogicalOpType::LOP_FILTER ||
			   current->getType() == logical::LogicalOpType::LOP_SKIP ||
			   current->getType() == logical::LogicalOpType::LOP_LIMIT ||
			   current->getType() == logical::LogicalOpType::LOP_SORT) {
			if ((current->getType() == logical::LogicalOpType::LOP_PROJECT &&
				 dynamic_cast<const logical::LogicalProject *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_FILTER &&
				 dynamic_cast<const logical::LogicalFilter *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_SKIP &&
				 dynamic_cast<const logical::LogicalSkip *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_LIMIT &&
				 dynamic_cast<const logical::LogicalLimit *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_SORT &&
				 dynamic_cast<const logical::LogicalSort *>(current) == nullptr)) {
				throw std::logic_error("logical Vector procedure runtime received a malformed unary operator");
			}
			operations.push_back(current);
			current = requireOnlyChild(*current);
		}
		std::ranges::reverse(operations);
		if (current->getType() != logical::LogicalOpType::LOP_CALL_PROCEDURE) {
			throw std::logic_error("logical procedure runtime requires a CallProcedure root");
		}
		const auto *call = dynamic_cast<const logical::LogicalCallProcedure *>(current);
		if (call == nullptr) {
			throw std::logic_error("logical procedure runtime received a malformed CallProcedure operator");
		}
		const auto arguments = call->resolveArgumentView(context.parameters);
		if (!supportsNativeProcedure(call->getProcedureName())) {
			// The parser accepted a valid CALL, but this native backend has no handler
			// for it. Keep that public capability error distinct from a malformed plan.
			throw std::runtime_error("logical backend does not support this stored procedure");
		}
		const NativeProcedureManifestEntry *procedure = findNativeProcedureManifestEntry(call->getProcedureName());
		if (procedure == nullptr) { // ZYX_COV_EXCL_LINE: supportsNativeProcedure above proves manifest membership.
			throw std::logic_error("logical procedure manifest lookup became inconsistent");
		}
		if (scope == LogicalProcedureExecutionScope::LPES_TRANSACTION_OVERLAY) {
			switch (nativeProcedureTransactionMode(procedure->kind)) {
				case NativeProcedureTransactionMode::NPTM_OVERLAY_SNAPSHOT_READ:
					break;
				case NativeProcedureTransactionMode::NPTM_TRANSACTION_LOCAL_CATALOG:
					if (projection_catalog_ == nullptr) {
						throw std::runtime_error("stored procedure requires a transaction-local projection catalog");
					}
					break;
				case NativeProcedureTransactionMode::NPTM_AUTOCOMMIT_ONLY:
					throw std::runtime_error(
							"stored procedure is autocommit-only because it has no transactional overlay semantics");
			}
		}
		if (readBounds.maximum_rows == 0U) {
			throw std::invalid_argument("logical procedure runtime requires a positive result-row bound");
		}
		// This is the shared backend-acquisition boundary for every manifest entry.
		// It must not rely on periodic sampling: an already expired query may not
		// even open a snapshot, maintenance operation, or administration service.
		context.checkGuardNow();
		if (procedure->kind == NativeProcedureKind::NPK_DBMS_LIST_CONFIG ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_GET_CONFIG ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_SET_CONFIG ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_SHOW_STATS ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_RESET_STATS ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES) {
			if (configuration_service_ == nullptr) {
				throw std::runtime_error("logical backend does not provide native administration services");
			}
			const auto requireConfigurationKey = [](const PropertyValue &value, const std::string_view procedureName) {
				if (value.getType() != PropertyType::STRING || std::get<std::string>(value.getVariant()).empty()) {
					throw std::invalid_argument(std::string(procedureName) +
												" requires a nonempty String configuration key");
				}
				return std::get<std::string>(value.getVariant());
			};
			ProcedureRows rows;
			utils::OwnedStringVector defaultColumns;
			std::optional<std::pair<std::string, PropertyValue>> deferredConfigurationUpdate;
			bool deferredStatisticsReset = false;
			bool deferredSlowQueryClear = false;
			switch (procedure->kind) {
				case NativeProcedureKind::NPK_DBMS_LIST_CONFIG: {
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.listConfig accepts no arguments");
					}
					for (const storage::LogicalGraphConfiguration &configuration: configuration_service_->list()) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "key", makeProcedurePropertyValue(configuration.key));
						detail::procedureMapEmplace(row.row.values, "value", configuration.value);
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.listConfig");
					}
					defaultColumns = {"key", "value"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_GET_CONFIG: {
					if (arguments.size() != 1U) {
						throw std::invalid_argument("dbms.getConfig expects one configuration key");
					}
					const std::string key = requireConfigurationKey(arguments.front(), "dbms.getConfig");
					const storage::LogicalGraphConfiguration configuration = configuration_service_->get(key);
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "key", makeProcedurePropertyValue(configuration.key));
					detail::procedureMapEmplace(row.row.values, "value", configuration.value);
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.getConfig");
					defaultColumns = {"key", "value"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_SET_CONFIG: {
					if (arguments.size() != 2U) {
						throw std::invalid_argument("dbms.setConfig expects (key, value)");
					}
					const std::string key = requireConfigurationKey(arguments.front(), "dbms.setConfig");
					deferredConfigurationUpdate.emplace(key, arguments[1]);
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "key", makeProcedurePropertyValue(key));
					detail::procedureMapEmplace(row.row.values, "value", arguments[1]);
					detail::procedureMapEmplace(row.row.values, "status", makeProcedurePropertyValue("OK"));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.setConfig");
					defaultColumns = {"key", "value", "status"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_SHOW_STATS:
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.showStats accepts no arguments");
					}
					for (const storage::LogicalGraphRuntimeStatistic &statistic: configuration_service_->statistics()) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "category", makeProcedurePropertyValue(statistic.category));
						detail::procedureMapEmplace(row.row.values, "metric", makeProcedurePropertyValue(statistic.metric));
						detail::procedureMapEmplace(row.row.values, "value", statistic.value);
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.showStats");
					}
					defaultColumns = {"category", "metric", "value"};
					break;
				case NativeProcedureKind::NPK_DBMS_RESET_STATS: {
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.resetStats accepts no arguments");
					}
					deferredStatisticsReset = true;
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "result", makeProcedurePropertyValue("Statistics reset"));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.resetStats");
					defaultColumns = {"result"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES:
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.listSlowQueries accepts no arguments");
					}
					for (const NativeSlowQueryRecord &record: configuration_service_->slowQueries()) {
						context.checkGuard();
						if (record.sequence > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
							record.elapsed_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							throw std::runtime_error("native slow-query recorder returned an unrepresentable value");
						}
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "sequence", makeProcedurePropertyValue(static_cast<int64_t>(record.sequence)));
						detail::procedureMapEmplace(row.row.values, "logicalPlan", makeProcedurePropertyValue(record.logical_plan_type));
						detail::procedureMapEmplace(row.row.values, "elapsedMs", makeProcedurePropertyValue(static_cast<int64_t>(record.elapsed_ms)));
						detail::procedureMapEmplace(row.row.values, "succeeded", makeProcedurePropertyValue(record.succeeded));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "dbms.listSlowQueries");
					}
					defaultColumns = {"sequence", "logicalPlan", "elapsedMs", "succeeded"};
					break;
				case NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES: {
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.clearSlowQueries accepts no arguments");
					}
					deferredSlowQueryClear = true;
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "result", makeProcedurePropertyValue("Slow query records cleared"));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "dbms.clearSlowQueries");
					defaultColumns = {"result"};
					break;
				}
				default:
					throw std::logic_error("logical administration procedure dispatch became inconsistent");
			}
			const bool hasDeferredAdministrationMutation = deferredConfigurationUpdate.has_value() ||
																				 deferredStatisticsReset || deferredSlowQueryClear;
			if (hasDeferredAdministrationMutation) {
				// Fully materialize the result pipeline before the administrative write.
				// A failing YIELD/WHERE/RETURN must not leave a durable configuration or
				// observability mutation behind after the client observes an error.
				QueryResult result = makeAdmittedProcedureResult(context);
				if (operations.empty()) {
					result = makeProcedureResult(rows, defaultColumns, readBounds, context);
				} else {
					context.checkGuard();
					std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
							read_backend_.openLogicalGraphReadSnapshot();
					if (!snapshot) {
						throw std::runtime_error("logical administration procedure backend could not open a read snapshot");
					}
					result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
							0U, nullptr, true);
				}
				context.checkGuardNow();
				if (deferredConfigurationUpdate.has_value()) {
					configuration_service_->set(deferredConfigurationUpdate->first, deferredConfigurationUpdate->second);
				} else if (deferredStatisticsReset) {
					configuration_service_->resetStatistics();
				} else {
					configuration_service_->resetSlowQueries();
				}
				return result;
			}
			if (operations.empty()) {
				return makeProcedureResult(std::move(rows), defaultColumns, readBounds, context);
			}
			context.checkGuard();
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = read_backend_.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical administration procedure backend could not open a read snapshot");
			}
			return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
					0U, nullptr, true);
		}
		if (procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_PROJECT ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_DROP ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_EXISTS ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_LIST ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA ||
			procedure->kind == NativeProcedureKind::NPK_GDS_PAGE_RANK ||
			procedure->kind == NativeProcedureKind::NPK_GDS_WCC ||
			procedure->kind == NativeProcedureKind::NPK_GDS_BETWEENNESS ||
			procedure->kind == NativeProcedureKind::NPK_GDS_CLOSENESS ||
			procedure->kind == NativeProcedureKind::NPK_GDS_LEIDEN ||
			procedure->kind == NativeProcedureKind::NPK_GDS_DIJKSTRA) {
			if (projection_catalog_ == nullptr) {
				throw std::runtime_error("logical backend does not provide a projection catalog");
			}
			if (readBounds.maximum_node_records == 0U || readBounds.maximum_metadata_pages_per_node_page == 0U ||
				readBounds.maximum_edges_per_node == 0U || readBounds.maximum_rows == 0U) {
				throw std::invalid_argument("logical projection procedures require positive graph read bounds");
			}
			context.checkGuard();
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = read_backend_.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical projection procedure backend could not open a read snapshot");
			}

			ProcedureRows rows;
			utils::OwnedStringVector defaultColumns;
			size_t retainedProcedureSourceBytes = 0U;
			std::unique_ptr<gds::PreparedLogicalProjectionCatalogMutation> deferredCatalogMutation;
			switch (procedure->kind) {
				case NativeProcedureKind::NPK_GDS_GRAPH_PROJECT: {
					utils::OwnedStdVector<std::reference_wrapper<const PropertyValue>> projectionArguments;
					projectionArguments.reserve(arguments.size());
					for (size_t index = 0U; index < arguments.size(); ++index) {
						projectionArguments.emplace_back(arguments[index]);
					}
					const gds::LogicalGraphProjectionSpec spec =
							gds::LogicalProjectionSpecCodec::decodeGraphProjectArguments(
									std::span<const std::reference_wrapper<const PropertyValue>>(projectionArguments));
					gds::LogicalProjectionCatalogCreatePreparation preparation = [&] {
                        utils::OwnedAllocationScope cacheAllocation(projection_memory_owner_
                            ? projection_memory_owner_ : utils::OwnedAllocationScope::owner());
                        return projection_catalog_->prepareCreate(
							spec, *snapshot,
							{.maximum_nodes = readBounds.maximum_node_records,
							 .maximum_metadata_pages = readBounds.maximum_metadata_pages_per_node_page,
							 .maximum_edges_per_node = readBounds.maximum_edges_per_node,
							 .maximum_property_bytes = readBounds.maximum_materialized_bytes},
							[&context] { context.checkGuard(); });
                    }();
					if (!preparation.mutation) {
						throw std::logic_error("logical projection catalog returned no prepared creation operation");
					}
					appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(preparation.descriptor), readBounds.maximum_rows,
													  "gds.graph.project");
					deferredCatalogMutation = std::move(preparation.mutation);
					defaultColumns = {"name", "nodeCount", "edgeCount"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_DROP: {
					const auto name = requireProjectionName(arguments, "gds.graph.drop", 1U);
					deferredCatalogMutation = projection_catalog_->prepareDrop(name);
					if (!deferredCatalogMutation) {
						throw std::out_of_range("logical projection catalog does not contain this name");
					}
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "name", makeProcedurePropertyValue(name));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.graph.drop");
					defaultColumns = {"name"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL: {
					if (!arguments.empty()) {
						throw std::invalid_argument("gds.graph.dropAll accepts no arguments");
					}
					gds::LogicalProjectionCatalogDropAllPreparation preparation = projection_catalog_->prepareDropAll();
					if (!preparation.mutation) {
						throw std::logic_error("logical projection catalog returned no prepared clear operation");
					}
					const size_t dropped = preparation.dropped_count;
					if (dropped > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
						throw std::overflow_error("logical projection catalog drop count cannot be represented");
					}
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "droppedCount", makeProcedurePropertyValue(static_cast<int64_t>(dropped)));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.graph.dropAll");
					deferredCatalogMutation = std::move(preparation.mutation);
					defaultColumns = {"droppedCount"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_EXISTS: {
					const auto name = requireProjectionName(arguments, "gds.graph.exists", 1U);
					BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
					detail::procedureMapEmplace(row.row.values, "name", makeProcedurePropertyValue(name));
					detail::procedureMapEmplace(row.row.values, "exists", makeProcedurePropertyValue(projection_catalog_->exists(name)));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.graph.exists");
					defaultColumns = {"name", "exists"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_LIST: {
					if (arguments.size() > 1U) {
						throw std::invalid_argument("gds.graph.list accepts zero or one projection name");
					}
					if (arguments.empty()) {
						for (const gds::LogicalProjectionDescriptor &descriptor: projection_catalog_->listOwned(
									 *snapshot, readBounds.maximum_rows, [&context] { context.checkGuard(); })) {
							context.checkGuard();
							appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(descriptor), readBounds.maximum_rows,
													  "gds.graph.list");
						}
					} else {
						const auto name = requireProjectionName(arguments, "gds.graph.list", 1U);
						// Listing one name is a filtered enumeration: a missing entry is an
						// empty result, while gds.graph.schema remains the strict lookup API.
						try {
							appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(projection_catalog_->describe(name, *snapshot)),
													  readBounds.maximum_rows, "gds.graph.list");
						} catch (const std::out_of_range &) {
							// A concurrent drop is indistinguishable from a missing filtered
							// entry and therefore has the same empty-list result.
						}
					}
					defaultColumns = {"name",			"nodeCount",	   "edgeCount",
									  "isWeighted",		"createdAtMillis", "buildMillis",
									  "memoryBytes",	"hasCsr",		   "csrMemoryBytes",
									  "sourceRevision", "currentRevision", "stale",
									  "nodeLabels",		"relationships"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA: {
					const auto name = requireProjectionName(arguments, "gds.graph.schema", 1U);
					appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(projection_catalog_->describe(name, *snapshot)),
												  readBounds.maximum_rows, "gds.graph.schema");
					defaultColumns = {"name", "nodeLabels", "relationships", "stale"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_PAGE_RANK: {
					if (arguments.empty() || arguments.size() > 3U) {
						throw std::invalid_argument(
								"gds.pageRank.stream expects (graphName[, maxIterations[, dampingFactor]])");
					}
					gds::LogicalPageRankOptions options;
					if (arguments.size() >= 2U) {
						options.maximum_iterations = static_cast<size_t>(
								requireGdsPositiveInteger(arguments[1], "gds.pageRank.stream maxIterations"));
					}
					if (arguments.size() == 3U) {
						options.damping_factor =
								requireGdsFiniteNumber(arguments[2], "gds.pageRank.stream dampingFactor");
					}
					const auto name = requireProjectionName(arguments, "gds.pageRank.stream", arguments.size());
					const auto scores =
							gds::LogicalGraphAlgorithms::pageRankOwned(*projection_catalog_->get(name), options,
								   {.maximum_nodes = readBounds.maximum_node_records,
									.maximum_edges = maximumGdsAlgorithmEdges,
									.maximum_iterations = maximumGdsAlgorithmIterations,
									.maximum_work_units = maximumGdsAlgorithmWorkUnits,
									.maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									   [&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, scores.size(), readBounds,
							checkedProcedureSizeMul(scores.capacity(), sizeof(gds::LogicalGdsNodeScore)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "nodeId", makeProcedurePropertyValue(checkedOutputNodeId(score.node_id)));
						detail::procedureMapEmplace(row.row.values, "score", makeProcedurePropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.pageRank.stream",
								&retainedBytes, outputByteAllowance);
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_WCC: {
					const auto name = requireProjectionName(arguments, "gds.wcc.stream", 1U);
					const auto components =
							gds::LogicalGraphAlgorithms::weaklyConnectedComponentsOwned(
									*projection_catalog_->get(name),
								{.maximum_nodes = readBounds.maximum_node_records,
								 .maximum_edges = maximumGdsAlgorithmEdges,
								 .maximum_iterations = maximumGdsAlgorithmIterations,
								 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
								 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									[&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, components.size(), readBounds,
							checkedProcedureSizeMul(components.capacity(), sizeof(gds::LogicalGdsNodeComponent)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeComponent &component: components) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "nodeId", makeProcedurePropertyValue(checkedOutputNodeId(component.node_id)));
						detail::procedureMapEmplace(row.row.values, "componentId",
											   makeProcedurePropertyValue(checkedOutputNodeId(component.component_id)));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.wcc.stream",
								&retainedBytes, outputByteAllowance);
					}
					defaultColumns = {"nodeId", "componentId"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_BETWEENNESS: {
					if (arguments.empty() || arguments.size() > 2U) {
						throw std::invalid_argument("gds.betweenness.stream expects (graphName[, samplingSize])");
					}
					gds::LogicalBetweennessOptions options;
					if (arguments.size() == 2U) {
						options.sampling_size =
								requireGdsNonNegativeInteger(arguments[1], "gds.betweenness.stream samplingSize");
					}
					const std::string name =
							requireProjectionName(arguments, "gds.betweenness.stream", arguments.size());
					const auto scores =
							gds::LogicalGraphAlgorithms::betweennessCentralityOwned(
									*projection_catalog_->get(name), options,
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
									 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									[&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, scores.size(), readBounds,
							checkedProcedureSizeMul(scores.capacity(), sizeof(gds::LogicalGdsNodeScore)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "nodeId", makeProcedurePropertyValue(checkedOutputNodeId(score.node_id)));
						detail::procedureMapEmplace(row.row.values, "score", makeProcedurePropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.betweenness.stream", &retainedBytes,
													  outputByteAllowance);
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_CLOSENESS: {
					const auto name = requireProjectionName(arguments, "gds.closeness.stream", 1U);
					const auto scores =
							gds::LogicalGraphAlgorithms::closenessCentralityOwned(
									*projection_catalog_->get(name),
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
									 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									[&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, scores.size(), readBounds,
							checkedProcedureSizeMul(scores.capacity(), sizeof(gds::LogicalGdsNodeScore)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "nodeId", makeProcedurePropertyValue(checkedOutputNodeId(score.node_id)));
						detail::procedureMapEmplace(row.row.values, "score", makeProcedurePropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.closeness.stream", &retainedBytes,
													  outputByteAllowance);
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_LEIDEN: {
					if (arguments.empty() || arguments.size() > 2U) {
						throw std::invalid_argument("gds.leiden.stream expects (graphName[, options])");
					}
					gds::LogicalLeidenOptions options;
					if (arguments.size() == 2U) {
						if (arguments[1].getType() != PropertyType::MAP) {
							throw std::invalid_argument("gds.leiden.stream options must be a map");
						}
						const PropertyValue::MapType &optionMap = arguments[1].getMap();
						if (optionMap.size() > LEIDEN_OPTION_NAMES.size()) {
							throw std::invalid_argument("gds.leiden.stream options exceed the supported entry bound");
						}
						for (const auto &[key, value]: optionMap) {
							context.checkGuard();
							if (std::ranges::find(LEIDEN_OPTION_NAMES, key) == LEIDEN_OPTION_NAMES.end()) {
								throw std::invalid_argument("gds.leiden.stream options contain an unknown key");
							}
							if (key == "maxIterations") {
								options.maximum_iterations = static_cast<size_t>(
										requireGdsPositiveInteger(value, "gds.leiden.stream maxIterations"));
							} else if (key == "maxLevels") {
								options.maximum_levels = static_cast<size_t>(
										requireGdsPositiveInteger(value, "gds.leiden.stream maxLevels"));
							} else if (key == "resolution") {
								options.resolution = requireGdsFiniteNumber(value, "gds.leiden.stream resolution");
							} else if (key == "refinementThreshold") {
								options.refinement_threshold =
										requireGdsFiniteNumber(value, "gds.leiden.stream refinementThreshold");
							} else if (key == "concurrency") {
								options.concurrency =
										requireGdsNonNegativeInteger(value, "gds.leiden.stream concurrency");
							}
						}
					}
					const auto name = requireProjectionName(arguments, "gds.leiden.stream", arguments.size());
					const auto communities = gds::LogicalGraphAlgorithms::leidenOwned(
							*projection_catalog_->get(name), options,
										{.maximum_nodes = readBounds.maximum_node_records,
										 .maximum_edges = maximumGdsAlgorithmEdges,
										 .maximum_iterations = maximumGdsAlgorithmIterations,
										 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
										 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
							[&context] { context.checkGuard(); }, thread_pool_);
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, communities.size(), readBounds,
							checkedProcedureSizeMul(communities.capacity(), sizeof(gds::LogicalGdsNodeCommunity)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeCommunity &community: communities) {
						context.checkGuard();
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "nodeId", makeProcedurePropertyValue(checkedOutputNodeId(community.node_id)));
						detail::procedureMapEmplace(row.row.values, "communityId",
											   makeProcedurePropertyValue(checkedOutputNodeId(community.community_id)));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.leiden.stream",
								&retainedBytes, outputByteAllowance);
					}
					defaultColumns = {"nodeId", "communityId"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_DIJKSTRA: {
					if (arguments.size() != 3U) {
						throw std::invalid_argument(
								"gds.shortestPath.dijkstra.stream expects (graphName, startId, endId)");
					}
					const auto name = requireProjectionName(arguments, "gds.shortestPath.dijkstra.stream", 3U);
					const uint64_t source = requireShortestPathNodeId(arguments[1], "startId");
					const uint64_t target = requireShortestPathNodeId(arguments[2], "endId");
					const gds::LogicalGdsWeightedPath path =
							gds::LogicalGraphAlgorithms::dijkstra(*projection_catalog_->get(name), source, target,
														  {.maximum_nodes = readBounds.maximum_node_records,
														   .maximum_edges = maximumGdsAlgorithmEdges,
														   .maximum_iterations = maximumGdsAlgorithmIterations,
														   .maximum_work_units = maximumGdsAlgorithmWorkUnits,
														   .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
																  [&context] { context.checkGuard(); });
					if (path.node_ids.size() != path.step_weights.size()) {
						throw std::logic_error("logical Dijkstra returned an invalid path shape");
					}
					// Projection paths still authenticate against this pinned snapshot,
					// even when the caller projects only costs and never requests Nodes.
					if (path.node_ids.size() > readBounds.maximum_node_records) {
						throw std::runtime_error("logical Dijkstra returned an over-bound path");
					}
					const size_t retainedPathBytes = checkedProcedureSizeAdd(sizeof(path),
							checkedProcedureSizeAdd(checkedProcedureSizeMul(path.node_ids.capacity(), sizeof(uint64_t)),
									checkedProcedureSizeMul(path.step_weights.capacity(), sizeof(double))));
					{
						MaterializedByteBudget validationBudget(readBounds.maximum_materialized_bytes);
						MaterializedByteReservation retainedPath(&validationBudget);
						retainedPath.set(retainedPathBytes);
						// Authenticate in bounded slices instead of copying and sorting the
						// entire path alongside Dijkstra's retained result vectors.
						const size_t maximumProbeRecords = std::min(readBounds.maximum_adjacency_page_records,
								(validationBudget.available() - std::min(validationBudget.available(), sizeof(size_t))) /
								(sizeof(uint64_t) + sizeof(bool)));
						if (!path.node_ids.empty() && maximumProbeRecords == 0U) {
							throw std::runtime_error("logical Dijkstra path validation exceeded its byte bound");
						}
						MaterializedByteReservation probeIds(&validationBudget);
						probeIds.set(checkedProcedureSizeMul(std::min(path.node_ids.size(), maximumProbeRecords), sizeof(uint64_t)));
						utils::OwnedStdVector<uint64_t> pathIds;
						pathIds.reserve(std::min(path.node_ids.size(), maximumProbeRecords));
						probeIds.set(checkedProcedureSizeMul(pathIds.capacity(), sizeof(uint64_t)));
						for (size_t offset = 0U; offset < path.node_ids.size();) {
							context.checkGuard();
							const size_t count = std::min(maximumProbeRecords, path.node_ids.size() - offset);
							pathIds.clear();
							pathIds.insert(pathIds.end(),
									path.node_ids.begin() + static_cast<std::ptrdiff_t>(offset),
									path.node_ids.begin() + static_cast<std::ptrdiff_t>(offset + count));
							std::ranges::sort(pathIds);
							pathIds.erase(std::unique(pathIds.begin(), pathIds.end()), pathIds.end());
							if (!pathIds.empty() && pathIds.front() == 0U) {
								throw std::runtime_error("logical Dijkstra returned an invalid path Node");
							}
							MaterializedByteReservation probeResults(&validationBudget);
							// Reserve byte-per-flag storage plus a packed-word rounding margin.
							probeResults.set(checkedProcedureSizeAdd(
								checkedProcedureSizeMul(pathIds.size(), sizeof(bool)), sizeof(size_t)));
							const auto present = snapshot->containsNodesSorted(pathIds);
							probeResults.set(checkedProcedureSizeAdd(present.capacity(), 7U) / 8U);
							if (present.size() != pathIds.size() ||
									std::ranges::any_of(present, [](bool exists) { return !exists; })) {
								throw std::runtime_error("logical Dijkstra path contains a missing Node");
							}
							offset += count;
						}
					} // Authentication buffers unwind before output row allocation.
					retainedProcedureSourceBytes = retainedPathBytes;
					const size_t outputByteAllowance = reserveBoundedProcedureRows(
							rows, path.node_ids.size(), readBounds, retainedPathBytes);
					size_t retainedBytes = 0U;
					double accumulatedCost = 0.0;
					for (size_t index = 0U; index < path.node_ids.size(); ++index) {
						context.checkGuard();
						// step_weights[index] is the edge used to reach this node. Account
						// for it before materializing the row so cost is cumulative at node.
						if (index != 0U) {
							accumulatedCost += path.step_weights[index];
						}
						BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						detail::procedureMapEmplace(row.row.values, "nodeId", makeProcedurePropertyValue(checkedOutputNodeId(path.node_ids[index])));
						detail::procedureMapEmplace(row.row.values, "node", storage::EntityRef{path.node_ids[index], false, 0U, 0U, 0U});
						detail::procedureMapEmplace(row.row.values, "cost", makeProcedurePropertyValue(accumulatedCost));
						detail::procedureMapEmplace(row.row.values, "totalCost", makeProcedurePropertyValue(path.total_weight));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.shortestPath.dijkstra.stream", &retainedBytes,
													  outputByteAllowance);
					}
					validateBufferedProcedureBytes(rows, readBounds.maximum_materialized_bytes);
					const double costTolerance =
							std::numeric_limits<double>::epsilon() *
							std::max({1.0, std::abs(accumulatedCost), std::abs(path.total_weight)}) * 8.0;
					if (!std::isfinite(accumulatedCost) || !std::isfinite(path.total_weight) ||
						(!path.node_ids.empty() && std::abs(accumulatedCost - path.total_weight) > costTolerance)) {
						throw std::runtime_error("logical Dijkstra returned inconsistent path costs");
					}
					defaultColumns = {"nodeId", "node", "cost", "totalCost"};
					break;
				}
				default: // NOLINTNEXTLINE(bugprone-branch-clone): this group is guarded above.
					throw std::logic_error("logical projection procedure dispatch became inconsistent");
			}
			QueryResult result = makeAdmittedProcedureResult(context);
			if (operations.empty() && procedure->kind != NativeProcedureKind::NPK_GDS_DIJKSTRA) {
				result = makeProcedureResult(std::move(rows), defaultColumns, readBounds, context);
			} else {
				result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
						retainedProcedureSourceBytes, operations.empty() && procedure->kind == NativeProcedureKind::NPK_GDS_DIJKSTRA
								? &defaultColumns
								: nullptr,
					true);
			}
			if (deferredCatalogMutation) {
				// Catalog visibility begins only after the result pipeline has fully
				// materialized. Do not check after commit: a completed catalog mutation
				// must never be reported as a late timeout.
				context.checkGuardNow();
				deferredCatalogMutation->commit();
			}
			return result;
		}
		if (procedure->kind == NativeProcedureKind::NPK_SHORTEST_PATH) {
			if (arguments.size() < 2U || arguments.size() > 3U) {
				throw std::invalid_argument("algo.shortestPath expects (startId, endId[, maxDepth])");
			}
			const uint64_t startNodeId = requireShortestPathNodeId(arguments[0], "startId");
			const uint64_t endNodeId = requireShortestPathNodeId(arguments[1], "endId");
			const size_t maximumDepth = arguments.size() == 3U ? requireShortestPathDepth(arguments[2]) : 15U;
			context.checkGuard();
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = read_backend_.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical shortest-path procedure backend could not open a read snapshot");
			}
			const utils::OwnedStdVector<uint64_t> path = findShortestPath(*snapshot, startNodeId, endNodeId, maximumDepth, readBounds, context);
			auto rows = makeBufferedShortestPathRows(path, readBounds);
			const size_t retainedPathBytes = checkedProcedureSizeAdd(sizeof(path),
					checkedProcedureSizeMul(path.capacity(), sizeof(uint64_t)));
			if (!operations.empty()) {
				return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
						retainedPathBytes, nullptr, true);
			}
			const utils::OwnedStringVector columns{"step", "node", "path_length"};
			return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
					retainedPathBytes, &columns, true);
		}
		if (procedure->kind == NativeProcedureKind::NPK_VECTOR_TRAIN) {
			if (arguments.size() != 1U) {
				throw std::invalid_argument("logical Vector train requires one index-name argument");
			}
			if (vector_maintenance_backend_ == nullptr) {
				throw std::runtime_error("logical backend does not support Vector index training");
			}
			if (maximumVectorTrainingRecords == 0U || maximumVectorTrainingPages == 0U) {
				throw std::invalid_argument("logical Vector train requires positive record and page bounds");
			}
			class QueryGuardCancellationCheckpoint final : public storage::CancellationCheckpoint {
			public:
				explicit QueryGuardCancellationCheckpoint(const QueryContext &context) : context_(context) {}

				void check() const override { context_.checkGuard(); }

			private:
				const QueryContext &context_;
			};

			QueryGuardCancellationCheckpoint cancellation(context);
			context.checkGuard();
            utils::OwnedHeapReservation trainingRequestProxies(utils::sharedAllocationOwner(),
                2U * utils::detail::standardContainerProxyBytes());
			std::unique_ptr<storage::PreparedLogicalGraphVectorTraining> prepared =
					vector_maintenance_backend_->prepareLogicalVectorTraining(
							{requireIndexName(arguments.front()), maximumVectorTrainingRecords, maximumVectorTrainingPages,
							 &cancellation});
			if (!prepared) {
				throw std::runtime_error("logical Vector maintenance backend returned no prepared training operation");
			}
			const storage::LogicalGraphVectorTrainingPreview preview = prepared->preview();
			const auto status = [&preview] {
				return makeProcedurePropertyValue(preview.will_train ? "Success" : "Skipped (Empty Data)");
			};
			// Result materialization remains entirely before commit(). A failure in
			// YIELD/WHERE/RETURN destroys the prepared operation and cannot publish a
			// derived Vector generation.
			QueryResult result = makeAdmittedProcedureResult(context);
			if (operations.empty()) {
				result.setColumns({"status"});
				ProcedureRows rows(1U);
                detail::ProcedurePayloadAdmissionScope singlePayloadScope(rows.front().allocation_credit);
				detail::procedureMapEmplace(rows.front().row.values, "status", status());
				detail::ProcedurePayloadAdmissionScope::admit(2U * detail::procedureEmptyMapHeapUpper<QueryResult::Row>());
                QueryResult::Row row;
                detail::procedureAdmitMapNode(row, "status");
				row.emplace("status", status());
				appendProcedureResultRow(result, rows, 0U, std::move(row), readBounds.maximum_materialized_bytes);
			} else {
				if (vector_backend_ == nullptr) {
					throw std::runtime_error("logical backend does not support Vector index search snapshots");
				}
				// The procedure produced a scalar row, but projections and predicates use
				// the same bounded logical expression evaluator as vector searches.
				std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot> snapshot =
						vector_backend_->openLogicalGraphVectorSearchSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical Vector procedure backend could not open a search snapshot");
				}
				BufferedProcedureRow source;
            detail::ProcedurePayloadAdmissionScope sourcePayloadScope(source.allocation_credit);
				detail::procedureMapEmplace(source.row.values, "status", status());
				ProcedureRows rows;
				rows.push_back(std::move(source));
				result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
						0U, nullptr, true);
			}
			// A prepared maintenance operation is now at its atomic commit point.
			// A periodic checkpoint can miss a deadline crossed while preparing a
			// small Vector generation. Sample immediately before the only durable
			// publication; never add a check after a successful commit.
			context.checkGuardNow();
			static_cast<void>(prepared->commit());
			return result;
		}
		if (procedure->kind != NativeProcedureKind::NPK_VECTOR_QUERY_NODES) {
			// This is a valid user query whose native implementation is unavailable,
			// not a malformed logical plan or an execution invariant violation.
			throw std::runtime_error("logical backend does not support this stored procedure");
		}
		if (arguments.size() != 3U) {
			throw std::invalid_argument("db.index.vector.queryNodes expects (indexName, topK, queryVector)");
		}
		if (readBounds.maximum_property_entries == 0U || maximumVectorMappingEntries == 0U ||
			maximumRawVectorReads == 0U || vectorCandidateSearchBudget == 0U || vectorCandidateRefinementBudget == 0U ||
			vectorCandidateSearchBudget > maximumVectorMappingEntries ||
			vectorCandidateRefinementBudget > maximumRawVectorReads) {
			throw std::invalid_argument("logical Vector procedure runtime requires positive read and search bounds");
		}
		if (vector_backend_ == nullptr) {
			throw std::runtime_error("logical backend does not support Vector index search");
		}

		class QueryGuardCancellationCheckpoint final : public storage::CancellationCheckpoint {
		public:
			explicit QueryGuardCancellationCheckpoint(const QueryContext &context) : context_(context) {}

			void check() const override { context_.checkGuard(); }

		private:
			const QueryContext &context_;
		};

		QueryGuardCancellationCheckpoint cancellation(context);
        // Loans are declared before the public request: its borrowed buffers
        // die before these admission owners. The request is immutable in search.
        utils::OwnedStdString requestNameLoan = requireIndexName(arguments[0]);
        utils::OwnedStdVector<float> queryVectorLoan;
        // Empty public containers and moved-from debug proxies overlap the
        // already-funded loan buffers on MSVC; the reservation dies last.
        utils::OwnedHeapReservation requestProxyLoan(utils::sharedAllocationOwner(),
            4U * utils::detail::standardContainerProxyBytes());
		storage::LogicalGraphVectorSearchRequest request;
		request.index_name = std::move(static_cast<std::string &>(requestNameLoan));
		request.top_k = requireTopK(arguments[1]);
		size_t retainedRequestBytes = retainedVectorRequestBytes(request);
		if (retainedRequestBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure request exceeded its materialized byte bound");
		}
		queryVectorLoan = requireQueryVector(arguments[2],
				readBounds.maximum_materialized_bytes - retainedRequestBytes, context);
        request.query_vector = std::move(static_cast<std::vector<float> &>(queryVectorLoan));
		retainedRequestBytes = retainedVectorRequestBytes(request);
		if (retainedRequestBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure request exceeded its materialized byte bound");
		}
		request.maximum_mapping_entries = maximumVectorMappingEntries;
		request.maximum_raw_vector_reads = maximumRawVectorReads;
		request.candidate_search_budget = vectorCandidateSearchBudget;
		request.candidate_refinement_budget = vectorCandidateRefinementBudget;
		request.cancellation = &cancellation;
		if (request.top_k > readBounds.maximum_rows) {
			throw std::invalid_argument("db.index.vector.queryNodes topK exceeds the logical row bound");
		}
		if (request.top_k == 0U) {
			QueryResult result = makeAdmittedProcedureResult(context);
			result.setColumns(operations.empty() ? std::vector<std::string>{"node", "score"}
													 : root.getOutputVariables());
			validateProcedureResultBudget(result, retainedRequestBytes, readBounds.maximum_materialized_bytes);
			return result;
		}
		// Validate row-window operators before any specialized path can return early
		// for an empty hit stream. The row-wise fallback performed this validation
		// even when the Vector search produced no candidates.
		for (const logical::LogicalOperator *operation: operations) {
			if (operation->getType() == logical::LogicalOpType::LOP_SKIP &&
				dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset() < 0) {
				throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
			}
			if (operation->getType() == logical::LogicalOpType::LOP_LIMIT &&
				dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit() < 0) {
				throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
			}
		}

		// Opening a transaction-owned Vector snapshot pins observable state, so a
		// timed-out procedure must fail before acquiring it.
		context.checkGuard();
		std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot> snapshot =
				vector_backend_->openLogicalGraphVectorSearchSnapshot();
		if (!snapshot) {
			throw std::runtime_error("logical Vector procedure backend could not open a search snapshot");
		}
		storage::LogicalGraphVectorSearchResult searchResult = snapshot->searchLogicalVector(request);
		context.checkGuard();
		std::vector<storage::LogicalGraphVectorSearchHit> &hits = searchResult.hits;
		if (hits.size() > request.top_k) {
			throw std::runtime_error("logical Vector procedure backend exceeded its requested result bound");
		}
		const size_t retainedHitBytes = checkedProcedureSizeMul(
			hits.capacity(), sizeof(storage::LogicalGraphVectorSearchHit));
		if (retainedRequestBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure request exceeded its materialized byte bound");
		}
		if (retainedHitBytes > readBounds.maximum_materialized_bytes - retainedRequestBytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure hit buffer exceeded its materialized byte bound");
		}
		const size_t retainedVectorSourceBytes = checkedProcedureSizeAdd(retainedRequestBytes, retainedHitBytes);
		if (!hits.empty() && retainedVectorSourceBytes >= readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure hit buffer leaves no output materialization budget");
		}
		LogicalPlanReadRuntimeBounds outputBounds = readBounds;
		outputBounds.maximum_materialized_bytes -= retainedVectorSourceBytes;
		if (const std::optional<size_t> prefix =
				vectorProcedureSortPrefix(operations, hits.size(), readBounds.maximum_rows,
						readBounds.maximum_distinct_rows);
				prefix.has_value()) {
			const auto postSkipFilterLimit = vectorProcedurePostSkipFilterLimit(operations);
			size_t candidateCount = *prefix;
			while (true) {
				QueryResult result = executeTopKVectorProcedure(hits, *snapshot, operations, root, candidateCount,
						readBounds, context, retainedRequestBytes);
				if (!postSkipFilterLimit.has_value() || result.rowCount() >= postSkipFilterLimit->result_limit ||
						candidateCount >= hits.size()) {
					return result;
				}
				const size_t doubled = candidateCount > std::numeric_limits<size_t>::max() / 2U
						? hits.size()
						: checkedProcedureSizeMul(candidateCount, 2U);
				const logical::LogicalProject *distinctTopKProject = vectorProcedureDistinctTopKProject(operations);
				const size_t candidateBound = distinctTopKProject == nullptr
						? hits.size()
						: std::min(hits.size(), readBounds.maximum_distinct_rows);
				const size_t nextCandidateCount = std::min(candidateBound, std::max(candidateCount + 1U, doubled));
				if (nextCandidateCount <= candidateCount) {
					return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context,
							retainedRequestBytes);
				}
				if (nextCandidateCount > readBounds.maximum_rows && nextCandidateCount < hits.size()) {
					return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context,
							retainedRequestBytes);
				}
				candidateCount = nextCandidateCount;
			}
		}
		if (std::ranges::any_of(operations, [](const logical::LogicalOperator *operation) {
				return operation->getType() == logical::LogicalOpType::LOP_SORT;
			})) {
			return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context,
					retainedRequestBytes);
		}
		if (const std::optional<size_t> prefix = vectorProcedurePrefixThroughLimit(operations, hits.size());
				prefix.has_value()) {
			const auto hitPrefix = std::span<storage::LogicalGraphVectorSearchHit>(hits).first(*prefix);
			validateVectorSearchHits(hitPrefix, readBounds, context);
			QueryResult result = makeAdmittedProcedureResult(context);
			setOwnedProcedureColumns(result, root);
			const size_t capacity = procedureNodeBatchCapacity(outputBounds);
			utils::OwnedStdVector<uint64_t> operatorRowCounts(operations.size(), 0U);
			for (size_t offset = 0U; offset < hitPrefix.size(); offset += capacity) {
				context.checkGuard();
				const auto batch = hitPrefix.subspan(offset, std::min(capacity, hitPrefix.size() - offset));
				if (!appendBufferedProcedureRows(makeBufferedVectorProcedureRows(batch, context), *snapshot,
						operations, readBounds, context, result, nullptr, &operatorRowCounts, retainedVectorSourceBytes, true)) {
					break;
				}
			}
			validateProcedureResultBudget(result, retainedVectorSourceBytes, readBounds.maximum_materialized_bytes);
			return result;
		}
		// Unary Project/Filter/SKIP/LIMIT layers can share bounded expression
		// batches. DISTINCT and row-count state carry across batches; LIMIT stops
		// pulling hit batches once its input quota has been consumed.
		if (!operations.empty() && std::ranges::all_of(operations, [](const logical::LogicalOperator *operation) {
				const auto type = operation->getType();
				return type == logical::LogicalOpType::LOP_FILTER || type == logical::LogicalOpType::LOP_PROJECT ||
						type == logical::LogicalOpType::LOP_SKIP || type == logical::LogicalOpType::LOP_LIMIT;
			})) {
			validateVectorSearchHits(hits, readBounds, context);
			QueryResult result = makeAdmittedProcedureResult(context);
			setOwnedProcedureColumns(result, root);
			const size_t capacity = procedureNodeBatchCapacity(outputBounds);
			std::vector<ProcedureDistinctRows, utils::OwnedPmrAllocator<ProcedureDistinctRows>> distinctRowsByOperation(operations.size());
			utils::OwnedStdVector<uint64_t> operatorRowCounts(operations.size(), 0U);
			for (size_t offset = 0U; offset < hits.size(); offset += capacity) {
				context.checkGuard();
				const auto batch = std::span<const storage::LogicalGraphVectorSearchHit>(hits).subspan(
						offset, std::min(capacity, hits.size() - offset));
				if (!appendBufferedProcedureRows(makeBufferedVectorProcedureRows(batch, context), *snapshot,
						operations, readBounds, context, result, &distinctRowsByOperation, &operatorRowCounts,
						retainedVectorSourceBytes, true)) {
					break;
				}
			}
			validateProcedureResultBudget(result, retainedVectorSourceBytes, readBounds.maximum_materialized_bytes);
			return result;
		}

		QueryResult result = makeAdmittedProcedureResult(context);
		result.setColumns({"node", "score"});
		validateVectorSearchHits(hits, readBounds, context);
		const size_t capacity = procedureNodeBatchCapacity(outputBounds);
		for (size_t offset = 0U; offset < hits.size(); offset += capacity) {
			context.checkGuard();
			const auto batch = std::span<const storage::LogicalGraphVectorSearchHit>(hits).subspan(
					offset, std::min(capacity, hits.size() - offset));
			if (!appendBufferedProcedureRows(makeBufferedVectorProcedureRows(batch, context), *snapshot, {},
					readBounds, context, result, nullptr, nullptr, retainedVectorSourceBytes, true)) {
				break;
			}
		}
		validateProcedureResultBudget(result, retainedVectorSourceBytes, readBounds.maximum_materialized_bytes);
		return result;
	}

} // namespace graph::query::execution
