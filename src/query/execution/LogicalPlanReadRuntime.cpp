#include "graph/storage/LogicalGraphNodeScan.hpp"
#ifndef ZYX_EXPERIMENTAL_STREAMING_COUNT
#define ZYX_EXPERIMENTAL_STREAMING_COUNT 0
#endif
#ifndef ZYX_EXPERIMENTAL_BORROWED_GROUP_KEY
#define ZYX_EXPERIMENTAL_BORROWED_GROUP_KEY 0
#endif
#if ZYX_EXPERIMENTAL_STREAMING_COUNT || defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
#define ZYX_BUILD_STREAMING_COUNT_PATH 1
#else
#define ZYX_BUILD_STREAMING_COUNT_PATH 0
#endif
#if ZYX_EXPERIMENTAL_BORROWED_GROUP_KEY || defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
#define ZYX_BUILD_BORROWED_GROUP_KEY_PATH 1
#else
#define ZYX_BUILD_BORROWED_GROUP_KEY_PATH 0
#endif
#include "query/execution/OrderByComparison.hpp"
#include "query/execution/DeferredReferenceRowVisitor.hpp"
#include "query/execution/ProcedurePayloadAdmission.hpp"
/**
 * @file LogicalPlanReadRuntime.cpp
 * @brief Streaming logical-plan runtime over the format-independent graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "query/execution/GuardedIdGather.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "query/execution/BoundProjectionReferences.hpp"
#include "query/execution/BoundExecutionProgram.hpp"
#include "query/execution/BoundExpressionInputs.hpp"
#include "query/execution/BatchEnvelope.hpp"
#include "query/execution/BoundNodeScanBatch.hpp"
#include "query/execution/BoundColumnLayout.hpp"
#include "query/execution/BoundNodeColumnPredicates.hpp"
#include "query/execution/StreamingPredicateCount.hpp"
#include "query/execution/BorrowedGroupKey.hpp"
#include "query/execution/PrimitiveNumericColumns.hpp"
#include "query/execution/ExternalQuerySort.hpp"
#include "query/execution/ExternalQueryKeySet.hpp"
#include "graph/query/execution/StringBufferBytes.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include "graph/query/execution/GuardedPropertyCopy.hpp"
#include "graph/query/execution/GuardedRowCopy.hpp"
#include "graph/query/execution/GuardedEntityCopy.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/execution/BoundedBatch.hpp"
#include "graph/query/execution/CsvReader.hpp"
#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/execution/SelectionVector.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalLoadCsv.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"

namespace graph::query::execution {
#if defined(ZYX_ENABLE_STORAGE_TEST_HOOKS)
    thread_local bool test_disable_streaming_count = false;
    thread_local bool test_enable_streaming_count = false;
    thread_local bool test_enable_borrowed_group_key = false;
    thread_local bool test_force_borrowed_group_collision = false;
    thread_local bool test_reject_borrowed_group_index = false;
#endif

	namespace {

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;
		using InternalVisitor = std::function<bool(LogicalPlanReadRow)>;
		using ChargedRowVisitor = std::function<bool(LogicalPlanReadRow, MaterializedByteReservation)>;
#include "read_runtime/ExecutionStateAndBudgets.inc"
#include "read_runtime/EntityValuesAndProperties.inc"
#include "read_runtime/PatternEvaluation.inc"
#include "read_runtime/ScalarFunctions.inc"
#include "read_runtime/ExpressionEvaluation.inc"
#include "read_runtime/RowBindingAndResults.inc"
#include "read_runtime/PlanInspectionAndBatching.inc"
#include "read_runtime/TraversalSupply.inc"
#include "read_runtime/NodeSupplyAndSorting.inc"
		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor, const ChargedRowVisitor *chargedReceiver,
										 const NodeColumnReceiver *columnReceiver) {
			checkGuard(state);
			if (state.shared.bound_program) {
				state.shared.bound_program->validateSnapshot(state.shared.snapshot);
				(void) state.shared.bound_program->view(state.shared.bound_program->id(op));
			}
			switch (op.getType()) {
#include "read_runtime/ScanFilterProjectOperators.inc"
#include "read_runtime/AggregateOperator.inc"
#include "read_runtime/JoinAndSubqueryOperators.inc"
#include "read_runtime/PathOperators.inc"
#include "read_runtime/RowAndWindowOperators.inc"
#include "read_runtime/TraversalOperator.inc"
				}
				default:
					throw std::logic_error("logical-plan read runtime does not support this logical operator");
			}
		}

		template<class Bind>
        void bindProgramWithHeadroom(ExecutionSharedState &shared, Bind bind) {
            shared.bound_program.reset();
            const size_t before = shared.materialized_budget.used();
            // Binding is optional acceleration. Retain at least seven eighths
            // of the remaining working budget for sources, carriers and output.
            const size_t allowance = shared.materialized_budget.available() / 8U;
            try {
                bind();
                const size_t bytes = shared.materialized_budget.used() - before;
                if (bytes <= allowance) {
                    debug::PerfTrace::addValue("logical_plan.bound_program.admitted_bytes", bytes);
                    return;
                }
            } catch (const MaterializedByteLimitExceeded &) {
                // Partial descriptors unwind before the charged row path runs.
            } catch (const detail::ResourceAdmissionDenied &) {
            }
            shared.bound_program.reset();
            debug::PerfTrace::addValue("logical_plan.bound_program.quota_fallbacks", 1);
        }

        void bindInjectedProgram(const logical::LogicalOperator &root, ExecutionState &state, const LogicalPlanReadRow &row) {
            if (!state.shared.snapshot.bindingSchemaIdentity()) return;
            bindProgramWithHeadroom(state.shared, [&] {
                detail::BudgetMemoryResource memory(state.shared.materialized_budget);
                std::pmr::vector<std::string_view> names(&memory);
                for (const auto &[name, value] : row.values) { checkGuardNow(state); names.push_back(name); }
                std::ranges::sort(names);
                state.shared.bound_program.emplace(root, state.shared.snapshot, state.shared.materialized_budget,
                    [&] { checkGuardNow(state); }, detail::BoundProgramLimits{}, std::pmr::get_default_resource(),
                    std::span<const std::string_view>(names));
            });
            if (state.shared.bound_program) debug::PerfTrace::addValue("logical_plan.bound_inputs.imported_programs", 1);
        }

		[[nodiscard]] LogicalPlanReadRuntimeStats
		executeOnSnapshot(storage::LogicalGraphReadSnapshot &snapshot, const logical::LogicalOperator &root,
						  const LogicalPlanReadRuntimeBounds &bounds, const FinalRowVisitor &visitor,
						  const QueryContext *context, const bool allowDeferredEntityRefs = false,
						  const ExecutionFinalizer *finalizer = nullptr, const bool allowCrossRowEntityBatches = false,
						  const size_t residentOutputBatchBytes = 0U) {
			validateBounds(bounds, static_cast<bool>(visitor));
            // Private metadata resolution, cursor storage and scalar binding are
            // QUERY work. Shared cache publication selects CACHE explicitly.
            std::optional<utils::OwnedAllocationScope> queryAllocation;
            if (context && context->resource_account)
                queryAllocation.emplace(std::make_shared<MaterializedByteBudget>(
                    bounds.maximum_materialized_bytes, context->resource_account, std::pmr::new_delete_resource()));
			ExecutionSharedState shared{snapshot, bounds, context, allowDeferredEntityRefs,
				allowCrossRowEntityBatches, residentOutputBatchBytes};
			ExecutionState state{shared, {}};
			try {
				// Token/physical binding requires an authenticated reader domain.
				// Readers without that capability retain the charged semantic row adapter.
				if (snapshot.bindingSchemaIdentity())
                    bindProgramWithHeadroom(shared, [&] {
                        shared.bound_program.emplace(root, snapshot, shared.materialized_budget, [&] { checkGuardNow(state); });
                    });
				static_cast<void>(visitOperator(root, state, [&](LogicalPlanReadRow row) {
					if (state.task.stats.rows_emitted ==
						bounds.maximum_rows) { // ZYX_COV_EXCL_LINE: this visitor returns false with the
											   // final row, so its producer cannot invoke it again.
						return false;
					}
					const bool continueVisitor = visitor(row, state);
					incrementBounded(state.task.stats.rows_emitted, "logical-plan read runtime row counter overflowed");
					return continueVisitor && state.task.stats.rows_emitted < bounds.maximum_rows;
				}));
				if (finalizer != nullptr) {
					(*finalizer)(state, false);
				}
			} catch (...) {
				if (finalizer != nullptr) (*finalizer)(state, true);
				throw;
			}
			return state.task.stats;
		}

		[[nodiscard]] LogicalPlanReadRuntimeStats
		executeInternal(const storage::LogicalGraphReadBackend &backend, const logical::LogicalOperator &root,
						const LogicalPlanReadRuntimeBounds &bounds, const FinalRowVisitor &visitor,
						const QueryContext *context, const bool allowDeferredEntityRefs = false,
						const ExecutionFinalizer *finalizer = nullptr, const bool allowCrossRowEntityBatches = false,
						const size_t residentOutputBatchBytes = 0U) {
            std::optional<utils::OwnedAllocationScope> queryAllocation;
            if (context && context->resource_account)
                queryAllocation.emplace(std::make_shared<MaterializedByteBudget>(
                    bounds.maximum_materialized_bytes, context->resource_account, std::pmr::new_delete_resource()));
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical graph backend returned no read snapshot");
			}
			return executeOnSnapshot(*snapshot, root, bounds, visitor, context, allowDeferredEntityRefs, finalizer,
									 allowCrossRowEntityBatches, residentOutputBatchBytes);
		}

	} // namespace

	std::vector<std::string> LogicalPlanReadRuntime::collectBatchedCompleteEntityVariables(
			const std::vector<const expressions::Expression *> &expressions) {
		return selectBatchableCompleteEntityVariables(expressions);
	}

	std::vector<LogicalPlanBatchedMetadataRequest> LogicalPlanReadRuntime::collectBatchedMetadataRequests(
			const std::vector<const expressions::Expression *> &expressions) {
		std::vector<LogicalPlanBatchedMetadataRequest> requests;
		for (const auto *expression: expressions) {
			std::vector<BatchableEntityPropertyPredicate> properties;
			std::vector<BatchableEntityMetadataPredicate> metadata;
			if (expression == nullptr ||
				!collectBatchableEntityPropertyPredicates(*expression, properties, nullptr, &metadata))
				continue;
			for (const auto &entry: metadata) {
				const LogicalPlanBatchedMetadataRequest request{entry.variable, entry.function};
				if (std::ranges::find(requests, request) == requests.end())
					requests.push_back(request);
			}
		}
		return requests;
	}

	bool LogicalPlanReadRuntime::collectBatchedPropertyRequests(
			const std::vector<const expressions::Expression *> &expressions,
			std::vector<LogicalPlanBatchedPropertyRequest> &requests) {
		for (const expressions::Expression *expression: expressions) {
			std::vector<BatchableEntityPropertyPredicate> collected;
			std::vector<BatchableEntityMetadataPredicate> metadata;
			if (expression == nullptr ||
				!collectBatchableEntityPropertyPredicates(*expression, collected, nullptr, &metadata)) {
				continue;
			}
			for (const BatchableEntityPropertyPredicate &request: collected) {
				const LogicalPlanBatchedPropertyRequest publicRequest{request.variable, request.key};
				if (std::ranges::find(requests, publicRequest) == requests.end()) {
					requests.push_back(publicRequest);
				}
			}
		}
		return !requests.empty();
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::execute(const logical::LogicalOperator &root,
																const LogicalPlanReadRuntimeBounds &bounds,
																const RowVisitor &visitor) const {
		// Keep the public callback contract fail-closed before wrapping it in
		// the state-aware internal visitor used by result materialization.
		validateBounds(bounds, static_cast<bool>(visitor));
		return executeWithBatchedOutputMaterialization(
				bounds,
				[&](const FinalRowVisitor &finalVisitor, const ExecutionFinalizer &finalizer,
					const size_t residentOutputBatchBytes) {
					return executeInternal(backend_, root, bounds, finalVisitor, nullptr, true, &finalizer, false,
										   residentOutputBatchBytes);
				},
				[&visitor](LogicalPlanReadRow &row, ExecutionState &) { return visitor(row); }, true);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::execute(const logical::LogicalOperator &root,
																const LogicalPlanReadRuntimeBounds &bounds,
																const RowVisitor &visitor,
																const QueryContext &context) const {
		validateBounds(bounds, static_cast<bool>(visitor));
		return executeWithBatchedOutputMaterialization(
				bounds,
				[&](const FinalRowVisitor &finalVisitor, const ExecutionFinalizer &finalizer,
					const size_t residentOutputBatchBytes) {
					return executeInternal(backend_, root, bounds, finalVisitor, &context, true, &finalizer, false,
										   residentOutputBatchBytes);
				},
				[&visitor](LogicalPlanReadRow &row, ExecutionState &) { return visitor(row); }, true, true, &context);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::executeWithSnapshot(storage::LogicalGraphReadSnapshot &snapshot,
																			const logical::LogicalOperator &root,
																			const LogicalPlanReadRuntimeBounds &bounds,
																			const RowVisitor &visitor,
																			const QueryContext &context) const {
		return executeWithBatchedOutputMaterialization(
				bounds,
				[&](const FinalRowVisitor &finalVisitor, const ExecutionFinalizer &finalizer,
					const size_t residentOutputBatchBytes) {
					return executeOnSnapshot(snapshot, root, bounds, finalVisitor, &context, true, &finalizer, false,
											 residentOutputBatchBytes);
				},
				[&visitor](LogicalPlanReadRow &row, ExecutionState &) { return visitor(row); }, true, true, &context);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::executeWithSnapshotAndInputRow(
			storage::LogicalGraphReadSnapshot &snapshot, const logical::LogicalOperator &root,
			const LogicalPlanReadRuntimeBounds &bounds, const LogicalPlanReadRow &inputRow, const RowVisitor &visitor,
			const QueryContext &context) const {
		validateBounds(bounds, static_cast<bool>(visitor));
		context.checkGuardNow();
		ExecutionSharedState shared{snapshot, bounds, &context, false, false};
		ExecutionState state{shared, {}};
		MaterializedByteReservation importedCacheBytes(&state.shared.materialized_budget);
		MaterializedByteReservation refreshedEntityBytes(&state.shared.materialized_budget);
		importedCacheBytes.set(logicalReadRowBytesIncluding(inputRow, [](const std::string &) { return true; }, &context, false, true));
		LogicalPlanReadRow refreshed;
		state.task.injected_row = &refreshed;
		const auto checkpoint = [&] { context.checkGuardNow(); };
		const auto invalidatedCache = [&](const std::string &alias) {
			const auto binding = inputRow.values.find(alias);
			return binding != inputRow.values.end() &&
				(std::holds_alternative<storage::LogicalGraphNode>(binding->second) ||
				 std::holds_alternative<storage::LogicalGraphEdge>(binding->second));
		};
		const auto copyCache = [&](const auto &source, auto &destination) {
			size_t retainedAliases = 0U;
			for (const auto &[alias, properties] : source) {
				checkpoint();
				if (!invalidatedCache(alias)) ++retainedAliases;
			}
			destination.reserve(retainedAliases);
			for (const auto &[alias, properties] : source) {
				checkpoint();
				if (invalidatedCache(alias)) continue;
				destination.emplace(detail::copyPropertyStringGuarded(alias, checkpoint),
					detail::copyPropertyMapGuarded(properties, checkpoint));
			}
		};
		copyCache(inputRow.property_cache, refreshed.property_cache);
		copyCache(inputRow.entity_metadata_cache, refreshed.entity_metadata_cache);
		for (const auto &alias : inputRow.complete_entity_property_cache)
			if (!invalidatedCache(alias)) refreshed.complete_entity_property_cache.emplace(detail::copyPropertyStringGuarded(alias, checkpoint));
		for (const auto &[alias, path] : inputRow.path_cache) {
			checkpoint();
			refreshed.path_cache.emplace(detail::copyPropertyStringGuarded(alias, checkpoint),
				detail::copyLogicalReadPathGuarded(path, checkpoint));
		}
		importedCacheBytes.set(logicalReadRowBytesIncluding(refreshed, [](const std::string &) { return true; }, &context, false));
		size_t nodeBindingCount = 0U;
		size_t edgeBindingCount = 0U;
		for (const auto &[_, value]: inputRow.values) {
			context.checkGuard();
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node, state);
				++nodeBindingCount;
			} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				validateEdge(*edge);
				++edgeBindingCount;
			}
		}
		MaterializedByteReservation routingBytes(&state.shared.materialized_budget);
		routingBytes.set(checkedSizeMul(checkedSizeAdd(nodeBindingCount, edgeBindingCount,
													   "logical-plan imported Entity routing count overflowed"),
										sizeof(uint64_t), "logical-plan imported Entity routing size overflowed"));
		std::vector<uint64_t> nodeIds;
		std::vector<uint64_t> edgeIds;
		nodeIds.reserve(nodeBindingCount);
		edgeIds.reserve(edgeBindingCount);
		for (const auto &[variable, value]: inputRow.values) {
			context.checkGuard();
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node, state);
				nodeIds.push_back(node->node_id);
				refreshed.values.emplace(variable, storage::EntityRef{node->node_id, false, 0U, 0U, 0U});
			} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				validateEdge(*edge);
				edgeIds.push_back(edge->edge_id);
				refreshed.values.emplace(variable, storage::EntityRef{edge->edge_id, true, edge->source_node_id,
																	  edge->target_node_id, 0U});
			} else {
				refreshed.values.emplace(variable, detail::copyLogicalReadValueGuarded(value, checkpoint));
			}
		}
		std::ranges::sort(nodeIds);
		nodeIds.erase(std::unique(nodeIds.begin(), nodeIds.end()), nodeIds.end());
		std::ranges::sort(edgeIds);
		edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());
		routingBytes.set(checkedSizeMul(nodeIds.capacity() + edgeIds.capacity(), sizeof(uint64_t),
										"logical-plan imported Entity routing size overflowed"));
		const auto refreshNodeBatch = [&](const size_t offset, const size_t count) {
			checkGuard(state);
			const auto ids = std::span<const uint64_t>(nodeIds).subspan(offset, count);
			MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
			auto nodes = readNodesShapedSortedBudgeted(state, ids, completeNodeReadShape(), &sourceBytes);
			context.checkGuardNow();
			if (nodes.size() != count)
				throw std::runtime_error("logical-plan imported Node batch changed cardinality");
			MaterializedByteReservation consumerBytes(&state.shared.materialized_budget);
			consumerBytes.set(
					checkedSizeMul(count, sizeof(size_t), "logical-plan imported Node consumer size overflowed"));
			std::vector<size_t> remaining(count, 0U);
			for (size_t index = 0U; index < count; ++index) {
				checkGuard(state);
				validateNode(nodes[index], state);
				if (nodes[index].node_id != ids[index])
					throw std::runtime_error("logical-plan imported Node batch changed ID order");
				if (nodes[index].properties.size() > bounds.maximum_property_entries)
					throw std::runtime_error("logical-plan imported Node exceeded the Property bound");
			}
			for (const auto &[_, value]: refreshed.values) {
				checkGuard(state);
				const auto *reference = std::get_if<storage::EntityRef>(&value);
				if (reference == nullptr || reference->is_edge)
					continue;
				const auto position = std::ranges::lower_bound(ids, reference->entity_id);
				if (position != ids.end() && *position == reference->entity_id)
					++remaining[static_cast<size_t>(position - ids.begin())];
			}
			size_t actualSourceBytes = checkedSizeMul(nodes.capacity(), sizeof(storage::LogicalGraphNode),
													  "logical-plan imported Node batch size overflowed");
			for (const auto &node: nodes)
				actualSourceBytes = checkedSizeAdd(actualSourceBytes,
												   logicalNodeCacheBytes(node, state.shared.context) - sizeof(storage::LogicalGraphNode),
												   "logical-plan imported Node batch size overflowed");
			sourceBytes.set(actualSourceBytes);
			for (size_t index = 0U; index < count; ++index) {
				checkGuard(state);
				const size_t payloadBytes = logicalNodeCacheBytes(nodes[index], state.shared.context) - sizeof(storage::LogicalGraphNode);
				for (auto &[_, value]: refreshed.values) {
					checkGuard(state);
					auto *reference = std::get_if<storage::EntityRef>(&value);
					if (reference == nullptr || reference->is_edge || reference->entity_id != ids[index])
						continue;
					const bool last = remaining[index] == 1U;
					const size_t previousBytes = refreshedEntityBytes.bytes();
					if (last) refreshedEntityBytes.absorb(sourceBytes.splitOff(payloadBytes));
					try {
						refreshedEntityBytes.set(checkedSizeAdd(previousBytes,
							logicalNodeCacheBytes(nodes[index], state.shared.context), "logical-plan refreshed entity size overflowed"));
						if (last) {
							value = LogicalPlanReadValue(std::move(nodes[index]));
						} else {
							auto copy = detail::copyLogicalEntityGuarded(nodes[index], checkpoint);
							refreshedEntityBytes.set(checkedSizeAdd(previousBytes, logicalNodeCacheBytes(copy, state.shared.context),
								"logical-plan refreshed entity size overflowed"));
							value = LogicalPlanReadValue(std::move(copy));
						}
						--remaining[index];
					} catch (...) {
						if (last) sourceBytes.absorb(refreshedEntityBytes.splitOff(payloadBytes));
						refreshedEntityBytes.set(previousBytes);
						throw;
					}
				}
			}
			sourceBytes.set(checkedSizeMul(nodes.capacity(), sizeof(storage::LogicalGraphNode),
										   "logical-plan imported Node batch size overflowed"));
		};
		std::function<void(size_t, size_t)> refreshNodes;
		refreshNodes = [&](const size_t offset, const size_t count) {
			checkGuard(state);
			try {
				refreshNodeBatch(offset, count);
			} catch (const std::runtime_error &error) {
				if (count == 1U || std::string_view(error.what()) != "shared materialization exceeded its byte bound")
					throw;
				const size_t first = count / 2U;
				refreshNodes(offset, first);
				refreshNodes(offset + first, count - first);
			}
		};
		const auto refreshEdgeBatch = [&](const size_t offset, const size_t count) {
			checkGuard(state);
			const auto ids = std::span<const uint64_t>(edgeIds).subspan(offset, count);
			MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
			auto edges = readEdgesShapedSortedBudgeted(state, ids, completeEdgeReadShape(), &sourceBytes);
			context.checkGuardNow();
			if (edges.size() != count)
				throw std::runtime_error("logical-plan imported Edge batch changed cardinality");
			MaterializedByteReservation consumerBytes(&state.shared.materialized_budget);
			consumerBytes.set(
					checkedSizeMul(count, sizeof(size_t), "logical-plan imported Edge consumer size overflowed"));
			std::vector<size_t> remaining(count, 0U);
			for (size_t index = 0U; index < count; ++index) {
				checkGuard(state);
				validateEdge(edges[index]);
				if (edges[index].edge_id != ids[index])
					throw std::runtime_error("logical-plan imported Edge batch changed ID order");
				if (edges[index].properties.size() > bounds.maximum_property_entries)
					throw std::runtime_error("logical-plan imported Edge exceeded the Property bound");
			}
			for (const auto &[_, value]: refreshed.values) {
				checkGuard(state);
				const auto *reference = std::get_if<storage::EntityRef>(&value);
				if (reference == nullptr || !reference->is_edge)
					continue;
				const auto position = std::ranges::lower_bound(ids, reference->entity_id);
				if (position != ids.end() && *position == reference->entity_id)
					++remaining[static_cast<size_t>(position - ids.begin())];
			}
			size_t actualSourceBytes = checkedSizeMul(edges.capacity(), sizeof(storage::LogicalGraphEdge),
													  "logical-plan imported Edge batch size overflowed");
			for (const auto &edge: edges)
				actualSourceBytes = checkedSizeAdd(actualSourceBytes,
												   logicalEdgeCacheBytes(edge, state.shared.context) - sizeof(storage::LogicalGraphEdge),
												   "logical-plan imported Edge batch size overflowed");
			sourceBytes.set(actualSourceBytes);
			for (size_t index = 0U; index < count; ++index) {
				checkGuard(state);
				const size_t payloadBytes = logicalEdgeCacheBytes(edges[index], state.shared.context) - sizeof(storage::LogicalGraphEdge);
				for (auto &[_, value]: refreshed.values) {
					checkGuard(state);
					auto *reference = std::get_if<storage::EntityRef>(&value);
					if (reference == nullptr || !reference->is_edge || reference->entity_id != ids[index])
						continue;
					const bool last = remaining[index] == 1U;
					const size_t previousBytes = refreshedEntityBytes.bytes();
					if (last) refreshedEntityBytes.absorb(sourceBytes.splitOff(payloadBytes));
					try {
						refreshedEntityBytes.set(checkedSizeAdd(previousBytes,
							logicalEdgeCacheBytes(edges[index], state.shared.context), "logical-plan refreshed entity size overflowed"));
						if (last) {
							value = LogicalPlanReadValue(std::move(edges[index]));
						} else {
							auto copy = detail::copyLogicalEntityGuarded(edges[index], checkpoint);
							refreshedEntityBytes.set(checkedSizeAdd(previousBytes, logicalEdgeCacheBytes(copy, state.shared.context),
								"logical-plan refreshed entity size overflowed"));
							value = LogicalPlanReadValue(std::move(copy));
						}
						--remaining[index];
					} catch (...) {
						if (last) sourceBytes.absorb(refreshedEntityBytes.splitOff(payloadBytes));
						refreshedEntityBytes.set(previousBytes);
						throw;
					}
				}
			}
			sourceBytes.set(checkedSizeMul(edges.capacity(), sizeof(storage::LogicalGraphEdge),
										   "logical-plan imported Edge batch size overflowed"));
		};
		std::function<void(size_t, size_t)> refreshEdges;
		refreshEdges = [&](const size_t offset, const size_t count) {
			checkGuard(state);
			try {
				refreshEdgeBatch(offset, count);
			} catch (const std::runtime_error &error) {
				if (count == 1U || std::string_view(error.what()) != "shared materialization exceeded its byte bound")
					throw;
				const size_t first = count / 2U;
				refreshEdges(offset, first);
				refreshEdges(offset + first, count - first);
			}
		};
		const size_t nodeFrameCapacity =
				std::max<size_t>(1U, state.shared.materialized_budget.available() / sizeof(storage::LogicalGraphNode));
		const size_t nodeBatchCapacity =
				std::min({bounds.maximum_node_records, DEFAULT_PROPERTY_BATCH_ROWS, nodeFrameCapacity});
		for (size_t offset = 0U; offset < nodeIds.size(); offset += nodeBatchCapacity) {
			checkGuard(state);
			refreshNodes(offset, std::min(nodeBatchCapacity, nodeIds.size() - offset));
		}
		const size_t edgeFrameCapacity =
				std::max<size_t>(1U, state.shared.materialized_budget.available() / sizeof(storage::LogicalGraphEdge));
		const size_t edgeBatchCapacity =
				std::min({bounds.maximum_edge_records, DEFAULT_PROPERTY_BATCH_ROWS, edgeFrameCapacity});
		for (size_t offset = 0U; offset < edgeIds.size(); offset += edgeBatchCapacity) {
			checkGuard(state);
			refreshEdges(offset, std::min(edgeBatchCapacity, edgeIds.size() - offset));
		}
		routingBytes.set(0U);
		std::vector<uint64_t>().swap(nodeIds);
		std::vector<uint64_t>().swap(edgeIds);
		context.checkGuardNow();
		bindInjectedProgram(root, state, refreshed);
		const bool completed = visitOperator(root, state, [&](LogicalPlanReadRow row) {
			if (state.task.stats.rows_emitted == bounds.maximum_rows) {
				throw std::runtime_error("logical-plan read runtime exceeded its row bound");
			}
			incrementBounded(state.task.stats.rows_emitted, "logical-plan read runtime row counter overflowed");
			materializeRowEntityRefs(row, state);
			enforceMaterializedRowBound(row, state);
			return invokeOutputVisitorChecked(state, [&] { return visitor(row); });
		});
		if (!completed) {
			throw std::runtime_error("logical-plan read runtime input-row execution was stopped by its visitor");
		}
		return state.task.stats;
	}

	LogicalPlanReadValue LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			storage::LogicalGraphReadSnapshot &snapshot, const expressions::Expression &expression,
			LogicalPlanReadRow &row, const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
		// Resolving an uncached Property collection touches durable storage, so
		// expression-only evaluation enforces the same finite read limits as a plan.
		validateBounds(bounds, true);
		logical::LogicalSingleRow bindingRoot;
		ExecutionSharedState shared{snapshot, bounds, &context, false, false};
		ExecutionState state{shared, {}};
		checkGuard(state);
		bindInjectedProgram(bindingRoot, state, row);
		std::optional<detail::BoundExpressionInputs> boundInputs;
		if (shared.bound_program) {
			const expressions::Expression *candidate = &expression;
			boundInputs.emplace(*shared.bound_program, shared.bound_program->root(),
				std::span<const expressions::Expression *const>(&candidate, 1U), snapshot, shared.materialized_budget,
				[&] { checkGuardNow(state); }, std::pmr::get_default_resource(), &context);
		}
		ScopedBoundInputs scopedInputs(state, boundInputs, row);
		return evaluateExpression(expression, row, state);
	}

	std::string LogicalPlanReadRuntime::makeDistinctRowKey(const LogicalPlanReadRow &row,
														   const std::vector<std::string> &columns) {
		if (columns.empty()) {
			throw std::invalid_argument("logical-plan DISTINCT key requires at least one output column");
		}
		return distinctUnionRowKey(row, columns);
	}

    utils::OwnedStdString detail::makeOwnedProcedureDistinctRowKey(const LogicalPlanReadRow &row,
            const std::vector<std::string> &columns) {
        if (columns.empty()) throw std::invalid_argument("procedure DISTINCT requires output columns");
        return distinctUnionRowKey<utils::OwnedStdString>(row, columns);
    }

	QueryResult LogicalPlanReadRuntime::executeToQueryResult(const logical::LogicalOperator &root,
															 const LogicalPlanReadRuntimeBounds &bounds) const {
		QueryResult result;
        static_cast<void>(populateResultColumns(result, root, bounds.maximum_materialized_bytes, nullptr));
		const std::vector<std::string> &columns = result.getColumns();
		const size_t columnBytes = queryResultColumnStorageBytes(columns);
		if (columnBytes > bounds.maximum_materialized_bytes) {
			throw std::runtime_error("logical-plan result columns exceeded their materialized byte bound");
		}
		LogicalPlanReadRuntimeBounds executionBounds = bounds;
		executionBounds.maximum_materialized_bytes -= columnBytes;
		static_cast<void>(executeWithBatchedOutputMaterialization(
				executionBounds,
				[&](const FinalRowVisitor &finalVisitor, const ExecutionFinalizer &finalizer,
					const size_t residentOutputBatchBytes) {
					return executeInternal(backend_, root, executionBounds, finalVisitor, nullptr, true, &finalizer,
										   true, residentOutputBatchBytes);
				},
				[&result, &columns](LogicalPlanReadRow &row, ExecutionState &state) {
					appendLogicalResultRow(result, row, columns, state);
					return true;
				},
				false, false));
		return result;
	}

	QueryResult LogicalPlanReadRuntime::executeToQueryResult(const logical::LogicalOperator &root,
															 const LogicalPlanReadRuntimeBounds &bounds,
															 const QueryContext &context) const {
		std::optional<detail::ResourceLease> columnParentCharge;
        // Debug MSVC vectors allocate empty proxies in the result constructor.
        // Admit these before construction; the populated result's detached
        // reservation takes over before this initial loan is replaced.
        if (context.resource_account) {
            columnParentCharge.emplace(context.resource_account);
            columnParentCharge->set(3U * detail::standardContainerProxyBytes());
        }
		QueryResult result;
        columnParentCharge = populateResultColumns(result, root, bounds.maximum_materialized_bytes, &context);
		const std::vector<std::string> &columns = result.getColumns();
		const size_t columnBytes = queryResultColumnStorageBytes(columns);
		if (columnBytes > bounds.maximum_materialized_bytes) {
			throw std::runtime_error("logical-plan result columns exceeded their materialized byte bound");
		}
		LogicalPlanReadRuntimeBounds executionBounds = bounds;
		executionBounds.maximum_materialized_bytes -= columnBytes;
		static_cast<void>(executeWithBatchedOutputMaterialization(
				executionBounds,
				[&](const FinalRowVisitor &finalVisitor, const ExecutionFinalizer &finalizer,
					const size_t residentOutputBatchBytes) {
					const ExecutionFinalizer retainOutput = [&](ExecutionState &state, bool failed) {
						finalizer(state, failed);
						if (failed) {
							// Free partial output before execution reservations refund it.
							result = QueryResult{};
						}
						if (!failed && context.resource_account) {
							auto charge = state.shared.final_result_reservation.detachToParent();
							charge.absorb(std::move(*columnParentCharge));
							result.retainExecutionCharge(std::move(charge));
						}
					};
					return executeInternal(backend_, root, executionBounds, finalVisitor, &context, true, &retainOutput,
										   true, residentOutputBatchBytes);
				},
				[&result, &columns](LogicalPlanReadRow &row, ExecutionState &state) {
					appendLogicalResultRow(result, row, columns, state);
					return true;
				},
				false, false, &context));
		return result;
	}

namespace detail {
LogicalPlanReadRuntimeStats visitDeferredReferenceRows(storage::LogicalGraphReadSnapshot &snapshot,
    const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &bounds,
    const LogicalPlanReadRuntime::RowVisitor &visitor, const QueryContext &context) {
    validateBounds(bounds, static_cast<bool>(visitor));
    return executeOnSnapshot(snapshot, root, bounds,
        [&](LogicalPlanReadRow &row, ExecutionState &state) {
            enforceMaterializedRowBound(row, state);
            return invokeOutputVisitorChecked(state, [&] { return visitor(row); });
        }, &context, true, nullptr, false);
}
size_t retainedLogicalReadRowBytes(const LogicalPlanReadRow &row, const QueryContext &context) {
    return logicalReadRowBytes(row, &context);
}
}

} // namespace graph::query::execution
