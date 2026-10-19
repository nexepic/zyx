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

	namespace {

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;
		using InternalVisitor = std::function<bool(LogicalPlanReadRow)>;
		using ChargedRowVisitor = std::function<bool(LogicalPlanReadRow, MaterializedByteReservation)>;
		struct NodeColumnReceiver {
			std::span<const std::string_view> keys;
			std::function<bool(std::span<const std::string>,
				std::span<const std::vector<PropertyValue>>, const SelectionVector &,
				std::span<const size_t>)> consume;
			std::function<bool(std::span<const std::string>, const detail::DirectPropertyColumns &,
				const SelectionVector &, std::span<const size_t>)> consumeDirect{};
			const logical::LogicalOperator *filterOwner = nullptr;
		};
		[[nodiscard]] constexpr bool supportsChargedRowHandoff(logical::LogicalOpType type) {
			return type == logical::LogicalOpType::LOP_SORT || type == logical::LogicalOpType::LOP_FILTER ||
				   type == logical::LogicalOpType::LOP_PROJECT || type == logical::LogicalOpType::LOP_LIMIT ||
				   type == logical::LogicalOpType::LOP_SKIP || type == logical::LogicalOpType::LOP_UNWIND ||
				   type == logical::LogicalOpType::LOP_UNION || type == logical::LogicalOpType::LOP_JOIN ||
				   type == logical::LogicalOpType::LOP_OPTIONAL_MATCH ||
				   type == logical::LogicalOpType::LOP_CALL_SUBQUERY || type == logical::LogicalOpType::LOP_LOAD_CSV ||
				   type == logical::LogicalOpType::LOP_TRAVERSAL;
		}
		// Property batches are bounded independently of the result bound.  The
		// value is a memory-safety policy, not a semantic limit; callers can still
		// lower it through the existing maximum_rows bound.
		constexpr size_t DEFAULT_PROPERTY_BATCH_ROWS = 512U;

		enum class ShortestPathResultMode { SPR_FULL, SPR_SIZE, SPR_LENGTH };

		void validateTraversalTargetProperties(const logical::LogicalTraversal &traversal) {
			const auto &targetPredicates = traversal.getTargetProperties();
			for (size_t index = 0U; index < targetPredicates.size(); ++index) {
				const auto &key = targetPredicates[index].first;
				if (key.empty() || std::any_of(targetPredicates.begin(),
						targetPredicates.begin() + index,
						[&](const auto &predicate) { return predicate.first == key; })) {
					throw std::invalid_argument("logical-plan target Property predicate has an "
												"empty or duplicate key");
				}
			}
		}

		struct ScopedPropertyColumns {
			const std::string *variable = nullptr;
			const std::vector<std::string> *keys = nullptr;
			std::vector<uint64_t> ids;
			std::vector<std::vector<PropertyValue>> columns;
		};


		// Query owners outlive every task and all borrowed batch views.
		// Shared caches and budgets are currently confined to one execution thread.
		struct ExecutionSharedState {
			storage::LogicalGraphReadSnapshot &snapshot;
			const LogicalPlanReadRuntimeBounds &bounds;
			const QueryContext *context = nullptr;
			// Public row callbacks and mutation planning retain fully materialized
			// entities. Only the result pipeline owns the deferred-row contract.
			bool allow_deferred_entity_refs = false;
			// QueryResult consumers may safely read ahead within a bounded batch. Public
			// row visitors keep this disabled so cancellation never materializes a later
			// path row that the caller did not request.
			bool allow_cross_row_entity_batches = false;
			size_t resident_output_batch_bytes = 0U;
			MaterializedByteBudget materialized_budget = context && context->resource_account
				? MaterializedByteBudget(bounds.maximum_materialized_bytes - resident_output_batch_bytes, context->resource_account)
				: MaterializedByteBudget(bounds.maximum_materialized_bytes - resident_output_batch_bytes);
			detail::ProcedureBudgetHandoff procedure_handoff{materialized_budget};
			MaterializedByteReservation materialized_cache_reservation{&materialized_budget};
			MaterializedByteReservation final_result_reservation{&materialized_budget};
			size_t final_result_bytes = 0U;
			// Logical snapshots are immutable. A small per-query cache avoids repeated
			// point reads when a high-degree adjacency list shares targets. The byte
			// budget is independent of the entry bound: a single LargeValue-bearing
			// entity must not turn this convenience cache into an unbounded copy of
			// the Property store.
			detail::BudgetMemoryResource entity_cache_memory{materialized_budget};
			std::pmr::unordered_map<uint64_t, storage::LogicalGraphNode> node_cache{&entity_cache_memory};
			// Keep the same bounded reuse for Edge references that cross a result
			// boundary more than once (for example, a repeated path or projection).
			std::pmr::unordered_map<uint64_t, storage::LogicalGraphEdge> edge_cache{&entity_cache_memory};
			size_t materialized_cache_bytes = 0U;
			std::optional<detail::BoundExecutionProgram> bound_program{};
		};

		struct ExecutionTaskState {
			LogicalPlanReadRuntimeStats stats;
			const LogicalPlanReadRow *injected_row = nullptr;
			const ScopedPropertyColumns *property_columns = nullptr;
			detail::BoundExpressionInputs *bound_inputs = nullptr;
			const logical::LogicalSort *topk_sort = nullptr;
			size_t topk_rows = 0U;
		};

		// A task borrows query resources; scoped input/column views never become
		// query-global state. Parallel scheduling requires shared work admission
		// and explicit statistics merging before multiple tasks can run together.
		struct ExecutionState {
			ExecutionSharedState &shared;
			ExecutionTaskState task;
		};

		class ScopedBoundInputs final {
		public:
			ScopedBoundInputs(ExecutionState &state, std::optional<detail::BoundExpressionInputs> &inputs,
				const LogicalPlanReadRow &row) : state_(state), previous_(state.task.bound_inputs) {
				if (inputs) { binding_.emplace(*inputs, row); state_.task.bound_inputs = &*inputs; }
			}
			~ScopedBoundInputs() { state_.task.bound_inputs = previous_; }
			ScopedBoundInputs(const ScopedBoundInputs &) = delete;
			ScopedBoundInputs &operator=(const ScopedBoundInputs &) = delete;
		private:
			ExecutionState &state_;
			detail::BoundExpressionInputs *previous_;
			std::optional<detail::BoundExpressionInputs::RowBinding> binding_;
		};

		using ExecutionFinalizer = std::function<void(ExecutionState &, bool)>;

		constexpr size_t MAXIMUM_MATERIALIZED_CACHE_BYTES = 4U * 1024U * 1024U;

		// Complete objects are an explicit read shape as well.  Keeping this at the
		// runtime boundary ensures every entity materialization uses the same shape
		// contract as selected-column readers, while legacy snapshots can still use
		// the virtual fallback implementation.
		[[nodiscard]] storage::EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] storage::EntityReadShape completeEdgeReadShape() {
			return {.include_labels = false, .include_type = true, .all_properties = true};
		}

		// Pattern-local bindings are materialized only with the fields required by
		// their WHERE/map expressions, including explicit complete-map needs. A null
		// plan means an unsupported scope, preserving the complete-read fallback.
		// Unresolved tokens use the named-column boundary and its backend fallback. Complete-map EXISTS retains its
		// lazy visitor.
		struct PatternBindingReadShape {
			storage::EntityReadShape target;
			storage::EntityReadShape relationship;
			std::vector<std::string> target_named_properties;
			std::vector<std::string> relationship_named_properties;
		};

		[[nodiscard]] std::optional<PatternBindingReadShape>
		inspectPatternBindingReadShape(const expressions::Expression *whereExpression,
									   const expressions::Expression *mapExpression, std::string_view targetVariable,
									   std::string_view relationshipVariable,
									   storage::LogicalGraphReadSnapshot &snapshot);

		[[nodiscard]] size_t checkedSizeAdd(size_t left, size_t right, const char *message) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				throw std::overflow_error(message);
			}
			return left + right;
		}

		[[nodiscard]] size_t checkedSizeMul(size_t left, size_t right, const char *message) {
			if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
				throw std::overflow_error(message);
			}
			return left * right;
		}

		[[nodiscard]] size_t csvStringCapacityBytes(const std::string &value) {
			return detail::dynamicStringBufferBytes(value);
		}

		[[nodiscard]] size_t logicalNodeCacheBytes(const storage::LogicalGraphNode &node, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = sizeof(node);
			bytes = checkedSizeAdd(
					bytes,
					checkedSizeMul(node.labels.capacity(), sizeof(std::string), "logical-plan Node cache size overflowed"),
					"logical-plan Node cache size overflowed");
			size_t labelsUntilGuard = 256U;
			for (const std::string &label: node.labels) {
				if (--labelsUntilGuard == 0U) {
					if (context != nullptr) context->checkGuardNow();
					labelsUntilGuard = 256U;
				}
				bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), "logical-plan Node cache size overflowed");
			}
			bytes = checkedSizeAdd(bytes,
								   checkedSizeMul(node.properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry),
												  "logical-plan Node cache size overflowed"),
								   "logical-plan Node cache size overflowed");
			for (const storage::LogicalGraphPropertyEntry &entry: node.properties) {
				bytes = checkedSizeAdd(bytes, std::max(entry.key.size(), detail::dynamicStringBufferBytes(entry.key)), "logical-plan Node cache size overflowed");
				bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(entry.value, [&] { if (context != nullptr) context->checkGuardNow(); }),
									   "logical-plan Node cache size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalEdgeCacheBytes(const storage::LogicalGraphEdge &edge, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = sizeof(edge);
			bytes = checkedSizeAdd(bytes, std::max(edge.type.size(), detail::dynamicStringBufferBytes(edge.type)), "logical-plan Edge cache size overflowed");
			bytes = checkedSizeAdd(bytes,
								   checkedSizeMul(edge.properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry),
												  "logical-plan Edge cache size overflowed"),
								   "logical-plan Edge cache size overflowed");
			for (const storage::LogicalGraphPropertyEntry &entry: edge.properties) {
				bytes = checkedSizeAdd(bytes, std::max(entry.key.size(), detail::dynamicStringBufferBytes(entry.key)), "logical-plan Edge cache size overflowed");
				bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(entry.value, [&] { if (context != nullptr) context->checkGuardNow(); }),
									   "logical-plan Edge cache size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalReadValueBytes(const LogicalPlanReadValue &value, const QueryContext *context = nullptr) {
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return detail::retainedPropertyValueBytes(*property, [&] { if (context != nullptr) context->checkGuardNow(); });
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return logicalNodeCacheBytes(*node, context);
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return logicalEdgeCacheBytes(*edge, context);
			}
			return sizeof(storage::EntityRef);
		}

		template<typename Include>
		[[nodiscard]] size_t logicalReadRowBytesIncluding(const LogicalPlanReadRow &row, Include includes, const QueryContext *context = nullptr, bool includeValues = true, bool excludeBoundEntityCaches = false) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = sizeof(row);
			const auto invalidatedCache = [&](const std::string &alias) {
				if (!excludeBoundEntityCaches) return false;
				const auto binding = row.values.find(alias);
				return binding != row.values.end() &&
					(std::holds_alternative<storage::LogicalGraphNode>(binding->second) ||
					 std::holds_alternative<storage::LogicalGraphEdge>(binding->second));
			};
			size_t untilCheckpoint = 256U;
			const auto checkpoint = [&] {
				if (--untilCheckpoint == 0U) {
					if (context != nullptr) context->checkGuardNow();
					untilCheckpoint = 256U;
				}
			};
			for (const auto &[name, value]: row.values) {
				checkpoint();
				if (!includeValues || !includes(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row materialization size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(value), "logical-plan row materialization size overflowed");
				bytes = checkedSizeAdd(bytes, logicalReadValueBytes(value, context),
									   "logical-plan row materialization size overflowed");
			}
			for (const auto &[name, properties]: row.property_cache) {
				checkpoint();
				if (!includes(name) || invalidatedCache(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row materialization size overflowed");
				for (const auto &[key, value]: properties) {
					bytes = checkedSizeAdd(bytes, key.size(), "logical-plan row materialization size overflowed");
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }),
										   "logical-plan row materialization size overflowed");
				}
			}
			for (const auto &[name, metadata]: row.entity_metadata_cache) {
				checkpoint();
				if (!includes(name) || invalidatedCache(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row metadata size overflowed");
				for (const auto &[key, value]: metadata) {
					bytes = checkedSizeAdd(bytes, key.size(), "logical-plan row metadata size overflowed");
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }),
										   "logical-plan row metadata size overflowed");
				}
			}
			for (const auto &[name, path]: row.path_cache) {
				checkpoint();
				if (!includes(name))
					continue;
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row path materialization size overflowed");
				// Entity estimates include live record objects; charge only spare outer slots here.
				bytes = checkedSizeAdd(bytes, checkedSizeMul(path.nodes.capacity() - path.nodes.size(),
					sizeof(storage::LogicalGraphNode), "logical-plan row path materialization size overflowed"),
					"logical-plan row path materialization size overflowed");
				bytes = checkedSizeAdd(bytes, checkedSizeMul(path.edges.capacity() - path.edges.size(),
					sizeof(storage::LogicalGraphEdge), "logical-plan row path materialization size overflowed"),
					"logical-plan row path materialization size overflowed");
				for (const storage::LogicalGraphNode &node: path.nodes) {
					bytes = checkedSizeAdd(bytes, logicalNodeCacheBytes(node, context),
										   "logical-plan row path materialization size overflowed");
				}
				for (const storage::LogicalGraphEdge &edge: path.edges) {
					bytes = checkedSizeAdd(bytes, logicalEdgeCacheBytes(edge, context),
										   "logical-plan row path materialization size overflowed");
				}
				bytes = checkedSizeAdd(bytes,
									   checkedSizeMul(path.node_refs.capacity(), sizeof(storage::EntityRef),
													  "logical-plan row path materialization size overflowed"),
									   "logical-plan row path materialization size overflowed");
				bytes = checkedSizeAdd(bytes,
									   checkedSizeMul(path.edge_refs.capacity(), sizeof(storage::EntityRef),
													  "logical-plan row path materialization size overflowed"),
									   "logical-plan row path materialization size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalReadRowBytes(const LogicalPlanReadRow &row, const QueryContext *context = nullptr) {
			return logicalReadRowBytesIncluding(row, [](const std::string &) { return true; }, context);
		}

		void enforceMaterializedRowBound(const LogicalPlanReadRow &row, const ExecutionState &state) {
			if (logicalReadRowBytes(row, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan row exceeded its materialized byte bound");
			}
		}

		[[nodiscard]] size_t propertyMapBytes(const PropertyMap &properties, const char *message,
			const QueryContext *context = nullptr) {
			return detail::retainedPropertyMapBytes(properties, [&] {
				if (context != nullptr) context->checkGuardNow();
			});
		}

		[[nodiscard]] size_t
		directPropertyMapStorageBytes(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
									  const char *message, const QueryContext *context) {
			size_t bytes = checkedSizeAdd(
					sizeof(PropertyMap),
					checkedSizeMul(entries.size(), sizeof(PropertyMap::value_type) + sizeof(void *), message), message);
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				bytes = checkedSizeAdd(bytes, entry.key.size(), message);
				bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(entry.value, [&] { if (context != nullptr) context->checkGuardNow(); }), message);
			}
			return bytes;
		}

		[[nodiscard]] size_t resultValueBytes(const ResultValue &value, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			constexpr const char *message = "logical-plan result materialization size overflowed";
			if (value.isPrimitive()) {
				return detail::retainedPropertyValueBytes(value.asPrimitive(), [&] { if (context != nullptr) context->checkGuardNow(); });
			}
			if (value.isLogicalNode()) {
				const LogicalNodeResult &node = value.asLogicalNode();
				size_t bytes = checkedSizeAdd(sizeof(node),
					checkedSizeMul(node.labels.capacity(), sizeof(std::string), message), message);
				size_t labelsUntilGuard = 256U;
				for (const std::string &label: node.labels) {
					if (--labelsUntilGuard == 0U) {
						if (context != nullptr) context->checkGuardNow();
						labelsUntilGuard = 256U;
					}
					bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), message);
				}
				return checkedSizeAdd(bytes, propertyMapBytes(node.properties, message, context), message);
			}
			if (value.isLogicalEdge()) {
				const LogicalEdgeResult &edge = value.asLogicalEdge();
				const size_t bytes = checkedSizeAdd(sizeof(edge), std::max(edge.type.size(), detail::dynamicStringBufferBytes(edge.type)), message);
				return checkedSizeAdd(bytes, propertyMapBytes(edge.properties, message, context), message);
			}
			throw std::logic_error("logical-plan runtime produced a physical Entity result");
		}

		[[nodiscard]] size_t materializedResultRowBytes(const QueryResult::Row &row, const ExecutionState &state) {
			size_t bytes = sizeof(row);
			for (const auto &[name, value]: row) {
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(value), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, resultValueBytes(value, state.shared.context), "logical-plan result row size overflowed");
			}
			if (state.shared.context && state.shared.context->resource_account) {
				bytes = (std::max)(bytes, checkedSizeAdd(sizeof(row), graph::query::detail::resultRowHeapBytes(row),
					"logical-plan result heap storage overflowed"));
			}
			if (bytes > state.shared.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
			}
			return bytes;
		}

		[[nodiscard]] size_t queryResultColumnStorageBytes(const std::vector<std::string> &columns) {
			// Include the empty rows/notifications vector proxies in the retained
			// result metadata; their later capacity growth is accounted separately.
			return checkedSizeAdd(graph::query::detail::resultStringsHeapBytes(columns),
				2U * detail::standardContainerProxyBytes(), "logical-plan result column storage size overflowed");
		}

		void appendMaterializedResultRow(QueryResult &result, QueryResult::Row &&row, ExecutionState &state,
			MaterializedByteReservation &&rowReservation) {
			const size_t rowBytes = materializedResultRowBytes(row, state);
			const size_t previousBytes = state.shared.final_result_bytes;
			state.shared.final_result_reservation.absorb(std::move(rowReservation));
			size_t nextBytes = checkedSizeAdd(previousBytes, rowBytes, "logical-plan result retention size overflowed");
			const size_t currentCapacity = result.getRows().capacity();
			if (result.getRows().size() == currentCapacity) {
				const size_t minimumCapacity =
						checkedSizeAdd(result.getRows().size(), 1U, "logical-plan result row capacity overflowed");
				const size_t doubledCapacity =
						currentCapacity == 0U
								? minimumCapacity
								: checkedSizeMul(currentCapacity, 2U, "logical-plan result row capacity overflowed");
				const size_t requestedCapacity = std::max(minimumCapacity, doubledCapacity);
				const size_t requestedGrowthBytes = detail::standardAllocatorRequestBytes(
					checkedSizeMul(requestedCapacity, sizeof(QueryResult::Row),
						"logical-plan result row capacity overflowed"), alignof(QueryResult::Row));
                const size_t relocationBytes = graph::query::detail::resultRowsRelocationHeapUpper(result.getRows());
				const size_t transientBytes = checkedSizeAdd(nextBytes,
                    checkedSizeAdd(requestedGrowthBytes, relocationBytes, "logical-plan result relocation peak overflowed"),
                    "logical-plan result growth peak overflowed");
				state.shared.final_result_reservation.set(transientBytes);
				try {
					result.reserveRows(requestedCapacity);
				} catch (...) {
					state.shared.final_result_reservation.set(previousBytes);
					throw;
				}
				const size_t oldCapacityBytes = detail::standardAllocatorRequestBytes(
					checkedSizeMul(currentCapacity, sizeof(QueryResult::Row),
						"logical-plan result row capacity overflowed"), alignof(QueryResult::Row));
				const size_t actualCapacityBytes = detail::standardVectorHeapBufferBytes(result.getRows()) -
					detail::standardContainerProxyBytes();
				const size_t retainedPayloadBytes = previousBytes - oldCapacityBytes;
				nextBytes = checkedSizeAdd(
						checkedSizeAdd(retainedPayloadBytes, rowBytes, "logical-plan result retention size overflowed"),
						actualCapacityBytes, "logical-plan result retention size overflowed");
				state.shared.final_result_reservation.set(nextBytes);
			} else {
				state.shared.final_result_reservation.set(nextBytes);
			}
			try {
				result.addRow(std::move(row));
			} catch (...) {
				state.shared.final_result_reservation.set(previousBytes);
				throw;
			}
			state.shared.final_result_bytes = nextBytes;
		}

		[[nodiscard]] size_t propertyColumnBytes(const std::vector<std::vector<PropertyValue>> &columns,
												 std::span<const std::string_view> keys, const char *message, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			size_t bytes = checkedSizeMul(columns.capacity(), sizeof(std::vector<PropertyValue>), message);
			for (size_t column = 0U; column < columns.size(); ++column) {
				bytes = checkedSizeAdd(bytes, checkedSizeMul(columns[column].capacity(), sizeof(PropertyValue), message), message);
				if (column < keys.size()) {
					bytes = checkedSizeAdd(bytes, keys[column].size(), message);
				}
				for (const PropertyValue &value: columns[column]) {
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }), message);
				}
			}
			return bytes;
		}

		[[nodiscard]] PropertyValue copyReservedPropertyValue(const PropertyValue &source,
				ExecutionState &state, MaterializedByteReservation &reservation) {
			const auto checkpoint = [&] { if (state.shared.context != nullptr) state.shared.context->checkGuardNow(); };
			const size_t previous = reservation.bytes();
			constexpr const char *message = "logical-plan copied Property payload size overflowed";
			reservation.set(checkedSizeAdd(previous, detail::retainedPropertyValueBytes(source, checkpoint), message));
			try {
				PropertyValue copy = state.shared.materialized_budget.transfersReservations()
                    ? graph::property_copy::copyPropertyValueGuarded(source, checkpoint)
                    : detail::copyPropertyValueGuarded(source, checkpoint);
				reservation.set(checkedSizeAdd(previous, detail::retainedPropertyValueBytes(copy, checkpoint), message));
				return copy;
			} catch (...) {
				reservation.set(previous);
				throw;
			}
		}

		[[nodiscard]] size_t logicalIndexCandidateBatchBytes(const storage::IndexCandidateBatch &batch, const QueryContext *context) {
			if (context != nullptr) context->checkGuardNow();
			constexpr const char *message = "logical-plan index candidate batch size overflowed";
			size_t bytes = checkedSizeMul(batch.candidates.capacity(), sizeof(storage::EntityRef), message);
			bytes = checkedSizeAdd(bytes, checkedSizeMul(batch.verified_property_columns.capacity(),
				sizeof(storage::IndexCandidateBatch::VerifiedPropertyColumn), message), message);
			for (const storage::IndexCandidateBatch::VerifiedPropertyColumn &column: batch.verified_property_columns) {
				bytes = checkedSizeAdd(bytes, std::max(column.key.capacity(), detail::dynamicStringBufferBytes(column.key)), message);
				bytes = checkedSizeAdd(bytes, checkedSizeMul(column.values.capacity(), sizeof(PropertyValue), message),
									   message);
				for (const PropertyValue &value: column.values) {
					bytes = checkedSizeAdd(bytes, detail::retainedPropertyValueBytes(value, [&] { if (context != nullptr) context->checkGuardNow(); }), message);
				}
			}
			return bytes;
		}

		void checkGuard(const ExecutionState &state) {
			if (state.shared.context != nullptr) {
				state.shared.context->checkGuard();
			}
		}

		// Copy/accounting helpers already bound work between checkpoints. Do not
		// apply the per-row guard sampling interval a second time at that boundary.
		void checkGuardNow(const ExecutionState &state) {
			if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
		}

		void validateNode(const storage::LogicalGraphNode &node, ExecutionState &state);
		void validateEdge(const storage::LogicalGraphEdge &edge);

		template<typename Record, typename Cache>
		void cacheEntityRecord(ExecutionState &state, Cache &cache, uint64_t id, Record &&record, size_t bytes,
							   size_t maximumRecords) {
			if (detail::ProcedurePayloadAdmissionScope::active()) return;
			if (cache.contains(id) || cache.size() >= maximumRecords ||
				bytes > MAXIMUM_MATERIALIZED_CACHE_BYTES -
								std::min(state.shared.materialized_cache_bytes, MAXIMUM_MATERIALIZED_CACHE_BYTES)) {
				return;
			}
			const size_t previousBytes = state.shared.materialized_cache_bytes;
			const size_t previousReservedBytes = state.shared.materialized_cache_reservation.bytes();
			using CachedRecord = typename Cache::mapped_type;
			state.shared.materialized_cache_reservation.set(checkedSizeAdd(previousReservedBytes, bytes - sizeof(CachedRecord),
				"logical-plan Entity cache payload size overflowed"));
			try {
				CachedRecord retained = [&]() -> CachedRecord {
					if constexpr (std::is_lvalue_reference_v<Record>) {
						return detail::copyLogicalEntityGuarded(record, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
					} else {
						return std::forward<Record>(record);
					}
				}();
				if constexpr (std::is_same_v<CachedRecord, storage::LogicalGraphNode>)
					bytes = logicalNodeCacheBytes(retained, state.shared.context);
				else
					bytes = logicalEdgeCacheBytes(retained, state.shared.context);
				const size_t retainedBytes = checkedSizeAdd(previousBytes, bytes,
					"logical-plan Entity cache size overflowed");
				if (retainedBytes > MAXIMUM_MATERIALIZED_CACHE_BYTES)
					throw std::runtime_error("logical-plan Entity cache exceeded its byte bound");
				state.shared.materialized_cache_reservation.set(checkedSizeAdd(previousReservedBytes, bytes - sizeof(CachedRecord),
					"logical-plan Entity cache payload size overflowed"));
				if (!cache.emplace(id, std::move(retained)).second) {
					state.shared.materialized_cache_reservation.set(previousReservedBytes);
					return;
				}
				state.shared.materialized_cache_bytes = retainedBytes;
			} catch (...) {
				state.shared.materialized_cache_reservation.set(previousReservedBytes);
				throw;
			}
		}

		void cacheNodeRecord(ExecutionState &state, const storage::LogicalGraphNode &node) {
			cacheEntityRecord(state, state.shared.node_cache, node.node_id, node, logicalNodeCacheBytes(node, state.shared.context),
							  state.shared.bounds.maximum_node_records);
		}
		void cacheNodeRecord(ExecutionState &state, storage::LogicalGraphNode &&node) {
			const uint64_t id = node.node_id;
			const size_t bytes = logicalNodeCacheBytes(node, state.shared.context);
			cacheEntityRecord(state, state.shared.node_cache, id, std::move(node), bytes, state.shared.bounds.maximum_node_records);
		}

		void cacheEdgeRecord(ExecutionState &state, const storage::LogicalGraphEdge &edge) {
			cacheEntityRecord(state, state.shared.edge_cache, edge.edge_id, edge, logicalEdgeCacheBytes(edge, state.shared.context),
							  state.shared.bounds.maximum_edge_records);
		}
		void cacheEdgeRecord(ExecutionState &state, storage::LogicalGraphEdge &&edge) {
			const uint64_t id = edge.edge_id;
			const size_t bytes = logicalEdgeCacheBytes(edge, state.shared.context);
			cacheEntityRecord(state, state.shared.edge_cache, id, std::move(edge), bytes, state.shared.bounds.maximum_edge_records);
		}

		// Borrow complete cache records while preparing an independently owned output.
		// unordered_map rehash preserves references; callers finish using this borrow
		// before invoking downstream visitors.
		[[nodiscard]] const storage::LogicalGraphNode &readNodeCachedBorrowed(ExecutionState &state, uint64_t nodeId,
				std::optional<storage::LogicalGraphNode> &uncached, MaterializedByteReservation &uncachedBytes) {
			if (const auto cached = state.shared.node_cache.find(nodeId); cached != state.shared.node_cache.end()) return cached->second;
			storage::LogicalGraphNode node;
			if (state.shared.snapshot.supportsShapedPropertyReadAdmission()) {
				uncachedBytes.set(sizeof(storage::LogicalGraphNode));
				const std::array<uint64_t, 1U> ids{nodeId};
				auto records = state.shared.snapshot.readNodesShapedSortedWithAdmission(ids, completeNodeReadShape(), [&](size_t bytes) {
					checkGuardNow(state);
					uncachedBytes.set(checkedSizeAdd(uncachedBytes.bytes(), bytes, "logical-plan target source size overflowed"));
				});
				if (records.size() != 1U) throw std::runtime_error("logical graph backend changed typed Node batch cardinality");
				node = std::move(records.front());
			} else {
				node = state.shared.snapshot.readNodeShaped(nodeId, completeNodeReadShape());
			}
			validateNode(node, state);
			if (node.node_id != nodeId) throw std::runtime_error("logical graph shaped Node read changed logical identity");
			// Cache admission owns the moved payload; no clone is constructed here.
			uncachedBytes.reset();
			cacheNodeRecord(state, std::move(node));
			if (const auto cached = state.shared.node_cache.find(nodeId); cached != state.shared.node_cache.end()) return cached->second;
			uncachedBytes.set(logicalNodeCacheBytes(node, state.shared.context));
			uncached.emplace(std::move(node));
			return *uncached;
		}

		[[nodiscard]] storage::LogicalGraphNode readNodeCached(ExecutionState &state, uint64_t nodeId) {
			if (const auto cached = state.shared.node_cache.find(nodeId); cached != state.shared.node_cache.end()) {
				return detail::copyLogicalEntityGuarded(cached->second, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			storage::LogicalGraphNode node = state.shared.snapshot.readNodeShaped(nodeId, completeNodeReadShape());
			validateNode(node, state);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph shaped Node read changed logical identity");
			}
			cacheNodeRecord(state, node);
			return node;
		}

		[[nodiscard]] storage::LogicalGraphEdge readEdgeCached(ExecutionState &state, uint64_t edgeId) {
			if (const auto cached = state.shared.edge_cache.find(edgeId); cached != state.shared.edge_cache.end()) {
				return detail::copyLogicalEntityGuarded(cached->second, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			storage::LogicalGraphEdge edge = state.shared.snapshot.readEdgeShaped(edgeId, completeEdgeReadShape());
			validateEdge(edge);
			if (edge.edge_id != edgeId) {
				throw std::runtime_error("logical graph shaped Edge read changed logical identity");
			}
			cacheEdgeRecord(state, edge);
			return edge;
		}

		void validateBounds(const LogicalPlanReadRuntimeBounds &bounds, bool hasVisitor) {
			if (!hasVisitor || bounds.maximum_node_records == 0U || bounds.maximum_node_page_records == 0U ||
				bounds.maximum_metadata_pages_per_node_page == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_edge_records == 0U ||
				bounds.maximum_edge_records == std::numeric_limits<size_t>::max() ||
				bounds.maximum_adjacency_page_records == 0U || bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U || bounds.maximum_distinct_rows == 0U ||
				bounds.maximum_materialized_bytes == 0U || bounds.maximum_candidate_bytes == 0U ||
				bounds.maximum_node_page_records > bounds.maximum_node_records ||
				bounds.maximum_adjacency_page_records > bounds.maximum_edges_per_node) {
				throw std::invalid_argument(
						"logical-plan read runtime requires positive compatible bounds and a visitor");
			}
		}

		void incrementBounded(size_t &value, const char *message) {
			if (value == std::numeric_limits<size_t>::max()) { // ZYX_COV_EXCL_LINE: bounded query resources cannot
															   // reach SIZE_MAX.
				throw std::overflow_error(message);
			}
			++value;
		}

		void validateAdjacentEdgeOrder(uint64_t edgeId, uint64_t &previousEdgeId, const char *context) {
			if (edgeId == 0U || (previousEdgeId != 0U && edgeId <= previousEdgeId)) {
				throw std::runtime_error(std::string("logical graph backend returned non-monotonic ") + context +
										 " adjacency Edge IDs");
			}
			previousEdgeId = edgeId;
		}

		[[nodiscard]] int hexDigit(char value) {
			if (value >= '0' && value <= '9') {
				return value - '0';
			}
			if (value >= 'a' && value <= 'f') {
				return value - 'a' + 10;
			}
			if (value >= 'A' && value <= 'F') {
				return value - 'A' + 10;
			}
			return -1;
		}

		[[nodiscard]] std::string decodeCsvFileUriPath(std::string_view uri) {
			constexpr std::string_view FILE_URI_PREFIX = "file://";
			if (!uri.starts_with(FILE_URI_PREFIX)) {
				throw std::invalid_argument("logical LOAD CSV accepts only file: URIs");
			}
			const std::string_view encodedPath = uri.substr(FILE_URI_PREFIX.size());
			if (encodedPath.empty() || encodedPath.front() != '/' ||
				encodedPath.find_first_of("?#") != std::string_view::npos) {
				throw std::invalid_argument("logical LOAD CSV has an invalid local-file URI");
			}
			std::string decoded;
			decoded.reserve(encodedPath.size());
			for (size_t index = 0U; index < encodedPath.size(); ++index) {
				const char current = encodedPath[index];
				if (current != '%') {
					if (current == '\0') {
						throw std::invalid_argument("logical LOAD CSV URI contains a NUL byte");
					}
					decoded += current;
					continue;
				}
				if (index + 2U >= encodedPath.size()) {
					throw std::invalid_argument("logical LOAD CSV URI has invalid percent encoding");
				}
				const int high = hexDigit(encodedPath[index + 1U]);
				const int low = hexDigit(encodedPath[index + 2U]);
				if (high < 0 || low < 0) {
					throw std::invalid_argument("logical LOAD CSV URI has invalid percent encoding");
				}
				const char decodedByte = static_cast<char>((high << 4) | low);
				if (decodedByte == '\0') {
					throw std::invalid_argument("logical LOAD CSV URI contains a NUL byte");
				}
				decoded += decodedByte;
				index += 2U;
			}
#ifdef _WIN32
			// A local Windows file URI uses file:///C:/..., never a UNC authority.
			if (decoded.size() >= 3U && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) &&
				decoded[2] == ':') {
				decoded.erase(0U, 1U);
			} else if (decoded.starts_with("//")) {
				throw std::invalid_argument("logical LOAD CSV does not permit UNC file paths");
			}
#endif
			return decoded;
		}

		[[nodiscard]] bool isWithinImportRoot(const std::filesystem::path &candidate,
											  const std::filesystem::path &root) {
			auto candidatePart = candidate.begin();
			for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++candidatePart) {
				if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::filesystem::path resolveCsvImportFile(const PropertyValue &value,
																 const QueryContext &context) {
			if (!context.csv_import_policy.import_root.has_value()) {
				throw std::runtime_error("logical LOAD CSV is disabled because no import root is configured");
			}
			if (context.csv_import_policy.maximum_file_bytes == 0U || context.csv_import_policy.maximum_rows == 0U ||
				context.csv_import_policy.maximum_record_bytes == 0U ||
				context.csv_import_policy.maximum_record_bytes > context.csv_import_policy.maximum_file_bytes ||
				context.csv_import_policy.maximum_columns == 0U ||
				context.csv_import_policy.maximum_field_bytes == 0U ||
				context.csv_import_policy.maximum_field_bytes > context.csv_import_policy.maximum_file_bytes) {
				throw std::invalid_argument("logical LOAD CSV has invalid import resource limits");
			}
			const auto *uri = std::get_if<std::string>(&value.getVariant());
			if (uri == nullptr) {
				throw std::invalid_argument("logical LOAD CSV URI expression must evaluate to a string");
			}
			const std::filesystem::path requested(decodeCsvFileUriPath(*uri));
			if (!requested.is_absolute()) {
				throw std::invalid_argument("logical LOAD CSV requires an absolute file URI path");
			}

			std::error_code error;
			const std::filesystem::path root =
					std::filesystem::weakly_canonical(*context.csv_import_policy.import_root, error);
			if (error || !std::filesystem::is_directory(root, error) || error) {
				throw std::runtime_error("logical LOAD CSV import root is inaccessible");
			}
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(requested, error);
			if (error || !std::filesystem::exists(canonical, error) || error ||
				!std::filesystem::is_regular_file(canonical, error) || error) {
				throw std::runtime_error("logical LOAD CSV requires an accessible regular file");
			}
			if (!isWithinImportRoot(canonical, root)) {
				throw std::runtime_error("logical LOAD CSV file is outside the configured import root");
			}
			const uintmax_t fileBytes = std::filesystem::file_size(canonical, error);
			if (error || fileBytes > static_cast<uintmax_t>(context.csv_import_policy.maximum_file_bytes)) {
				throw std::runtime_error("logical LOAD CSV file exceeds its byte bound");
			}
			return canonical;
		}

		[[nodiscard]] const logical::LogicalOperator &requireOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.size() != 1U ||
				children.front() == nullptr) { // ZYX_COV_EXCL_LINE: planner-owned unary operators expose one child or
											   // throw before execution.
				throw std::runtime_error("logical-plan read runtime received an operator without one valid child");
			}
			return *children.front();
		}

		[[nodiscard]] bool isNull(const PropertyValue &value) { return value.getType() == PropertyType::NULL_TYPE; }

		[[nodiscard]] std::optional<bool> booleanOrNull(const PropertyValue &value) {
			if (isNull(value)) {
				return std::nullopt;
			}
			if (const bool *boolean = std::get_if<bool>(&value.getVariant())) {
				return *boolean;
			}
			throw std::runtime_error("logical-plan read runtime requires a Boolean predicate value");
		}

		[[nodiscard]] PropertyValue makeBoolean(std::optional<bool> value) {
			return value.has_value() ? PropertyValue(*value) : PropertyValue();
		}

		[[nodiscard]] int64_t checkedAdd(int64_t left, int64_t right) {
			if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
				(right < 0 && left < std::numeric_limits<int64_t>::min() - right)) {
				throw std::overflow_error("logical-plan integer addition overflowed");
			}
			return left + right;
		}

		[[nodiscard]] int64_t checkedSubtract(int64_t left, int64_t right) {
			if ((right > 0 && left < std::numeric_limits<int64_t>::min() + right) ||
				(right < 0 && left > std::numeric_limits<int64_t>::max() + right)) {
				throw std::overflow_error("logical-plan integer subtraction overflowed");
			}
			return left - right;
		}

		[[nodiscard]] int64_t checkedMultiply(int64_t left, int64_t right) {
			if (left == 0 || right == 0) {
				return 0;
			}
			if ((left == -1 && right == std::numeric_limits<int64_t>::min()) ||
				(right == -1 && left == std::numeric_limits<int64_t>::min())) {
				throw std::overflow_error("logical-plan integer multiplication overflowed");
			}
			if ((left > 0 && right > 0 && left > std::numeric_limits<int64_t>::max() / right) ||
				(left > 0 && right < 0 && right < std::numeric_limits<int64_t>::min() / left) ||
				(left < 0 && right > 0 && left < std::numeric_limits<int64_t>::min() / right) ||
				(left < 0 && right < 0 && left < std::numeric_limits<int64_t>::max() / right)) {
				throw std::overflow_error("logical-plan integer multiplication overflowed");
			}
			return left * right;
		}

		[[nodiscard]] std::pair<int, int> addMonthsToYearMonth(int year, int month, int64_t months) {
			const int64_t zeroBasedMonth = checkedAdd(checkedMultiply(static_cast<int64_t>(year), 12LL),
													  checkedAdd(static_cast<int64_t>(month) - 1LL, months));
			int64_t resultYear = zeroBasedMonth / 12LL;
			int64_t resultMonth = zeroBasedMonth % 12LL;
			if (resultMonth < 0LL) {
				resultMonth += 12LL;
				--resultYear;
			}
			if (resultYear < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
				resultYear > static_cast<int64_t>(std::numeric_limits<int>::max())) {
				throw std::overflow_error("logical-plan temporal year overflowed");
			}
			return {static_cast<int>(resultYear), static_cast<int>(resultMonth + 1LL)};
		}

		[[nodiscard]] int64_t durationWholeDays(const TemporalDuration &duration) {
			constexpr int64_t NANOS_PER_DAY = 86'400LL * 1'000'000'000LL;
			return checkedAdd(duration.days, duration.nanos / NANOS_PER_DAY);
		}

		[[nodiscard]] PropertyValue addDurationToDate(const TemporalDate &date, const TemporalDuration &duration) {
			const auto [year, month] = addMonthsToYearMonth(date.year(), date.month(), duration.months);
			const TemporalDate monthAdjusted = TemporalDate::fromYMD(year, month, date.day());
			const int64_t epochDays =
					checkedAdd(static_cast<int64_t>(monthAdjusted.epochDays), durationWholeDays(duration));
			if (epochDays < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
				epochDays > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
				throw std::overflow_error("logical-plan temporal date overflowed");
			}
			return PropertyValue(TemporalDate{static_cast<int32_t>(epochDays)});
		}

		[[nodiscard]] PropertyValue addDurationToDateTime(const TemporalDateTime &dateTime,
														  const TemporalDuration &duration) {
			const auto [year, month] = addMonthsToYearMonth(dateTime.year(), dateTime.month(), duration.months);
			TemporalDateTime result = TemporalDateTime::fromComponents(year, month, dateTime.day(), dateTime.hour(),
																	   dateTime.minute(), dateTime.second());
			constexpr int64_t MILLIS_PER_DAY = 86'400LL * 1'000LL;
			const int64_t dayMillis = checkedMultiply(duration.days, MILLIS_PER_DAY);
			const int64_t nanosMillis = duration.nanos / 1'000'000LL;
			result.epochMillis = checkedAdd(result.epochMillis, checkedAdd(dayMillis, nanosMillis));
			return PropertyValue(result);
		}

		[[nodiscard]] PropertyValue temporalComponent(const PropertyValue &value, std::string_view component) {
			if (value.getType() == PropertyType::DATE) {
				const TemporalDate &date = std::get<TemporalDate>(value.getVariant());
				if (component == "year")
					return PropertyValue(static_cast<int64_t>(date.year()));
				if (component == "month")
					return PropertyValue(static_cast<int64_t>(date.month()));
				if (component == "day")
					return PropertyValue(static_cast<int64_t>(date.day()));
				if (component == "epochDays")
					return PropertyValue(static_cast<int64_t>(date.epochDays));
			}
			if (value.getType() == PropertyType::DATETIME) {
				const TemporalDateTime &dateTime = std::get<TemporalDateTime>(value.getVariant());
				if (component == "year")
					return PropertyValue(static_cast<int64_t>(dateTime.year()));
				if (component == "month")
					return PropertyValue(static_cast<int64_t>(dateTime.month()));
				if (component == "day")
					return PropertyValue(static_cast<int64_t>(dateTime.day()));
				if (component == "hour")
					return PropertyValue(static_cast<int64_t>(dateTime.hour()));
				if (component == "minute")
					return PropertyValue(static_cast<int64_t>(dateTime.minute()));
				if (component == "second")
					return PropertyValue(static_cast<int64_t>(dateTime.second()));
				if (component == "epochMillis")
					return PropertyValue(dateTime.epochMillis);
			}
			if (value.getType() == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(value.getVariant());
				if (component == "months")
					return PropertyValue(duration.months);
				if (component == "days")
					return PropertyValue(duration.days);
				if (component == "seconds")
					return PropertyValue(duration.nanos / 1'000'000'000LL);
				if (component == "nanoseconds")
					return PropertyValue(duration.nanos);
			}
			return PropertyValue();
		}

		[[nodiscard]] PropertyValue scalarValue(const LogicalPlanReadValue &value) {
			if (const PropertyValue *property = std::get_if<PropertyValue>(&value)) {
				return *property;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return PropertyValue(node->node_id);
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return PropertyValue(edge->edge_id);
			}
			return PropertyValue(std::get<storage::EntityRef>(value).entity_id);
		}

		void validateNode(const storage::LogicalGraphNode &node, ExecutionState &state) {
			if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			if (node.node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Node");
			}
			detail::BudgetMemoryResource labelMemory(state.shared.materialized_budget);
			std::pmr::unordered_set<std::string_view> labels(&labelMemory);
			detail::procedureVectorReserve(labels, node.labels.size());
			size_t labelsUntilGuard = 256U;
			for (const std::string &label: node.labels) {
				if (--labelsUntilGuard == 0U) {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					labelsUntilGuard = 256U;
				}
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical graph backend returned an invalid Node label set");
				}
			}
		}

		void validateNodeIdentity(ExecutionState &state, uint64_t nodeId) {
			checkGuard(state);
			const auto node = state.shared.snapshot.readNodeShaped(nodeId, {});
			validateNode(node, state);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph endpoint identity changed during validation");
			}
		}

		void validateEdge(const storage::LogicalGraphEdge &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Edge");
			}
		}

		void validateEdge(const storage::LogicalGraphEdgeTopology &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Edge topology");
			}
		}

		void validateEntityRef(const storage::EntityRef &reference) {
			if (reference.entity_id == 0U ||
				(reference.is_edge && (reference.source_node_id == 0U || reference.target_node_id == 0U))) {
				throw std::runtime_error("logical graph backend returned an invalid Entity reference");
			}
		}

		void validateNodeRef(const storage::EntityRef &reference, const char *message) {
			if (reference.entity_id == 0U || reference.is_edge) {
				throw std::runtime_error(message);
			}
		}

		void validateEdgeRef(const storage::EntityRef &reference, const char *message) {
			if (reference.entity_id == 0U || !reference.is_edge || reference.source_node_id == 0U ||
				reference.target_node_id == 0U) {
				throw std::runtime_error(message);
			}
		}

		void validateEdgeReferenceRecord(const storage::EntityRef &reference, const storage::LogicalGraphEdge &edge,
										 ExecutionState &state, const char *context) {
			validateEdge(edge);
			if (edge.edge_id != reference.entity_id || edge.source_node_id != reference.source_node_id ||
				edge.target_node_id != reference.target_node_id) {
				throw std::runtime_error(std::string(context) +
										 " topology disagrees with its materialized relationship");
			}
			if (reference.type_token != 0U && state.shared.snapshot.supportsNativeRelationshipTypeTokens(edge.type)) {
				const std::optional<uint64_t> resolved = state.shared.snapshot.findRelationshipTypeToken(edge.type);
				if (!resolved.has_value() || *resolved != reference.type_token) {
					throw std::runtime_error(std::string(context) +
											 " relationship type disagrees with its materialized relationship");
				}
			}
		}

		void appendMissingReferenceId(std::vector<uint64_t> &ids, uint64_t id, size_t maximumCount,
									  MaterializedByteReservation &slots) {
			if (ids.size() == ids.capacity()) {
				const size_t previousBytes = slots.bytes();
				const size_t capacity = ids.capacity();
				const size_t nextCapacity =
						capacity == 0U
								? 1U
								: std::min(maximumCount, capacity > maximumCount / 2U ? maximumCount : capacity * 2U);
				slots.set(checkedSizeMul(nextCapacity, sizeof(uint64_t),
										 "logical-plan reference ID buffer size overflowed"));
				try {
					ids.reserve(nextCapacity);
					slots.set(checkedSizeMul(ids.capacity(), sizeof(uint64_t),
											 "logical-plan reference ID buffer size overflowed"));
				} catch (...) {
					slots.set(previousBytes);
					throw;
				}
			}
			ids.push_back(id);
		}

		template<typename Record>
		void appendReferenceFallback(std::vector<Record> &records, Record &&record, size_t recordBytes,
									 size_t maximumCount, size_t &heapBytes, MaterializedByteReservation &reservation) {
			const size_t nextHeapBytes = checkedSizeAdd(heapBytes, recordBytes - sizeof(Record),
														"logical-plan reference fallback size overflowed");
			const size_t previousBytes = reservation.bytes();
			const size_t capacity = records.capacity();
			const size_t nextCapacity =
					records.size() == capacity
							? (capacity == 0U ? 1U
											  : std::min(maximumCount,
														 capacity > maximumCount / 2U ? maximumCount : capacity * 2U))
							: capacity;
			reservation.set(checkedSizeAdd(
					nextHeapBytes,
					checkedSizeMul(nextCapacity, sizeof(Record), "logical-plan reference fallback size overflowed"),
					"logical-plan reference fallback size overflowed"));
			try {
				if (nextCapacity != capacity)
					records.reserve(nextCapacity);
				reservation.set(checkedSizeAdd(nextHeapBytes,
											   checkedSizeMul(records.capacity(), sizeof(Record),
															  "logical-plan reference fallback size overflowed"),
											   "logical-plan reference fallback size overflowed"));
				records.push_back(std::move(record));
			} catch (...) {
				reservation.set(previousBytes);
				throw;
			}
			heapBytes = nextHeapBytes;
		}

		[[nodiscard]] std::vector<storage::LogicalGraphNode>
		readNodesShapedSortedBudgeted(ExecutionState &state, std::span<const uint64_t> ids,
									  const storage::EntityReadShape &shape,
									  MaterializedByteReservation *retainedSource = nullptr) {
			MaterializedByteReservation temporarySource(&state.shared.materialized_budget);
			MaterializedByteReservation &sourceBytes = retainedSource == nullptr ? temporarySource : *retainedSource;
			if (retainedSource != nullptr) {
				sourceBytes.set(checkedSizeMul(ids.size(), sizeof(storage::LogicalGraphNode),
											   "logical-plan shaped Node source frame size overflowed"));
			}
			if (!state.shared.snapshot.supportsShapedPropertyReadAdmission()) {
				return state.shared.snapshot.readNodesShapedSorted(ids, shape);
			}
			return state.shared.snapshot.readNodesShapedSortedWithAdmission(ids, shape, [&](size_t bytes) {
				checkGuard(state);
				sourceBytes.set(
						checkedSizeAdd(sourceBytes.bytes(), bytes, "logical-plan shaped Node source size overflowed"));
			});
		}

		[[nodiscard]] std::vector<storage::LogicalGraphEdge>
		readEdgesShapedSortedBudgeted(ExecutionState &state, std::span<const uint64_t> ids,
									  const storage::EntityReadShape &shape,
									  MaterializedByteReservation *retainedSource = nullptr) {
			MaterializedByteReservation temporarySource(&state.shared.materialized_budget);
			MaterializedByteReservation &sourceBytes = retainedSource == nullptr ? temporarySource : *retainedSource;
			if (retainedSource != nullptr) {
				sourceBytes.set(checkedSizeMul(ids.size(), sizeof(storage::LogicalGraphEdge),
											   "logical-plan shaped Edge source frame size overflowed"));
			}
			if (!state.shared.snapshot.supportsShapedPropertyReadAdmission()) {
				return state.shared.snapshot.readEdgesShapedSorted(ids, shape);
			}
			return state.shared.snapshot.readEdgesShapedSortedWithAdmission(ids, shape, [&](size_t bytes) {
				checkGuard(state);
				sourceBytes.set(
						checkedSizeAdd(sourceBytes.bytes(), bytes, "logical-plan shaped Edge source size overflowed"));
			});
		}

		[[nodiscard]] size_t packedBooleanBytes(size_t capacity) {
			constexpr size_t bitsPerByte = std::numeric_limits<unsigned char>::digits;
			return capacity / bitsPerByte + (capacity % bitsPerByte != 0U ? 1U : 0U);
		}

		void retainShapedBatchBytes(const std::vector<storage::LogicalGraphNode> &records,
									MaterializedByteReservation &reservation, const QueryContext *context) {
			size_t bytes = checkedSizeMul(records.capacity(), sizeof(storage::LogicalGraphNode),
										  "logical-plan shaped Node batch capacity overflowed");
			for (const auto &record: records) {
				bytes = checkedSizeAdd(bytes, logicalNodeCacheBytes(record, context) - sizeof(record),
									   "logical-plan shaped Node batch size overflowed");
			}
			reservation.set(bytes);
		}

		void retainShapedBatchBytes(const std::vector<storage::LogicalGraphEdge> &records,
									MaterializedByteReservation &reservation, const QueryContext *context) {
			size_t bytes = checkedSizeMul(records.capacity(), sizeof(storage::LogicalGraphEdge),
										  "logical-plan shaped Edge batch capacity overflowed");
			for (const auto &record: records) {
				bytes = checkedSizeAdd(bytes, logicalEdgeCacheBytes(record, context) - sizeof(record),
									   "logical-plan shaped Edge batch size overflowed");
			}
			reservation.set(bytes);
		}

		void admitReferenceResultBytes(MaterializedByteReservation &reservation, size_t bytes, const char *message) {
			if (reservation.wouldExceed(bytes))
				throw std::runtime_error(message);
			reservation.set(bytes);
		}

		[[nodiscard]] std::vector<storage::LogicalGraphNode>
		materializeNodeReferences(std::span<const storage::EntityRef> references, ExecutionState &state,
								  const bool collectResults = true,
								  MaterializedByteReservation *resultBytes = nullptr) {
			std::vector<uint64_t> missingIds;
			MaterializedByteReservation missingIdSlots(&state.shared.materialized_budget);
			for (const storage::EntityRef &reference: references) {
				validateNodeRef(
						reference,
						"logical-plan materialization received an invalid or Edge reference where a Node was required");
				if (!state.shared.node_cache.contains(reference.entity_id)) {
					appendMissingReferenceId(missingIds, reference.entity_id, references.size(), missingIdSlots);
				}
			}
			std::ranges::sort(missingIds);
			missingIds.erase(std::unique(missingIds.begin(), missingIds.end()), missingIds.end());
			if (missingIds.size() > state.shared.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan Node materialization exceeded its record bound");
			}

			MaterializedByteReservation fallbackBytes(&state.shared.materialized_budget);
			std::vector<storage::LogicalGraphNode> loaded;
			size_t fallbackHeapBytes = 0U;
			// A record enters this fallback only when the shared cache declines it.
			for (size_t offset = 0U; offset < missingIds.size();) {
				checkGuard(state);
				size_t end = std::min(missingIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
				MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
				std::vector<storage::LogicalGraphNode> batch;
				while (true) {
					const auto ids = std::span<const uint64_t>(missingIds).subspan(offset, end - offset);
					try {
						batch = readNodesShapedSortedBudgeted(state, ids, completeNodeReadShape(), &sourceBytes);
						if (batch.size() != end - offset) {
							throw std::runtime_error("logical-plan shaped Node batch changed result cardinality");
						}
						for (size_t index = 0U; index < batch.size(); ++index) {
							validateNode(batch[index], state);
							if (batch[index].node_id != missingIds[offset + index]) {
								throw std::runtime_error("logical-plan shaped Node batch changed logical ID order");
							}
						}
						retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
						break;
					} catch (const std::runtime_error &error) {
						if (std::string_view(error.what()) != "shared materialization exceeded its byte bound" ||
							end - offset == 1U) {
							throw;
						}
						std::vector<storage::LogicalGraphNode>().swap(batch);
						sourceBytes.reset();
						end = offset + std::max<size_t>(1U, (end - offset) / 2U);
					}
				}
				for (size_t index = 0U; index < batch.size(); ++index) {
					const uint64_t id = batch[index].node_id;
					const size_t recordBytes = logicalNodeCacheBytes(batch[index], state.shared.context);
					const size_t previousSourceBytes = sourceBytes.bytes();
					// The vector frame remains live, but its heap payload is transferred
					// to either the cache or the fallback buffer.
					sourceBytes.set(previousSourceBytes - (recordBytes - sizeof(storage::LogicalGraphNode)));
					cacheNodeRecord(state, std::move(batch[index]));
					if (!state.shared.node_cache.contains(id) && collectResults) {
						appendReferenceFallback(loaded, std::move(batch[index]), recordBytes, missingIds.size(),
												fallbackHeapBytes, fallbackBytes);
					} else if (!state.shared.node_cache.contains(id)) {
						sourceBytes.set(previousSourceBytes);
					}
				}
				offset = end;
			}

			if (!collectResults)
				return {};
			std::vector<storage::LogicalGraphNode> result;
			if (resultBytes != nullptr) {
				const size_t slotBytes = checkedSizeMul(references.size(), sizeof(storage::LogicalGraphNode),
														"logical-plan Node reference result capacity overflowed");
				admitReferenceResultBytes(*resultBytes, slotBytes,
										  "logical-plan Node reference result exceeded its shared byte bound");
				try {
					result.reserve(references.size());
				} catch (...) {
					resultBytes->reset();
					throw;
				}
				admitReferenceResultBytes(*resultBytes,
										  checkedSizeMul(result.capacity(), sizeof(storage::LogicalGraphNode),
														 "logical-plan Node reference result capacity overflowed"),
										  "logical-plan Node reference result exceeded its shared byte bound");
			} else {
				result.reserve(references.size());
			}
			const auto appendResult = [&](const storage::LogicalGraphNode &node) {
				const size_t previousBytes = resultBytes == nullptr ? 0U : resultBytes->bytes();
				if (resultBytes != nullptr) {
					admitReferenceResultBytes(*resultBytes,
											  checkedSizeAdd(previousBytes, logicalNodeCacheBytes(node, state.shared.context) - sizeof(node),
															 "logical-plan Node reference result size overflowed"),
											  "logical-plan Node reference result exceeded its shared byte bound");
				}
				try {
					result.push_back(node);
				} catch (...) {
					if (resultBytes != nullptr)
						resultBytes->set(previousBytes);
					throw;
				}
			};
			for (const storage::EntityRef &reference: references) {
				if (const auto cached = state.shared.node_cache.find(reference.entity_id); cached != state.shared.node_cache.end()) {
					validateNode(cached->second, state);
					appendResult(cached->second);
					continue;
				}
				const auto position =
						std::ranges::lower_bound(loaded, reference.entity_id, {}, &storage::LogicalGraphNode::node_id);
				if (position == loaded.end() || position->node_id != reference.entity_id) {
					throw std::runtime_error("logical-plan shaped Node batch lost a requested Node");
				}
				appendResult(*position);
			}
			return result;
		}

		[[nodiscard]] std::vector<storage::LogicalGraphEdge>
		materializeEdgeReferences(std::span<const storage::EntityRef> references, ExecutionState &state,
								  const bool collectResults = true,
								  MaterializedByteReservation *resultBytes = nullptr) {
			std::vector<uint64_t> missingIds;
			MaterializedByteReservation missingIdSlots(&state.shared.materialized_budget);
			for (const storage::EntityRef &reference: references) {
				validateEdgeRef(reference, "logical-plan materialization received an invalid or Node reference where "
										   "an Edge was required");
				if (!state.shared.edge_cache.contains(reference.entity_id)) {
					appendMissingReferenceId(missingIds, reference.entity_id, references.size(), missingIdSlots);
				}
			}
			std::ranges::sort(missingIds);
			missingIds.erase(std::unique(missingIds.begin(), missingIds.end()), missingIds.end());
			if (missingIds.size() > state.shared.bounds.maximum_edge_records) {
				throw std::runtime_error("logical-plan Edge materialization exceeded its record bound");
			}

			MaterializedByteReservation fallbackBytes(&state.shared.materialized_budget);
			std::vector<storage::LogicalGraphEdge> loaded;
			size_t fallbackHeapBytes = 0U;
			for (size_t offset = 0U; offset < missingIds.size();) {
				checkGuard(state);
				size_t end = std::min(missingIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
				MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
				std::vector<storage::LogicalGraphEdge> batch;
				while (true) {
					const auto ids = std::span<const uint64_t>(missingIds).subspan(offset, end - offset);
					try {
						batch = readEdgesShapedSortedBudgeted(state, ids, completeEdgeReadShape(), &sourceBytes);
						if (batch.size() != end - offset) {
							throw std::runtime_error("logical-plan shaped Edge batch changed result cardinality");
						}
						for (size_t index = 0U; index < batch.size(); ++index) {
							validateEdge(batch[index]);
							if (batch[index].edge_id != missingIds[offset + index]) {
								throw std::runtime_error("logical-plan shaped Edge batch changed logical ID order");
							}
						}
						retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
						break;
					} catch (const std::runtime_error &error) {
						if (std::string_view(error.what()) != "shared materialization exceeded its byte bound" ||
							end - offset == 1U) {
							throw;
						}
						std::vector<storage::LogicalGraphEdge>().swap(batch);
						sourceBytes.reset();
						end = offset + std::max<size_t>(1U, (end - offset) / 2U);
					}
				}
				for (size_t index = 0U; index < batch.size(); ++index) {
					const uint64_t id = batch[index].edge_id;
					const size_t recordBytes = logicalEdgeCacheBytes(batch[index], state.shared.context);
					const size_t previousSourceBytes = sourceBytes.bytes();
					sourceBytes.set(previousSourceBytes - (recordBytes - sizeof(storage::LogicalGraphEdge)));
					cacheEdgeRecord(state, std::move(batch[index]));
					if (!state.shared.edge_cache.contains(id)) {
						appendReferenceFallback(loaded, std::move(batch[index]), recordBytes, missingIds.size(),
												fallbackHeapBytes, fallbackBytes);
					}
				}
				offset = end;
			}

			std::vector<storage::LogicalGraphEdge> result;
			if (collectResults) {
				if (resultBytes != nullptr) {
					const size_t slotBytes = checkedSizeMul(references.size(), sizeof(storage::LogicalGraphEdge),
															"logical-plan Edge reference result capacity overflowed");
					admitReferenceResultBytes(*resultBytes, slotBytes,
											  "logical-plan Edge reference result exceeded its shared byte bound");
					try {
						result.reserve(references.size());
					} catch (...) {
						resultBytes->reset();
						throw;
					}
					admitReferenceResultBytes(*resultBytes,
											  checkedSizeMul(result.capacity(), sizeof(storage::LogicalGraphEdge),
															 "logical-plan Edge reference result capacity overflowed"),
											  "logical-plan Edge reference result exceeded its shared byte bound");
				} else {
					result.reserve(references.size());
				}
			}
			for (const storage::EntityRef &reference: references) {
				const storage::LogicalGraphEdge *edge = nullptr;
				if (const auto cached = state.shared.edge_cache.find(reference.entity_id); cached != state.shared.edge_cache.end()) {
					edge = &cached->second;
				} else {
					const auto position = std::ranges::lower_bound(loaded, reference.entity_id, {},
																   &storage::LogicalGraphEdge::edge_id);
					if (position == loaded.end() || position->edge_id != reference.entity_id) {
						throw std::runtime_error("logical-plan shaped Edge batch lost a requested relationship");
					}
					edge = &*position;
				}
				validateEdgeReferenceRecord(reference, *edge, state, "logical graph Entity reference");
				if (collectResults) {
					const size_t previousBytes = resultBytes == nullptr ? 0U : resultBytes->bytes();
					if (resultBytes != nullptr) {
						admitReferenceResultBytes(*resultBytes,
												  checkedSizeAdd(previousBytes,
																 logicalEdgeCacheBytes(*edge, state.shared.context) - sizeof(*edge),
																 "logical-plan Edge reference result size overflowed"),
												  "logical-plan Edge reference result exceeded its shared byte bound");
					}
					try {
						result.push_back(*edge);
					} catch (...) {
						if (resultBytes != nullptr)
							resultBytes->set(previousBytes);
						throw;
					}
				}
			}
			return result;
		}

		[[nodiscard]] storage::LogicalGraphNode materializeNodeValue(ExecutionState &state,
																	 const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node, state);
				return detail::copyLogicalEntityGuarded(*node, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateNodeRef(
						*reference,
						"logical-plan expected a Node reference but received an invalid or relationship reference");
				return readNodeCached(state, reference->entity_id);
			}
			throw std::runtime_error("logical-plan expected a Node variable");
		}

		[[nodiscard]] uint64_t nodeBindingId(const LogicalPlanReadValue &value, ExecutionState &state) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node, state);
				return node->node_id;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateNodeRef(
						*reference,
						"logical-plan expected a Node reference but received an invalid or relationship reference");
				return reference->entity_id;
			}
			throw std::runtime_error("logical-plan expected a Node variable");
		}

		[[nodiscard]] storage::LogicalGraphNode patternSourceIdentity(ExecutionState &state,
																	  const LogicalPlanReadValue &value) {
			const uint64_t id = nodeBindingId(value, state);
			if (std::holds_alternative<storage::EntityRef>(value)) {
				if (!state.shared.snapshot.supportsNodeExistenceProbes()) {
					validateNodeIdentity(state, id);
					return {id, {}, {}};
				}
				checkGuard(state);
				const std::array<uint64_t, 1U> ids{id};
				const auto present = state.shared.snapshot.containsNodesSorted(ids);
				if (present.size() != 1U || !present.front()) {
					throw std::runtime_error("logical-plan pattern source is missing from its pinned snapshot");
				}
			}
			return {id, {}, {}};
		}

		[[nodiscard]] storage::LogicalGraphEdge materializeEdgeValue(ExecutionState &state,
																	 const LogicalPlanReadValue &value) {
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				validateEdge(*edge);
				return detail::copyLogicalEntityGuarded(*edge, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateEdgeRef(
						*reference,
						"logical-plan expected a relationship reference but received an invalid or Node reference");
				const storage::LogicalGraphEdge edge = readEdgeCached(state, reference->entity_id);
				validateEdgeReferenceRecord(*reference, edge, state, "logical graph Entity reference");
				return edge;
			}
			throw std::runtime_error("logical-plan expected a relationship variable");
		}

		[[nodiscard]] PropertyMap readInlineProperties(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													   ExecutionState &state) {
			if (entries.size() > state.shared.bounds.maximum_property_entries) {
				throw std::runtime_error("logical graph backend returned too many direct Property entries");
			}
			incrementBounded(state.task.stats.property_sets_materialized,
							 "logical-plan Property materialization counter overflowed");
			PropertyMap properties;
			properties.reserve(entries.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				checkGuard(state);
				if (entry.key.empty() || !properties.emplace(entry.key, detail::copyPropertyValueGuarded(entry.value, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				})).second) {
					throw std::runtime_error("logical graph backend returned an invalid direct Property entry set");
				}
			}
			return properties;
		}

		[[nodiscard]] PropertyMap readNodeProperties(storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphNode &node, ExecutionState &state) {
			static_cast<void>(snapshot);
			return readInlineProperties(node.properties, state);
		}

		[[nodiscard]] PropertyMap readEdgeProperties(storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphEdge &edge, ExecutionState &state) {
			static_cast<void>(snapshot);
			return readInlineProperties(edge.properties, state);
		}

		[[nodiscard]] bool propertiesMatch(const PropertyMap &properties,
										   const std::vector<std::pair<std::string, PropertyValue>> &predicates) {
			for (const auto &[key, expected]: predicates) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				const auto found = properties.find(key);
				if (found == properties.end() || found->second != expected) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool nodeMatches(storage::LogicalGraphReadSnapshot &snapshot,
									   const storage::LogicalGraphNode &node, const std::vector<std::string> &labels,
									   const std::vector<std::pair<std::string, PropertyValue>> &predicates,
									   ExecutionState &state) {
			validateNode(node, state);
			std::unordered_set<std::string_view> actualLabels(node.labels.begin(), node.labels.end());
			std::unordered_set<std::string_view> requiredLabels;
			requiredLabels.reserve(labels.size());
			for (const std::string &label: labels) {
				checkGuard(state);
				if (label.empty() || !requiredLabels.insert(label).second) {
					throw std::invalid_argument("logical-plan Node scan has an empty or duplicate label");
				}
				if (!actualLabels.contains(label)) {
					return false;
				}
			}
			if (predicates.empty()) {
				return true;
			}
			return propertiesMatch(readNodeProperties(snapshot, node, state), predicates);
		}

		[[nodiscard]] bool
		nodeScanMatches(storage::LogicalGraphReadSnapshot &snapshot, const storage::LogicalGraphNode &node,
						const logical::LogicalNodeScan &scan,
						const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
						ExecutionState &state) {
			validateNode(node, state);
			std::unordered_set<std::string_view> actualLabels(node.labels.begin(), node.labels.end());
			std::unordered_set<std::string_view> requiredLabels;
			requiredLabels.reserve(scan.getLabels().size());
			for (const std::string &label: scan.getLabels()) {
				checkGuard(state);
				if (label.empty() || !requiredLabels.insert(label).second) {
					throw std::invalid_argument("logical-plan Node scan has an empty or duplicate label");
				}
				if (!actualLabels.contains(label)) {
					return false;
				}
			}
			const bool needsProperties = !scan.getPropertyPredicates().empty() || !parameterPredicates.empty() ||
										 !scan.getRangePredicates().empty() || scan.getCompositeEquality().has_value();
			if (!needsProperties) {
				return true;
			}
			const PropertyMap properties = readNodeProperties(snapshot, node, state);
			if (!propertiesMatch(properties, scan.getPropertyPredicates())) {
				return false;
			}
			if (!propertiesMatch(properties, parameterPredicates)) {
				return false;
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				checkGuard(state);
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan Node range predicate has an empty key");
				}
				const auto found = properties.find(range.key);
				if (found == properties.end()) {
					return false;
				}
				if (!isNull(range.minValue) &&
					(range.minInclusive ? found->second < range.minValue : found->second <= range.minValue)) {
					return false;
				}
				if (!isNull(range.maxValue) &&
					(range.maxInclusive ? found->second > range.maxValue : found->second >= range.maxValue)) {
					return false;
				}
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					checkGuard(state);
					if (composite->keys[index].empty()) {
						throw std::invalid_argument("logical-plan Node composite predicate has an empty key");
					}
					const auto found = properties.find(composite->keys[index]);
					if (found == properties.end() || found->second != composite->values[index]) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] std::optional<std::vector<std::pair<std::string, PropertyValue>>>
		resolveParameterPropertyPredicates(const logical::LogicalNodeScan &scan, ExecutionState &state) {
			std::unordered_set<std::string_view> keys;
			keys.reserve(scan.getPropertyPredicates().size() + scan.getParameterPropertyPredicates().size());
			for (const auto &[key, value]: scan.getPropertyPredicates()) {
				static_cast<void>(value);
				if (key.empty() || !keys.insert(key).second) {
					throw std::invalid_argument(
							"logical-plan Node scan has an empty or duplicate Property predicate key");
				}
			}
			std::vector<std::pair<std::string, PropertyValue>> result;
			result.reserve(scan.getParameterPropertyPredicates().size());
			for (const logical::ParameterPropertyPredicate &predicate: scan.getParameterPropertyPredicates()) {
				checkGuard(state);
				if (predicate.key.empty() || predicate.parameter_name.empty() || !keys.insert(predicate.key).second) {
					throw std::invalid_argument("logical-plan Node scan has an empty, duplicate, or conflicting "
												"parameter Property predicate key");
				}
				if (state.shared.context == nullptr) {
					throw std::runtime_error("logical-plan parameter equality predicate requires a QueryContext");
				}
				const auto value = state.shared.context->parameters.find(predicate.parameter_name);
				if (value == state.shared.context->parameters.end()) {
					throw std::runtime_error("logical-plan read runtime could not resolve a query parameter");
				}
				// Cypher equality with null does not select a Node, so it need not
				// materialize a Property collection only to discard every candidate.
				if (isNull(value->second)) {
					return std::nullopt;
				}
				result.emplace_back(predicate.key, value->second);
			}
			return result;
		}

		[[nodiscard]] bool edgeMatches(storage::LogicalGraphReadSnapshot &snapshot,
									   const storage::LogicalGraphEdge &edge, const std::string &type,
									   const std::unordered_map<std::string, PropertyValue> &predicates,
									   ExecutionState &state) {
			validateEdge(edge);
			if (!type.empty() && edge.type != type) {
				return false;
			}
			if (predicates.empty()) {
				return true;
			}
			std::vector<std::pair<std::string, PropertyValue>> ordered;
			ordered.reserve(predicates.size());
			for (const auto &[key, value]: predicates) {
				ordered.emplace_back(key, value);
			}
			return propertiesMatch(readEdgeProperties(snapshot, edge, state), ordered);
		}

		[[nodiscard]] const LogicalPlanReadValue &requireBinding(const LogicalPlanReadRow &row,
																 const std::string &variable) {
			const auto found = row.values.find(variable);
			if (found == row.values.end()) {
				throw std::runtime_error("logical-plan read runtime could not resolve a variable");
			}
			return found->second;
		}

		[[nodiscard]] storage::LogicalGraphAdjacencyTopologyPage readTopologyPageBudgeted(
			ExecutionState &state, uint64_t nodeId, storage::LogicalGraphReadDirection direction,
			std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			MaterializedByteReservation &sourceBytes) {
			struct SourcePressure {};
			const auto admit = [&](size_t bytes) {
				checkGuard(state);
				if (sourceBytes.wouldExceed(bytes)) throw SourcePressure{};
				sourceBytes.set(bytes);
			};
			size_t requested = maximumRecords;
			for (;;) {
				checkGuard(state);
				try {
					auto page = state.shared.snapshot.supportsAdjacencyTopologyBufferAdmission()
						? state.shared.snapshot.scanAdjacentEdgeTopologiesWithBufferAdmission(nodeId, direction, continuation, requested, admit)
						: state.shared.snapshot.scanAdjacentEdgeTopologies(nodeId, direction, continuation, requested);
					if (page.edges.size() > requested || (page.edges.empty() && page.continuation))
						throw std::runtime_error("logical graph topology source returned an invalid page shape");
					uint64_t previous = continuation ? continuation->previous_edge_id : 0U;
					if (continuation && (page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id))
						throw std::runtime_error("logical graph topology source returned a discontinuous page");
					size_t bytes = checkedSizeMul(page.edges.capacity(), sizeof(storage::LogicalGraphEdgeTopology),
						"logical-plan topology capacity overflowed");
					for (const auto &edge: page.edges) {
						validateEdge(edge);
						if (edge.edge_id <= previous ||
							(direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING ? edge.source_node_id : edge.target_node_id) != nodeId)
							throw std::runtime_error("logical graph topology source returned invalid order or endpoint");
						previous = edge.edge_id;
						bytes = checkedSizeAdd(bytes, edge.type.capacity(), "logical-plan topology names overflowed");
					}
					if (page.continuation && (page.continuation->previous_edge_id != previous || page.continuation->next_edge_id <= previous))
						throw std::runtime_error("logical graph topology source returned an invalid continuation");
					admit(bytes);
					return page;
				} catch (const SourcePressure &) {
					sourceBytes.reset();
					if (requested <= 1U) throw std::runtime_error("shared materialization exceeded its byte bound");
					requested = std::max<size_t>(1U, requested / 2U);
				}
			}
		}

		[[nodiscard]] storage::LogicalGraphAdjacencyReferencePage readReferencePageBudgeted(
			ExecutionState &state, uint64_t nodeId, storage::LogicalGraphReadDirection direction,
			std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			MaterializedByteReservation &sourceBytes) {
			struct SourcePressure {};
			const auto admit = [&](size_t bytes) {
				checkGuard(state);
				if (sourceBytes.wouldExceed(bytes)) throw SourcePressure{};
				sourceBytes.set(bytes);
			};
			size_t requested = maximumRecords;
			for (;;) {
				checkGuard(state);
				try {
					auto page = state.shared.snapshot.supportsAdjacencyReferenceBufferAdmission()
						? state.shared.snapshot.scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction, continuation, requested, admit)
						: state.shared.snapshot.scanAdjacentEdgeRefs(nodeId, direction, continuation, requested);
					if (page.edges.size() > requested || (page.edges.empty() && page.continuation))
						throw std::runtime_error("logical graph reference source returned an invalid page shape");
					admit(checkedSizeMul(page.edges.capacity(), sizeof(storage::EntityRef),
						"logical-plan reference capacity overflowed"));
					return page;
				} catch (const SourcePressure &) {
					sourceBytes.reset();
					if (requested <= 1U) throw std::runtime_error("shared materialization exceeded its byte bound");
					requested = std::max<size_t>(1U, requested / 2U);
				}
			}
		}

		[[nodiscard]] PropertyValue readSelectedPropertyBudgeted(storage::LogicalGraphReadSnapshot &snapshot,
			uint64_t id, bool edge, std::string_view key, ExecutionState &state) {
			checkGuard(state);
			if (!snapshot.supportsPropertyReadAdmission())
				return edge ? snapshot.readEdgeProperty(id, key) : snapshot.readNodeProperty(id, key);
			MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
			sourceBytes.set(sizeof(std::vector<PropertyValue>) + sizeof(PropertyValue));
			const std::function<void(size_t)> admission = [&](size_t bytes) {
				checkGuard(state);
				sourceBytes.set(checkedSizeAdd(sourceBytes.bytes(), bytes,
					"logical-plan selected Property source size overflowed"));
			};
			const std::span<const uint64_t> ids(&id, 1U);
			const std::span<const std::string_view> keys(&key, 1U);
			auto columns = edge ? snapshot.readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)
				: snapshot.readNodePropertyColumnsSortedWithAdmission(ids, keys, admission);
			if (columns.size() != 1U || columns.front().size() != 1U)
				throw std::runtime_error("logical graph selected Property columns changed their shape");
			const size_t frames = checkedSizeAdd(
				checkedSizeMul(columns.capacity(), sizeof(std::vector<PropertyValue>),
					"logical-plan selected Property column size overflowed"),
				checkedSizeMul(columns.front().capacity(), sizeof(PropertyValue),
					"logical-plan selected Property row size overflowed"),
				"logical-plan selected Property frame size overflowed");
			sourceBytes.set(checkedSizeAdd(frames, property_utils::getPropertyValueSize(columns.front().front()),
				"logical-plan selected Property payload size overflowed"));
			checkGuard(state);
			return std::move(columns.front().front());
		}

		[[nodiscard]] PropertyValue readProperty(storage::LogicalGraphReadSnapshot &snapshot, LogicalPlanReadRow &row,
												 const std::string &variable, const std::string &key,
												 ExecutionState &state, const LogicalPlanReadValue *boundBinding = nullptr) {
			if (key.empty()) {
				throw std::invalid_argument("logical-plan Property access has an empty key");
			}
			const auto copyResult = [&](const PropertyValue &source, bool borrowed = false) {
				// Fixed scalar alternatives cannot allocate payload buffers. Their
				// owning output slots remain charged by the receiving row/container.
				switch (source.getType()) {
					case PropertyType::NULL_TYPE:
					case PropertyType::BOOLEAN:
					case PropertyType::INTEGER:
					case PropertyType::DOUBLE:
					case PropertyType::DATE:
					case PropertyType::DATETIME:
					case PropertyType::DURATION:
						return source;
					default: break;
				}
				MaterializedByteReservation copyBytes(&state.shared.materialized_budget);
				PropertyValue copied = copyReservedPropertyValue(source, state, copyBytes);
				debug::PerfTrace::addValue("logical_plan.property.owned_result_copy_bytes",
					static_cast<int64_t>(copyBytes.bytes()));
				if (borrowed) debug::PerfTrace::addValue("logical_plan.prefetch.borrowed_value_copy_bytes",
					static_cast<int64_t>(copyBytes.bytes()));
				return copied;
			};
			if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end()) {
				const auto found = cached->second.find(key);
				if (found != cached->second.end()) {
					return copyResult(found->second);
				}
				if (row.complete_entity_property_cache.contains(variable)) {
					return PropertyValue();
				}
				// An EntityRef cache is a selected-field cache, not proof that the
				// requested field is absent. Continue to the token-aware native read
				// for a field not seen on this row yet.
				const auto *binding = boundBinding;
				if (binding == nullptr) {
					const auto found = row.values.find(variable);
					if (found != row.values.end()) binding = &found->second;
				}
				if (binding == nullptr || !std::holds_alternative<storage::EntityRef>(*binding)) {
					return PropertyValue();
				}
			}
			const LogicalPlanReadValue &binding = boundBinding == nullptr ? requireBinding(row, variable) : *boundBinding;
			if (const auto *reference = std::get_if<storage::EntityRef>(&binding);
				reference != nullptr && !reference->is_edge && state.task.property_columns != nullptr &&
				*state.task.property_columns->variable == variable) {
				validateNodeRef(*reference, "logical-plan borrowed Property received an invalid Node reference");
				const auto &view = *state.task.property_columns;
				const auto id = std::ranges::lower_bound(view.ids, reference->entity_id);
				const auto keyPosition = std::ranges::find(*view.keys, key);
				if (id != view.ids.end() && *id == reference->entity_id && keyPosition != view.keys->end()) {
					debug::PerfTrace::addValue("logical_plan.prefetch.borrowed_values", 1);
					return copyResult(view.columns[keyPosition - view.keys->begin()][id - view.ids.begin()], true);
				}
			}
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() == PropertyType::DATE || value->getType() == PropertyType::DATETIME ||
					value->getType() == PropertyType::DURATION) {
					return temporalComponent(*value, key);
				}
				if (value->getType() != PropertyType::MAP) {
					return PropertyValue();
				}
				const auto &properties = value->getMap();
				const auto found = properties.find(key);
				return found == properties.end() ? PropertyValue() : copyResult(found->second);
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				auto [cached, inserted] =
						detail::procedureMapEmplace(row.property_cache, variable, readNodeProperties(snapshot, *node, state));
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : copyResult(found->second);
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
				auto [cached, inserted] =
						detail::procedureMapEmplace(row.property_cache, variable, readEdgeProperties(snapshot, *edge, state));
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : copyResult(found->second);
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(binding);
			validateEntityRef(reference);
			// Native snapshots resolve the property name to a token and decode only
			// the requested value. The virtual default retains complete-read
			// compatibility for non-native logical backends.
			PropertyValue value = readSelectedPropertyBudgeted(
				snapshot, reference.entity_id, reference.is_edge, key, state);
			// The selected value is already owned. Transfer it into the row cache;
			// only the independent expression result needs a payload copy.
			auto [cacheOwner, cacheInserted] = detail::procedureMapTryEmplace(row.property_cache, variable);
            static_cast<void>(cacheInserted);
            const auto [cached, inserted] = detail::procedureMapInsertOrAssign(cacheOwner->second, key, std::move(value));
			static_cast<void>(inserted);
			return copyResult(cached->second);
		}

		[[nodiscard]] bool
		nodeRefScanMatches(storage::LogicalGraphReadSnapshot &snapshot, LogicalPlanReadRow &row,
						   const std::string &variable, const logical::LogicalNodeScan &scan,
						   const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
						   ExecutionState &state, const bool labelsAlreadyMatched = false) {
			const storage::EntityRef &reference = std::get<storage::EntityRef>(requireBinding(row, variable));
			validateNodeRef(reference, "logical-plan Node predicate received an invalid or Edge reference");
			if (!labelsAlreadyMatched && !snapshot.nodeHasLabels(reference.entity_id, scan.getLabels())) {
				return false;
			}
			const auto inspectProperty = [&](const std::string &key, const auto &inspect) {
				if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end()) {
					if (const auto found = cached->second.find(key); found != cached->second.end()) {
						return inspect(found->second);
					}
				}
				return inspect(readProperty(snapshot, row, variable, key, state));
			};
			const auto equals = [&](const std::string &key, const PropertyValue &expected) {
				checkGuard(state);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				if (isNull(expected)) {
					return false;
				}
				return inspectProperty(
						key, [&](const PropertyValue &actual) { return !isNull(actual) && actual == expected; });
			};
			for (const auto &predicate: scan.getPropertyPredicates()) {
				if (!equals(predicate.first, predicate.second)) {
					return false;
				}
			}
			for (const auto &predicate: parameterPredicates) {
				if (!equals(predicate.first, predicate.second)) {
					return false;
				}
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				checkGuard(state);
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan Node range predicate has an empty key");
				}
				const bool matches = inspectProperty(range.key, [&](const PropertyValue &actual) {
					return !isNull(actual) &&
						   (isNull(range.minValue) ||
							!(range.minInclusive ? actual < range.minValue : actual <= range.minValue)) &&
						   (isNull(range.maxValue) ||
							!(range.maxInclusive ? actual > range.maxValue : actual >= range.maxValue));
				});
				if (!matches)
					return false;
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					if (!equals(composite->keys[index], composite->values[index])) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] const PropertyMap &readBindingProperties(storage::LogicalGraphReadSnapshot &snapshot,
															   LogicalPlanReadRow &row, const std::string &variable,
															   ExecutionState &state) {
			const LogicalPlanReadValue &binding = requireBinding(row, variable);
			const bool isEntityReference = std::holds_alternative<storage::EntityRef>(binding);
			if (const auto cached = row.property_cache.find(variable);
				cached != row.property_cache.end() &&
				(!isEntityReference || row.complete_entity_property_cache.contains(variable))) {
				return cached->second;
			}
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() != PropertyType::MAP) {
					throw std::runtime_error(
							"logical-plan entity Property function requires a node, relationship, or Map");
				}
				return detail::procedureMapEmplace(row.property_cache, variable, detail::copyPropertyMapGuarded(value->getMap(), [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				})).first->second;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				return detail::procedureMapEmplace(row.property_cache, variable, readNodeProperties(snapshot, *node, state)).first->second;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
				return detail::procedureMapEmplace(row.property_cache, variable, readEdgeProperties(snapshot, *edge, state)).first->second;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(binding);
			validateEntityRef(reference);
			PropertyMap complete = reference.is_edge
										   ? readEdgeProperties(snapshot, materializeEdgeValue(state, binding), state)
										   : readNodeProperties(snapshot, materializeNodeValue(state, binding), state);
			auto [cached, inserted] = detail::procedureMapInsertOrAssign(row.property_cache, variable, std::move(complete));
			static_cast<void>(inserted);
			detail::procedureSetInsert(row.complete_entity_property_cache, variable);
			return cached->second;
		}

		[[nodiscard]] size_t retainedExpressionValueBytes(const PropertyValue &value, const ExecutionState &state) {
			return detail::retainedPropertyValueBytes(value, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
		}

		// List comprehensions, reducers, and quantifiers bind short-lived scalar
		// values. Restoring both the value and its Property cache prevents a nested
		// expression from corrupting an outer row binding with the same name.
		class ScopedRowBinding final {
		public:
			template<typename Value>
			ScopedRowBinding(LogicalPlanReadRow &row, std::string_view variable, const Value &value,
							 ExecutionState &state) : row_(row), binding_bytes_(&state.shared.materialized_budget) {
				if (variable.empty()) {
					throw std::invalid_argument("logical-plan temporary variable name cannot be empty");
				}
				size_t bytes = checkedSizeAdd(variable.size(), sizeof(LogicalPlanReadValue),
											  "logical-plan scoped binding size overflowed");
				bytes = checkedSizeAdd(bytes, bindingPayloadBytes(value, state),
									   "logical-plan scoped binding size overflowed");
				binding_bytes_.set(bytes);
				variable_.assign(variable);
				if (const auto previous = row_.values.find(variable_); previous != row_.values.end()) {
					bytes = checkedSizeAdd(bytes, logicalReadValueBytes(previous->second, state.shared.context),
										   "logical-plan scoped binding size overflowed");
					bytes = checkedSizeAdd(bytes, sizeof(LogicalPlanReadValue),
										   "logical-plan scoped binding size overflowed");
				}
				if (const auto previous = row_.property_cache.find(variable_); previous != row_.property_cache.end()) {
					bytes = checkedSizeAdd(
							bytes, propertyMapBytes(previous->second, "logical-plan scoped binding size overflowed", state.shared.context),
							"logical-plan scoped binding size overflowed");
				}
				if (const auto previous = row_.entity_metadata_cache.find(variable_);
					previous != row_.entity_metadata_cache.end()) {
					bytes = checkedSizeAdd(
							bytes, propertyMapBytes(previous->second, "logical-plan scoped binding size overflowed", state.shared.context),
							"logical-plan scoped binding size overflowed");
				}
				binding_bytes_.set(bytes);
				if (const auto previous = row_.values.find(variable_); previous != row_.values.end()) {
					previous_value_ = previous->second;
				}
				if (const auto previous = row_.property_cache.find(variable_); previous != row_.property_cache.end()) {
					previous_properties_ = previous->second;
					previous_properties_complete_ = row_.complete_entity_property_cache.contains(variable_);
				}
				if (const auto previous = row_.entity_metadata_cache.find(variable_);
					previous != row_.entity_metadata_cache.end()) {
					previous_metadata_ = previous->second;
				}
				row_.values.insert_or_assign(variable_, LogicalPlanReadValue(value));
				row_.property_cache.erase(variable_);
				row_.complete_entity_property_cache.erase(variable_);
				row_.entity_metadata_cache.erase(variable_);
			}

			~ScopedRowBinding() {
				if (previous_value_.has_value()) {
					row_.values.insert_or_assign(variable_, std::move(*previous_value_));
				} else {
					row_.values.erase(variable_);
				}
				if (previous_properties_.has_value()) {
					row_.property_cache.insert_or_assign(variable_, std::move(*previous_properties_));
					if (previous_properties_complete_.value_or(false)) {
						row_.complete_entity_property_cache.insert(variable_);
					} else {
						row_.complete_entity_property_cache.erase(variable_);
					}
				} else {
					row_.property_cache.erase(variable_);
					row_.complete_entity_property_cache.erase(variable_);
				}
				if (previous_metadata_.has_value()) {
					row_.entity_metadata_cache.insert_or_assign(variable_, std::move(*previous_metadata_));
				} else {
					row_.entity_metadata_cache.erase(variable_);
				}
			}

			ScopedRowBinding(const ScopedRowBinding &) = delete;
			ScopedRowBinding &operator=(const ScopedRowBinding &) = delete;

		private:
			[[nodiscard]] static size_t bindingPayloadBytes(const PropertyValue &value, const ExecutionState &state) {
				return retainedExpressionValueBytes(value, state);
			}
			[[nodiscard]] static size_t bindingPayloadBytes(const storage::LogicalGraphNode &value, const ExecutionState &state) {
				return logicalNodeCacheBytes(value, state.shared.context);
			}
			[[nodiscard]] static size_t bindingPayloadBytes(const storage::LogicalGraphEdge &value, const ExecutionState &state) {
				return logicalEdgeCacheBytes(value, state.shared.context);
			}

			LogicalPlanReadRow &row_;
			MaterializedByteReservation binding_bytes_;
			std::string variable_;
			std::optional<LogicalPlanReadValue> previous_value_;
			std::optional<PropertyMap> previous_properties_;
			std::optional<bool> previous_properties_complete_;
			std::optional<std::unordered_map<std::string, PropertyValue>> previous_metadata_;
		};

		[[nodiscard]] const std::vector<PropertyValue> &
		requireBoundedList(const PropertyValue &value, const ExecutionState &state, const char *operation) {
			if (value.getType() != PropertyType::LIST) {
				throw std::runtime_error(std::string("logical-plan ") + operation + " requires a List value");
			}
			const std::vector<PropertyValue> &list = value.getList();
			if (list.size() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error(std::string("logical-plan ") + operation +
										 " exceeded the configured List bound");
			}
			return list;
		}

		[[nodiscard]] size_t materializedListStorageBytes(const PropertyValue &value, const ExecutionState &state) {
			const auto estimates = detail::propertyBufferEstimates(value, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
			const size_t logicalBytes = checkedSizeAdd(estimates.logicalBytes,
								  checkedSizeMul(value.getList().capacity(), sizeof(PropertyValue),
												 "logical-plan List capacity overflowed"),
								  "logical-plan List storage size overflowed");
			return std::max(logicalBytes, estimates.heapBytes);
		}

		[[nodiscard]] PropertyValue
		evaluateScalarExpression(const expressions::Expression &expression, LogicalPlanReadRow &row,
								 ExecutionState &state,
								 ShortestPathResultMode shortestMode = ShortestPathResultMode::SPR_FULL);

		using PatternMatchVisitor =
				std::function<bool(const storage::LogicalGraphEdge &, const storage::LogicalGraphNode &)>;

		// Evaluate the common topology-only EXISTS shape without materializing an
		// Edge or target Node.  The native reference page is already authenticated
		// against the pinned snapshot; endpoint probes retain the fail-closed
		// invariant while keeping the hot path independent of Property pages.
		[[nodiscard]] std::optional<bool> findPatternReferenceMatch(const uint64_t sourceNodeId,
																	const std::string &edgeType,
																	expressions::PatternDirection direction,
																	ExecutionState &state) {
			if (!state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() || !state.shared.snapshot.supportsNodeExistenceProbes() ||
				(!edgeType.empty() && !state.shared.snapshot.supportsNativeRelationshipTypeTokens(edgeType)) ||
				(direction != expressions::PatternDirection::PAT_OUTGOING &&
				 direction != expressions::PatternDirection::PAT_INCOMING &&
				 direction != expressions::PatternDirection::PAT_BOTH)) {
				return std::nullopt;
			}
			if (sourceNodeId == 0U) {
				throw std::runtime_error("logical-plan pattern expression has an invalid source Node identity");
			}
			std::optional<uint64_t> edgeTypeToken;
			if (!edgeType.empty()) {
				edgeTypeToken = state.shared.snapshot.findRelationshipTypeToken(edgeType);
				if (!edgeTypeToken.has_value()) {
					return false;
				}
			}
			size_t sourceEdges = 0U;
			const auto scanDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				for (;;) {
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords = std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
														state.shared.bounds.maximum_adjacency_page_records);
					MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
					const auto page = readReferencePageBudgeted(state, sourceNodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
					if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value()) ||
						(continuation.has_value() &&
						 (page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
						throw std::runtime_error(
								"logical graph backend returned an invalid pattern-expression reference page");
					}
					// Validate and filter the complete page before probing endpoints.  The
					// endpoint requests are then sorted/deduplicated for one bounded
					// Directory lookup, while candidates retain their original order.
					MaterializedByteReservation candidateBytes(&state.shared.materialized_budget);
					candidateBytes.set(checkedSizeMul(page.edges.size(), sizeof(const storage::EntityRef *),
						"logical-plan pattern candidate slots overflowed"));
					std::vector<const storage::EntityRef *> candidates;
					candidates.reserve(page.edges.size());
					candidateBytes.set(checkedSizeMul(candidates.capacity(), sizeof(const storage::EntityRef *),
						"logical-plan pattern candidate capacity overflowed"));
					for (const storage::EntityRef &reference: page.edges) {
						checkGuard(state);
						validateEdgeRef(reference,
										"logical graph backend returned an invalid pattern-expression reference");
						if (reference.entity_id <= previousEdgeId) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression reference");
						}
						previousEdgeId = reference.entity_id;
						const bool endpointMatches =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? reference.source_node_id == sourceNodeId
										: reference.target_node_id == sourceNodeId;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression endpoint");
						}
						incrementBounded(sourceEdges,
										 "logical-plan pattern-expression per-Node Edge counter overflowed");
						incrementBounded(state.task.stats.edges_scanned,
										 "logical-plan pattern-expression Edge counter overflowed");
						if (direction == expressions::PatternDirection::PAT_BOTH &&
							adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							reference.source_node_id == sourceNodeId) {
							continue;
						}
						if (edgeTypeToken.has_value() && reference.type_token != *edgeTypeToken) {
							continue;
						}
						candidates.push_back(&reference);
					}
					if (candidates.empty()) {
						if (!page.continuation.has_value()) {
							return false;
						}
					} else if (state.shared.snapshot.supportsNodeExistenceProbes()) {
						MaterializedByteReservation targetBytes(&state.shared.materialized_budget);
						targetBytes.set(checkedSizeMul(candidates.size(), sizeof(uint64_t),
							"logical-plan pattern endpoint slots overflowed"));
						std::vector<uint64_t> targetIds;
						targetIds.reserve(candidates.size());
						targetBytes.set(checkedSizeMul(targetIds.capacity(), sizeof(uint64_t),
							"logical-plan pattern endpoint capacity overflowed"));
						for (const storage::EntityRef *reference: candidates) {
							targetIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? reference->target_node_id
														: reference->source_node_id);
						}
						detail::sortUniqueIdsGuarded(targetIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
						// Reserve a complete machine word for the final partial bit block.
						const auto presenceSize = [&](size_t bits) {
							return checkedSizeMul(bits / 8U + (bits % 8U != 0U), 1U,
								"logical-plan pattern presence capacity overflowed");
						};
						presenceBytes.set(checkedSizeAdd(presenceSize(targetIds.size()), sizeof(size_t),
							"logical-plan pattern presence slots overflowed"));
						const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetIds);
						presenceBytes.set(presenceSize(present.capacity()));
						checkGuard(state);
						if (present.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed pattern-expression endpoint cardinality");
						}
						for (const storage::EntityRef *reference: candidates) {
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference->target_node_id
											: reference->source_node_id;
							const auto position = std::ranges::lower_bound(targetIds, targetId);
							if (position == targetIds.end() || *position != targetId ||
								!present[static_cast<size_t>(position - targetIds.begin())]) {
								throw std::runtime_error(
										"logical graph backend returned an Edge topology with a missing endpoint");
							}
							return true;
						}
					} else {
						// Preserve the defensive fallback for logical readers that do not
						// implement the optional batch probe.
						const uint64_t targetId =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? candidates.front()->target_node_id
										: candidates.front()->source_node_id;
						if (!state.shared.snapshot.containsNode(targetId)) {
							throw std::runtime_error(
									"logical graph backend returned an Edge topology with a missing endpoint");
						}
						return true;
					}
					if (!page.continuation.has_value()) {
						return false;
					}
					if (page.edges.empty() || page.continuation->previous_edge_id != page.edges.back().entity_id ||
						page.continuation->next_edge_id == 0U ||
						page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error(
								"logical graph backend returned an incomplete pattern-expression reference scan");
					}
					continuation = page.continuation;
				}
			};
			if (direction == expressions::PatternDirection::PAT_OUTGOING ||
				direction == expressions::PatternDirection::PAT_BOTH) {
				if (scanDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
					return true;
				}
			}
			if (direction == expressions::PatternDirection::PAT_INCOMING ||
				direction == expressions::PatternDirection::PAT_BOTH) {
				if (scanDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING)) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool visitPatternTargets(const storage::LogicalGraphNode &source, const std::string &edgeType,
											   std::string_view targetLabel, expressions::PatternDirection direction,
											   ExecutionState &state, const PatternMatchVisitor &visitor,
											   const PatternBindingReadShape *bindingShape = nullptr,
											   bool lazyCandidates = false) {
			if (!visitor) {
				throw std::invalid_argument("logical-plan pattern expression requires a target visitor");
			}
			validateNode(source, state);
			const auto validateCandidate = [&](const storage::LogicalGraphEdge &edge,
											   const storage::LogicalGraphNode &target) {
				validateNode(target, state);
				validateEdge(edge);
				if (lazyCandidates && (target.properties.size() > state.shared.bounds.maximum_property_entries ||
									   edge.properties.size() > state.shared.bounds.maximum_property_entries ||
									   checkedSizeAdd(logicalNodeCacheBytes(target, state.shared.context), logicalEdgeCacheBytes(edge, state.shared.context),
													  "logical-plan EXISTS candidate byte count overflowed") >
											   state.shared.bounds.maximum_materialized_bytes)) {
					throw std::runtime_error("logical-plan EXISTS candidate exceeded its Property or byte bound");
				}
			};

			const auto appendNamedProperties = [&](auto &records, const std::vector<uint64_t> &ids,
												   const std::vector<std::string> &keys, bool edgeRecords,
												   MaterializedByteReservation &recordBytes) {
				if (ids.empty() || keys.empty())
					return;
				if (keys.size() > state.shared.bounds.maximum_property_entries) {
					throw std::runtime_error("logical-plan pattern named columns exceeded their Property bound");
				}
				for (size_t index = 0U; index < records.size(); ++index) {
					const auto &record = records[index];
					uint64_t id;
					if constexpr (requires { record.node_id; })
						id = record.node_id;
					else
						id = record.edge_id;
					if (id != ids[index])
						throw std::runtime_error("logical graph pattern named columns changed entity order");
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(keys.size());
				for (const auto &key: keys) {
					if (std::ranges::any_of(records, [&](const auto &record) {
							return std::ranges::find(record.properties, key,
													 &storage::LogicalGraphPropertyEntry::key) ==
								   record.properties.end();
						}))
						keyViews.push_back(key);
				}
				// Compatibility readers may already return complete records despite
				// the shape. Reuse those fields instead of issuing a redundant column read.
				if (keyViews.empty())
					return;
				checkGuard(state);
				MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
				const size_t columnFrames = checkedSizeAdd(
						checkedSizeMul(keyViews.size(), sizeof(std::vector<PropertyValue>),
									   "logical-plan pattern column frame size overflowed"),
						checkedSizeMul(checkedSizeMul(keyViews.size(), ids.size(),
													  "logical-plan pattern column frame size overflowed"),
									   sizeof(PropertyValue), "logical-plan pattern column frame size overflowed"),
						"logical-plan pattern column frame size overflowed");
				columnBytes.set(columnFrames);
				const std::function<void(size_t)> admitProperty = [&](size_t bytes) {
					checkGuard(state);
					columnBytes.set(checkedSizeAdd(columnBytes.bytes(), bytes,
												   "logical-plan pattern Property source size overflowed"));
				};
				const bool sourceAdmission = state.shared.snapshot.supportsPropertyReadAdmission();
				auto columns =
						edgeRecords ? (sourceAdmission ? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
																 ids, keyViews, admitProperty)
													   : state.shared.snapshot.readEdgePropertyColumnsSorted(ids, keyViews))
									: (sourceAdmission ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
																 ids, keyViews, admitProperty)
													   : state.shared.snapshot.readNodePropertyColumnsSorted(ids, keyViews));
				if (columns.size() != keyViews.size()) {
					throw std::runtime_error("logical graph pattern named columns changed column count");
				}
				size_t retainedColumns = checkedSizeMul(columns.capacity(), sizeof(std::vector<PropertyValue>),
														"logical-plan pattern column size overflowed");
				for (const auto &column: columns) {
					if (column.size() != ids.size()) {
						throw std::runtime_error("logical graph pattern named columns changed row count");
					}
					retainedColumns = checkedSizeAdd(retainedColumns,
													 checkedSizeMul(column.capacity(), sizeof(PropertyValue),
																	"logical-plan pattern column size overflowed"),
													 "logical-plan pattern column size overflowed");
					for (const PropertyValue &value: column) {
						retainedColumns = checkedSizeAdd(retainedColumns, property_utils::getPropertyValueSize(value),
														 "logical-plan pattern column size overflowed");
					}
				}
				columnBytes.set(retainedColumns);
				for (size_t column = 0U; column < keyViews.size(); ++column) {
					for (size_t index = 0U; index < records.size(); ++index) {
						checkGuard(state);
						auto &entries = records[index].properties;
						const auto existing =
								std::ranges::find(entries, keyViews[column], &storage::LogicalGraphPropertyEntry::key);
						const size_t transferred = property_utils::getPropertyValueSize(columns[column][index]);
						const size_t previousValue =
								existing == entries.end() ? 0U : property_utils::getPropertyValueSize(existing->value);
						const size_t entryGrowth =
								existing == entries.end()
										? checkedSizeAdd(sizeof(storage::LogicalGraphPropertyEntry),
														 keyViews[column].size(),
														 "logical-plan pattern Property entry size overflowed")
										: 0U;
						columnBytes.set(columnBytes.bytes() - transferred);
						recordBytes.set(
								checkedSizeAdd(recordBytes.bytes(),
											   checkedSizeAdd(entryGrowth, transferred,
															  "logical-plan pattern Property handoff size overflowed"),
											   "logical-plan pattern Property handoff size overflowed"));
						if (existing == entries.end())
							entries.push_back({std::string(keyViews[column]), std::move(columns[column][index])});
						else
							existing->value = std::move(columns[column][index]);
						recordBytes.set(recordBytes.bytes() - previousValue);
					}
				}
				size_t bytes = 0U;
				for (const auto &record: records) {
					size_t recordBytes;
					if constexpr (requires { record.node_id; })
						recordBytes = logicalNodeCacheBytes(record, state.shared.context);
					else
						recordBytes = logicalEdgeCacheBytes(record, state.shared.context);
					bytes = checkedSizeAdd(bytes, recordBytes, "logical-plan pattern named columns size overflowed");
					if (record.properties.size() > state.shared.bounds.maximum_property_entries ||
						bytes > state.shared.bounds.maximum_materialized_bytes) {
						throw std::runtime_error(
								"logical-plan pattern named columns exceeded their materialized bound");
					}
				}
			};

			if (direction != expressions::PatternDirection::PAT_OUTGOING &&
				direction != expressions::PatternDirection::PAT_INCOMING &&
				direction != expressions::PatternDirection::PAT_BOTH) {
				throw std::invalid_argument("logical-plan pattern expression has an unknown direction");
			}
			std::optional<uint64_t> edgeTypeToken;
			const bool canUseReferencePages =
					state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
					(edgeType.empty() || state.shared.snapshot.supportsNativeRelationshipTypeTokens(edgeType));
			if (canUseReferencePages && !edgeType.empty()) {
				edgeTypeToken = state.shared.snapshot.findRelationshipTypeToken(edgeType);
				if (!edgeTypeToken.has_value()) {
					return true;
				}
			}
			size_t sourceEdges = 0U;
			// The target label predicate is fixed for this pattern invocation. Keep a
			// bounded per-invocation cache so repeated targets in a supernode do not
			// reopen the same Entity Map entry for the same logical predicate.
			using LabelMatch = std::pair<uint64_t, bool>;
			MaterializedByteReservation labelCacheBytes(&state.shared.materialized_budget);
			std::vector<LabelMatch> targetLabelMatches;
			const auto cachedLabel = [&](uint64_t id) {
				return std::ranges::find(targetLabelMatches, id, &LabelMatch::first);
			};
			MaterializedByteReservation labelFilterBytes(&state.shared.materialized_budget);
			std::vector<std::string> targetLabelFilter;
			if (!targetLabel.empty()) {
				const size_t nameBytes = std::max(targetLabel.size(), std::string{}.capacity());
				labelFilterBytes.set(checkedSizeAdd(sizeof(std::string), nameBytes,
					"logical-plan pattern label filter slots overflowed"));
				targetLabelFilter.reserve(1U);
				labelFilterBytes.set(checkedSizeAdd(checkedSizeMul(targetLabelFilter.capacity(), sizeof(std::string),
					"logical-plan pattern label filter capacity overflowed"), nameBytes,
					"logical-plan pattern label filter name overflowed"));
				targetLabelFilter.emplace_back(targetLabel);
				labelFilterBytes.set(checkedSizeAdd(checkedSizeMul(targetLabelFilter.capacity(), sizeof(std::string),
					"logical-plan pattern label filter capacity overflowed"), targetLabelFilter.front().capacity(),
					"logical-plan pattern label filter name overflowed"));
			}
			const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				if (canUseReferencePages) {
					for (;;) {
						if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
							throw std::runtime_error(
									"logical-plan pattern expression exceeded its per-Node Edge bound");
						}
						const size_t pageRecords =
								std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
										 lazyCandidates ? size_t{1U} : state.shared.bounds.maximum_adjacency_page_records);
						MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
						const auto page = readReferencePageBudgeted(state, source.node_id, adjacencyDirection, continuation, pageRecords, referenceBytes);
						if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
							throw std::runtime_error("logical graph backend returned an invalid pattern-expression "
													 "adjacency reference page");
						}
						if (continuation.has_value() &&
							(page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id)) {
							throw std::runtime_error("logical graph backend returned a discontinuous "
													 "pattern-expression adjacency reference page");
						}
						// Validate and filter the whole bounded reference page first.  A
						// single sorted label-column lookup then serves all distinct target
						// Nodes on the page instead of reopening the Entity Map per Edge.
						MaterializedByteReservation candidateBytes(&state.shared.materialized_budget);
						candidateBytes.set(checkedSizeMul(page.edges.size(), sizeof(const storage::EntityRef *),
							"logical-plan pattern candidate slots overflowed"));
						std::vector<const storage::EntityRef *> candidates;
						candidates.reserve(page.edges.size());
						candidateBytes.set(checkedSizeMul(candidates.capacity(), sizeof(const storage::EntityRef *),
							"logical-plan pattern candidate capacity overflowed"));
						for (const storage::EntityRef &reference: page.edges) {
							checkGuard(state);
							validateEdgeRef(
									reference,
									"logical graph backend returned an invalid pattern-expression adjacency reference");
							if (reference.entity_id <= previousEdgeId) {
								throw std::runtime_error("logical graph backend returned an invalid pattern-expression "
														 "adjacency reference");
							}
							previousEdgeId = reference.entity_id;
							const bool endpointMatches =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.source_node_id == source.node_id
											: reference.target_node_id == source.node_id;
							if (!endpointMatches) {
								throw std::runtime_error(
										"logical graph backend returned an invalid pattern-expression Edge reference");
							}
							incrementBounded(sourceEdges,
											 "logical-plan pattern-expression per-Node Edge counter overflowed");
							incrementBounded(state.task.stats.edges_scanned,
											 "logical-plan pattern-expression Edge counter overflowed");
							if (direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								reference.source_node_id == source.node_id) {
								continue;
							}
							if (edgeTypeToken.has_value() && reference.type_token != *edgeTypeToken) {
								continue;
							}
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.target_node_id
											: reference.source_node_id;
							static_cast<void>(targetId);
							candidates.push_back(&reference);
						}
						if (state.shared.snapshot.supportsNodeExistenceProbes() && !candidates.empty()) {
							detail::BudgetMemoryResource endpointMemory(state.shared.materialized_budget);
							std::pmr::vector<uint64_t> endpointIds{&endpointMemory};
							endpointIds.reserve(candidates.size());
							for (const auto *candidate: candidates) {
								endpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? candidate->target_node_id : candidate->source_node_id);
							}
							detail::sortUniqueIdsGuarded(endpointIds, [&] {
								if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							});
							MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
							presenceBytes.set(packedBooleanBytes(endpointIds.size()));
							const auto present = state.shared.snapshot.containsNodesSorted(endpointIds);
							presenceBytes.set(packedBooleanBytes(present.capacity()));
							checkGuard(state);
							if (present.size() != endpointIds.size()) {
								throw std::runtime_error("logical graph backend changed pattern-expression endpoint cardinality");
							}
							if (std::ranges::any_of(present, [](bool value) { return !value; })) {
								throw std::runtime_error("logical graph backend returned a pattern-expression Edge with a missing endpoint");
							}
						}
						MaterializedByteReservation labelIdsBytes(&state.shared.materialized_budget);
						MaterializedByteReservation labelBitsBytes(&state.shared.materialized_budget);
						std::vector<uint64_t> targetIds;
						std::vector<bool> matches;
						if (!targetLabel.empty() && !candidates.empty()) {
							labelIdsBytes.set(checkedSizeMul(candidates.size(), sizeof(uint64_t),
								"logical-plan pattern label slots overflowed"));
							targetIds.reserve(candidates.size());
							labelIdsBytes.set(checkedSizeMul(targetIds.capacity(), sizeof(uint64_t),
								"logical-plan pattern label capacity overflowed"));
							for (const auto *candidate: candidates) {
								const uint64_t id = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									? candidate->target_node_id : candidate->source_node_id;
								if (cachedLabel(id) == targetLabelMatches.end()) targetIds.push_back(id);
							}
							detail::sortUniqueIdsGuarded(targetIds, [&] {
								if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							});
							if (!targetIds.empty()) {
								labelBitsBytes.set(checkedSizeAdd(targetIds.size() / 8U + (targetIds.size() % 8U != 0U),
									sizeof(size_t), "logical-plan pattern label bit slots overflowed"));
								matches = state.shared.snapshot.readNodeLabelMatchesSorted(targetIds, targetLabelFilter);
								labelBitsBytes.set(matches.capacity() / 8U + (matches.capacity() % 8U != 0U));
								checkGuard(state);
								if (matches.size() != targetIds.size())
									throw std::runtime_error("logical graph backend changed pattern-expression target label cardinality");
								const size_t cacheLimit = std::min(state.shared.bounds.maximum_rows, size_t{64U});
								for (size_t index = 0U; index < targetIds.size() && targetLabelMatches.size() < cacheLimit; ++index) {
									if (targetLabelMatches.size() == targetLabelMatches.capacity()) {
										const size_t old = targetLabelMatches.capacity();
										const size_t next = old == 0U ? std::min(cacheLimit, size_t{8U}) : old + std::min(old, cacheLimit - old);
										const size_t peak = (old + next) * sizeof(LabelMatch);
										if (labelCacheBytes.wouldExceed(peak)) break; // Cache insertion is optional.
										labelCacheBytes.set(peak);
										targetLabelMatches.reserve(next);
										labelCacheBytes.set(targetLabelMatches.capacity() * sizeof(LabelMatch));
									}
									targetLabelMatches.emplace_back(targetIds[index], matches[index]);
								}
							}
						}
						size_t matchingCount = 0U;
						for (const storage::EntityRef *referencePointer: candidates) {
							const storage::EntityRef &reference = *referencePointer;
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.target_node_id
											: reference.source_node_id;
							if (!targetLabel.empty()) {
								const auto cached = cachedLabel(targetId);
								const auto position = std::ranges::lower_bound(targetIds, targetId);
								const bool labelsMatch = cached != targetLabelMatches.end() ? cached->second
									: (position != targetIds.end() && *position == targetId && matches[position - targetIds.begin()]);
								if (!labelsMatch) {
									continue;
								}
							}
							candidates[matchingCount++] = referencePointer;
						}
						candidates.resize(matchingCount);
						std::vector<uint64_t>().swap(targetIds);
						std::vector<bool>().swap(matches);
						labelIdsBytes.reset();
						labelBitsBytes.reset();
						if (bindingShape == nullptr) {
							// Complete/fallback paths retain the original lazy visitor
							// contract.  In particular, EXISTS may stop after the first
							// match and must not read the rest of the page speculatively.
							for (const storage::EntityRef *referencePointer: candidates) {
								const storage::EntityRef &reference = *referencePointer;
								const storage::LogicalGraphEdge edge = readEdgeCached(state, reference.entity_id);
								if (edge.source_node_id != reference.source_node_id ||
									edge.target_node_id != reference.target_node_id ||
									(!edgeType.empty() && edge.type != edgeType)) {
									throw std::runtime_error("logical graph backend returned an adjacency reference "
															 "with mismatched Edge data");
								}
								const uint64_t targetId =
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.target_node_id
												: reference.source_node_id;
								const storage::LogicalGraphNode target = readNodeCached(state, targetId);
								if (!visitor(edge, target)) {
									return false;
								}
							}
							if (!page.continuation.has_value()) {
								return true;
							}
							if (sourceEdges == state.shared.bounds.maximum_edges_per_node || page.edges.empty() ||
								page.continuation->previous_edge_id != page.edges.back().entity_id ||
								page.continuation->next_edge_id == 0U ||
								page.continuation->previous_edge_id == page.continuation->next_edge_id) {
								throw std::runtime_error("logical graph backend returned an incomplete "
														 "pattern-expression adjacency reference scan");
							}
							continuation = page.continuation;
							continue;
						}
						MaterializedByteReservation gatherBytes(&state.shared.materialized_budget);
						gatherBytes.set(checkedSizeMul(candidates.size(), 2U * sizeof(uint64_t),
							"logical-plan pattern gather slots overflowed"));
						std::vector<uint64_t> pageEdgeIds;
						pageEdgeIds.reserve(candidates.size());
						gatherBytes.set(checkedSizeMul(checkedSizeAdd(pageEdgeIds.capacity(), candidates.size(),
							"logical-plan pattern gather slots overflowed"), sizeof(uint64_t),
							"logical-plan pattern gather capacity overflowed"));
						std::vector<uint64_t> pageTargetIds;
						pageTargetIds.reserve(candidates.size());
						gatherBytes.set(checkedSizeMul(checkedSizeAdd(pageEdgeIds.capacity(), pageTargetIds.capacity(),
							"logical-plan pattern gather capacity overflowed"), sizeof(uint64_t),
							"logical-plan pattern gather capacity overflowed"));
						for (const storage::EntityRef *referencePointer: candidates) {
							pageEdgeIds.push_back(referencePointer->entity_id);
							pageTargetIds.push_back(adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? referencePointer->target_node_id
															: referencePointer->source_node_id);
						}
						detail::sortUniqueIdsGuarded(pageEdgeIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						detail::sortUniqueIdsGuarded(pageTargetIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						storage::EntityReadShape edgeShape =
								bindingShape == nullptr ? completeEdgeReadShape() : bindingShape->relationship;
						if (!edgeType.empty()) {
							edgeShape.include_type = true;
						}
						storage::EntityReadShape targetShape =
								bindingShape == nullptr ? completeNodeReadShape() : bindingShape->target;
						targetShape.include_labels = targetShape.include_labels || !targetLabel.empty();
						MaterializedByteReservation pageEdgeBytes(&state.shared.materialized_budget);
						std::vector<storage::LogicalGraphEdge> pageEdges =
								pageEdgeIds.empty()
										? std::vector<storage::LogicalGraphEdge>{}
										: readEdgesShapedSortedBudgeted(state, pageEdgeIds, edgeShape, &pageEdgeBytes);
						if (pageEdges.size() != pageEdgeIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed pattern-expression Edge batch cardinality");
						}
						retainShapedBatchBytes(pageEdges, pageEdgeBytes, state.shared.context);
						MaterializedByteReservation pageTargetBytes(&state.shared.materialized_budget);
						std::vector<storage::LogicalGraphNode> pageTargets =
								pageTargetIds.empty() ? std::vector<storage::LogicalGraphNode>{}
													  : readNodesShapedSortedBudgeted(state, pageTargetIds, targetShape,
																					  &pageTargetBytes);
						if (pageTargets.size() != pageTargetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed pattern-expression target batch cardinality");
						}
						retainShapedBatchBytes(pageTargets, pageTargetBytes, state.shared.context);
						if (bindingShape != nullptr) {
							appendNamedProperties(pageEdges, pageEdgeIds, bindingShape->relationship_named_properties,
												  true, pageEdgeBytes);
							retainShapedBatchBytes(pageEdges, pageEdgeBytes, state.shared.context);
							appendNamedProperties(pageTargets, pageTargetIds, bindingShape->target_named_properties,
												  false, pageTargetBytes);
							retainShapedBatchBytes(pageTargets, pageTargetBytes, state.shared.context);
						}
						for (const storage::EntityRef *referencePointer: candidates) {
							const storage::EntityRef &reference = *referencePointer;
							const uint64_t targetId =
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? reference.target_node_id
											: reference.source_node_id;
							const auto edgePosition = std::ranges::lower_bound(pageEdges, reference.entity_id, {},
																			   &storage::LogicalGraphEdge::edge_id);
							const auto targetPosition = std::ranges::lower_bound(pageTargets, targetId, {},
																				 &storage::LogicalGraphNode::node_id);
							if (edgePosition == pageEdges.end() || targetPosition == pageTargets.end() ||
								edgePosition->edge_id != reference.entity_id || targetPosition->node_id != targetId) {
								throw std::runtime_error("logical graph backend lost a pattern-expression batch entry");
							}
							const storage::LogicalGraphEdge &edge = *edgePosition;
							if (edge.source_node_id != reference.source_node_id ||
								edge.target_node_id != reference.target_node_id ||
								(!edgeType.empty() && edge.type != edgeType)) {
								throw std::runtime_error("logical graph backend returned an adjacency reference with "
														 "mismatched Edge data");
							}
							validateCandidate(edge, *targetPosition);
							if (!visitor(edge, *targetPosition)) {
								return false;
							}
						}
						if (!page.continuation.has_value()) {
							return true;
						}
						if (sourceEdges == state.shared.bounds.maximum_edges_per_node || page.edges.empty() ||
							page.continuation->previous_edge_id != page.edges.back().entity_id ||
							page.continuation->next_edge_id == 0U ||
							page.continuation->previous_edge_id == page.continuation->next_edge_id) {
							throw std::runtime_error("logical graph backend returned an incomplete pattern-expression "
													 "adjacency reference scan");
						}
						continuation = page.continuation;
					}
				}
				for (;;) {
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords =
							std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
									 lazyCandidates ? size_t{1U} : state.shared.bounds.maximum_adjacency_page_records);
					// The pattern visitor has no Edge Property predicate on this path.
					// Scan topology first, then materialize the selected full Edge batch
					// only for candidates that survive endpoint/type validation.
					MaterializedByteReservation topologyBytes(&state.shared.materialized_budget);
					const auto page = readTopologyPageBudgeted(state, source.node_id,
						adjacencyDirection, continuation, pageRecords, topologyBytes);
					if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
						throw std::runtime_error(
								"logical graph backend returned an invalid pattern-expression adjacency page");
					}
					if (continuation.has_value() &&
						(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
						throw std::runtime_error(
								"logical graph backend returned a discontinuous pattern-expression adjacency page");
					}
					// A complete adjacency page still exposes endpoint IDs independently of
					// the target Node payload. Authenticate the distinct endpoint set once
					// before the per-match materialization loop so parallel edges do not
					// repeat an Entity Directory descent. Readers without the optional batch
					// probe retain the complete-read fallback below.
					MaterializedByteReservation endpointIdsBytes(&state.shared.materialized_budget);
					endpointIdsBytes.set(checkedSizeMul(page.edges.size(), sizeof(uint64_t),
						"logical-plan pattern topology endpoint slots overflowed"));
					std::vector<uint64_t> pageTargetIds;
					pageTargetIds.reserve(page.edges.size());
					endpointIdsBytes.set(checkedSizeMul(pageTargetIds.capacity(), sizeof(uint64_t),
						"logical-plan pattern topology endpoint capacity overflowed"));
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						checkGuard(state);
						validateEdge(edge);
						const bool selfLoopDuplicate =
								direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								edge.source_node_id == source.node_id;
						if (!selfLoopDuplicate && (edgeType.empty() || edge.type == edgeType)) {
							pageTargetIds.push_back(adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
															: edge.source_node_id);
						}
					}
					detail::sortUniqueIdsGuarded(pageTargetIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
					if (state.shared.snapshot.supportsNodeExistenceProbes()) {
						presenceBytes.set(checkedSizeAdd(pageTargetIds.size() / 8U + (pageTargetIds.size() % 8U != 0U),
							sizeof(size_t), "logical-plan pattern topology presence slots overflowed"));
					}
					const std::vector<bool> pageTargetPresent =
							state.shared.snapshot.supportsNodeExistenceProbes()
									? state.shared.snapshot.containsNodesSorted(pageTargetIds)
									: std::vector<bool>{};
					presenceBytes.set(pageTargetPresent.capacity() / 8U + (pageTargetPresent.capacity() % 8U != 0U));
					checkGuard(state);
					if (state.shared.snapshot.supportsNodeExistenceProbes() &&
						pageTargetPresent.size() != pageTargetIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed pattern-expression endpoint cardinality");
					}
					if (state.shared.snapshot.supportsNodeExistenceProbes() &&
						std::ranges::any_of(pageTargetPresent, [](bool present) { return !present; })) {
						throw std::runtime_error("logical graph backend returned a pattern-expression Edge "
												 "with a missing endpoint");
					}
					if (!targetLabel.empty() && !pageTargetIds.empty()) {
						MaterializedByteReservation labelBytes(&state.shared.materialized_budget);
						labelBytes.set(packedBooleanBytes(pageTargetIds.size()));
						std::vector<bool> labels;
						if (state.shared.snapshot.supportsNodeExistenceProbes()) {
							labels = state.shared.snapshot.readNodeLabelMatchesSorted(pageTargetIds, targetLabelFilter);
						} else {
							MaterializedByteReservation nodesBytes(&state.shared.materialized_budget);
							const auto nodes = readNodesShapedSortedBudgeted(
									state, pageTargetIds, {.include_labels = true}, &nodesBytes);
							if (nodes.size() != pageTargetIds.size()) {
								throw std::runtime_error("logical graph backend changed pattern-expression target batch cardinality");
							}
							labels.reserve(nodes.size());
							for (size_t index = 0U; index < nodes.size(); ++index) {
								if (nodes[index].node_id != pageTargetIds[index]) {
									throw std::runtime_error("logical graph backend returned mismatched pattern-expression target identity");
								}
								labels.push_back(std::ranges::find(nodes[index].labels, targetLabel) != nodes[index].labels.end());
							}
						}
						labelBytes.set(packedBooleanBytes(labels.capacity()));
						checkGuard(state);
						if (labels.size() != pageTargetIds.size()) {
							throw std::runtime_error("logical graph backend changed pattern-expression target label cardinality");
						}
						size_t accepted = 0U;
						for (size_t index = 0U; index < labels.size(); ++index) {
							if (labels[index]) pageTargetIds[accepted++] = pageTargetIds[index];
						}
						pageTargetIds.resize(accepted);
					}
					MaterializedByteReservation edgeIdsBytes(&state.shared.materialized_budget);
					edgeIdsBytes.set(checkedSizeMul(page.edges.size(), sizeof(uint64_t),
						"logical-plan pattern topology Edge slots overflowed"));
					std::vector<uint64_t> pageEdgeIds;
					pageEdgeIds.reserve(page.edges.size());
					edgeIdsBytes.set(checkedSizeMul(pageEdgeIds.capacity(), sizeof(uint64_t),
						"logical-plan pattern topology Edge capacity overflowed"));
					for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
						// Do not decode complete Edge payloads for candidates that are
						// guaranteed to be discarded by direction/type semantics.  The
						// topology page remains the validation source; only surviving IDs
						// enter the bounded shaped batch below.
						const bool selfLoopDuplicate =
								direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								candidate.source_node_id == source.node_id;
						const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? candidate.target_node_id : candidate.source_node_id;
						if (!selfLoopDuplicate && (edgeType.empty() || candidate.type == edgeType) &&
							(targetLabel.empty() || std::ranges::binary_search(pageTargetIds, targetId))) {
							pageEdgeIds.push_back(candidate.edge_id);
						}
					}
					detail::sortUniqueIdsGuarded(pageEdgeIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					storage::EntityReadShape edgeShape =
							bindingShape == nullptr ? completeEdgeReadShape() : bindingShape->relationship;
					if (!edgeType.empty()) {
						edgeShape.include_type = true;
					}
					storage::EntityReadShape targetShape =
							bindingShape == nullptr ? completeNodeReadShape() : bindingShape->target;
					targetShape.include_labels = targetShape.include_labels || !targetLabel.empty();
					MaterializedByteReservation pageEdgeBytes(&state.shared.materialized_budget);
					std::vector<storage::LogicalGraphEdge> pageEdgeRecords =
							pageEdgeIds.empty()
									? std::vector<storage::LogicalGraphEdge>{}
									: readEdgesShapedSortedBudgeted(state, pageEdgeIds, edgeShape, &pageEdgeBytes);
					if (pageEdgeRecords.size() != pageEdgeIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed pattern-expression Edge batch cardinality");
					}
					retainShapedBatchBytes(pageEdgeRecords, pageEdgeBytes, state.shared.context);
					MaterializedByteReservation pageTargetBytes(&state.shared.materialized_budget);
					std::vector<storage::LogicalGraphNode> pageTargetRecords =
							pageTargetIds.empty() ? std::vector<storage::LogicalGraphNode>{}
												  : readNodesShapedSortedBudgeted(state, pageTargetIds, targetShape,
																				  &pageTargetBytes);
					if (pageTargetRecords.size() != pageTargetIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed pattern-expression target batch cardinality");
					}
					retainShapedBatchBytes(pageTargetRecords, pageTargetBytes, state.shared.context);
					if (bindingShape != nullptr) {
						appendNamedProperties(pageEdgeRecords, pageEdgeIds, bindingShape->relationship_named_properties,
											  true, pageEdgeBytes);
						retainShapedBatchBytes(pageEdgeRecords, pageEdgeBytes, state.shared.context);
						appendNamedProperties(pageTargetRecords, pageTargetIds, bindingShape->target_named_properties,
											  false, pageTargetBytes);
						retainShapedBatchBytes(pageTargetRecords, pageTargetBytes, state.shared.context);
					}
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						checkGuard(state);
						validateEdge(edge);
						validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "pattern-expression");
						const bool endpointMatches =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.source_node_id == source.node_id
										: edge.target_node_id == source.node_id;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression Edge");
						}
						incrementBounded(sourceEdges,
										 "logical-plan pattern-expression per-Node Edge counter overflowed");
						incrementBounded(state.task.stats.edges_scanned,
										 "logical-plan pattern-expression Edge counter overflowed");
						// A correct global adjacency index returns a self-loop in both directions.
						// OUT is visited first, so omit only that duplicate without retaining a
						// degree-sized Edge-ID set.
						if (direction == expressions::PatternDirection::PAT_BOTH &&
							adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							edge.source_node_id == source.node_id) {
							continue;
						}
						if (!edgeType.empty() && edge.type != edgeType) {
							continue;
						}
						const uint64_t targetId =
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.target_node_id
										: edge.source_node_id;
						if (!targetLabel.empty() && !std::ranges::binary_search(pageTargetIds, targetId)) {
							continue;
						}
						const auto edgePosition = std::ranges::lower_bound(pageEdgeIds, edge.edge_id);
						if (edgePosition == pageEdgeIds.end() || *edgePosition != edge.edge_id) {
							throw std::runtime_error(
									"logical graph backend lost a pattern-expression Edge batch entry");
						}
						const storage::LogicalGraphEdge &materializedEdge =
								pageEdgeRecords[static_cast<size_t>(edgePosition - pageEdgeIds.begin())];
						if (materializedEdge.edge_id != edge.edge_id ||
							materializedEdge.source_node_id != edge.source_node_id ||
							materializedEdge.target_node_id != edge.target_node_id ||
							(!edgeType.empty() && materializedEdge.type != edgeType)) {
							throw std::runtime_error(
									"logical graph backend returned an adjacency Edge with mismatched batch data");
						}
						const auto targetPosition = std::ranges::lower_bound(pageTargetRecords, targetId, {},
																			 &storage::LogicalGraphNode::node_id);
						if (targetPosition == pageTargetRecords.end() || targetPosition->node_id != targetId) {
							throw std::runtime_error(
									"logical graph backend lost a pattern-expression target batch entry");
						}
						if (!targetLabel.empty() &&
							std::ranges::find(targetPosition->labels, targetLabel) == targetPosition->labels.end()) {
							continue;
						}
						validateCandidate(materializedEdge, *targetPosition);
						if (!visitor(materializedEdge, *targetPosition)) {
							return false;
						}
					}
					if (!page.continuation.has_value()) {
						return true;
					}
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node ||
						page.continuation->previous_edge_id != page.edges.back().edge_id ||
						page.continuation->next_edge_id == 0U ||
						page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error(
								"logical graph backend returned an incomplete pattern-expression adjacency scan");
					}
					continuation = page.continuation;
				}
			};
			if ((direction == expressions::PatternDirection::PAT_OUTGOING ||
				 direction == expressions::PatternDirection::PAT_BOTH) &&
				!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
				return false;
			}
			if (direction == expressions::PatternDirection::PAT_INCOMING ||
				direction == expressions::PatternDirection::PAT_BOTH) {
				return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
			}
			return true;
		}

		[[nodiscard]] LogicalPlanReadValue evaluateExpression(const expressions::Expression &expression,
															  LogicalPlanReadRow &row, ExecutionState &state) {
			using expressions::ExpressionType;
			switch (expression.getExpressionType()) {
				case ExpressionType::LITERAL: {
					const auto &literal = dynamic_cast<const expressions::LiteralExpression &>(expression);
					if (literal.isNull()) {
						return PropertyValue();
					}
					if (literal.isBoolean()) {
						return PropertyValue(literal.getBooleanValue());
					}
					if (literal.isInteger()) {
						return PropertyValue(literal.getIntegerValue());
					}
					if (literal.isDouble()) {
						return PropertyValue(literal.getDoubleValue());
					}
					if (literal.isPropertyValue()) {
						detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(literal.getPropertyValue()));
						return literal.getPropertyValue();
					}
					detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(literal.getStringValue().size()));
					return PropertyValue(literal.getStringValue());
				}
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS: {
					const auto &reference = dynamic_cast<const expressions::VariableReferenceExpression &>(expression);
					if (state.task.bound_inputs && state.task.bound_inputs->usesRow(row)) {
						const auto *instruction = state.task.bound_inputs->instruction(expression);
						const auto *value = instruction ? state.task.bound_inputs->value(*instruction) : nullptr;
						if (value && instruction->kind == detail::BoundInputKind::BIK_VALUE) {
							debug::PerfTrace::addValue("logical_plan.bound_expression.slot_reads", 1);
                            detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(*value)); return *value;
						}
						if (value && instruction->kind == detail::BoundInputKind::BIK_PROPERTY) {
							const auto property = state.task.bound_inputs->property(*instruction, state.shared.snapshot);
							if (property && (property->source != *instruction->source || property->key != reference.getPropertyName()))
								throw std::logic_error("bound property descriptor disagrees with its expression");
							debug::PerfTrace::addValue("logical_plan.bound_expression.property_reads", 1);
							return readProperty(state.shared.snapshot, row, reference.getVariableName(), reference.getPropertyName(), state, value);
						}
					}
					if (reference.hasProperty()) {
						// A WITH/RETURN projection without AS keeps the expression text as
						// its column name (for example `WITH n.value`). Resolve that
						// projected column before interpreting the same text as a property
						// access on a variable that is no longer in scope.
						if (const auto projected = row.values.find(reference.toString());
							projected != row.values.end()) {
							detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(projected->second));
							return projected->second;
						}
						return readProperty(state.shared.snapshot, row, reference.getVariableName(),
											reference.getPropertyName(), state);
					}
					const auto &binding = requireBinding(row, reference.getVariableName());
                    detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(binding));
                    return binding;
				}
				case ExpressionType::PARAMETER: {
					if (state.shared.context == nullptr) {
						throw std::runtime_error("logical-plan parameter evaluation requires a QueryContext");
					}
					const auto &parameter = dynamic_cast<const expressions::ParameterExpression &>(expression);
					const PropertyValue *value = nullptr;
					if (state.task.bound_inputs && state.task.bound_inputs->usesRow(row)) {
						const auto *input = state.task.bound_inputs->instruction(expression);
						if (input && input->kind == detail::BoundInputKind::BIK_PARAMETER)
							value = state.task.bound_inputs->parameter(*input, state.shared.context);
					}
					if (!value) {
						const auto found = state.shared.context->parameters.find(parameter.getParameterName());
						if (found != state.shared.context->parameters.end()) value = &found->second;
					}
					if (!value) {
						throw std::runtime_error("logical-plan read runtime could not resolve a query parameter");
					}
					if (value->getType() == PropertyType::STRING || value->getType() == PropertyType::LIST || value->getType() == PropertyType::MAP || value->getType() == PropertyType::BYTES) {
						MaterializedByteReservation copyBytes(&state.shared.materialized_budget);
						return copyReservedPropertyValue(*value, state, copyBytes);
					}
					return *value;
				}
				case ExpressionType::EXPR_LIST_LITERAL: {
					const auto &list = dynamic_cast<const expressions::ListLiteralExpression &>(expression);
					if (list.isStatic()) {
						if (list.getValue().getList().size() > state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan List literal exceeded the configured List bound");
						}
						MaterializedByteReservation copyBytes(&state.shared.materialized_budget);
						copyBytes.set(materializedListStorageBytes(list.getValue(), state));
						return list.getValue();
					}
					if (list.getElements().size() > state.shared.bounds.maximum_rows) {
						throw std::runtime_error("logical-plan List literal exceeded the configured List bound");
					}
					MaterializedByteReservation valuesReservation(&state.shared.materialized_budget);
					valuesReservation.set(
							checkedSizeAdd(sizeof(uint32_t),
										   checkedSizeMul(list.getElements().size(), sizeof(PropertyValue),
														  "logical-plan List literal capacity overflowed"),
										   "logical-plan List literal size overflowed"));
					std::vector<PropertyValue> values;
					values.reserve(list.getElements().size());
					const size_t valuesCapacityBytes =
							checkedSizeAdd(sizeof(uint32_t),
										   checkedSizeMul(values.capacity(), sizeof(PropertyValue),
														  "logical-plan List literal capacity overflowed"),
										   "logical-plan List literal size overflowed");
					valuesReservation.set(valuesCapacityBytes);
					for (const auto &element: list.getElements()) {
						PropertyValue value = scalarValue(evaluateExpression(*element, row, state));
						valuesReservation.set(checkedSizeAdd(valuesReservation.bytes(),
															 retainedExpressionValueBytes(value, state),
															 "logical-plan List literal size overflowed"));
						values.push_back(std::move(value));
					}
					return PropertyValue(std::move(values));
				}
				default:
					return evaluateScalarExpression(expression, row, state);
			}
		}

		[[nodiscard]] PropertyValue arithmetic(expressions::BinaryOperatorType operation, const PropertyValue &left,
											   const PropertyValue &right) {
			if (isNull(left) || isNull(right)) {
				return PropertyValue();
			}
			const PropertyType leftType = left.getType();
			const PropertyType rightType = right.getType();
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DATE &&
				rightType == PropertyType::DURATION) {
				return addDurationToDate(std::get<TemporalDate>(left.getVariant()),
										 std::get<TemporalDuration>(right.getVariant()));
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DURATION &&
				rightType == PropertyType::DATE) {
				return addDurationToDate(std::get<TemporalDate>(right.getVariant()),
										 std::get<TemporalDuration>(left.getVariant()));
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DURATION) {
				return addDurationToDateTime(std::get<TemporalDateTime>(left.getVariant()),
											 std::get<TemporalDuration>(right.getVariant()));
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD && leftType == PropertyType::DURATION &&
				rightType == PropertyType::DATETIME) {
				return addDurationToDateTime(std::get<TemporalDateTime>(right.getVariant()),
											 std::get<TemporalDuration>(left.getVariant()));
			}
			if (leftType == PropertyType::DURATION && rightType == PropertyType::DURATION) {
				const TemporalDuration &leftDuration = std::get<TemporalDuration>(left.getVariant());
				const TemporalDuration &rightDuration = std::get<TemporalDuration>(right.getVariant());
				if (operation == expressions::BinaryOperatorType::BOP_ADD) {
					return PropertyValue(TemporalDuration{checkedAdd(leftDuration.months, rightDuration.months),
														  checkedAdd(leftDuration.days, rightDuration.days),
														  checkedAdd(leftDuration.nanos, rightDuration.nanos)});
				}
				if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT) {
					return PropertyValue(TemporalDuration{checkedSubtract(leftDuration.months, rightDuration.months),
														  checkedSubtract(leftDuration.days, rightDuration.days),
														  checkedSubtract(leftDuration.nanos, rightDuration.nanos)});
				}
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATE &&
				rightType == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(right.getVariant());
				return addDurationToDate(std::get<TemporalDate>(left.getVariant()),
										 TemporalDuration{checkedSubtract(0LL, duration.months),
														  checkedSubtract(0LL, duration.days),
														  checkedSubtract(0LL, duration.nanos)});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(right.getVariant());
				return addDurationToDateTime(std::get<TemporalDateTime>(left.getVariant()),
											 TemporalDuration{checkedSubtract(0LL, duration.months),
															  checkedSubtract(0LL, duration.days),
															  checkedSubtract(0LL, duration.nanos)});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATE &&
				rightType == PropertyType::DATE) {
				const TemporalDate &leftDate = std::get<TemporalDate>(left.getVariant());
				const TemporalDate &rightDate = std::get<TemporalDate>(right.getVariant());
				return PropertyValue(
						TemporalDuration{0LL, static_cast<int64_t>(leftDate.epochDays) - rightDate.epochDays, 0LL});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DATETIME) {
				constexpr int64_t MILLIS_PER_DAY = 86'400LL * 1'000LL;
				const int64_t milliseconds =
						checkedSubtract(std::get<TemporalDateTime>(left.getVariant()).epochMillis,
										std::get<TemporalDateTime>(right.getVariant()).epochMillis);
				return PropertyValue(TemporalDuration{0LL, milliseconds / MILLIS_PER_DAY,
													  (milliseconds % MILLIS_PER_DAY) * 1'000'000LL});
			}
			if (operation == expressions::BinaryOperatorType::BOP_ADD &&
				(leftType == PropertyType::STRING || rightType == PropertyType::STRING)) {
				// Keep the logical path aligned with the established expression evaluator.
				return PropertyValue(expressions::EvaluationContext::toString(left) +
									 expressions::EvaluationContext::toString(right));
			}
			const int64_t *leftInteger = std::get_if<int64_t>(&left.getVariant());
			const int64_t *rightInteger = std::get_if<int64_t>(&right.getVariant());
			const double leftNumber = leftInteger ? static_cast<double>(*leftInteger)
									  : std::get_if<double>(&left.getVariant())
											  ? *std::get_if<double>(&left.getVariant())
											  : std::numeric_limits<double>::quiet_NaN();
			const double rightNumber = rightInteger ? static_cast<double>(*rightInteger)
									   : std::get_if<double>(&right.getVariant())
											   ? *std::get_if<double>(&right.getVariant())
											   : std::numeric_limits<double>::quiet_NaN();
			if (!std::isfinite(leftNumber) || !std::isfinite(rightNumber)) {
				throw std::runtime_error("logical-plan arithmetic requires numeric operands");
			}
			if (operation == expressions::BinaryOperatorType::BOP_MODULO && (!leftInteger || !rightInteger)) {
				throw std::runtime_error("logical-plan modulo requires integer operands");
			}
			if ((operation == expressions::BinaryOperatorType::BOP_DIVIDE ||
				 operation == expressions::BinaryOperatorType::BOP_MODULO) &&
				rightNumber == 0.0) {
				throw std::runtime_error("logical-plan arithmetic attempted division by zero");
			}
			if (leftInteger && rightInteger) {
				switch (operation) {
					case expressions::BinaryOperatorType::BOP_ADD:
						return PropertyValue(checkedAdd(*leftInteger, *rightInteger));
					case expressions::BinaryOperatorType::BOP_SUBTRACT:
						return PropertyValue(checkedSubtract(*leftInteger, *rightInteger));
					case expressions::BinaryOperatorType::BOP_MULTIPLY:
						return PropertyValue(checkedMultiply(*leftInteger, *rightInteger));
					case expressions::BinaryOperatorType::BOP_MODULO:
						if (*leftInteger == std::numeric_limits<int64_t>::min() && *rightInteger == -1) {
							throw std::overflow_error("logical-plan integer modulo overflowed");
						}
						return PropertyValue(*leftInteger % *rightInteger);
					default:
						break;
				}
			}
			PropertyValue result;
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_ADD:
					result = PropertyValue(leftNumber + rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_SUBTRACT:
					result = PropertyValue(leftNumber - rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_MULTIPLY:
					result = PropertyValue(leftNumber * rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_DIVIDE:
					result = PropertyValue(leftNumber / rightNumber);
					break;
				case expressions::BinaryOperatorType::BOP_POWER:
					result = PropertyValue(std::pow(leftNumber, rightNumber));
					break;
				default: // ZYX_COV_EXCL_LINE: isArithmeticOperator admits every arithmetic enum value handled above.
					throw std::logic_error("logical-plan runtime received an unsupported arithmetic operation");
			}
			const double value = std::get<double>(result.getVariant());
			if (!std::isfinite(value)) {
				throw std::overflow_error("logical-plan floating-point arithmetic produced a non-finite result");
			}
			return result;
		}

		[[nodiscard]] PropertyValue comparison(expressions::BinaryOperatorType operation, const PropertyValue &left,
											   const PropertyValue &right) {
			if (isNull(left) || isNull(right)) {
				return PropertyValue();
			}
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_EQUAL:
					return PropertyValue(left == right);
				case expressions::BinaryOperatorType::BOP_NOT_EQUAL:
					return PropertyValue(left != right);
				case expressions::BinaryOperatorType::BOP_LESS:
					return PropertyValue(left < right);
				case expressions::BinaryOperatorType::BOP_GREATER:
					return PropertyValue(left > right);
				case expressions::BinaryOperatorType::BOP_LESS_EQUAL:
					return PropertyValue(left <= right);
				case expressions::BinaryOperatorType::BOP_GREATER_EQUAL:
					return PropertyValue(left >= right);
				default:
					throw std::logic_error("logical-plan runtime received an unsupported comparison operation");
			}
		}

		[[nodiscard]] PropertyValue logical(expressions::BinaryOperatorType operation, const PropertyValue &left,
											const PropertyValue &right) {
			const std::optional<bool> leftBoolean = booleanOrNull(left);
			const std::optional<bool> rightBoolean = booleanOrNull(right);
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_AND:
					if (leftBoolean == false || rightBoolean == false) {
						return PropertyValue(false);
					}
					return makeBoolean(leftBoolean.has_value() && rightBoolean.has_value() ? std::optional<bool>(true)
																						   : std::nullopt);
				case expressions::BinaryOperatorType::BOP_OR:
					if (leftBoolean == true || rightBoolean == true) {
						return PropertyValue(true);
					}
					return makeBoolean(leftBoolean.has_value() && rightBoolean.has_value() ? std::optional<bool>(false)
																						   : std::nullopt);
				case expressions::BinaryOperatorType::BOP_XOR:
					return !leftBoolean.has_value() || !rightBoolean.has_value()
								   ? PropertyValue()
								   : PropertyValue(*leftBoolean != *rightBoolean);
				default: // ZYX_COV_EXCL_LINE: isLogicalOperator admits every logical enum value handled above.
					throw std::logic_error("logical-plan runtime received an unsupported logical operation");
			}
		}

		[[nodiscard]] PropertyValue stringOperation(expressions::BinaryOperatorType operation,
													const PropertyValue &left, const PropertyValue &right) {
			if (isNull(left) || isNull(right)) {
				return PropertyValue();
			}
			const std::string leftString = expressions::EvaluationContext::toString(left);
			const std::string rightString = expressions::EvaluationContext::toString(right);
			switch (operation) {
				case expressions::BinaryOperatorType::BOP_STARTS_WITH:
					return PropertyValue(leftString.starts_with(rightString));
				case expressions::BinaryOperatorType::BOP_ENDS_WITH:
					return PropertyValue(leftString.ends_with(rightString));
				case expressions::BinaryOperatorType::BOP_CONTAINS:
					return PropertyValue(leftString.find(rightString) != std::string::npos);
				case expressions::BinaryOperatorType::BOP_REGEX_MATCH:
					try {
						return PropertyValue(std::regex_match(
								leftString, std::regex(rightString, std::regex_constants::ECMAScript)));
					} catch (const std::regex_error &) {
						throw std::runtime_error("logical-plan received an invalid regular expression");
					}
				default: // ZYX_COV_EXCL_LINE: callers dispatch only the three supported String operator enum values.
					throw std::logic_error("logical-plan runtime received an unsupported string operation");
			}
		}

		[[nodiscard]] std::string lowerCase(std::string_view value) {
			std::string result(value);
			std::ranges::transform(result, result.begin(),
								   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return result;
		}

		void requireScalarFunctionArgumentType(const PropertyValue &value, const PropertyType expected,
											   std::string_view functionName, const size_t argumentIndex) {
			if (isNull(value) || value.getType() == expected) {
				return;
			}
			throw std::runtime_error("logical-plan " + std::string(functionName) + "() argument " +
									 std::to_string(argumentIndex + 1U) + " has an invalid type");
		}

		void requireScalarFunctionArgumentOneOf(const PropertyValue &value, const PropertyType first,
												const PropertyType second, std::string_view functionName,
												const size_t argumentIndex) {
			if (isNull(value) || value.getType() == first || value.getType() == second) {
				return;
			}
			throw std::runtime_error("logical-plan " + std::string(functionName) + "() argument " +
									 std::to_string(argumentIndex + 1U) + " has an invalid type");
		}

		// The legacy FunctionRegistry remains a storage-neutral implementation
		// detail during retirement, but the native public path owns Cypher's typed
		// contract. Validate it here instead of inheriting permissive display
		// conversion behavior.
		void validateNativeScalarFunctionTypes(const std::string_view functionName,
											   const std::vector<PropertyValue> &arguments) {
			const bool unaryNumeric = functionName == "abs" || functionName == "ceil" || functionName == "floor" ||
									  functionName == "round" || functionName == "sqrt" || functionName == "sign" ||
									  functionName == "log" || functionName == "log10" || functionName == "exp" ||
									  functionName == "sin" || functionName == "cos" || functionName == "tan" ||
									  functionName == "asin" || functionName == "acos" || functionName == "atan";
			if (unaryNumeric) {
				requireScalarFunctionArgumentOneOf(arguments.front(), PropertyType::INTEGER, PropertyType::DOUBLE,
												   functionName, 0U);
				return;
			}
			if (functionName == "pow" || functionName == "atan2") {
				for (size_t index = 0U; index < arguments.size(); ++index) {
					requireScalarFunctionArgumentOneOf(arguments[index], PropertyType::INTEGER, PropertyType::DOUBLE,
													   functionName, index);
				}
				return;
			}
			const bool unaryString = functionName == "upper" || functionName == "lower" || functionName == "toupper" ||
									 functionName == "tolower" || functionName == "trim" || functionName == "ltrim" ||
									 functionName == "rtrim";
			if (unaryString) {
				requireScalarFunctionArgumentType(arguments.front(), PropertyType::STRING, functionName, 0U);
				return;
			}
			if (functionName == "substring") {
				requireScalarFunctionArgumentType(arguments[0], PropertyType::STRING, functionName, 0U);
				requireScalarFunctionArgumentType(arguments[1], PropertyType::INTEGER, functionName, 1U);
				if (arguments.size() == 3U) {
					requireScalarFunctionArgumentType(arguments[2], PropertyType::INTEGER, functionName, 2U);
				}
				return;
			}
			if (functionName == "startswith" || functionName == "endswith" || functionName == "contains" ||
				functionName == "split") {
				requireScalarFunctionArgumentType(arguments[0], PropertyType::STRING, functionName, 0U);
				requireScalarFunctionArgumentType(arguments[1], PropertyType::STRING, functionName, 1U);
				return;
			}
			if (functionName == "replace") {
				for (size_t index = 0U; index < arguments.size(); ++index) {
					requireScalarFunctionArgumentType(arguments[index], PropertyType::STRING, functionName, index);
				}
				return;
			}
			if (functionName == "left" || functionName == "right") {
				requireScalarFunctionArgumentType(arguments[0], PropertyType::STRING, functionName, 0U);
				requireScalarFunctionArgumentType(arguments[1], PropertyType::INTEGER, functionName, 1U);
				return;
			}
			if (functionName == "length" || functionName == "size" || functionName == "reverse") {
				requireScalarFunctionArgumentOneOf(arguments.front(), PropertyType::STRING, PropertyType::LIST,
												   functionName, 0U);
				return;
			}
			if (functionName == "head" || functionName == "tail" || functionName == "last" || functionName == "nodes" ||
				functionName == "relationships") {
				requireScalarFunctionArgumentType(arguments.front(), PropertyType::LIST, functionName, 0U);
			}
		}

		[[nodiscard]] PropertyValue evaluateNativeToBoolean(const PropertyValue &argument) {
			if (isNull(argument)) {
				return PropertyValue();
			}
			if (const bool *value = std::get_if<bool>(&argument.getVariant())) {
				return PropertyValue(*value);
			}
			if (const int64_t *value = std::get_if<int64_t>(&argument.getVariant())) {
				return PropertyValue(*value != 0);
			}
			if (const double *value = std::get_if<double>(&argument.getVariant())) {
				if (!std::isfinite(*value)) {
					throw std::runtime_error("logical-plan toBoolean() requires a finite numeric argument");
				}
				return PropertyValue(*value != 0.0);
			}
			if (const std::string *value = std::get_if<std::string>(&argument.getVariant())) {
				const std::string normalized = lowerCase(*value);
				if (normalized == "true") {
					return PropertyValue(true);
				}
				if (normalized == "false") {
					return PropertyValue(false);
				}
				throw std::runtime_error("logical-plan toBoolean() requires true or false text");
			}
			throw std::runtime_error("logical-plan toBoolean() requires a Boolean, numeric value, or true/false text");
		}

		[[nodiscard]] PropertyValue evaluateEntityFunction(const expressions::FunctionCallExpression &function,
														   LogicalPlanReadRow &row, ExecutionState &state) {
			const std::vector<std::unique_ptr<expressions::Expression>> &arguments = function.getArguments();
			if (arguments.size() != 1U || arguments.front() == nullptr) {
				throw std::runtime_error("logical-plan entity function requires exactly one variable argument");
			}
			const auto *reference =
					dynamic_cast<const expressions::VariableReferenceExpression *>(arguments.front().get());
			if (reference == nullptr || reference->hasProperty()) {
				throw std::runtime_error("logical-plan entity function requires a bare variable argument");
			}
			const LogicalPlanReadValue &binding = requireBinding(row, reference->getVariableName());
			const std::string name = lowerCase(function.getFunctionName());
			if (name == "id") {
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
					return PropertyValue(node->node_id);
				}
				if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
					return PropertyValue(edge->edge_id);
				}
				if (const auto *reference = std::get_if<storage::EntityRef>(&binding)) {
					validateEntityRef(*reference);
					return PropertyValue(reference->entity_id);
				}
				throw std::runtime_error("logical-plan id() requires a node or relationship variable");
			}
			if (name == "labels") {
				if (const auto metadata = row.entity_metadata_cache.find(reference->getVariableName());
					metadata != row.entity_metadata_cache.end()) {
					if (const auto cached = metadata->second.find(name); cached != metadata->second.end()) {
						detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(cached->second));
						return cached->second;
					}
				}
				// A deferred Node reference needs only its labels for labels(). Keep the
				// read shape narrow so the Property root is not opened merely to answer
				// an introspection function. Complete Node values already carry labels.
				const storage::LogicalGraphNode node = [&]() {
					if (const auto *referenceValue = std::get_if<storage::EntityRef>(&binding)) {
						validateNodeRef(*referenceValue, "logical-plan labels() requires a valid node variable");
						return state.shared.snapshot.readNodeShaped(referenceValue->entity_id, {.include_labels = true});
					}
					return materializeNodeValue(state, binding);
				}();
				if (node.labels.size() > state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan labels() exceeded the configured List bound");
				}
				std::vector<PropertyValue> labels;
				labels.reserve(node.labels.size());
				for (const std::string &label: node.labels) {
					detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(label.size()));
                    labels.emplace_back(label);
				}
				PropertyValue result(std::move(labels));
				auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(row.entity_metadata_cache, reference->getVariableName());
                static_cast<void>(metadataInserted);
                detail::procedureMapInsertOrAssign(metadataOwner->second, name, result);
				return result;
			}
			if (name == "type") {
				if (const auto metadata = row.entity_metadata_cache.find(reference->getVariableName());
					metadata != row.entity_metadata_cache.end()) {
					if (const auto cached = metadata->second.find(name); cached != metadata->second.end()) {
						detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(cached->second));
						return cached->second;
					}
				}
				// type() is topology metadata. Resolve just the relationship type for a
				// deferred Edge reference; full Edge properties are unnecessary here.
				if (const auto *referenceValue = std::get_if<storage::EntityRef>(&binding)) {
					validateEdgeRef(*referenceValue, "logical-plan type() requires a valid relationship variable");
					PropertyValue result(
							state.shared.snapshot.readEdgeShaped(referenceValue->entity_id, {.include_type = true}).type);
					auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(row.entity_metadata_cache, reference->getVariableName());
                static_cast<void>(metadataInserted);
                detail::procedureMapInsertOrAssign(metadataOwner->second, name, result);
					return result;
				}
				PropertyValue result(materializeEdgeValue(state, binding).type);
				auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(row.entity_metadata_cache, reference->getVariableName());
                static_cast<void>(metadataInserted);
                detail::procedureMapInsertOrAssign(metadataOwner->second, name, result);
				return result;
			}
			if (name != "keys" && name != "properties") {
				throw std::logic_error("logical-plan received an unknown entity introspection function");
			}
			const auto *scalar = std::get_if<PropertyValue>(&binding);
			if (!std::holds_alternative<storage::LogicalGraphNode>(binding) &&
				!std::holds_alternative<storage::LogicalGraphEdge>(binding) &&
				!std::holds_alternative<storage::EntityRef>(binding) &&
				(scalar == nullptr || scalar->getType() != PropertyType::MAP)) {
				throw std::runtime_error(
						"logical-plan entity Property function requires a node, relationship, or Map variable");
			}
			const PropertyMap &properties =
					readBindingProperties(state.shared.snapshot, row, reference->getVariableName(), state);
			if (name == "properties") {
                detail::ProcedurePayloadAdmissionScope::admit(detail::procedurePayloadHeapUpper<true>(properties));
				return PropertyValue(properties);
			}
			if (properties.size() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan keys() exceeded the configured List bound");
			}
			std::vector<std::string> orderedKeys;
			detail::procedureVectorReserve(orderedKeys, properties.size());
			for (const auto &[key, value]: properties) {
				static_cast<void>(value);
				detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(key.size()));
                orderedKeys.push_back(key);
			}
			std::ranges::sort(orderedKeys);
			std::vector<PropertyValue> keys;
			detail::procedureVectorReserve(keys, orderedKeys.size());
			for (const std::string &key: orderedKeys) {
				detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(key.size()));
                keys.emplace_back(key);
			}
			return PropertyValue(std::move(keys));
		}

		[[nodiscard]] PropertyValue evaluateRangeFunction(const std::vector<PropertyValue> &arguments,
														  ExecutionState &state) {
			if (arguments.size() < 2U || arguments.size() > 3U ||
				std::ranges::any_of(arguments, [](const PropertyValue &argument) { return isNull(argument); })) {
				return PropertyValue();
			}
			const auto toInteger = [](const PropertyValue &value) -> int64_t {
				if (const auto *integer = std::get_if<int64_t>(&value.getVariant())) {
					return *integer;
				}
				if (const auto *number = std::get_if<double>(&value.getVariant());
					number != nullptr && std::isfinite(*number) &&
					*number >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
					*number < static_cast<double>(std::numeric_limits<int64_t>::max())) {
					return static_cast<int64_t>(*number);
				}
				throw std::runtime_error("logical-plan range() requires numeric arguments");
			};
			const int64_t start = toInteger(arguments[0]);
			const int64_t end = toInteger(arguments[1]);
			const int64_t step = arguments.size() == 3U ? toInteger(arguments[2]) : 1;
			if (step == 0) {
				throw std::runtime_error("range() step cannot be zero");
			}
			MaterializedByteReservation listBytes(&state.shared.materialized_budget);
			std::vector<PropertyValue> result;
			size_t payloadBytes = 0U;
			const auto admitListBytes = [&](size_t capacity, size_t payload) {
				const size_t bytes =
						checkedSizeAdd(sizeof(uint32_t),
									   checkedSizeAdd(checkedSizeMul(capacity, sizeof(PropertyValue),
																	 "logical-plan range() capacity overflowed"),
													  payload, "logical-plan range() storage size overflowed"),
									   "logical-plan range() storage size overflowed");
				if (listBytes.wouldExceed(bytes)) {
					throw std::runtime_error("logical-plan range() exceeded its shared byte bound");
				}
				listBytes.set(bytes);
			};
			const bool hasValues = step > 0 ? start <= end : start >= end;
			if (hasValues && state.shared.bounds.maximum_rows != 0U) {
				const uint64_t distance = step > 0 ? static_cast<uint64_t>(end) - static_cast<uint64_t>(start)
												   : static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
				const uint64_t magnitude =
						step > 0 ? static_cast<uint64_t>(step) : uint64_t{0} - static_cast<uint64_t>(step);
				const uint64_t additionalValues = distance / magnitude;
				const size_t initialCapacity =
						std::min(state.shared.bounds.maximum_rows,
								 additionalValues >= 15U ? size_t{16} : static_cast<size_t>(additionalValues + 1U));
				admitListBytes(initialCapacity, sizeof(int64_t));
				result.reserve(initialCapacity);
				admitListBytes(result.capacity(), sizeof(int64_t));
			}
			int64_t current = start;
			while ((step > 0 && current <= end) || (step < 0 && current >= end)) {
				checkGuard(state);
				if (result.size() == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan range() exceeded the configured List bound");
				}
				const size_t nextPayloadBytes =
						checkedSizeAdd(payloadBytes, sizeof(int64_t), "logical-plan range() payload size overflowed");
				if (result.size() == result.capacity()) {
					const size_t capacity = result.capacity();
					const size_t nextCapacity = capacity == 0U								? 1U
												: capacity > state.shared.bounds.maximum_rows / 2U ? state.shared.bounds.maximum_rows
																							: capacity * 2U;
					admitListBytes(nextCapacity, nextPayloadBytes);
					result.reserve(nextCapacity);
				}
				admitListBytes(result.capacity(), nextPayloadBytes);
				result.emplace_back(current);
				payloadBytes = nextPayloadBytes;
				if ((step > 0 && current > std::numeric_limits<int64_t>::max() - step) ||
					(step < 0 && current < std::numeric_limits<int64_t>::min() - step)) {
					break;
				}
				current += step;
			}
			return PropertyValue(std::move(result));
		}

		[[nodiscard]] PropertyValue evaluateFunctionExpression(const expressions::FunctionCallExpression &function,
															   LogicalPlanReadRow &row, ExecutionState &state) {
			const expressions::ScalarFunction *registered =
					expressions::FunctionRegistry::getInstance().lookupScalarFunction(function.getFunctionName());
			if (registered == nullptr) {
				throw std::runtime_error("logical-plan runtime could not resolve a scalar function");
			}
			if (!registered->validateArgCount(function.getArgumentCount())) {
				throw std::runtime_error("logical-plan scalar function has an invalid argument count");
			}
			if (function.getArgumentCount() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan scalar function exceeded the configured argument bound");
			}
			if (dynamic_cast<const expressions::EntityIntrospectionFunction *>(registered) != nullptr) {
				return evaluateEntityFunction(function, row, state);
			}
			const std::string normalizedName = lowerCase(function.getFunctionName());
			if ((normalizedName == "size" || normalizedName == "length") && function.getArgumentCount() == 1U) {
				const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
						function.getArguments().front().get());
				if (reference != nullptr && !reference->hasProperty()) {
					const auto cached = row.entity_metadata_cache.find(reference->getVariableName());
					if (cached != row.entity_metadata_cache.end()) {
						const auto scalar = cached->second.find("$shortest_path_" + normalizedName);
						if (scalar != cached->second.end())
							return scalar->second;
					}
				}
			}
			if ((normalizedName == "size" || normalizedName == "length") && function.getArgumentCount() == 1U &&
				function.getArguments().front() != nullptr &&
				function.getArguments().front()->getExpressionType() == expressions::ExpressionType::SHORTEST_PATH) {
				return evaluateScalarExpression(*function.getArguments().front(), row, state,
												normalizedName == "size" ? ShortestPathResultMode::SPR_SIZE
																		 : ShortestPathResultMode::SPR_LENGTH);
			}
			std::vector<PropertyValue> arguments;
			MaterializedByteReservation argumentBytes(&state.shared.materialized_budget);
			const size_t requestedArgumentSlotsBytes =
					checkedSizeMul(function.getArgumentCount(), sizeof(PropertyValue),
								   "logical-plan scalar function argument storage size overflowed");
			argumentBytes.set(requestedArgumentSlotsBytes);
			arguments.reserve(function.getArgumentCount());
			const size_t argumentSlotsBytes =
					checkedSizeMul(arguments.capacity(), sizeof(PropertyValue),
								   "logical-plan scalar function argument storage size overflowed");
			argumentBytes.set(argumentSlotsBytes);
			size_t argumentPayloadBytes = 0U;
			for (const std::unique_ptr<expressions::Expression> &argument: function.getArguments()) {
				if (argument == nullptr) {
					throw std::logic_error("logical-plan scalar function has a null argument expression");
				}
				PropertyValue value = scalarValue(evaluateExpression(*argument, row, state));
				if (value.getType() == PropertyType::LIST && value.getList().size() > state.shared.bounds.maximum_rows) {
					throw std::runtime_error(
							"logical-plan scalar function received a List exceeding the configured List bound");
				}
				const size_t nextPayloadBytes =
						checkedSizeAdd(argumentPayloadBytes, retainedExpressionValueBytes(value, state),
									   "logical-plan scalar function argument payload size overflowed");
				argumentBytes.set(checkedSizeAdd(argumentSlotsBytes, nextPayloadBytes,
												 "logical-plan scalar function argument storage size overflowed"));
				arguments.push_back(std::move(value));
				argumentPayloadBytes = nextPayloadBytes;
			}
			if (normalizedName == "range") {
				return evaluateRangeFunction(arguments, state);
			}
			if (normalizedName == "toboolean") {
				return evaluateNativeToBoolean(arguments.front());
			}
			validateNativeScalarFunctionTypes(normalizedName, arguments);
			if (normalizedName == "length" && arguments.size() == 1U &&
				arguments.front().getType() == PropertyType::LIST) {
				const std::vector<PropertyValue> &elements = arguments.front().getList();
				bool isPath = !elements.empty();
				size_t relationships = 0U;
				for (size_t index = 0U; index < elements.size(); ++index) {
					if (elements[index].getType() != PropertyType::MAP) {
						isPath = false;
						break;
					}
					const auto type = elements[index].getMap().find("_type");
					const std::string expected = index % 2U == 0U ? "node" : "relationship";
					if (type == elements[index].getMap().end() || type->second.getType() != PropertyType::STRING ||
						std::get<std::string>(type->second.getVariant()) != expected) {
						isPath = false;
						break;
					}
					relationships += index % 2U == 0U ? 0U : 1U;
				}
				if (isPath) {
					if (relationships > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
						throw std::overflow_error("logical-plan path length exceeds the integer result range");
					}
					return PropertyValue(static_cast<int64_t>(relationships));
				}
			}
			Record emptyRecord;
			const std::unordered_map<std::string, PropertyValue> emptyParameters;
			const std::unordered_map<std::string, PropertyValue> &parameters =
					state.shared.context == nullptr ? emptyParameters : state.shared.context->parameters;
			const expressions::EvaluationContext evaluationContext(
					emptyRecord, parameters, {.maximum_collection_elements = state.shared.bounds.maximum_rows});
			PropertyValue result = registered->evaluate(arguments, evaluationContext);
			if (result.getType() == PropertyType::LIST && result.getList().size() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error(
						"logical-plan scalar function returned a List exceeding the configured List bound");
			}
			return result;
		}

		[[nodiscard]] PropertyValue evaluateScalarExpression(const expressions::Expression &expression,
															 LogicalPlanReadRow &row, ExecutionState &state,
															 ShortestPathResultMode shortestMode) {
			using expressions::BinaryOperatorType;
			using expressions::ExpressionType;
			switch (expression.getExpressionType()) {
				case ExpressionType::LITERAL:
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS:
				case ExpressionType::PARAMETER:
				case ExpressionType::EXPR_LIST_LITERAL:
					return scalarValue(evaluateExpression(expression, row, state));
				case ExpressionType::BINARY_OP: {
					const auto &binary = dynamic_cast<const expressions::BinaryOpExpression &>(expression);
					const PropertyValue left = scalarValue(evaluateExpression(*binary.getLeft(), row, state));
					MaterializedByteReservation operandBytes(&state.shared.materialized_budget);
					operandBytes.set(retainedExpressionValueBytes(left, state));
					const BinaryOperatorType operation = binary.getOperator();
					if (operation == BinaryOperatorType::BOP_AND || operation == BinaryOperatorType::BOP_OR) {
						const std::optional<bool> leftBoolean = booleanOrNull(left);
						if ((operation == BinaryOperatorType::BOP_AND && leftBoolean == false) ||
							(operation == BinaryOperatorType::BOP_OR && leftBoolean == true)) {
							return PropertyValue(*leftBoolean);
						}
					}
					const PropertyValue right = scalarValue(evaluateExpression(*binary.getRight(), row, state));
					operandBytes.set(checkedSizeAdd(operandBytes.bytes(), retainedExpressionValueBytes(right, state),
													"logical-plan binary operand storage size overflowed"));
					if (expressions::isArithmeticOperator(operation)) {
						return arithmetic(operation, left, right);
					}
					if (operation == BinaryOperatorType::BOP_STARTS_WITH ||
						operation == BinaryOperatorType::BOP_ENDS_WITH ||
						operation == BinaryOperatorType::BOP_CONTAINS ||
						operation == BinaryOperatorType::BOP_REGEX_MATCH) {
						return stringOperation(operation, left, right);
					}
					if (expressions::isComparisonOperator(operation)) {
						return comparison(operation, left, right);
					}
					if (expressions::isLogicalOperator(operation)) {
						return logical(operation, left, right);
					}
					throw std::logic_error("logical-plan runtime does not support this binary operation");
				}
				case ExpressionType::UNARY_OP: {
					const auto &unary = dynamic_cast<const expressions::UnaryOpExpression &>(expression);
					const PropertyValue operand = scalarValue(evaluateExpression(*unary.getOperand(), row, state));
					if (isNull(operand)) {
						return PropertyValue();
					}
					switch (unary.getOperator()) {
						case expressions::UnaryOperatorType::UOP_MINUS: {
							if (const int64_t *integer = std::get_if<int64_t>(&operand.getVariant())) {
								if (*integer == std::numeric_limits<int64_t>::min()) {
									throw std::overflow_error("logical-plan integer negation overflowed");
								}
								return PropertyValue(-*integer);
							}
							if (const double *number = std::get_if<double>(&operand.getVariant())) {
								return PropertyValue(-*number);
							}
							throw std::runtime_error("logical-plan unary minus requires a numeric operand");
						}
						case expressions::UnaryOperatorType::UOP_NOT: {
							const std::optional<bool> predicate = booleanOrNull(operand);
							return makeBoolean(predicate.has_value() ? std::optional<bool>(!*predicate) : std::nullopt);
						}
					}
					throw std::logic_error("logical-plan runtime received an unknown unary operation");
				}
				case ExpressionType::IN_LIST: {
					const auto &in = dynamic_cast<const expressions::InExpression &>(expression);
					const PropertyValue actual = scalarValue(evaluateExpression(*in.getValue(), row, state));
					if (isNull(actual)) {
						return PropertyValue();
					}
					MaterializedByteReservation operandBytes(&state.shared.materialized_budget);
					operandBytes.set(retainedExpressionValueBytes(actual, state));
					if (in.hasDynamicList()) {
						const PropertyValue candidates =
								scalarValue(evaluateExpression(*in.getListExpression(), row, state));
						if (isNull(candidates)) {
							return PropertyValue();
						}
						operandBytes.set(checkedSizeAdd(operandBytes.bytes(),
														retainedExpressionValueBytes(candidates, state),
														"logical-plan IN operand storage size overflowed"));
						bool sawNull = false;
						for (const PropertyValue &candidate: requireBoundedList(candidates, state, "IN")) {
							if (isNull(candidate)) {
								sawNull = true;
								continue;
							}
							if (actual == candidate) {
								return PropertyValue(true);
							}
						}
						return sawNull ? PropertyValue() : PropertyValue(false);
					}
					bool sawNull = false;
					for (const PropertyValue &candidate: in.getListValues()) {
						if (isNull(candidate)) {
							sawNull = true;
							continue;
						}
						if (actual == candidate) {
							return PropertyValue(true);
						}
					}
					return sawNull ? PropertyValue() : PropertyValue(false);
				}
				case ExpressionType::IS_NULL: {
					const auto &isNullExpression = dynamic_cast<const expressions::IsNullExpression &>(expression);
					const bool null =
							isNull(scalarValue(evaluateExpression(*isNullExpression.getExpression(), row, state)));
					return PropertyValue(isNullExpression.isNegated() ? !null : null);
				}
				case ExpressionType::FUNCTION_CALL: {
					return evaluateFunctionExpression(
							dynamic_cast<const expressions::FunctionCallExpression &>(expression), row, state);
				}
				case ExpressionType::CASE_EXPRESSION: {
					const auto &caseExpression = dynamic_cast<const expressions::CaseExpression &>(expression);
					if (caseExpression.isSimpleCase()) {
						const PropertyValue comparisonValue =
								scalarValue(evaluateExpression(*caseExpression.getComparisonExpression(), row, state));
						MaterializedByteReservation comparisonBytes(&state.shared.materialized_budget);
						comparisonBytes.set(retainedExpressionValueBytes(comparisonValue, state));
						for (const expressions::CaseBranch &branch: caseExpression.getBranches()) {
							if (!branch.whenExpression || !branch.thenExpression) {
								throw std::logic_error("logical-plan CASE expression has an incomplete branch");
							}
							const PropertyValue whenValue =
									scalarValue(evaluateExpression(*branch.whenExpression, row, state));
							// Only the current WHEN value coexists with the comparison.
							// Release its charge before evaluating the next branch.
							MaterializedByteReservation whenBytes(&state.shared.materialized_budget);
							whenBytes.set(retainedExpressionValueBytes(whenValue, state));
							if (!isNull(whenValue) && comparisonValue == whenValue) {
								return scalarValue(evaluateExpression(*branch.thenExpression, row, state));
							}
						}
					} else {
						for (const expressions::CaseBranch &branch: caseExpression.getBranches()) {
							if (!branch.whenExpression || !branch.thenExpression) {
								throw std::logic_error("logical-plan CASE expression has an incomplete branch");
							}
							const std::optional<bool> predicate =
									booleanOrNull(scalarValue(evaluateExpression(*branch.whenExpression, row, state)));
							if (predicate == true) {
								return scalarValue(evaluateExpression(*branch.thenExpression, row, state));
							}
						}
					}
					return caseExpression.getElseExpression() == nullptr
								   ? PropertyValue()
								   : scalarValue(evaluateExpression(*caseExpression.getElseExpression(), row, state));
				}
				case ExpressionType::LIST_SLICE: {
					const auto &slice = dynamic_cast<const expressions::ListSliceExpression &>(expression);
					if (slice.getList() == nullptr) {
						throw std::logic_error("logical-plan List slice has no source expression");
					}
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					PropertyValue listValue = scalarValue(evaluateExpression(*slice.getList(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "List slice");
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					if (list.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
						throw std::overflow_error("logical-plan List slice length exceeds signed index capacity");
					}
					const int64_t length = static_cast<int64_t>(list.size());
					const auto evaluateIndex = [&](const expressions::Expression *indexExpression,
												   const char *description) -> int64_t {
						if (indexExpression == nullptr) {
							throw std::logic_error(std::string("logical-plan List slice has no ") + description);
						}
						const PropertyValue value = scalarValue(evaluateExpression(*indexExpression, row, state));
						if (const auto *integer = std::get_if<int64_t>(&value.getVariant())) {
							return *integer;
						}
						throw std::runtime_error("logical-plan List slice index must be an integer");
					};
					const auto normalize = [length](int64_t index) {
						if (index >= 0) {
							return index;
						}
						return index < -length ? std::numeric_limits<int64_t>::min() : length + index;
					};
					if (!slice.hasRange()) {
						const int64_t index = normalize(evaluateIndex(slice.getStart(), "index"));
						if (index < 0 || index >= length)
							return PropertyValue();
						MaterializedByteReservation selectedBytes(&state.shared.materialized_budget);
						selectedBytes.set(retainedExpressionValueBytes(list[static_cast<size_t>(index)], state));
						return list[static_cast<size_t>(index)];
					}
					int64_t start =
							slice.getStart() == nullptr ? 0 : normalize(evaluateIndex(slice.getStart(), "start"));
					int64_t end = slice.getEnd() == nullptr ? length : normalize(evaluateIndex(slice.getEnd(), "end"));
					start = std::clamp(start, int64_t{0}, length);
					end = std::clamp(end, int64_t{0}, length);
					if (start == 0 && end == length)
						return listValue;
					const size_t resultCount = static_cast<size_t>(std::max(int64_t{0}, end - start));
					size_t resultSize =
							checkedSizeAdd(sizeof(uint32_t),
										   checkedSizeMul(resultCount, sizeof(PropertyValue),
														  "logical-plan List slice result capacity overflowed"),
										   "logical-plan List slice result size overflowed");
					for (int64_t index = start; index < end; ++index) {
						resultSize = checkedSizeAdd(
								resultSize, retainedExpressionValueBytes(list[static_cast<size_t>(index)], state),
								"logical-plan List slice result size overflowed");
					}
					MaterializedByteReservation resultBytes(&state.shared.materialized_budget);
					resultBytes.set(resultSize);
					std::vector<PropertyValue> result;
					result.reserve(resultCount);
					resultBytes.set(
							checkedSizeAdd(resultSize,
										   checkedSizeMul(result.capacity() - resultCount, sizeof(PropertyValue),
														  "logical-plan List slice result capacity overflowed"),
										   "logical-plan List slice result size overflowed"));
					for (int64_t index = start; index < end; ++index) {
						result.push_back(list[static_cast<size_t>(index)]);
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::LIST_COMPREHENSION: {
					const auto &comprehension =
							dynamic_cast<const expressions::ListComprehensionExpression &>(expression);
					if (comprehension.getListExpr() == nullptr || comprehension.getVariable().empty()) {
						throw std::logic_error("logical-plan List comprehension has no source or variable");
					}
					const PropertyValue listValue =
							scalarValue(evaluateExpression(*comprehension.getListExpr(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "List comprehension");
					if (comprehension.getType() ==
						expressions::ListComprehensionExpression::ComprehensionType::COMP_REDUCE) {
						throw std::logic_error("logical-plan List comprehension cannot use a reduce mode");
					}
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					MaterializedByteReservation resultBytes(&state.shared.materialized_budget);
					std::vector<PropertyValue> result;
					size_t resultPayloadBytes = sizeof(uint32_t);
					for (const PropertyValue &element: list) {
						ScopedRowBinding binding(row, comprehension.getVariable(), element, state);
						if (comprehension.getWhereExpr() != nullptr) {
							const PropertyValue predicate =
									scalarValue(evaluateExpression(*comprehension.getWhereExpr(), row, state));
							if (!isNull(predicate) && predicate.getType() != PropertyType::BOOLEAN) {
								throw std::runtime_error(
										"logical-plan List comprehension predicate must be Boolean or NULL");
							}
							if (isNull(predicate) || !std::get<bool>(predicate.getVariant())) {
								continue;
							}
						}
						PropertyValue mapped =
								comprehension.getMapExpr() != nullptr
										? scalarValue(evaluateExpression(*comprehension.getMapExpr(), row, state))
										: element;
						const size_t nextPayloadBytes =
								checkedSizeAdd(resultPayloadBytes, retainedExpressionValueBytes(mapped, state),
											   "logical-plan List comprehension result size overflowed");
						if (nextPayloadBytes > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan List comprehension exceeded its materialized byte bound");
						}
						const size_t nextCapacity =
								result.size() == result.capacity()
										? std::min(list.size(),
												   std::max(result.size() + 1U,
															checkedSizeMul(result.capacity(), 2U,
																		   "logical-plan List comprehension result "
																		   "capacity overflowed")))
										: result.capacity();
						resultBytes.set(checkedSizeAdd(
								nextPayloadBytes,
								checkedSizeMul(checkedSizeAdd(nextCapacity,
											 nextCapacity != result.capacity() ? result.capacity() : 0U,
											 "logical-plan List growth slot count overflowed"), sizeof(PropertyValue),
											   "logical-plan List comprehension result capacity overflowed"),
								"logical-plan List comprehension result size overflowed"));
						if (nextCapacity != result.capacity()) {
							result.reserve(nextCapacity);
							resultBytes.set(checkedSizeAdd(
									nextPayloadBytes,
									checkedSizeMul(result.capacity(), sizeof(PropertyValue),
												   "logical-plan List comprehension result capacity overflowed"),
									"logical-plan List comprehension result size overflowed"));
						}
						result.push_back(std::move(mapped));
						resultPayloadBytes = nextPayloadBytes;
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::EXPR_REDUCE: {
					const auto &reduce = dynamic_cast<const expressions::ReduceExpression &>(expression);
					if (reduce.getInitialExpr() == nullptr || reduce.getListExpr() == nullptr ||
						reduce.getBodyExpr() == nullptr || reduce.getAccumulator().empty() ||
						reduce.getVariable().empty()) {
						throw std::logic_error("logical-plan REDUCE expression is incomplete");
					}
					MaterializedByteReservation accumulatorBytes(&state.shared.materialized_budget);
					PropertyValue accumulator = scalarValue(evaluateExpression(*reduce.getInitialExpr(), row, state));
					accumulatorBytes.set(retainedExpressionValueBytes(accumulator, state));
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					const PropertyValue listValue = scalarValue(evaluateExpression(*reduce.getListExpr(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "REDUCE");
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					for (const PropertyValue &element: list) {
						MaterializedByteReservation nextBytes(&state.shared.materialized_budget);
						PropertyValue next;
						{
							ScopedRowBinding accumulatorBinding(row, reduce.getAccumulator(), accumulator, state);
							ScopedRowBinding elementBinding(row, reduce.getVariable(), element, state);
							next = scalarValue(evaluateExpression(*reduce.getBodyExpr(), row, state));
						}
						nextBytes.set(retainedExpressionValueBytes(next, state));
						accumulator = std::move(next);
						accumulatorBytes = std::move(nextBytes);
					}
					return accumulator;
				}
				case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
					const auto &quantifier =
							dynamic_cast<const expressions::QuantifierFunctionExpression &>(expression);
					if (quantifier.getListExpression() == nullptr || quantifier.getWhereExpression() == nullptr ||
						quantifier.getVariable().empty()) {
						throw std::logic_error("logical-plan quantifier expression is incomplete");
					}
					MaterializedByteReservation sourceListBytes(&state.shared.materialized_budget);
					const PropertyValue listValue =
							scalarValue(evaluateExpression(*quantifier.getListExpression(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "quantifier");
					sourceListBytes.set(materializedListStorageBytes(listValue, state));
					const std::string function = lowerCase(quantifier.getFunctionName());
					if (function != "all" && function != "any" && function != "none" && function != "single") {
						throw std::runtime_error("logical-plan quantifier function is not supported");
					}
					size_t matches = 0U;
					bool sawNull = false;
					for (const PropertyValue &element: list) {
						ScopedRowBinding binding(row, quantifier.getVariable(), element, state);
						const PropertyValue predicate =
								scalarValue(evaluateExpression(*quantifier.getWhereExpression(), row, state));
						if (isNull(predicate)) {
							sawNull = true;
							continue;
						}
						if (predicate.getType() != PropertyType::BOOLEAN) {
							throw std::runtime_error("logical-plan quantifier predicate must be Boolean or NULL");
						}
						const bool matched = std::get<bool>(predicate.getVariant());
						if (function == "all" && !matched) {
							return PropertyValue(false);
						}
						if (function == "any" && matched) {
							return PropertyValue(true);
						}
						if (function == "none" && matched) {
							return PropertyValue(false);
						}
						if (function == "single" && matched && ++matches > 1U) {
							return PropertyValue(false);
						}
					}
					if (function == "all" || function == "none") {
						return sawNull ? PropertyValue() : PropertyValue(true);
					}
					if (function == "any") {
						return sawNull ? PropertyValue() : PropertyValue(false);
					}
					return matches == 1U ? PropertyValue(true) : (sawNull ? PropertyValue() : PropertyValue(false));
				}
				case ExpressionType::MAP_PROJECTION: {
					const auto &projection = dynamic_cast<const expressions::MapProjectionExpression &>(expression);
					if (!projection.isMapLiteral() && projection.getVariable().empty()) {
						throw std::logic_error("logical-plan Map projection has an empty variable");
					}
					if (projection.getItems().size() > state.shared.bounds.maximum_property_entries) {
						throw std::runtime_error("logical-plan Map projection exceeded the configured Property bound");
					}
					if (!projection.isMapLiteral()) {
						const LogicalPlanReadValue &binding = requireBinding(row, projection.getVariable());
						if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
							if (value->getType() == PropertyType::NULL_TYPE) {
								return PropertyValue();
							}
							if (value->getType() != PropertyType::MAP) {
								throw std::runtime_error("logical-plan Map projection requires a node, relationship, "
														 "map, or NULL variable");
							}
						}
					}
					MaterializedByteReservation resultBytes(&state.shared.materialized_budget);
					PropertyValue::MapType result;
					size_t resultPayloadBytes = sizeof(uint32_t);
					resultBytes.set(resultPayloadBytes);
					const auto insertResult = [&](const std::string &key, const PropertyValue &value) {
						const auto previous = result.find(key);
						const bool newKey = previous == result.end();
						if (newKey && result.size() == state.shared.bounds.maximum_property_entries) {
							throw std::runtime_error(
									"logical-plan Map projection exceeded the configured Property bound");
						}
						const size_t valueBytes = property_utils::getPropertyValueSize(value);
						size_t nextPayloadBytes = resultPayloadBytes;
						if (newKey) {
							nextPayloadBytes = checkedSizeAdd(
									nextPayloadBytes,
									checkedSizeAdd(checkedSizeAdd(key.size(), sizeof(size_t),
																  "logical-plan Map projection result size overflowed"),
												   valueBytes, "logical-plan Map projection result size overflowed"),
									"logical-plan Map projection result size overflowed");
						} else {
							const size_t previousBytes = property_utils::getPropertyValueSize(previous->second);
							nextPayloadBytes = checkedSizeAdd(nextPayloadBytes - previousBytes, valueBytes,
															  "logical-plan Map projection result size overflowed");
						}
						const size_t nextEntries = result.size() + static_cast<size_t>(newKey);
						const size_t entryBytes =
								checkedSizeMul(nextEntries, sizeof(PropertyValue::MapType::value_type) + sizeof(void *),
											   "logical-plan Map projection result capacity overflowed");
						const size_t nextResultBytes = checkedSizeAdd(
								nextPayloadBytes, entryBytes, "logical-plan Map projection result size overflowed");
						if (newKey) {
							resultBytes.set(nextResultBytes);
							result.insert_or_assign(key, value);
						} else {
							MaterializedByteReservation replacementBytes(&state.shared.materialized_budget);
							replacementBytes.set(valueBytes);
							result.insert_or_assign(key, value);
							replacementBytes.reset();
							resultBytes.set(nextResultBytes);
						}
						resultPayloadBytes = nextPayloadBytes;
					};
					for (const expressions::MapProjectionItem &item: projection.getItems()) {
						switch (item.type) {
							case expressions::MapProjectionItemType::MPROP_PROPERTY: {
								if (projection.isMapLiteral()) {
									throw std::logic_error("logical-plan Map literal has a Property selector");
								}
								if (item.key.empty()) {
									throw std::logic_error("logical-plan Map projection has an empty Property key");
								}
								const PropertyValue selected =
										readProperty(state.shared.snapshot, row, projection.getVariable(), item.key, state);
								MaterializedByteReservation selectedBytes(&state.shared.materialized_budget);
								selectedBytes.set(property_utils::getPropertyValueSize(selected));
								insertResult(item.key, selected);
								break;
							}
							case expressions::MapProjectionItemType::MPROP_LITERAL: {
								if (item.key.empty()) {
									throw std::logic_error("logical-plan Map projection has an empty literal key");
								}
								const PropertyValue literal =
										item.expression == nullptr
												? PropertyValue()
												: scalarValue(evaluateExpression(*item.expression, row, state));
								MaterializedByteReservation literalBytes(&state.shared.materialized_budget);
								literalBytes.set(property_utils::getPropertyValueSize(literal));
								insertResult(item.key, literal);
								break;
							}
							case expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES: {
								if (projection.isMapLiteral()) {
									throw std::logic_error("logical-plan Map literal has an all-Properties selector");
								}
								MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
								const LogicalPlanReadValue &binding = requireBinding(row, projection.getVariable());
								const auto *mapValue = std::get_if<PropertyValue>(&binding);
								const auto cached = row.property_cache.find(projection.getVariable());
								const PropertyMap &properties =
										mapValue != nullptr && cached == row.property_cache.end()
												? mapValue->getMap()
												: readBindingProperties(state.shared.snapshot, row, projection.getVariable(),
																		state);
								sourceBytes.set(propertyMapBytes(properties,
																 "logical-plan Map projection source size overflowed"));
								for (const auto &[key, value]: properties) {
									insertResult(key, value);
								}
								break;
							}
						}
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::EXPR_EXISTS: {
					const auto &exists = dynamic_cast<const expressions::ExistsExpression &>(expression);
					if (!exists.hasStructuredPattern() || exists.getSourceVar().empty()) {
						return PropertyValue(false);
					}
					const auto source = row.values.find(exists.getSourceVar());
					if (source == row.values.end()) {
						return PropertyValue(false);
					}
					const bool hasRelationship = exists.getPattern().find('-') != std::string::npos;
					if (hasRelationship && !exists.hasWhereClause() && exists.getTargetVar().empty() &&
						exists.getTargetLabel().empty()) {
						if (const std::optional<bool> referenceMatch = findPatternReferenceMatch(
									nodeBindingId(source->second, state), exists.getRelType(), exists.getDirection(), state);
							referenceMatch.has_value()) {
							return PropertyValue(*referenceMatch);
						}
					}
					const storage::LogicalGraphNode sourceNode = patternSourceIdentity(state, source->second);
					const auto matchesWhere = [&](const storage::LogicalGraphNode *target) {
						if (target != nullptr && !exists.getTargetVar().empty()) {
							if (const auto bound = row.values.find(exists.getTargetVar());
								bound != row.values.end() && nodeBindingId(bound->second, state) != target->node_id) {
								return false;
							}
							ScopedRowBinding binding(row, exists.getTargetVar(), *target, state);
							return !exists.hasWhereClause() ||
								   booleanOrNull(scalarValue(
										   evaluateExpression(*exists.getWhereExpression(), row, state))) == true;
						}
						return !exists.hasWhereClause() || booleanOrNull(scalarValue(evaluateExpression(
																   *exists.getWhereExpression(), row, state))) == true;
					};
					if (!hasRelationship) {
						return PropertyValue(matchesWhere(nullptr));
					}
					const std::optional<PatternBindingReadShape> patternShape = inspectPatternBindingReadShape(
							exists.getWhereExpression(), nullptr, exists.getTargetVar(), {}, state.shared.snapshot);
					bool matched = false;
					static_cast<void>(visitPatternTargets(
							sourceNode, exists.getRelType(), exists.getTargetLabel(), exists.getDirection(), state,
							[&](const storage::LogicalGraphEdge &, const storage::LogicalGraphNode &target) {
								if (!matchesWhere(&target)) {
									return true;
								}
								matched = true;
								return false;
							},
							patternShape.has_value() ? &*patternShape : nullptr, true));
					return PropertyValue(matched);
				}
				case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
					const auto &comprehension =
							dynamic_cast<const expressions::PatternComprehensionExpression &>(expression);
					if (comprehension.getSourceVar().empty() || comprehension.getMapExpression() == nullptr) {
						throw std::logic_error(
								"logical-plan Pattern comprehension has an incomplete pattern or map expression");
					}
					const auto source = row.values.find(comprehension.getSourceVar());
					if (source == row.values.end()) {
						return PropertyValue(std::vector<PropertyValue>{});
					}
					const storage::LogicalGraphNode sourceNode = patternSourceIdentity(state, source->second);
					MaterializedByteReservation valuesReservation(&state.shared.materialized_budget);
					std::vector<PropertyValue> values;
					size_t valuesBytes = sizeof(uint32_t);
					const auto appendValue = [&](const storage::LogicalGraphEdge *edge,
												 const storage::LogicalGraphNode *target) {
						const auto appendBoundValue = [&]() {
							if (comprehension.hasWhereClause() &&
								booleanOrNull(scalarValue(
										evaluateExpression(*comprehension.getWhereExpression(), row, state))) != true) {
								return true;
							}
							if (values.size() == state.shared.bounds.maximum_rows) {
								throw std::runtime_error(
										"logical-plan Pattern comprehension exceeded its result bound");
							}
							PropertyValue mapped =
									scalarValue(evaluateExpression(*comprehension.getMapExpression(), row, state));
							const size_t mappedBytes = retainedExpressionValueBytes(mapped, state);
							const size_t nextValueBytes =
									checkedSizeAdd(valuesBytes, mappedBytes,
												   "logical-plan Pattern comprehension result size overflowed");
							if (nextValueBytes > state.shared.bounds.maximum_materialized_bytes) {
								throw std::runtime_error(
										"logical-plan Pattern comprehension exceeded its materialized byte bound");
							}
							const size_t nextCapacity =
									values.size() == values.capacity()
											? std::min(state.shared.bounds.maximum_rows,
													   std::max(values.size() + 1U,
																checkedSizeMul(values.capacity(), 2U,
																			   "logical-plan Pattern comprehension "
																			   "capacity overflowed")))
											: values.capacity();
							valuesReservation.set(checkedSizeAdd(
									nextValueBytes,
									checkedSizeMul(checkedSizeAdd(nextCapacity,
												 nextCapacity != values.capacity() ? values.capacity() : 0U,
												 "logical-plan Pattern growth slot count overflowed"), sizeof(PropertyValue),
												   "logical-plan Pattern comprehension capacity overflowed"),
									"logical-plan Pattern comprehension result size overflowed"));
							if (nextCapacity != values.capacity()) {
								values.reserve(nextCapacity);
								valuesReservation.set(checkedSizeAdd(nextValueBytes,
									checkedSizeMul(values.capacity(), sizeof(PropertyValue),
										"logical-plan Pattern comprehension capacity overflowed"),
									"logical-plan Pattern comprehension result size overflowed"));
							}
							values.push_back(std::move(mapped));
							valuesBytes = nextValueBytes;
							return true;
						};
						if (target != nullptr && !comprehension.getTargetVar().empty()) {
							if (const auto bound = row.values.find(comprehension.getTargetVar());
								bound != row.values.end() && nodeBindingId(bound->second, state) != target->node_id) {
								return true;
							}
							ScopedRowBinding targetBinding(row, comprehension.getTargetVar(), *target, state);
							if (edge != nullptr && !comprehension.getRelationshipVar().empty()) {
								ScopedRowBinding relationshipBinding(row, comprehension.getRelationshipVar(), *edge,
																	 state);
								return appendBoundValue();
							}
							return appendBoundValue();
						}
						if (edge != nullptr && !comprehension.getRelationshipVar().empty()) {
							ScopedRowBinding relationshipBinding(row, comprehension.getRelationshipVar(), *edge, state);
							return appendBoundValue();
						}
						return appendBoundValue();
					};
					const bool hasRelationship = comprehension.getPattern().find('-') != std::string::npos;
					const auto finishValues = [&]() {
						PropertyValue result(std::move(values));
						// Confirm that the incremental charge matches the returned value.
						if (property_utils::getPropertyValueSize(result) > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan Pattern comprehension exceeded its materialized byte bound");
						}
						return result;
					};
					if (!hasRelationship) {
						static_cast<void>(appendValue(nullptr, nullptr));
						return finishValues();
					}
					const std::optional<PatternBindingReadShape> patternShape = inspectPatternBindingReadShape(
							comprehension.getWhereExpression(), comprehension.getMapExpression(),
							comprehension.getTargetVar(), comprehension.getRelationshipVar(), state.shared.snapshot);
					static_cast<void>(visitPatternTargets(
							sourceNode, comprehension.getRelType(), comprehension.getTargetLabel(),
							comprehension.getDirection(), state,
							[&](const storage::LogicalGraphEdge &edge, const storage::LogicalGraphNode &target) {
								return appendValue(&edge, &target);
							},
							patternShape.has_value() ? &*patternShape : nullptr));
					return finishValues();
				}
				case ExpressionType::SHORTEST_PATH: {
					const auto &shortest = dynamic_cast<const expressions::ShortestPathExpression &>(expression);
					if (shortest.getStartVar().empty() || shortest.getEndVar().empty() || shortest.getMinHops() < 0 ||
						shortest.getMaxHops() < shortest.getMinHops()) {
						throw std::invalid_argument("logical-plan shortestPath has an invalid pattern or hop range");
					}
					if (shortest.getDirection() != expressions::PatternDirection::PAT_OUTGOING &&
						shortest.getDirection() != expressions::PatternDirection::PAT_INCOMING &&
						shortest.getDirection() != expressions::PatternDirection::PAT_BOTH) {
						throw std::invalid_argument("logical-plan shortestPath has an unknown direction");
					}
					const auto startBinding = row.values.find(shortest.getStartVar());
					const auto endBinding = row.values.find(shortest.getEndVar());
					if (startBinding == row.values.end() || endBinding == row.values.end()) {
						return PropertyValue();
					}
					// Shortest-path expansion is topology-only until a path is emitted. Resolve
					// stable logical IDs directly so a disconnected pair does not decode either
					// endpoint's labels or Properties merely to seed the BFS. Authenticate both
					// deferred endpoints with one sorted probe when the backend supports it;
					// readers without that capability retain the complete-read fallback.
					const uint64_t startNodeId = nodeBindingId(startBinding->second, state);
					const uint64_t endNodeId = nodeBindingId(endBinding->second, state);
					if (state.shared.snapshot.supportsNodeExistenceProbes()) {
						std::vector<uint64_t> endpointIds;
						if (std::holds_alternative<storage::EntityRef>(startBinding->second)) {
							endpointIds.push_back(startNodeId);
						}
						if (std::holds_alternative<storage::EntityRef>(endBinding->second)) {
							endpointIds.push_back(endNodeId);
						}
						std::ranges::sort(endpointIds);
						endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
						if (!endpointIds.empty()) {
							const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(endpointIds);
							if (present.size() != endpointIds.size()) {
								throw std::runtime_error(
										"logical graph shortestPath endpoint probe cardinality changed");
							}
							for (size_t index = 0U; index < endpointIds.size(); ++index) {
								if (!present[index]) {
									throw std::runtime_error("logical graph shortestPath endpoint Node does not exist");
								}
							}
						}
					} else {
						if (std::holds_alternative<storage::EntityRef>(startBinding->second)) {
							validateNodeIdentity(state, startNodeId);
						}
						if (std::holds_alternative<storage::EntityRef>(endBinding->second)) {
							validateNodeIdentity(state, endNodeId);
						}
					}
					int maximumHops = shortest.getMaxHops();
					if (state.shared.context != nullptr && state.shared.context->maxVarLengthDepth.has_value()) {
						if (*state.shared.context->maxVarLengthDepth < 0) {
							throw std::invalid_argument("logical-plan shortestPath has a negative query depth bound");
						}
						maximumHops = std::min(maximumHops, *state.shared.context->maxVarLengthDepth);
					}
					if (maximumHops < shortest.getMinHops()) {
						return PropertyValue();
					}
					if (static_cast<uint64_t>(maximumHops) > state.shared.bounds.maximum_node_records) {
						throw std::runtime_error("logical-plan shortestPath exceeds its Node-depth bound");
					}

					struct Parent {
						uint64_t node_id = 0U;
						storage::LogicalGraphEdgeTopology edge;
					};
					// The lease outlives the BFS containers it represents.
					MaterializedByteReservation shortestPathStateBytes(&state.shared.materialized_budget);
					std::unordered_map<uint64_t, int> distances;
					std::unordered_map<uint64_t, std::vector<Parent>> parents;
					std::deque<uint64_t> frontier;
					shortestPathStateBytes.set(sizeof(uint64_t) + sizeof(int) + sizeof(uint64_t));
					distances.emplace(startNodeId, 0);
					frontier.push_back(startNodeId);
					size_t parentEntries = 0U;
					std::optional<int> targetDistance = startNodeId == endNodeId ? std::optional<int>{0} : std::nullopt;
					size_t exploredEdges = 0U;
					const size_t maximumExploredEdges = [&]() {
						if (state.shared.bounds.maximum_node_records >
							std::numeric_limits<size_t>::max() / state.shared.bounds.maximum_edges_per_node) {
							throw std::overflow_error("logical-plan shortestPath Edge budget overflowed");
						}
						return state.shared.bounds.maximum_node_records * state.shared.bounds.maximum_edges_per_node;
					}();
					const auto chargeShortestPathState = [&](size_t bytes) {
						const size_t remaining =
								state.shared.bounds.maximum_materialized_bytes -
								std::min(shortestPathStateBytes.bytes(), state.shared.bounds.maximum_materialized_bytes);
						if (bytes > remaining) {
							throw std::runtime_error(
									"logical-plan shortestPath exceeded its materialized state byte bound");
						}
						shortestPathStateBytes.set(checkedSizeAdd(shortestPathStateBytes.bytes(), bytes,
																  "logical-plan shortestPath state size overflowed"));
					};
					const bool supportsExistenceProbes = state.shared.snapshot.supportsNodeExistenceProbes();
					std::optional<uint64_t> relationshipTypeToken;
					if (!shortest.getRelType().empty() &&
						state.shared.snapshot.supportsNativeRelationshipTypeTokens(shortest.getRelType())) {
						relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(shortest.getRelType());
						if (!relationshipTypeToken.has_value()) {
							return PropertyValue();
						}
					}
					const bool useNativeAdjacencyRefs =
							state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
							(shortest.getRelType().empty() || relationshipTypeToken.has_value());
					const auto visitAdjacent = [&](uint64_t nodeId, const auto &onEdge) {
						size_t sourceEdges = 0U;
						const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
							std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
							uint64_t previousEdgeId = 0U;
							for (;;) {
								if (exploredEdges == maximumExploredEdges ||
									sourceEdges == state.shared.bounds.maximum_edges_per_node) {
									throw std::runtime_error("logical-plan shortestPath exceeded its total Edge bound");
								}
								const size_t perNodeRemaining = state.shared.bounds.maximum_edges_per_node - sourceEdges;
								const size_t pageRecords =
										std::min(perNodeRemaining, state.shared.bounds.maximum_adjacency_page_records);
								MaterializedByteReservation adjacencyPageBytes(&state.shared.materialized_budget);
								std::vector<storage::LogicalGraphEdgeTopology> pageEdges;
								std::optional<storage::LogicalGraphAdjacencyContinuation> pageContinuation;
								if (useNativeAdjacencyRefs) {
									MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
									const auto page = readReferencePageBudgeted(state, nodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "shortestPath adjacency reference page");
									}
									if (continuation.has_value() &&
										(page.edges.empty() ||
										 page.edges.front().entity_id != continuation->next_edge_id)) {
										throw std::runtime_error("logical graph backend returned a discontinuous "
																 "shortestPath adjacency reference page");
									}
									// Reference source buffers have their own reservation. Charge only
									// the projected topology here, including every retained type capacity.
									const size_t typeCapacity = relationshipTypeToken.has_value()
										? std::max(std::string{}.capacity(), shortest.getRelType().size()) : std::string{}.capacity();
									size_t typeBytes = 0U;
									for (const auto &reference: page.edges) {
										typeBytes = checkedSizeAdd(typeBytes,
											relationshipTypeToken && reference.type_token == *relationshipTypeToken
												? typeCapacity : std::string{}.capacity(),
											"logical-plan shortestPath projected type capacity overflowed");
									}
									const auto projectedBytes = [&](size_t capacity, size_t names) {
										return checkedSizeAdd(checkedSizeMul(capacity, sizeof(storage::LogicalGraphEdgeTopology),
											"logical-plan shortestPath projected capacity overflowed"), names,
											"logical-plan shortestPath projected names overflowed");
									};
									adjacencyPageBytes.set(projectedBytes(page.edges.size(), typeBytes));
									pageEdges.reserve(page.edges.size());
									adjacencyPageBytes.set(projectedBytes(pageEdges.capacity(), typeBytes));
									size_t actualTypeBytes = 0U;
									size_t remainingTypeBytes = typeBytes;
									for (const storage::EntityRef &reference: page.edges) {
										validateEdgeRef(reference, "logical graph backend returned an invalid or Node "
																   "reference from a shortestPath adjacency page");
										const bool typeMatches = !relationshipTypeToken.has_value() ||
																 reference.type_token == *relationshipTypeToken;
										pageEdges.push_back({reference.entity_id, reference.source_node_id,
															 reference.target_node_id,
															 typeMatches && relationshipTypeToken.has_value()
																	 ? shortest.getRelType()
																	 : std::string{}});
										actualTypeBytes = checkedSizeAdd(actualTypeBytes, pageEdges.back().type.capacity(),
											"logical-plan shortestPath actual type capacity overflowed");
										remainingTypeBytes -= typeMatches && relationshipTypeToken ? typeCapacity : std::string{}.capacity();
										adjacencyPageBytes.set(projectedBytes(pageEdges.capacity(), checkedSizeAdd(actualTypeBytes,
											remainingTypeBytes, "logical-plan shortestPath total type capacity overflowed")));
									}
									pageContinuation = page.continuation;
								} else {
									auto page = readTopologyPageBudgeted(state, nodeId, adjacencyDirection,
										continuation, pageRecords, adjacencyPageBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "shortestPath adjacency page");
									}
									if (continuation.has_value() &&
										(page.edges.empty() ||
										 page.edges.front().edge_id != continuation->next_edge_id)) {
										throw std::runtime_error("logical graph backend returned a discontinuous "
																 "shortestPath adjacency page");
									}

									pageEdges = std::move(page.edges);
									pageContinuation = page.continuation;
								}
								if (pageEdges.size() > pageRecords ||
									(pageEdges.empty() && pageContinuation.has_value())) {
									throw std::runtime_error(
											"logical graph backend returned an invalid shortestPath adjacency page");
								}
								if (continuation.has_value() &&
									(pageEdges.empty() || pageEdges.front().edge_id != continuation->next_edge_id)) {
									throw std::runtime_error("logical graph backend returned a discontinuous "
															 "shortestPath adjacency page");
								}
								// Endpoint validation is independent of edge traversal order. Resolve
								// each distinct endpoint once per page so parallel edges and repeated
								// targets do not repeat an Entity Directory descent.
								std::vector<uint64_t> endpointIds;
								if (supportsExistenceProbes) {
									endpointIds.reserve(pageEdges.size());
									for (const storage::LogicalGraphEdgeTopology &candidate: pageEdges) {
										endpointIds.push_back(
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? candidate.target_node_id
														: candidate.source_node_id);
									}
									std::ranges::sort(endpointIds);
									endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()),
													  endpointIds.end());
								}
								const size_t endpointIdBytes =
										checkedSizeMul(endpointIds.capacity(), sizeof(uint64_t),
													   "logical-plan shortestPath endpoint probe size overflowed");
								// Pre-admit one machine word per result bit. This upper estimate
								// covers packed-vector word rounding before the backend allocates it.
								const size_t expectedPresenceBytes =
										checkedSizeMul(endpointIds.size(), sizeof(uint64_t),
													   "logical-plan shortestPath endpoint probe size overflowed");
								const size_t endpointProbeBytes =
										checkedSizeAdd(endpointIdBytes, expectedPresenceBytes,
													   "logical-plan shortestPath endpoint probe size overflowed");
								adjacencyPageBytes.set(
										checkedSizeAdd(adjacencyPageBytes.bytes(), endpointProbeBytes,
													   "logical-plan shortestPath adjacency page size overflowed"));
								const std::vector<bool> endpointPresent =
										supportsExistenceProbes ? state.shared.snapshot.containsNodesSorted(endpointIds)
																: std::vector<bool>{};
								if (supportsExistenceProbes) {
									const size_t actualPresenceBytes =
											checkedSizeAdd(endpointPresent.capacity(), 7U,
														   "logical-plan shortestPath endpoint probe size overflowed") /
											8U;
									adjacencyPageBytes.set(checkedSizeAdd(
											adjacencyPageBytes.bytes() - expectedPresenceBytes, actualPresenceBytes,
											"logical-plan shortestPath adjacency page size overflowed"));
								}
								for (const storage::LogicalGraphEdgeTopology &edge: pageEdges) {
									checkGuard(state);
									validateEdge(edge);
									validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "shortestPath");
									const bool endpointMatches =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.source_node_id == nodeId
													: edge.target_node_id == nodeId;
									if (!endpointMatches) {
										throw std::runtime_error(
												"logical graph backend returned an invalid shortestPath Edge");
									}
									incrementBounded(exploredEdges,
													 "logical-plan shortestPath Edge counter overflowed");
									incrementBounded(sourceEdges,
													 "logical-plan shortestPath per-Node Edge counter overflowed");
									incrementBounded(state.task.stats.edges_scanned,
													 "logical-plan shortestPath runtime Edge counter overflowed");
									if (shortest.getDirection() == expressions::PatternDirection::PAT_BOTH &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										edge.source_node_id == nodeId) {
										continue;
									}
									if (!shortest.getRelType().empty() && edge.type != shortest.getRelType()) {
										continue;
									}
									const uint64_t next =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
									// A native probe authenticates the endpoint in the same pinned
									// snapshot without materializing labels or Properties for every
									// BFS frontier candidate. Readers without that stronger contract
									// authenticate identity with a shaped point read.
									if (supportsExistenceProbes) {
										const auto endpoint = std::ranges::lower_bound(endpointIds, next);
										if (endpoint == endpointIds.end() || *endpoint != next ||
											!endpointPresent[static_cast<size_t>(endpoint - endpointIds.begin())]) {
											throw std::runtime_error("logical graph backend returned a shortestPath "
																	 "Edge with a missing endpoint");
										}
									} else {
										validateNodeIdentity(state, next);
									}
									if (!onEdge(edge, next)) {
										return false;
									}
								}
								if (!pageContinuation.has_value()) {
									return true;
								}
								if (sourceEdges == state.shared.bounds.maximum_edges_per_node ||
									pageContinuation->previous_edge_id != pageEdges.back().edge_id ||
									pageContinuation->next_edge_id == 0U ||
									pageContinuation->previous_edge_id == pageContinuation->next_edge_id) {
									throw std::runtime_error(
											"logical graph backend returned an invalid shortestPath continuation");
								}
								continuation = pageContinuation;
							}
						};
						if ((shortest.getDirection() == expressions::PatternDirection::PAT_OUTGOING ||
							 shortest.getDirection() == expressions::PatternDirection::PAT_BOTH) &&
							!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
							return false;
						}
						if (shortest.getDirection() == expressions::PatternDirection::PAT_INCOMING ||
							shortest.getDirection() == expressions::PatternDirection::PAT_BOTH) {
							return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
						}
						return true;
					};

					while (!frontier.empty()) {
						checkGuard(state);
						const uint64_t current = frontier.front();
						frontier.pop_front();
						const int currentDistance = distances.at(current);
						if (targetDistance.has_value() && currentDistance >= *targetDistance) {
							continue;
						}
						if (currentDistance == maximumHops) {
							continue;
						}
						static_cast<void>(visitAdjacent(current, [&](const storage::LogicalGraphEdgeTopology &edge,
																	 uint64_t next) {
							const int nextDistance = currentDistance + 1;
							if (nextDistance > maximumHops) {
								return true;
							}
							// A target reached before the requested lower hop bound cannot
							// contribute to a simple path. Do not let that too-short route
							// suppress a later valid route to the same target.
							if (next == endNodeId && nextDistance < shortest.getMinHops()) {
								return true;
							}
							const auto existing = distances.find(next);
							if (existing == distances.end()) {
								if (distances.size() == state.shared.bounds.maximum_node_records) {
									throw std::runtime_error(
											"logical-plan shortestPath exceeded its frontier Node bound");
								}
								chargeShortestPathState(sizeof(uint64_t) + sizeof(int) + sizeof(void *) +
														sizeof(uint64_t));
								distances.emplace(next, nextDistance);
								frontier.push_back(next);
								if (parentEntries == state.shared.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								chargeShortestPathState(
										checkedSizeAdd(sizeof(Parent), edge.type.size(),
													   "logical-plan shortestPath parent-DAG size overflowed"));
								parents[next].push_back({current, edge});
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							} else if (existing->second == nextDistance) {
								if (parentEntries == state.shared.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								chargeShortestPathState(
										checkedSizeAdd(sizeof(Parent), edge.type.size(),
													   "logical-plan shortestPath parent-DAG size overflowed"));
								parents[next].push_back({current, edge});
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							}
							if (next == endNodeId) {
								targetDistance = nextDistance;
							}
							return true;
						}));
					}
					if (!targetDistance.has_value() || *targetDistance < shortest.getMinHops()) {
						return PropertyValue();
					}
					for (auto &[_, entries]: parents) {
						std::ranges::sort(entries, {}, [](const Parent &parent) {
							return std::pair{parent.node_id, parent.edge.edge_id};
						});
					}

					if (shortestMode != ShortestPathResultMode::SPR_FULL) {
						if (!shortest.isAll()) {
							const size_t elements =
									checkedSizeAdd(checkedSizeMul(static_cast<size_t>(*targetDistance), size_t{2U},
																  "logical-plan shortestPath size overflowed"),
												   size_t{1U}, "logical-plan shortestPath size overflowed");
							if (elements > state.shared.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan scalar function received a List exceeding the "
														 "configured List bound");
							}
							if (elements > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
								throw std::overflow_error(
										"logical-plan shortestPath size exceeds integer result range");
							}
							if (shortestMode == ShortestPathResultMode::SPR_LENGTH) {
								const auto markerMatches = [&](uint64_t id, bool edge) {
									checkGuard(state);
									const auto present = edge ? state.shared.snapshot.edgePropertyExists(id, "_type")
															  : state.shared.snapshot.nodePropertyExists(id, "_type");
									if (present.has_value() && !*present)
										return true;
									PropertyValue marker;
									if (present.has_value()) {
										marker = readSelectedPropertyBudgeted(state.shared.snapshot, id, edge, "_type", state);
									} else {
										// Unsupported presence probes retain the complete reader
										// fallback and distinguish a missing key from explicit NULL.
										const auto entries = edge ? readEdgeCached(state, id).properties
																  : readNodeCached(state, id).properties;
										const auto field = std::ranges::find(entries, "_type",
																			 &storage::LogicalGraphPropertyEntry::key);
										if (field == entries.end())
											return true;
										marker = field->value;
									}
									if (property_utils::getPropertyValueSize(marker) >
										state.shared.bounds.maximum_materialized_bytes) {
										throw std::runtime_error(
												"logical-plan shortestPath marker exceeded its materialized bound");
									}
									return marker.getType() == PropertyType::STRING &&
										   std::get<std::string>(marker.getVariant()) ==
												   (edge ? "relationship" : "node");
								};
								uint64_t current = endNodeId;
								for (;;) {
									if (!markerMatches(current, false))
										return PropertyValue(static_cast<int64_t>(elements));
									if (current == startNodeId)
										break;
									const auto previous = parents.find(current);
									if (previous == parents.end() || previous->second.empty()) {
										throw std::runtime_error(
												"logical-plan shortestPath has an incomplete parent DAG");
									}
									const Parent &parent = previous->second.front();
									if (!markerMatches(parent.edge.edge_id, true))
										return PropertyValue(static_cast<int64_t>(elements));
									current = parent.node_id;
								}
								return PropertyValue(static_cast<int64_t>(*targetDistance));
							}
							return PropertyValue(static_cast<int64_t>(elements));
						}
						// Count paths in the parent DAG without constructing path lists or
						// entity maps. Parallel edges retain their path multiplicity.
						std::unordered_map<uint64_t, size_t> pathCounts;
						std::function<size_t(uint64_t)> countPaths = [&](uint64_t id) -> size_t {
							checkGuard(state);
							if (id == startNodeId)
								return 1U;
							if (const auto cached = pathCounts.find(id); cached != pathCounts.end())
								return cached->second;
							const auto previous = parents.find(id);
							if (previous == parents.end() || previous->second.empty()) {
								throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
							}
							size_t count = 0U;
							for (const Parent &parent: previous->second) {
								const size_t contribution = countPaths(parent.node_id);
								if (contribution > state.shared.bounds.maximum_rows - count) {
									throw std::runtime_error(
											"logical-plan shortestPath exceeded its result-path bound");
								}
								count += contribution;
							}
							chargeShortestPathState(sizeof(std::pair<const uint64_t, size_t>));
							pathCounts.emplace(id, count);
							return count;
						};
						const size_t count = countPaths(endNodeId);
						if (count > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
							throw std::overflow_error("logical-plan shortestPath count exceeds integer result range");
						}
						return PropertyValue(static_cast<int64_t>(count));
					}

					const auto idValue = [](uint64_t id) {
						if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							throw std::overflow_error(
									"logical-plan shortestPath cannot represent an entity ID in a Property value");
						}
						return PropertyValue(static_cast<int64_t>(id));
					};
					const auto materializePath = [&](const std::vector<uint64_t> &nodeIds,
													 const std::vector<storage::LogicalGraphEdgeTopology> &edges,
													 MaterializedByteReservation &outputBytes) {
						const size_t pathElements = checkedSizeAdd(nodeIds.size(), edges.size(),
																   "logical-plan shortestPath result size overflowed");
						outputBytes.set(
								checkedSizeAdd(sizeof(uint32_t),
											   checkedSizeMul(pathElements, sizeof(PropertyValue),
															  "logical-plan shortestPath result size overflowed"),
											   "logical-plan shortestPath result size overflowed"));
						std::vector<PropertyValue> path;
						path.reserve(pathElements);
						const auto appendEntity = [&](const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													  std::string_view type, uint64_t id) {
							if (entries.size() > state.shared.bounds.maximum_property_entries) {
								throw std::runtime_error(
										"logical graph backend returned too many direct Property entries");
							}
							incrementBounded(state.task.stats.property_sets_materialized,
											 "logical-plan Property materialization counter overflowed");
							MaterializedByteReservation keyBytes(&state.shared.materialized_budget);
							keyBytes.set(checkedSizeMul(entries.size(), sizeof(std::string_view),
														"logical-plan shortestPath Property key size overflowed"));
							std::vector<std::string_view> keys;
							keys.reserve(entries.size());
							size_t mapBytes = sizeof(uint32_t);
							bool hasType = false;
							bool hasId = false;
							for (const auto &entry: entries) {
								checkGuard(state);
								if (entry.key.empty()) {
									throw std::runtime_error(
											"logical graph backend returned an invalid direct Property entry set");
								}
								keys.push_back(entry.key);
								hasType |= entry.key == "_type";
								hasId |= entry.key == "_id";
								mapBytes = checkedSizeAdd(
										mapBytes,
										checkedSizeAdd(
												checkedSizeAdd(entry.key.size(), sizeof(size_t),
															   "logical-plan shortestPath result size overflowed"),
												property_utils::getPropertyValueSize(entry.value),
												"logical-plan shortestPath result size overflowed"),
										"logical-plan shortestPath result size overflowed");
							}
							std::ranges::sort(keys);
							if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
								throw std::runtime_error(
										"logical graph backend returned an invalid direct Property entry set");
							}
							std::vector<std::string_view>().swap(keys);
							keyBytes.reset();
							if (!hasType) {
								mapBytes = checkedSizeAdd(mapBytes, sizeof(size_t) + 5U + sizeof(size_t) + type.size(),
														  "logical-plan shortestPath result size overflowed");
							}
							if (!hasId) {
								mapBytes = checkedSizeAdd(mapBytes, sizeof(size_t) + 3U + sizeof(int64_t),
														  "logical-plan shortestPath result size overflowed");
							}
							outputBytes.set(checkedSizeAdd(outputBytes.bytes(), mapBytes,
														   "logical-plan shortestPath result size overflowed"));
							PropertyValue::MapType entityMap;
							entityMap.reserve(checkedSizeAdd(entries.size(), 2U,
															 "logical-plan shortestPath Property count overflowed"));
							entityMap.emplace("_type", PropertyValue(std::string(type)));
							entityMap.emplace("_id", idValue(id));
							for (const auto &entry: entries) {
								entityMap.insert_or_assign(entry.key, entry.value);
							}
							path.emplace_back(std::move(entityMap));
						};
						MaterializedByteReservation orderedNodeBytes(&state.shared.materialized_budget);
						orderedNodeBytes.set(
								checkedSizeMul(nodeIds.size(), sizeof(uint64_t),
											   "logical-plan shortestPath ordered Node ID size overflowed"));
						std::vector<uint64_t> orderedNodeIds = nodeIds;
						std::ranges::sort(orderedNodeIds);
						orderedNodeIds.erase(std::unique(orderedNodeIds.begin(), orderedNodeIds.end()),
											 orderedNodeIds.end());
						MaterializedByteReservation nodeRecordBytes(&state.shared.materialized_budget);
						nodeRecordBytes.set(checkedSizeMul(orderedNodeIds.size(), sizeof(storage::LogicalGraphNode),
														   "logical-plan shortestPath retained Node size overflowed"));
						std::vector<storage::LogicalGraphNode> nodeRecords;
						nodeRecords.reserve(orderedNodeIds.size());
						nodeRecordBytes.set(checkedSizeMul(nodeRecords.capacity(), sizeof(storage::LogicalGraphNode),
														   "logical-plan shortestPath retained Node size overflowed"));
						for (size_t offset = 0U; offset < orderedNodeIds.size();
							 offset += DEFAULT_PROPERTY_BATCH_ROWS) {
							const size_t count = std::min(DEFAULT_PROPERTY_BATCH_ROWS, orderedNodeIds.size() - offset);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							auto batch = readNodesShapedSortedBudgeted(
									state, std::span<const uint64_t>(orderedNodeIds).subspan(offset, count),
									completeNodeReadShape(), &sourceBytes);
							if (batch.size() != count) {
								throw std::runtime_error(
										"logical-plan shortestPath Node batch changed its cardinality");
							}
							for (size_t index = 0U; index < count; ++index) {
								validateNode(batch[index], state);
								if (batch[index].node_id != orderedNodeIds[offset + index]) {
									throw std::runtime_error(
											"logical-plan shortestPath Node batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							for (auto &node: batch) {
								const size_t heapBytes = logicalNodeCacheBytes(node, state.shared.context) - sizeof(node);
								sourceBytes.set(sourceBytes.bytes() - heapBytes);
								nodeRecordBytes.set(
										checkedSizeAdd(nodeRecordBytes.bytes(), heapBytes,
													   "logical-plan shortestPath retained Node size overflowed"));
								nodeRecords.push_back(std::move(node));
							}
						}
						std::vector<uint64_t>().swap(orderedNodeIds);
						orderedNodeBytes.reset();
						std::vector<uint64_t> orderedEdgeIds;
						MaterializedByteReservation orderedEdgeBytes(&state.shared.materialized_budget);
						orderedEdgeBytes.set(
								checkedSizeMul(edges.size(), sizeof(uint64_t),
											   "logical-plan shortestPath ordered Edge ID size overflowed"));
						orderedEdgeIds.reserve(edges.size());
						for (const auto &edge: edges) {
							orderedEdgeIds.push_back(edge.edge_id);
						}
						std::ranges::sort(orderedEdgeIds);
						orderedEdgeIds.erase(std::unique(orderedEdgeIds.begin(), orderedEdgeIds.end()),
											 orderedEdgeIds.end());
						MaterializedByteReservation edgeRecordBytes(&state.shared.materialized_budget);
						edgeRecordBytes.set(checkedSizeMul(orderedEdgeIds.size(), sizeof(storage::LogicalGraphEdge),
														   "logical-plan shortestPath retained Edge size overflowed"));
						std::vector<storage::LogicalGraphEdge> edgeRecords;
						edgeRecords.reserve(orderedEdgeIds.size());
						edgeRecordBytes.set(checkedSizeMul(edgeRecords.capacity(), sizeof(storage::LogicalGraphEdge),
														   "logical-plan shortestPath retained Edge size overflowed"));
						for (size_t offset = 0U; offset < orderedEdgeIds.size();
							 offset += DEFAULT_PROPERTY_BATCH_ROWS) {
							const size_t count = std::min(DEFAULT_PROPERTY_BATCH_ROWS, orderedEdgeIds.size() - offset);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							auto batch = readEdgesShapedSortedBudgeted(
									state, std::span<const uint64_t>(orderedEdgeIds).subspan(offset, count),
									completeEdgeReadShape(), &sourceBytes);
							if (batch.size() != count) {
								throw std::runtime_error(
										"logical-plan shortestPath Edge batch changed its cardinality");
							}
							for (size_t index = 0U; index < count; ++index) {
								validateEdge(batch[index]);
								if (batch[index].edge_id != orderedEdgeIds[offset + index]) {
									throw std::runtime_error(
											"logical-plan shortestPath Edge batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							for (auto &edge: batch) {
								const size_t heapBytes = logicalEdgeCacheBytes(edge, state.shared.context) - sizeof(edge);
								sourceBytes.set(sourceBytes.bytes() - heapBytes);
								edgeRecordBytes.set(
										checkedSizeAdd(edgeRecordBytes.bytes(), heapBytes,
													   "logical-plan shortestPath retained Edge size overflowed"));
								edgeRecords.push_back(std::move(edge));
							}
						}
						std::vector<uint64_t>().swap(orderedEdgeIds);
						orderedEdgeBytes.reset();
						for (size_t index = 0U; index < nodeIds.size(); ++index) {
							const auto nodeIt = std::ranges::lower_bound(nodeRecords, nodeIds[index], {},
																		 &storage::LogicalGraphNode::node_id);
							if (nodeIt == nodeRecords.end() || nodeIt->node_id != nodeIds[index]) {
								throw std::runtime_error("logical-plan shortestPath Node batch lost a path Node");
							}
							const storage::LogicalGraphNode &node = *nodeIt;
							validateNode(node, state);
							appendEntity(node.properties, "node", node.node_id);
							if (index == edges.size()) {
								continue;
							}
							const storage::LogicalGraphEdgeTopology &topology = edges[index];
							const auto edgeIt = std::ranges::lower_bound(edgeRecords, topology.edge_id, {},
																		 &storage::LogicalGraphEdge::edge_id);
							if (edgeIt == edgeRecords.end() || edgeIt->edge_id != topology.edge_id) {
								throw std::runtime_error("logical-plan shortestPath Edge batch lost a path Edge");
							}
							const storage::LogicalGraphEdge &edge = *edgeIt;
							validateEdge(edge);
							// A native untyped adjacency reference carries no relationship name.
							// Its empty topology type is therefore an unknown marker, not a claim
							// that the persisted Edge type is empty.
							if (edge.source_node_id != topology.source_node_id ||
								edge.target_node_id != topology.target_node_id ||
								(!topology.type.empty() && edge.type != topology.type)) {
								throw std::runtime_error("logical graph backend changed an Edge while materializing a "
														 "pinned shortestPath result");
							}
							appendEntity(edge.properties, "relationship", edge.edge_id);
						}
						return PropertyValue(std::move(path));
					};

					MaterializedByteReservation allPathReservation(&state.shared.materialized_budget);
					std::vector<PropertyValue> allPaths;
					size_t allPathBytes = 0U;
					MaterializedByteReservation reversedPathBytes(&state.shared.materialized_budget);
					reversedPathBytes.set(sizeof(uint64_t));
					std::vector<uint64_t> reversedNodes;
					reversedNodes.reserve(1U);
					reversedNodes.push_back(endNodeId);
					std::vector<storage::LogicalGraphEdgeTopology> reversedEdges;
					size_t reversedTypeBytes = 0U;
					const auto reversedStorageBytes = [&](size_t nodeCapacity, size_t edgeCapacity, size_t typeBytes) {
						return checkedSizeAdd(
								checkedSizeAdd(checkedSizeMul(nodeCapacity, sizeof(uint64_t),
															  "logical-plan shortestPath reverse Node size overflowed"),
											   checkedSizeMul(edgeCapacity, sizeof(storage::LogicalGraphEdgeTopology),
															  "logical-plan shortestPath reverse Edge size overflowed"),
											   "logical-plan shortestPath reverse path size overflowed"),
								typeBytes, "logical-plan shortestPath reverse path size overflowed");
					};
					std::function<void(uint64_t)> collectPaths = [&](uint64_t nodeId) {
						if (allPaths.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan shortestPath exceeded its result-path bound");
						}
						if (nodeId == startNodeId) {
							MaterializedByteReservation forwardPathBytes(&state.shared.materialized_budget);
							size_t forwardBytes = checkedSizeAdd(
									checkedSizeMul(reversedNodes.size(), sizeof(uint64_t),
												   "logical-plan shortestPath forward Node size overflowed"),
									checkedSizeMul(reversedEdges.size(), sizeof(storage::LogicalGraphEdgeTopology),
												   "logical-plan shortestPath forward Edge size overflowed"),
									"logical-plan shortestPath forward path size overflowed");
							for (const auto &edge: reversedEdges) {
								forwardBytes = checkedSizeAdd(forwardBytes, edge.type.size(),
															  "logical-plan shortestPath forward path size overflowed");
							}
							forwardPathBytes.set(forwardBytes);
							std::vector<uint64_t> nodes(reversedNodes.rbegin(), reversedNodes.rend());
							std::vector<storage::LogicalGraphEdgeTopology> edges(reversedEdges.rbegin(),
																				 reversedEdges.rend());
							MaterializedByteReservation pendingPathBytes(&state.shared.materialized_budget);
							PropertyValue path = materializePath(nodes, edges, pendingPathBytes);
							const size_t pathBytes = property_utils::getPropertyValueSize(path);
							if (pathBytes > state.shared.bounds.maximum_materialized_bytes ||
								allPathBytes > state.shared.bounds.maximum_materialized_bytes - pathBytes) {
								throw std::runtime_error(
										"logical-plan shortestPath exceeded its materialized byte bound");
							}
							const size_t nextPathBytes = checkedSizeAdd(
									allPathBytes, pathBytes, "logical-plan shortestPath result size overflowed");
							const size_t nextCapacity =
									allPaths.size() == allPaths.capacity()
											? std::min(state.shared.bounds.maximum_rows,
													   std::max(allPaths.size() + 1U,
																checkedSizeMul(allPaths.capacity(), 2U,
																			   "logical-plan shortestPath result "
																			   "capacity overflowed")))
											: allPaths.capacity();
							const size_t retainedBytes = checkedSizeAdd(
									nextPathBytes,
									checkedSizeMul(nextCapacity, sizeof(PropertyValue),
												   "logical-plan shortestPath result capacity overflowed"),
									"logical-plan shortestPath result size overflowed");
							// The completed-path owner takes over from the construction lease.
							// No allocation occurs between releasing and acquiring these charges.
							pendingPathBytes.reset();
							allPathReservation.set(retainedBytes);
							if (nextCapacity != allPaths.capacity())
								allPaths.reserve(nextCapacity);
							allPaths.push_back(std::move(path));
							allPathBytes = nextPathBytes;
							return;
						}
						const auto previous = parents.find(nodeId);
						if (previous == parents.end() || previous->second.empty()) {
							throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
						}
						for (const Parent &parent: previous->second) {
							const size_t nodeCapacity =
									reversedNodes.size() == reversedNodes.capacity()
											? checkedSizeMul(
													  reversedNodes.capacity(), 2U,
													  "logical-plan shortestPath reverse Node capacity overflowed")
											: reversedNodes.capacity();
							const size_t edgeCapacity =
									reversedEdges.size() == reversedEdges.capacity()
											? std::max(size_t{1U}, checkedSizeMul(reversedEdges.capacity(), 2U,
																				  "logical-plan shortestPath reverse "
																				  "Edge capacity overflowed"))
											: reversedEdges.capacity();
							const size_t nextTypeBytes =
									checkedSizeAdd(reversedTypeBytes, parent.edge.type.size(),
												   "logical-plan shortestPath reverse Edge type size overflowed");
							reversedPathBytes.set(reversedStorageBytes(nodeCapacity, edgeCapacity, nextTypeBytes));
							if (nodeCapacity != reversedNodes.capacity())
								reversedNodes.reserve(nodeCapacity);
							if (edgeCapacity != reversedEdges.capacity())
								reversedEdges.reserve(edgeCapacity);
							reversedNodes.push_back(parent.node_id);
							reversedEdges.push_back(parent.edge);
							reversedTypeBytes = nextTypeBytes;
							collectPaths(parent.node_id);
							reversedEdges.pop_back();
							reversedTypeBytes -= parent.edge.type.size();
							reversedPathBytes.set(reversedStorageBytes(reversedNodes.capacity(),
																	   reversedEdges.capacity(), reversedTypeBytes));
							reversedNodes.pop_back();
							if (!shortest.isAll() && !allPaths.empty()) {
								return;
							}
						}
					};
					if (!shortest.isAll()) {
						uint64_t current = endNodeId;
						while (current != startNodeId) {
							const auto previous = parents.find(current);
							if (previous == parents.end() || previous->second.empty()) {
								throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
							}
							const Parent &parent = previous->second.front();
							const size_t nodeCapacity =
									reversedNodes.size() == reversedNodes.capacity()
											? checkedSizeMul(
													  reversedNodes.capacity(), 2U,
													  "logical-plan shortestPath reverse Node capacity overflowed")
											: reversedNodes.capacity();
							const size_t edgeCapacity =
									reversedEdges.size() == reversedEdges.capacity()
											? std::max(size_t{1U}, checkedSizeMul(reversedEdges.capacity(), 2U,
																				  "logical-plan shortestPath reverse "
																				  "Edge capacity overflowed"))
											: reversedEdges.capacity();
							const size_t nextTypeBytes =
									checkedSizeAdd(reversedTypeBytes, parent.edge.type.size(),
												   "logical-plan shortestPath reverse Edge type size overflowed");
							reversedPathBytes.set(reversedStorageBytes(nodeCapacity, edgeCapacity, nextTypeBytes));
							if (nodeCapacity != reversedNodes.capacity())
								reversedNodes.reserve(nodeCapacity);
							if (edgeCapacity != reversedEdges.capacity())
								reversedEdges.reserve(edgeCapacity);
							reversedNodes.push_back(parent.node_id);
							reversedEdges.push_back(parent.edge);
							reversedTypeBytes = nextTypeBytes;
							current = parent.node_id;
						}
						std::ranges::reverse(reversedNodes);
						std::ranges::reverse(reversedEdges);
						// The parent DAG and BFS frontier are no longer needed once the
						// deterministic first shortest route is captured. Release them before
						// reading complete Node/Edge payloads for the returned path.
						decltype(parents){}.swap(parents);
						decltype(distances){}.swap(distances);
						decltype(frontier){}.swap(frontier);
						shortestPathStateBytes.reset();
						MaterializedByteReservation outputBytes(&state.shared.materialized_budget);
						return materializePath(reversedNodes, reversedEdges, outputBytes);
					}
					collectPaths(endNodeId);
					if (allPaths.empty()) {
						throw std::runtime_error("logical-plan shortestPath failed to materialize a discovered path");
					}
					return shortest.isAll() ? PropertyValue(std::move(allPaths)) : std::move(allPaths.front());
				}
				default:
					throw std::logic_error("logical-plan runtime does not support this expression type");
			}
		}

		[[nodiscard]] std::optional<std::pair<bool, uint64_t>>
		logicalEntityIdentity(const LogicalPlanReadValue &candidate) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&candidate)) {
				return std::pair<bool, uint64_t>{false, node->node_id};
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&candidate)) {
				return std::pair<bool, uint64_t>{true, edge->edge_id};
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&candidate)) {
				return std::pair<bool, uint64_t>{reference->is_edge, reference->entity_id};
			}
			return std::nullopt;
		}

		[[nodiscard]] bool logicalValuesEqual(const LogicalPlanReadValue &left, const LogicalPlanReadValue &right) {
			const auto leftIdentity = logicalEntityIdentity(left);
			const auto rightIdentity = logicalEntityIdentity(right);
			if (leftIdentity.has_value() && rightIdentity.has_value()) {
				return *leftIdentity == *rightIdentity;
			}
			return left == right;
		}

		[[nodiscard]] bool bindValue(LogicalPlanReadRow &row, const std::string &variable, LogicalPlanReadValue value) {
			if (variable.empty()) {
				return true;
			}
			const auto found = row.values.find(variable);
			if (found == row.values.end()) {
				row.values.emplace(variable, std::move(value));
				return true;
			}
			// A deferred EntityRef and a materialized entity represent the same
			// logical binding.  Compare their stable identity rather than their
			// storage representation so repeated variables (notably self-loops)
			// retain Cypher's identity semantics across lazy materialization.
			return logicalValuesEqual(found->second, value);
		}

		// A bare variable projection keeps the same logical Entity binding under a
		// new name. Transfer selected-field and metadata caches with that binding so
		// a later Project, Sort, or Aggregate does not reread fields already resolved
		// in the same snapshot.
		void propagateProjectedEntityCaches(const expressions::Expression &expression, const LogicalPlanReadRow &input,
											LogicalPlanReadRow &projected, const std::string &alias, ExecutionState &state) {
			const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(&expression);
			if (reference == nullptr || reference->hasProperty()) {
				return;
			}
			const std::string &sourceVariable = reference->getVariableName();
			const auto sourceValue = input.values.find(sourceVariable);
			const auto projectedValue = projected.values.find(alias);
			if (sourceValue == input.values.end() || projectedValue == projected.values.end() ||
				!logicalValuesEqual(sourceValue->second, projectedValue->second)) {
				return;
			}
			if (!logicalEntityIdentity(sourceValue->second).has_value()) {
				return;
			}
			if (const auto properties = input.property_cache.find(sourceVariable);
				properties != input.property_cache.end()) {
				projected.property_cache.emplace(alias, detail::copyPropertyMapGuarded(properties->second, [&] { checkGuardNow(state); }));
				if (input.complete_entity_property_cache.contains(sourceVariable)) {
					projected.complete_entity_property_cache.insert(alias);
				}
			}
			if (const auto metadata = input.entity_metadata_cache.find(sourceVariable);
				metadata != input.entity_metadata_cache.end()) {
				projected.entity_metadata_cache.emplace(alias, detail::copyPropertyMapGuarded(metadata->second, [&] { checkGuardNow(state); }));
			}
		}


		// DISTINCT needs a structural key, not a display string: PropertyValue::toString()
		// intentionally permits human-readable ambiguity in nested lists and maps.
		template<typename String>
		void appendDistinctUint64(String &output, uint64_t value) {
			for (size_t byte = 0U; byte < sizeof(value); ++byte) {
				output += static_cast<char>(value & 0xFFU);
				value >>= 8U;
			}
		}

		template<typename String>
		void appendDistinctString(String &output, std::string_view value) {
			appendDistinctUint64(output, value.size());
			output.append(value);
		}

		template<typename String>
		void appendDistinctPropertyValue(String &output, const PropertyValue &value) {
			const auto appendType = [&output](PropertyType type) {
				output += static_cast<char>(static_cast<uint8_t>(type));
			};
			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					appendType(PropertyType::NULL_TYPE);
					return;
				case PropertyType::BOOLEAN:
					appendType(PropertyType::BOOLEAN);
					output += std::get<bool>(value.getVariant()) ? '\x01' : '\x00';
					return;
				case PropertyType::INTEGER:
					appendType(PropertyType::INTEGER);
					appendDistinctUint64(output, static_cast<uint64_t>(std::get<int64_t>(value.getVariant())));
					return;
				case PropertyType::DOUBLE: {
					appendType(PropertyType::DOUBLE);
					double number = std::get<double>(value.getVariant());
					// PropertyValue treats -0.0 and +0.0 as equal, so DISTINCT must too.
					if (number == 0.0) {
						number = 0.0;
					}
					appendDistinctUint64(output, std::bit_cast<uint64_t>(number));
					return;
				}
				case PropertyType::STRING:
					appendType(PropertyType::STRING);
					appendDistinctString(output, std::get<std::string>(value.getVariant()));
					return;
				case PropertyType::LIST: {
					appendType(PropertyType::LIST);
					const auto &list = value.getList();
					appendDistinctUint64(output, list.size());
					for (const PropertyValue &element: list) {
						appendDistinctPropertyValue(output, element);
					}
					return;
				}
				case PropertyType::MAP: {
					appendType(PropertyType::MAP);
					const auto &map = value.getMap();
					const auto scratchResource = [&]() -> std::pmr::memory_resource * {
						if constexpr (std::is_same_v<String, std::pmr::string>) return output.get_allocator().resource();
						else if constexpr (std::is_same_v<String, utils::OwnedStdString>) {
                            const auto owner = utils::OwnedAllocationScope::owner();
                            return owner ? owner.get() : std::pmr::new_delete_resource();
                        } else return std::pmr::new_delete_resource();
					};
					std::pmr::vector<const PropertyValue::MapType::value_type *> entries{scratchResource()};
					entries.reserve(map.size());
					for (const auto &entry: map) {
						entries.push_back(&entry);
					}
					std::sort(entries.begin(), entries.end(),
							  [](const auto *left, const auto *right) { return left->first < right->first; });
					appendDistinctUint64(output, entries.size());
					for (const auto *entry: entries) {
						appendDistinctString(output, entry->first);
						appendDistinctPropertyValue(output, entry->second);
					}
					return;
				}
				case PropertyType::DATE:
				case PropertyType::DATETIME:
				case PropertyType::DURATION:
					appendType(value.getType());
					appendDistinctString(output, value.toString());
					return;
				case PropertyType::BYTES: {
					appendType(PropertyType::BYTES);
					const auto &bytes = value.getBytes();
					appendDistinctUint64(output, bytes.size());
					for (const std::byte byte: bytes) {
						output += static_cast<char>(std::to_integer<uint8_t>(byte));
					}
					return;
				}
				case PropertyType::UNKNOWN: // ZYX_COV_EXCL_LINE: PropertyValue has no UNKNOWN or COMPOSITE variant.
				case PropertyType::COMPOSITE: // ZYX_COV_EXCL_LINE: PropertyValue has no UNKNOWN or COMPOSITE variant.
					throw std::runtime_error("logical-plan DISTINCT received an unsupported Property value");
			}
			throw std::runtime_error(
					"logical-plan DISTINCT received an unknown Property value"); // ZYX_COV_EXCL_LINE: the PropertyType
																				 // switch is exhaustive for
																				 // PropertyValue.
		}

		template<typename String>
		void appendDistinctValueKey(String &output, const LogicalPlanReadValue &value) {
			if (const PropertyValue *property = std::get_if<PropertyValue>(&value)) {
				output += 'P';
				appendDistinctPropertyValue(output, *property);
				return;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				output += 'N';
				appendDistinctUint64(output, node->node_id);
				return;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				output += 'E';
				appendDistinctUint64(output, edge->edge_id);
				return;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(value);
			validateEntityRef(reference);
			output += reference.is_edge ? 'E' : 'N';
			appendDistinctUint64(output, reference.entity_id);
		}

		[[nodiscard]] std::pmr::string distinctRowKey(const LogicalPlanReadRow &row,
												 const std::vector<logical::LogicalProjectItem> &items, std::pmr::memory_resource *memory) {
			std::pmr::string result{memory};
			for (const logical::LogicalProjectItem &item: items) {
				const auto found = row.values.find(item.alias);
				if (found == row.values.end()) { // ZYX_COV_EXCL_LINE: Project validates aliases and emplaces every
												 // requested value before DISTINCT keys are built.
					throw std::runtime_error("logical-plan Project failed to bind an output alias");
				}
				appendDistinctString(result, item.alias);
				appendDistinctValueKey(result, found->second);
			}
			return result;
		}

		[[nodiscard]] std::vector<std::string> boundOutputNames(const logical::LogicalOperator &op, ExecutionState &state,
			MaterializedByteReservation &charge) {
			if (!state.shared.bound_program) return op.getOutputVariables(); // explicit IR adapter
			const auto &program = *state.shared.bound_program; program.validateSnapshot(state.shared.snapshot);
			const auto outputs = program.view(program.id(op)).declared_outputs;
			size_t bytes = checkedSizeMul(outputs.size(), sizeof(std::string), "bound output metadata overflowed");
			const auto inlineCapacity = std::string{}.capacity();
			for (const auto slot : outputs) {
				checkGuardNow(state); const auto name = program.name(slot);
				if (name.size() > inlineCapacity) bytes = checkedSizeAdd(bytes, name.size() + 1U, "bound output name overflowed");
			}
			charge.set(bytes);
			std::vector<std::string> names; names.reserve(outputs.size());
			for (const auto slot : outputs) { checkGuardNow(state); names.emplace_back(program.name(slot)); }
			bytes = checkedSizeMul(names.capacity(), sizeof(std::string), "bound output capacity overflowed");
			for (const auto &name : names) bytes = checkedSizeAdd(bytes, detail::dynamicStringBufferBytes(name), "bound output buffers overflowed");
			charge.set(bytes); return names;
		}

		[[nodiscard]] LogicalPlanReadRow normalizeOutputRow(LogicalPlanReadRow row,
															const std::vector<std::string> &columns) {
			LogicalPlanReadRow normalized;
			normalized.values.reserve(columns.size());
			normalized.property_cache.reserve(columns.size());
			normalized.path_cache.reserve(columns.size());
			// The caller transfers an owned row. Move container nodes, including
			// caches, so normalization does not duplicate large Property/path payloads.
			for (const std::string &column: columns) {
				if (normalized.values.contains(column))
					continue;
				const auto value = row.values.find(column);
				if (value == row.values.end()) {
					throw std::runtime_error("logical-plan runtime child did not bind a declared output variable");
				}
				normalized.values.insert(row.values.extract(value));
				if (const auto properties = row.property_cache.find(column); properties != row.property_cache.end()) {
					normalized.property_cache.insert(row.property_cache.extract(properties));
					if (row.complete_entity_property_cache.contains(column)) {
						normalized.complete_entity_property_cache.insert(
								row.complete_entity_property_cache.extract(column));
					}
				}
				if (const auto metadata = row.entity_metadata_cache.find(column);
					metadata != row.entity_metadata_cache.end()) {
					normalized.entity_metadata_cache.insert(row.entity_metadata_cache.extract(metadata));
				}
				if (const auto path = row.path_cache.find(column); path != row.path_cache.end()) {
					normalized.path_cache.insert(row.path_cache.extract(path));
				}
			}
			return normalized;
		}

		void mergeCompatibleRowCaches(LogicalPlanReadRow &destination, const LogicalPlanReadRow &source, ExecutionState &state,
									  const std::pmr::unordered_set<std::string_view> *excludedAliases = nullptr) {
			const auto checkpoint = [&] { checkGuardNow(state); };
			const auto includeAlias = [excludedAliases](const std::string &alias) {
				return excludedAliases == nullptr || !excludedAliases->contains(alias);
			};
			for (const auto &[alias, properties]: source.property_cache) {
				if (!includeAlias(alias)) {
					continue;
				}
				const auto existing = destination.property_cache.find(alias);
				if (existing != destination.property_cache.end() && existing->second != properties) {
					throw std::runtime_error(
							"logical-plan row combination received incompatible Property cache entries");
				}
				if (existing == destination.property_cache.end()) {
					destination.property_cache.emplace(alias, detail::copyPropertyMapGuarded(properties, checkpoint));
				}
				if (source.complete_entity_property_cache.contains(alias)) {
					destination.complete_entity_property_cache.insert(alias);
				}
			}
			for (const auto &[alias, metadata]: source.entity_metadata_cache) {
				if (!includeAlias(alias)) {
					continue;
				}
				auto &target = destination.entity_metadata_cache[alias];
				for (const auto &[key, value]: metadata) {
					checkpoint();
					const auto entry = target.find(key);
					if (entry == target.end()) {
						target.emplace(key, detail::copyPropertyValueGuarded(value, checkpoint));
					} else if (entry->second != value) {
						throw std::runtime_error(
								"logical-plan row combination received incompatible Entity metadata cache entries");
					}
				}
			}
			for (const auto &[alias, path]: source.path_cache) {
				if (!includeAlias(alias)) {
					continue;
				}
				checkpoint();
				const auto entry = destination.path_cache.find(alias);
				if (entry == destination.path_cache.end()) {
					destination.path_cache.emplace(alias, detail::copyLogicalReadPathGuarded(path, checkpoint));
				} else if (entry->second != path) {
					throw std::runtime_error("logical-plan row combination received incompatible path cache entries");
				}
			}
		}

		// The source is consumed, unlike reusable JOIN/OPTIONAL MATCH rows.
		void mergeOwnedRowCaches(LogicalPlanReadRow &destination, LogicalPlanReadRow source) {
			for (const auto &[alias, properties]: source.property_cache) {
				const auto found = destination.property_cache.find(alias);
				if (found != destination.property_cache.end() && found->second != properties) {
					throw std::runtime_error(
							"logical-plan row combination received incompatible Property cache entries");
				}
			}
			for (const auto &[alias, metadata]: source.entity_metadata_cache) {
				const auto found = destination.entity_metadata_cache.find(alias);
				if (found == destination.entity_metadata_cache.end())
					continue;
				for (const auto &[key, value]: metadata) {
					const auto entry = found->second.find(key);
					if (entry != found->second.end() && entry->second != value) {
						throw std::runtime_error(
								"logical-plan row combination received incompatible Entity metadata cache entries");
					}
				}
			}
			for (const auto &[alias, path]: source.path_cache) {
				const auto found = destination.path_cache.find(alias);
				if (found != destination.path_cache.end() && found->second != path) {
					throw std::runtime_error("logical-plan row combination received incompatible path cache entries");
				}
			}
			destination.property_cache.merge(source.property_cache);
			destination.complete_entity_property_cache.merge(source.complete_entity_property_cache);
			destination.path_cache.merge(source.path_cache);
			destination.entity_metadata_cache.merge(source.entity_metadata_cache);
			for (auto &[alias, metadata]: source.entity_metadata_cache) {
				destination.entity_metadata_cache.at(alias).merge(metadata);
			}
		}

        [[nodiscard]] utils::OwnedStringVector visibleOutputColumns(const logical::LogicalOperator &root) {
            const auto all = root.getOwnedOutputVariables();
            utils::OwnedStringVector columns;
            columns.reserve(all.size());
            for (const auto &column : all) if (!isAnonymousVariable(column)) columns.push_back(column);
            return columns;
        }

        // Metadata and the final standard-vector copy overlap during handoff.
        // Both have admission before copying; detach transfers the final copy's
        // existing parent credit without a refund/re-admission window.
        [[nodiscard]] std::optional<detail::ResourceLease> populateResultColumns(QueryResult &result,
                const logical::LogicalOperator &root, size_t maximumBytes, const QueryContext *context) {
            const bool managed = context && context->resource_account;
            auto memory = managed
                ? std::make_shared<MaterializedByteBudget>(maximumBytes, context->resource_account, std::pmr::new_delete_resource())
                : std::make_shared<MaterializedByteBudget>(maximumBytes);
            utils::OwnedAllocationScope scope(memory);
            const auto columns = visibleOutputColumns(root);
            if (columns.size() > (std::numeric_limits<size_t>::max)()/sizeof(std::string))
                throw std::overflow_error("result column backing overflowed");
            size_t copyUpper = utils::detail::addStandardHeapBytes(3U * utils::detail::standardContainerProxyBytes(),
                utils::detail::standardAllocatorRequestBytes(columns.size()*sizeof(std::string), alignof(std::string)));
            for (const auto &column : columns)
                copyUpper = utils::detail::addStandardHeapBytes(copyUpper, utils::detail::standardFreshStringHeapUpper(column.size()));
            MaterializedByteReservation copied(memory.get());
            copied.set(copyUpper);
            result.setColumns(static_cast<const std::vector<std::string> &>(columns));
            const auto retained = queryResultColumnStorageBytes(result.getColumns());
            // The fresh-copy model is audited for the supported standard STLs.
            if (retained > copyUpper) std::terminate();
            copied.set(retained);
            if (managed) return copied.detachToParent();
            return std::nullopt;
        }

		template<typename String = std::string>
		[[nodiscard]] String distinctUnionRowKey(const LogicalPlanReadRow &row,
													  const std::vector<std::string> &columns,
			std::pmr::memory_resource *memory = std::pmr::new_delete_resource()) {
			String result = [&] {
				if constexpr (std::is_same_v<String, std::pmr::string>) return String{memory};
				else return String{};
			}();
			for (const std::string &column: columns) {
				const auto value = row.values.find(column);
				if (value ==
					row.values.end()) { // normalizeOutputRow validates every declared output before this key is built.
					throw std::runtime_error("logical-plan UNION lost a declared output variable");
				}
				appendDistinctString(result, column);
				appendDistinctValueKey(result, value->second);
			}
			return result;
		}

		[[nodiscard]] std::pmr::string bindingKey(const LogicalPlanReadRow &row,
			std::span<const std::string *const> columns, std::pmr::memory_resource *memory) {
			std::pmr::string result{memory};
			for (const auto *name: columns) {
				const auto &column = *name;
				const auto value = row.values.find(column);
				if (value == row.values.end()) {
					throw std::runtime_error("logical-plan runtime child did not bind a declared join variable");
				}
				appendDistinctString(result, column);
				appendDistinctValueKey(result, value->second);
			}
			return result;
		}

		[[nodiscard]] AggregateFunctionType aggregateFunctionType(std::string_view name) {
			std::string normalized;
			normalized.reserve(name.size());
			for (const char character: name) {
				normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
			}
			if (normalized == "count") {
				return AggregateFunctionType::AGG_COUNT;
			}
			if (normalized == "sum") {
				return AggregateFunctionType::AGG_SUM;
			}
			if (normalized == "avg") {
				return AggregateFunctionType::AGG_AVG;
			}
			if (normalized == "min") {
				return AggregateFunctionType::AGG_MIN;
			}
			if (normalized == "max") {
				return AggregateFunctionType::AGG_MAX;
			}
			if (normalized == "collect") {
				return AggregateFunctionType::AGG_COLLECT;
			}
			if (normalized == "stdev") {
				return AggregateFunctionType::AGG_STDEV;
			}
			if (normalized == "stdevp") {
				return AggregateFunctionType::AGG_STDEVP;
			}
			if (normalized == "percentiledisc") {
				return AggregateFunctionType::AGG_PERCENTILE_DISC;
			}
			if (normalized == "percentilecont") {
				return AggregateFunctionType::AGG_PERCENTILE_CONT;
			}
			throw std::invalid_argument("logical-plan Aggregate has an unsupported function");
		}

		[[nodiscard]] double aggregatePercentile(const logical::LogicalAggItem &item, AggregateFunctionType function,
												 ExecutionState &state) {
			const bool isPercentile = function == AggregateFunctionType::AGG_PERCENTILE_DISC ||
									  function == AggregateFunctionType::AGG_PERCENTILE_CONT;
			if (!isPercentile) {
				if (item.extraArg) {
					throw std::invalid_argument("logical-plan Aggregate has an unexpected extra argument");
				}
				return 0.5;
			}
			if (!item.extraArg) {
				throw std::invalid_argument("logical-plan Aggregate percentile requires a percentile argument");
			}
			LogicalPlanReadRow emptyRow;
			const PropertyValue value = scalarValue(evaluateExpression(*item.extraArg, emptyRow, state));
			const int64_t *integer = std::get_if<int64_t>(&value.getVariant());
			const double *number = std::get_if<double>(&value.getVariant());
			const double percentile = integer != nullptr  ? static_cast<double>(*integer)
									  : number != nullptr ? *number
														  : std::numeric_limits<double>::quiet_NaN();
			if (!std::isfinite(percentile) || percentile < 0.0 || percentile > 1.0) {
				throw std::invalid_argument(
						"logical-plan Aggregate percentile must be a finite value between zero and one");
			}
			return percentile;
		}

		struct MaterializedEntityKey {
			uint64_t id;
			bool is_edge;
			bool operator==(const MaterializedEntityKey &) const = default;
		};
		struct MaterializedEntityKeyHash {
			size_t operator()(const MaterializedEntityKey &key) const noexcept {
				return std::hash<uint64_t>{}(key.id) ^
					(key.is_edge ? std::numeric_limits<size_t>::max() : 0U);
			}
		};
		using MaterializedPropertyCache =
			std::pmr::unordered_map<MaterializedEntityKey, PropertyMap, MaterializedEntityKeyHash>;
		using FinalRowVisitor = std::function<bool(LogicalPlanReadRow &, ExecutionState &)>;
		using BatchSelectionPredicate = std::function<bool(LogicalPlanReadRow &, ExecutionState &)>;

		template<typename Entity>
		[[nodiscard]] const PropertyMap *findMatchingRowProperties(const LogicalPlanReadRow &row,
																   const Entity &entity) {
			for (const auto &[alias, candidate]: row.values) {
				const auto *matchingEntity = std::get_if<Entity>(&candidate);
				if (matchingEntity == nullptr || *matchingEntity != entity)
					continue;
				if (const auto cached = row.property_cache.find(alias); cached != row.property_cache.end()) {
					return &cached->second;
				}
			}
			return nullptr;
		}

		template<typename Entity>
		[[nodiscard]] const PropertyMap &
		materializeEntityProperties(const LogicalPlanReadRow &row, const Entity &entity, ExecutionState &state,
									MaterializedPropertyCache &materialized,
									MaterializedByteReservation *sourceBytes) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			const MaterializedEntityKey cacheKey = [&entity]() {
				if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>)
					return MaterializedEntityKey{entity.node_id, false};
				else
					return MaterializedEntityKey{entity.edge_id, true};
			}();
			if (const auto cached = materialized.find(cacheKey); cached != materialized.end()) {
				return cached->second;
			}
			if (const PropertyMap *cached = findMatchingRowProperties(row, entity)) {
				return *cached;
			}
			size_t predictedMapBytes = 0U;
			if (sourceBytes != nullptr) {
				if (entity.properties.size() > state.shared.bounds.maximum_property_entries) {
					throw std::runtime_error("logical graph backend returned too many direct Property entries");
				}
				constexpr const char *message = "logical-plan NamedPath Property cache size overflowed";
				size_t bytes = directPropertyMapStorageBytes(entity.properties, message, state.shared.context);
				bytes -= sizeof(PropertyMap);
				predictedMapBytes = bytes;
				const size_t nextBytes = checkedSizeAdd(sourceBytes->bytes(), bytes, message);
				if (sourceBytes->wouldExceed(nextBytes)) {
					throw std::runtime_error("logical-plan NamedPath Property cache exceeded its shared byte bound");
				}
				sourceBytes->set(nextBytes);
			}
			PropertyMap properties = [&] {
				if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
					validateNode(entity, state);
					return readNodeProperties(state.shared.snapshot, entity, state);
				} else {
					validateEdge(entity);
					return readEdgeProperties(state.shared.snapshot, entity, state);
				}
			}();
			if (sourceBytes != nullptr) {
				const size_t actualMapBytes = propertyMapBytes(properties,
					"logical-plan NamedPath Property cache size overflowed", state.shared.context) - sizeof(PropertyMap);
				sourceBytes->set(checkedSizeAdd(sourceBytes->bytes() - predictedMapBytes, actualMapBytes,
					"logical-plan NamedPath Property cache size overflowed"));
			}
			return materialized.emplace(cacheKey, std::move(properties)).first->second;
		}

		[[nodiscard]] PropertyMap copyResultPropertyMap(const PropertyMap &properties, ExecutionState &state) {
			return detail::copyPropertyMapGuarded(properties, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
		}

		template<typename Entity>
		[[nodiscard]] PropertyMap materializeUniqueEntityProperties(const LogicalPlanReadRow &row, const Entity &entity,
																	ExecutionState &state) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			if (const PropertyMap *cached = findMatchingRowProperties(row, entity))
				return copyResultPropertyMap(*cached, state);
			if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
				validateNode(entity, state);
				return readNodeProperties(state.shared.snapshot, entity, state);
			} else {
				validateEdge(entity);
				return readEdgeProperties(state.shared.snapshot, entity, state);
			}
		}

		template<typename Entity>
		[[nodiscard]] PropertyMap materializeCompleteEntityProperties(const Entity &entity, ExecutionState &state) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			PropertyMap properties;
			properties.reserve(entity.properties.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entity.properties) {
				checkGuard(state);
				if (!properties.emplace(entry.key, detail::copyPropertyValueGuarded(entry.value, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				})).second) {
					throw std::runtime_error("logical-plan result received duplicate complete Entity properties");
				}
			}
			return properties;
		}

		[[nodiscard]] size_t completeResultPropertyBytes(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
														 ExecutionState &state) {
			detail::BudgetMemoryResource keyMemory(state.shared.materialized_budget);
			std::pmr::unordered_set<std::string_view> keys(&keyMemory);
			keys.reserve(entries.size());
			size_t bytes = sizeof(PropertyMap);
			for (const auto &entry: entries) {
				checkGuard(state);
				if (!keys.insert(entry.key).second) {
					throw std::runtime_error("logical-plan result received duplicate complete Entity properties");
				}
				bytes = checkedSizeAdd(bytes, entry.key.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(PropertyValue), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, retainedExpressionValueBytes(entry.value, state),
									   "logical-plan result row size overflowed");
			}
			return bytes;
		}

		template<typename Entity>
		[[nodiscard]] size_t completeEntityResultBytes(const Entity &entity, ExecutionState &state) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
				size_t bytes = checkedSizeAdd(sizeof(LogicalNodeResult),
				checkedSizeMul(entity.labels.capacity(), sizeof(std::string),
					"logical-plan result row size overflowed"), "logical-plan result row size overflowed");
				size_t labelsUntilGuard = 256U;
				for (const std::string &label: entity.labels) {
					if (--labelsUntilGuard == 0U) {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						labelsUntilGuard = 256U;
					}
					bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), "logical-plan result row size overflowed");
				}
				return checkedSizeAdd(bytes, completeResultPropertyBytes(entity.properties, state),
									  "logical-plan result row size overflowed");
			} else {
				const size_t bytes = checkedSizeAdd(sizeof(LogicalEdgeResult), std::max(entity.type.size(), detail::dynamicStringBufferBytes(entity.type)),
													"logical-plan result row size overflowed");
				return checkedSizeAdd(bytes, completeResultPropertyBytes(entity.properties, state),
									  "logical-plan result row size overflowed");
			}
		}

		void admitCompleteEntityResult(size_t entityBytes, size_t rowPrefixBytes, size_t columnNameBytes,
									   ExecutionState &state, MaterializedByteReservation &reservation) {
			size_t nextBytes =
					checkedSizeAdd(rowPrefixBytes, columnNameBytes, "logical-plan result row size overflowed");
			nextBytes = checkedSizeAdd(nextBytes, sizeof(ResultValue), "logical-plan result row size overflowed");
			nextBytes = checkedSizeAdd(nextBytes, entityBytes, "logical-plan result row size overflowed");
			if (nextBytes > state.shared.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
			}
			reservation.set(nextBytes);
		}

		[[nodiscard]] ResultValue materializeResultValue(const LogicalPlanReadRow &row,
														 const LogicalPlanReadValue &value, ExecutionState &state,
														 MaterializedPropertyCache &materialized, size_t rowPrefixBytes,
														 size_t columnNameBytes, bool rowPreflighted,
														 bool repeatedEntity, MaterializedByteReservation &outputReservation,
			MaterializedByteReservation &propertyCacheReservation) {
			const auto copyCheckpoint = [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			};
			if (!rowPreflighted && !std::holds_alternative<storage::EntityRef>(value)) {
				size_t predictedBytes;
				if (const auto *property = std::get_if<PropertyValue>(&value))
					predictedBytes = retainedExpressionValueBytes(*property, state);
				else if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value))
					predictedBytes = completeEntityResultBytes(*node, state);
				else
					predictedBytes = completeEntityResultBytes(std::get<storage::LogicalGraphEdge>(value), state);
				admitCompleteEntityResult(predictedBytes, rowPrefixBytes, columnNameBytes, state, outputReservation);
			}
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return ResultValue(detail::copyPropertyValueGuarded(*property, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				}));
			}
			// Borrow a prefetched complete record while constructing the public
			// result. Only a cache miss needs a local shaped read.
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateEntityRef(*reference);
				if (reference->is_edge) {
					MaterializedByteReservation loadedBytes(&state.shared.materialized_budget);
					std::optional<storage::LogicalGraphEdge> loaded;
					const auto cached = state.shared.edge_cache.find(reference->entity_id);
					const storage::LogicalGraphEdge *edge = nullptr;
					if (cached != state.shared.edge_cache.end()) {
						edge = &cached->second;
					} else {
						loaded.emplace(readEdgeCached(state, reference->entity_id));
						edge = &*loaded;
					}
					validateEdgeReferenceRecord(*reference, *edge, state, "logical graph Entity reference");
					const std::optional<size_t> entityBytes =
							rowPreflighted ? std::nullopt
										   : std::optional<size_t>(completeEntityResultBytes(*edge, state));
					if (loaded)
						loadedBytes.set(logicalEdgeCacheBytes(*edge, state.shared.context));
					if (entityBytes)
						admitCompleteEntityResult(*entityBytes, rowPrefixBytes, columnNameBytes, state, outputReservation);
					PropertyMap properties = materializeCompleteEntityProperties(*edge, state);
					return ResultValue(LogicalEdgeResult{edge->edge_id, edge->source_node_id, edge->target_node_id,
														 detail::copyPropertyStringGuarded(edge->type, copyCheckpoint), std::move(properties)});
				}
				MaterializedByteReservation loadedBytes(&state.shared.materialized_budget);
				std::optional<storage::LogicalGraphNode> loaded;
				const auto cached = state.shared.node_cache.find(reference->entity_id);
				const storage::LogicalGraphNode *node = nullptr;
				if (cached != state.shared.node_cache.end()) {
					node = &cached->second;
				} else {
					loaded.emplace(readNodeCached(state, reference->entity_id));
					node = &*loaded;
				}
				const std::optional<size_t> entityBytes =
						rowPreflighted ? std::nullopt : std::optional<size_t>(completeEntityResultBytes(*node, state));
				if (loaded)
					loadedBytes.set(logicalNodeCacheBytes(*node, state.shared.context));
				if (entityBytes)
					admitCompleteEntityResult(*entityBytes, rowPrefixBytes, columnNameBytes, state, outputReservation);
				PropertyMap properties = materializeCompleteEntityProperties(*node, state);
				return ResultValue(LogicalNodeResult{node->node_id, detail::copyPropertyStringsGuarded(node->labels, copyCheckpoint), std::move(properties)});
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				if (!repeatedEntity) {
					PropertyMap properties = materializeUniqueEntityProperties(row, *node, state);
					return ResultValue(LogicalNodeResult{node->node_id, detail::copyPropertyStringsGuarded(node->labels, copyCheckpoint), std::move(properties)});
				}
				const PropertyMap &properties = materializeEntityProperties(row, *node, state, materialized, &propertyCacheReservation);
				return ResultValue(LogicalNodeResult{node->node_id, detail::copyPropertyStringsGuarded(node->labels, copyCheckpoint), copyResultPropertyMap(properties, state)});
			}
			const storage::LogicalGraphEdge &edge = std::get<storage::LogicalGraphEdge>(value);
			if (!repeatedEntity) {
				PropertyMap properties = materializeUniqueEntityProperties(row, edge, state);
				return ResultValue(LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, detail::copyPropertyStringGuarded(edge.type, copyCheckpoint),
													 std::move(properties)});
			}
			const PropertyMap &properties = materializeEntityProperties(row, edge, state, materialized, &propertyCacheReservation);
			return ResultValue(
					LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, detail::copyPropertyStringGuarded(edge.type, copyCheckpoint), copyResultPropertyMap(properties, state)});
		}

		[[nodiscard]] size_t inlineResultPropertyBytes(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													   ExecutionState &state) {
			if (entries.size() > state.shared.bounds.maximum_property_entries) {
				throw std::runtime_error("logical graph backend returned too many direct Property entries");
			}
			detail::BudgetMemoryResource keyMemory(state.shared.materialized_budget);
			std::pmr::unordered_set<std::string_view> keys(&keyMemory);
			keys.reserve(entries.size());
			size_t bytes = sizeof(PropertyMap);
			for (const auto &entry: entries) {
				checkGuard(state);
				if (entry.key.empty() || !keys.insert(entry.key).second) {
					throw std::runtime_error("logical graph backend returned an invalid direct Property entry set");
				}
				bytes = checkedSizeAdd(bytes, entry.key.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(PropertyValue), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, retainedExpressionValueBytes(entry.value, state),
									   "logical-plan result row size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t predictableEntityResultBytes(const LogicalPlanReadRow &row,
														  const storage::LogicalGraphNode &node,
														  ExecutionState &state) {
			const PropertyMap *cached = findMatchingRowProperties(row, node);
			if (cached == nullptr)
				validateNode(node, state);
			size_t bytes = checkedSizeAdd(sizeof(LogicalNodeResult),
				checkedSizeMul(node.labels.capacity(), sizeof(std::string),
					"logical-plan result row size overflowed"), "logical-plan result row size overflowed");
			size_t labelsUntilGuard = 256U;
			for (const std::string &label: node.labels) {
				if (--labelsUntilGuard == 0U) {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					labelsUntilGuard = 256U;
				}
				bytes = checkedSizeAdd(bytes, std::max(label.size(), detail::dynamicStringBufferBytes(label)), "logical-plan result row size overflowed");
			}
			return checkedSizeAdd(bytes,
								  cached != nullptr
										  ? propertyMapBytes(*cached, "logical-plan result row size overflowed", state.shared.context)
										  : inlineResultPropertyBytes(node.properties, state),
								  "logical-plan result row size overflowed");
		}

		[[nodiscard]] size_t predictableEntityResultBytes(const LogicalPlanReadRow &row,
														  const storage::LogicalGraphEdge &edge,
														  ExecutionState &state) {
			const PropertyMap *cached = findMatchingRowProperties(row, edge);
			if (cached == nullptr)
				validateEdge(edge);
			const size_t bytes = checkedSizeAdd(sizeof(LogicalEdgeResult), std::max(edge.type.size(), detail::dynamicStringBufferBytes(edge.type)),
												"logical-plan result row size overflowed");
			return checkedSizeAdd(bytes,
								  cached != nullptr
										  ? propertyMapBytes(*cached, "logical-plan result row size overflowed", state.shared.context)
										  : inlineResultPropertyBytes(edge.properties, state),
								  "logical-plan result row size overflowed");
		}

		[[nodiscard]] std::optional<size_t> predictableReferenceResultBytes(const storage::EntityRef &reference,
																			ExecutionState &state) {
			validateEntityRef(reference);
			if (reference.is_edge) {
				const auto cached = state.shared.edge_cache.find(reference.entity_id);
				if (cached == state.shared.edge_cache.end())
					return std::nullopt;
				const auto &edge = cached->second;
				validateEdgeReferenceRecord(reference, edge, state, "logical graph Entity reference");
				return completeEntityResultBytes(edge, state);
			}
			const auto cached = state.shared.node_cache.find(reference.entity_id);
			if (cached == state.shared.node_cache.end())
				return std::nullopt;
			const auto &node = cached->second;
			validateNode(node, state);
			return completeEntityResultBytes(node, state);
		}

		[[nodiscard]] std::optional<std::pair<bool, uint64_t>> resultEntityIdentity(const LogicalPlanReadValue &value) {
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				return std::pair{reference->is_edge, reference->entity_id};
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return std::pair{false, node->node_id};
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return std::pair{true, edge->edge_id};
			}
			return std::nullopt;
		}

		void appendLogicalResultRow(QueryResult &result, LogicalPlanReadRow &row,
								const std::vector<std::string> &columns, ExecutionState &state) {
			// Scalars and unique Entities whose complete records are already in
			// memory have an exact logical result size before payloads are copied.
			size_t predictableBytes = sizeof(QueryResult::Row);
			bool predictable = true;
			for (size_t columnIndex = 0U; columnIndex < columns.size(); ++columnIndex) {
				const std::string &column = columns[columnIndex];
				const auto found = row.values.find(column);
				if (found == row.values.end()) {
					throw std::runtime_error("logical-plan runtime did not bind a declared output variable");
				}
				predictableBytes =
						checkedSizeAdd(predictableBytes, column.size(), "logical-plan result row size overflowed");
				predictableBytes = checkedSizeAdd(predictableBytes, sizeof(ResultValue),
												  "logical-plan result row size overflowed");
				if (const auto *property = std::get_if<PropertyValue>(&found->second)) {
					predictableBytes = checkedSizeAdd(predictableBytes, retainedExpressionValueBytes(*property, state),
													  "logical-plan result row size overflowed");
					continue;
				}
				const auto identity = resultEntityIdentity(found->second);
				for (size_t previous = 0U; previous < columnIndex; ++previous) {
					const auto prior = row.values.find(columns[previous]);
					if (prior == row.values.end())
						continue;
					if (identity == resultEntityIdentity(prior->second)) {
						predictable = false;
						break;
					}
				}
				if (!predictable)
					break;
				std::optional<size_t> entityBytes;
				if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
					entityBytes = predictableReferenceResultBytes(*reference, state);
				} else if (const auto *node = std::get_if<storage::LogicalGraphNode>(&found->second)) {
					entityBytes = predictableEntityResultBytes(row, *node, state);
				} else {
					entityBytes = predictableEntityResultBytes(row, std::get<storage::LogicalGraphEdge>(found->second),
															   state);
				}
				if (!entityBytes) {
					predictable = false;
					break;
				}
				predictableBytes =
						checkedSizeAdd(predictableBytes, *entityBytes, "logical-plan result row size overflowed");
			}
			MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
			if (predictable) {
				if (predictableBytes > state.shared.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
				}
				if (predictableBytes > state.shared.materialized_budget.available()) {
					throw std::runtime_error("shared materialization exceeded its byte bound");
				}
				outputReservation.set(predictableBytes);
			}
			MaterializedByteReservation propertyCacheReservation(&state.shared.materialized_budget);
			QueryResult::Row output;
			output.reserve(columns.size());
			detail::BudgetMemoryResource propertyCacheMemory(state.shared.materialized_budget);
			MaterializedPropertyCache materialized(&propertyCacheMemory);
			size_t constructedBytes = sizeof(QueryResult::Row);
			for (const std::string &column: columns) {
				const auto found = row.values.find(column);
				if (found == row.values.end()) {
					throw std::runtime_error("logical-plan runtime did not bind a declared output variable");
				}
				bool repeatedEntity = false;
				if (const auto identity = resultEntityIdentity(found->second)) {
					for (const std::string &otherColumn: columns) {
						if (&otherColumn == &column)
							continue;
						const auto other = row.values.find(otherColumn);
						if (other != row.values.end() && resultEntityIdentity(other->second) == identity) {
							repeatedEntity = true;
							break;
						}
					}
				}
				ResultValue value = materializeResultValue(row, found->second, state, materialized, constructedBytes,
														   column.size(), predictable, repeatedEntity, outputReservation, propertyCacheReservation);
				const size_t valueBytes = resultValueBytes(value, state.shared.context);
				if (output.emplace(column, std::move(value)).second) {
					constructedBytes =
							checkedSizeAdd(constructedBytes, column.size(), "logical-plan result row size overflowed");
					constructedBytes = checkedSizeAdd(constructedBytes, sizeof(ResultValue),
													  "logical-plan result row size overflowed");
					constructedBytes =
							checkedSizeAdd(constructedBytes, valueBytes, "logical-plan result row size overflowed");
				}
				outputReservation.set(predictable ? std::max(predictableBytes, constructedBytes) : constructedBytes);
			}
			appendMaterializedResultRow(result, std::move(output), state, std::move(outputReservation));
		}

		void materializeRowEntityRefs(LogicalPlanReadRow &row, ExecutionState &state) {
			std::vector<storage::EntityRef> nodeReferences;
			std::vector<storage::EntityRef> edgeReferences;
			for (const auto &[_, value]: row.values) {
				if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
					validateEntityRef(*reference);
					(reference->is_edge ? edgeReferences : nodeReferences).push_back(*reference);
				}
			}
			const std::vector<storage::LogicalGraphNode> nodes = materializeNodeReferences(nodeReferences, state);
			const std::vector<storage::LogicalGraphEdge> edges = materializeEdgeReferences(edgeReferences, state);
			size_t nodeIndex = 0U;
			size_t edgeIndex = 0U;
			for (auto &[variable, value]: row.values) {
				if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
					if (reference->is_edge) {
						value = edges[edgeIndex++];
					} else {
						value = nodes[nodeIndex++];
					}
					row.property_cache.erase(variable);
					row.complete_entity_property_cache.erase(variable);
				}
			}
		}

		void appendUniquePrefetchReference(std::vector<storage::EntityRef> &references,
										   std::unordered_set<uint64_t> &ids, const storage::EntityRef &reference,
										   MaterializedByteReservation &referenceSlots,
										   MaterializedByteReservation &idStorage) {
			if (ids.contains(reference.entity_id))
				return;
			constexpr const char *message = "logical-plan reference prefetch scratch size overflowed";
			constexpr size_t idNodeBytes = sizeof(uint64_t) + 2U * sizeof(void *);
			const auto admitScratch = [](MaterializedByteReservation &reservation, size_t nextBytes) {
				if (reservation.wouldExceed(nextBytes)) {
					throw std::runtime_error("logical-plan reference prefetch scratch exceeded its shared byte bound");
				}
				reservation.set(nextBytes);
			};
			if (references.size() == references.capacity()) {
				const size_t nextCapacity =
						references.capacity() == 0U ? 1U : checkedSizeMul(references.capacity(), 2U, message);
				const size_t previousSlots = referenceSlots.bytes();
				const size_t nextSlots = checkedSizeMul(nextCapacity, sizeof(storage::EntityRef), message);
				admitScratch(referenceSlots, checkedSizeAdd(previousSlots, nextSlots, message));
				try {
					references.reserve(nextCapacity);
				} catch (...) {
					referenceSlots.set(previousSlots);
					throw;
				}
				admitScratch(referenceSlots,
							 checkedSizeMul(references.capacity(), sizeof(storage::EntityRef), message));
				const size_t previousIds = idStorage.bytes();
				const size_t plannedBuckets = checkedSizeMul(nextCapacity, 2U, message);
				admitScratch(
						idStorage,
						checkedSizeAdd(previousIds, checkedSizeMul(plannedBuckets, sizeof(void *), message), message));
				try {
					ids.reserve(nextCapacity);
				} catch (...) {
					idStorage.set(previousIds);
					throw;
				}
				admitScratch(idStorage,
							 checkedSizeAdd(checkedSizeMul(ids.size(), idNodeBytes, message),
											checkedSizeMul(ids.bucket_count(), sizeof(void *), message), message));
			}
			const size_t previousIds = idStorage.bytes();
			admitScratch(idStorage, checkedSizeAdd(previousIds, idNodeBytes, message));
			try {
				if (!ids.insert(reference.entity_id).second) {
					idStorage.set(previousIds);
					return;
				}
			} catch (...) {
				idStorage.set(previousIds);
				throw;
			}
			references.push_back(reference);
		}

		template<typename T>
		void reserveNamedPathSlots(std::vector<T> &values, size_t capacity, MaterializedByteReservation &slots) {
			if (capacity <= values.capacity())
				return;
			constexpr const char *message = "logical-plan NamedPath scratch capacity overflowed";
			const size_t previous = slots.bytes();
			const size_t peakBytes = checkedSizeAdd(previous, checkedSizeMul(capacity, sizeof(T), message), message);
			slots.set(peakBytes);
			try {
				values.reserve(capacity);
			} catch (...) {
				slots.set(previous);
				throw;
			}
			slots.set(checkedSizeMul(values.capacity(), sizeof(T), message));
		}

		void appendNamedPathReference(std::vector<storage::EntityRef> &references, const storage::EntityRef &reference,
									  size_t maximumCount, MaterializedByteReservation &slots) {
			if (references.size() >= maximumCount) {
				throw std::runtime_error("logical-plan NamedPath exceeded its deferred reference bound");
			}
			if (references.size() == references.capacity()) {
				const size_t nextCapacity = references.capacity() == 0U ? 1U
																		: (references.capacity() > maximumCount / 2U
																				   ? maximumCount
																				   : references.capacity() * 2U);
				reserveNamedPathSlots(references, nextCapacity, slots);
			}
			references.push_back(reference);
		}

		void prefetchEntityRefsInRows(std::span<const LogicalPlanReadRow> rows, ExecutionState &state,
									  const bool includePathReferences = false) {
			MaterializedByteReservation nodeReferenceSlots(&state.shared.materialized_budget);
			MaterializedByteReservation edgeReferenceSlots(&state.shared.materialized_budget);
			MaterializedByteReservation nodeIdStorage(&state.shared.materialized_budget);
			MaterializedByteReservation edgeIdStorage(&state.shared.materialized_budget);
			std::vector<storage::EntityRef> nodeReferences;
			std::vector<storage::EntityRef> edgeReferences;
			std::unordered_set<uint64_t> nodeIds;
			std::unordered_set<uint64_t> edgeIds;
			const auto collectReference = [&](const storage::EntityRef &reference, const bool allowZeroNode) {
				if (reference.entity_id == 0U && allowZeroNode) {
					return;
				}
				validateEntityRef(reference);
				if (reference.is_edge) {
					appendUniquePrefetchReference(edgeReferences, edgeIds, reference, edgeReferenceSlots,
												  edgeIdStorage);
				} else {
					appendUniquePrefetchReference(nodeReferences, nodeIds, reference, nodeReferenceSlots,
												  nodeIdStorage);
				}
			};
			for (const LogicalPlanReadRow &row: rows) {
				for (const auto &[_, value]: row.values) {
					const auto *reference = std::get_if<storage::EntityRef>(&value);
					if (reference == nullptr) {
						continue;
					}
					collectReference(*reference, false);
				}
				if (includePathReferences) {
					for (const auto &[_, path]: row.path_cache) {
						for (const storage::EntityRef &reference: path.node_refs) {
							// Variable-length paths use a zero Node reference as an explicit
							// marker for an already materialized endpoint.
							collectReference(reference, true);
						}
						for (const storage::EntityRef &reference: path.edge_refs) {
							collectReference(reference, false);
						}
					}
				}
			}
			std::ranges::sort(nodeReferences, {}, &storage::EntityRef::entity_id);
			std::ranges::sort(edgeReferences, {}, &storage::EntityRef::entity_id);
			static_cast<void>(materializeNodeReferences(nodeReferences, state, false));
			static_cast<void>(materializeEdgeReferences(edgeReferences, state, false));
		}

		void materializeEntityRefsInRows(std::span<LogicalPlanReadRow> rows, ExecutionState &state,
			MaterializedByteReservation &outputBytes) {
			prefetchEntityRefsInRows(rows, state);
			for (LogicalPlanReadRow &row: rows) {
				for (auto &[variable, value]: row.values) {
					const auto *referenceValue = std::get_if<storage::EntityRef>(&value);
					if (referenceValue == nullptr) continue;
					const storage::EntityRef reference = *referenceValue;
					// A declined optional cache entry is not an absent entity. Keep
					// fallback source ownership live, and admit the independently
					// owned output payload before copying it into the buffered row.
					const auto copyRecord = [&](const auto &record) {
						using Record = std::decay_t<decltype(record)>;
						const size_t bytes = [&] {
							if constexpr (std::is_same_v<Record, storage::LogicalGraphNode>)
								return logicalNodeCacheBytes(record, state.shared.context);
							else return logicalEdgeCacheBytes(record, state.shared.context);
						}();
						const size_t previous = logicalReadValueBytes(value, state.shared.context);
						if (previous > outputBytes.bytes())
							throw std::logic_error("logical-plan output reference charge is incomplete");
						outputBytes.set(checkedSizeAdd(outputBytes.bytes() - previous, bytes,
							"logical-plan owned output entity size overflowed"));
						value = detail::copyLogicalEntityGuarded(record, [&] { checkGuardNow(state); });
					};
					if (reference.is_edge) {
						const auto edge = state.shared.edge_cache.find(reference.entity_id);
						if (edge != state.shared.edge_cache.end()) {
							validateEdgeReferenceRecord(reference, edge->second, state,
								"logical graph batched Entity reference");
							copyRecord(edge->second);
						} else {
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							const std::array<storage::EntityRef, 1U> references{reference};
							auto records = materializeEdgeReferences(references, state, true, &sourceBytes);
							validateEdgeReferenceRecord(reference, records.front(), state,
								"logical graph uncached batched Entity reference");
							copyRecord(records.front());
						}
					} else {
						MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
						std::optional<storage::LogicalGraphNode> uncached;
						const auto &node = readNodeCachedBorrowed(state, reference.entity_id, uncached, sourceBytes);
						validateNode(node, state);
						copyRecord(node);
					}
					row.property_cache.erase(variable);
					row.complete_entity_property_cache.erase(variable);
				}
			}
		}

		template<typename Invoke>
		[[nodiscard]] bool invokeOutputVisitorChecked(ExecutionState &state, Invoke &&invoke) {
			checkGuardNow(state);
			const auto fence = state.shared.snapshot.mutationFence();
			const bool keepGoing = invoke();
			// The last callback (including an early stop) may have no subsequent source checkpoint.
			checkGuardNow(state);
			const auto currentFence = state.shared.snapshot.mutationFence();
			if (fence.has_value() && currentFence != fence)
				throw std::runtime_error("logical-plan snapshot changed during output callback");
			return keepGoing;
		}

		template<typename Run, typename Emit>
		[[nodiscard]] LogicalPlanReadRuntimeStats
		executeWithBatchedOutputMaterialization(const LogicalPlanReadRuntimeBounds &bounds, Run &&run, Emit &&emit,
												const bool preserveVisitorEarlyStop = false,
												const bool replaceDeferredEntityRefs = true,
                                                const QueryContext *allocationContext = nullptr) {
			// A public row visitor is allowed to stop the producer after the first
			// emitted row. Keep that contract strict by flushing one row at a time;
			// QueryResult materialization has no such callback and can use larger
			// cross-row batches safely.
			// Keep reserved row slots to at most 1/16 of the byte allowance where
			// possible, leaving most of the budget for the rows and source payloads.
			constexpr size_t outputBatchSlotBudgetDivisor = 16U;
            const size_t allocationAllowance = allocationContext && allocationContext->resource_account
                ? std::min(bounds.maximum_materialized_bytes, allocationContext->resource_account->available())
                : bounds.maximum_materialized_bytes;
			const size_t slotBytes = checkedSizeMul(sizeof(LogicalPlanReadRow), outputBatchSlotBudgetDivisor,
													"logical-plan output batch slot bound overflowed");
			const size_t maximumOutputSlots =
					allocationAllowance < sizeof(LogicalPlanReadRow)
							? 0U
							: std::max<size_t>(1U, allocationAllowance / slotBytes);
			const size_t batchCapacity =
					preserveVisitorEarlyStop
							? std::min<size_t>(1U, maximumOutputSlots)
							: std::min({DEFAULT_PROPERTY_BATCH_ROWS, bounds.maximum_rows, maximumOutputSlots});
			if (batchCapacity == 0U) {
                // Private compiled nodes can legitimately hold part of the same
                // QUERY account. Report parent pressure as admission denial,
                // preserving the distinction from an invalid local row bound.
                if (bounds.maximum_rows != 0U && bounds.maximum_materialized_bytes >= sizeof(LogicalPlanReadRow) &&
                    allocationContext && allocationContext->resource_account && allocationAllowance < sizeof(LogicalPlanReadRow))
                    throw detail::ResourceAdmissionDenied();
				throw std::runtime_error("logical-plan output materialization exceeds its row or byte bound");
			}
			// The output batch is live while upstream sources (including LOAD CSV)
			// continue reading. Charge it to their shared execution budget.
            // Account actual allocator requests before allocating, including
            // unused output slots. The execution budget separately excludes this
            // resident capacity from its local allowance, without a second parent charge.
            std::optional<MaterializedByteBudget> outputStorage;
            if (allocationContext && allocationContext->resource_account)
                outputStorage.emplace(allocationAllowance, allocationContext->resource_account);
            MaterializedByteReservation bufferedReservation;
            bool budgetBound = false;
            std::pmr::vector<LogicalPlanReadRow> rows(outputStorage
                ? static_cast<std::pmr::memory_resource *>(&*outputStorage) : std::pmr::get_default_resource());
			rows.reserve(batchCapacity);
			const size_t residentOutputBatchBytes = checkedSizeMul(rows.capacity(), sizeof(LogicalPlanReadRow),
																   "logical-plan output batch storage size overflowed");
			if (residentOutputBatchBytes >= bounds.maximum_materialized_bytes) {
				throw std::invalid_argument("logical-plan output batch storage exhausts its materialized byte bound");
			}
			size_t bufferedBytes = 0U;
			bool stoppedByVisitor = false;
			bool finalized = false;
			// The vector's full row-slot capacity is already reserved above. Charge
			// only the row payload here to avoid counting each active object twice.
			const auto outputRowPayloadBytes = [](const LogicalPlanReadRow &row, const ExecutionState &state) {
				return logicalReadRowBytes(row, state.shared.context) - sizeof(LogicalPlanReadRow);
			};
			const auto flush = [&](ExecutionState &state) {
				if (rows.empty() || stoppedByVisitor) {
					return !stoppedByVisitor;
				}
				if (replaceDeferredEntityRefs) {
					materializeEntityRefsInRows(rows, state, bufferedReservation);
				} else {
					prefetchEntityRefsInRows(rows, state);
				}
				size_t materializedBytes = 0U;
				for (const LogicalPlanReadRow &row: rows) {
					materializedBytes = checkedSizeAdd(materializedBytes, outputRowPayloadBytes(row, state),
													   "logical-plan output batch materialization size overflowed");
				}
				bufferedReservation.set(materializedBytes);
				bufferedBytes = materializedBytes;
				for (LogicalPlanReadRow &row: rows) {
					enforceMaterializedRowBound(row, state);
					const bool keepGoing = preserveVisitorEarlyStop
						? invokeOutputVisitorChecked(state, [&] { return emit(row, state); })
						: emit(row, state);
					if (!keepGoing) {
						stoppedByVisitor = true;
						rows.clear();
						bufferedReservation.reset();
						bufferedBytes = 0U;
						return false;
					}
					const size_t releasedBytes = outputRowPayloadBytes(row, state);
					row = {};
					bufferedReservation.set(bufferedReservation.bytes() - releasedBytes);
				}
				rows.clear();
				bufferedReservation.reset();
				bufferedBytes = 0U;
				return true;
			};
			const FinalRowVisitor collect = [&](LogicalPlanReadRow &row, ExecutionState &state) {
				if (!budgetBound) {
					bufferedReservation = MaterializedByteReservation(&state.shared.materialized_budget);
					budgetBound = true;
				}
				const size_t rowBytes = outputRowPayloadBytes(row, state);
				if (!rows.empty() && (rows.size() == batchCapacity ||
									  rowBytes > bounds.maximum_materialized_bytes -
														 std::min(bufferedBytes, bounds.maximum_materialized_bytes) ||
									  rowBytes > state.shared.materialized_budget.available())) {
					if (!flush(state)) {
						return false;
					}
				}
				const size_t nextBytes = checkedSizeAdd(bufferedBytes, rowBytes,
														"logical-plan output batch materialization size overflowed");
				bufferedReservation.set(nextBytes);
				try {
					rows.push_back(std::move(row));
				} catch (...) {
					bufferedReservation.set(bufferedBytes);
					throw;
				}
				bufferedBytes = nextBytes;
				if (rows.size() == batchCapacity) {
					return flush(state);
				}
				return true;
			};
			const ExecutionFinalizer finalize = [&](ExecutionState &state, bool failed) {
				if (!finalized) {
					finalized = true;
					const auto release = [&] {
						std::pmr::vector<LogicalPlanReadRow>(rows.get_allocator().resource()).swap(rows);
						// Detach while the ExecutionState budget is still alive, including zero-charge owners.
						bufferedReservation = MaterializedByteReservation{};
					};
					try {
						if (!failed && !stoppedByVisitor) static_cast<void>(flush(state));
					} catch (...) {
						release();
						throw;
					}
					release();
				}
			};
			const LogicalPlanReadRuntimeStats stats = run(collect, finalize, residentOutputBatchBytes);
			if (!finalized) {
				throw std::logic_error("logical-plan output materialization run did not finalize its execution state");
			}
			return stats;
		}

		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor,
										 const ChargedRowVisitor *chargedReceiver = nullptr,
										 const NodeColumnReceiver *columnReceiver = nullptr);

		struct BatchableEntityPropertyPredicate {
			std::string variable;
			std::string key;
		};

		struct BatchableEntityMetadataPredicate {
			std::string variable;
			std::string function;
		};

		void appendBatchableEntityPropertyPredicate(std::vector<BatchableEntityPropertyPredicate> &result,
													BatchableEntityPropertyPredicate candidate) {
			const bool duplicate = std::ranges::any_of(result, [&](const auto &existing) {
				return existing.variable == candidate.variable && existing.key == candidate.key;
			});
			if (!duplicate) {
				result.push_back(std::move(candidate));
			}
		}

		void appendBatchableEntityMetadataPredicate(std::vector<BatchableEntityMetadataPredicate> &result,
													BatchableEntityMetadataPredicate candidate) {
			const bool duplicate = std::ranges::any_of(result, [&](const auto &existing) {
				return existing.variable == candidate.variable && existing.function == candidate.function;
			});
			if (!duplicate) {
				result.push_back(std::move(candidate));
			}
		}

		void appendCompleteEntityVariable(std::vector<std::string> &result, std::string_view variable) {
			if (variable.empty() || std::ranges::find(result, variable) != result.end()) {
				return;
			}
			result.emplace_back(variable);
		}

		// Scoped list bindings only shadow outer entities inside their bodies;
		// source lists and REDUCE initializers still evaluate in the outer scope.
		// Track exclusions separately from allowed pattern bindings, since the
		// latter also enforce selected-only shape safety for properties()/keys().
		// Identify expression shapes that require the complete Entity payload.  This
		// is deliberately separate from selected-column discovery: a Sort or Project
		// may contain both a safe scalar Property and `properties(n)`/`n{.*}`.  The
		// caller can then materialize the complete bindings once per bounded batch
		// without disabling the selected-column work for sibling expressions.
		[[nodiscard]] bool
		collectCompleteEntityVariables(const expressions::Expression &expression, std::vector<std::string> &result,
									   const std::unordered_set<std::string> *allowedVariables = nullptr,
									   const std::unordered_set<std::string> *shadowedVariables = nullptr) {
			using ExpressionType = expressions::ExpressionType;
			const auto collectNested = [&](const expressions::Expression *nested) {
				return nested == nullptr ||
					   collectCompleteEntityVariables(*nested, result, allowedVariables, shadowedVariables);
			};
			switch (expression.getExpressionType()) {
				case ExpressionType::LITERAL:
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS:
				case ExpressionType::PARAMETER:
				case ExpressionType::SHORTEST_PATH:
					return true;
				case ExpressionType::EXPR_LIST_LITERAL: {
					const auto &list = static_cast<const expressions::ListLiteralExpression &>(expression);
					if (list.isStatic()) {
						return true;
					}
					bool complete = true;
					for (const auto &element: list.getElements()) {
						complete = collectNested(element.get()) && complete;
					}
					return complete;
				}
				case ExpressionType::LIST_COMPREHENSION: {
					const auto &comprehension =
							static_cast<const expressions::ListComprehensionExpression &>(expression);
					const bool listComplete = collectNested(comprehension.getListExpr());
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					scopedVariables.insert(comprehension.getVariable());
					const auto collectScoped = [&](const expressions::Expression *nested) {
						return nested == nullptr ||
							   collectCompleteEntityVariables(*nested, result, allowedVariables, &scopedVariables);
					};
					const bool whereComplete = collectScoped(comprehension.getWhereExpr());
					const bool mapComplete = collectScoped(comprehension.getMapExpr());
					return listComplete && whereComplete && mapComplete;
				}
				case ExpressionType::EXPR_REDUCE: {
					const auto &reduce = static_cast<const expressions::ReduceExpression &>(expression);
					const bool initialComplete = collectNested(reduce.getInitialExpr());
					const bool listComplete = collectNested(reduce.getListExpr());
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					scopedVariables.insert(reduce.getVariable());
					scopedVariables.insert(reduce.getAccumulator());
					const bool bodyComplete = reduce.getBodyExpr() == nullptr ||
											  collectCompleteEntityVariables(*reduce.getBodyExpr(), result,
																			 allowedVariables, &scopedVariables);
					return initialComplete && listComplete && bodyComplete;
				}
				case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
					const auto &quantifier = static_cast<const expressions::QuantifierFunctionExpression &>(expression);
					const bool listComplete = collectNested(quantifier.getListExpression());
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					scopedVariables.insert(quantifier.getVariable());
					const bool whereComplete = quantifier.getWhereExpression() == nullptr ||
											   collectCompleteEntityVariables(*quantifier.getWhereExpression(), result,
																			  allowedVariables, &scopedVariables);
					return listComplete && whereComplete;
				}
				case ExpressionType::BINARY_OP: {
					const auto &binary = static_cast<const expressions::BinaryOpExpression &>(expression);
					if (binary.getLeft() == nullptr || binary.getRight() == nullptr) {
						return false;
					}
					const bool leftComplete = collectNested(binary.getLeft());
					const bool rightComplete = collectNested(binary.getRight());
					return leftComplete && rightComplete;
				}
				case ExpressionType::UNARY_OP: {
					const auto &unary = static_cast<const expressions::UnaryOpExpression &>(expression);
					return unary.getOperand() != nullptr && collectNested(unary.getOperand());
				}
				case ExpressionType::FUNCTION_CALL: {
					const auto &function = static_cast<const expressions::FunctionCallExpression &>(expression);
					const std::string functionName = lowerCase(function.getFunctionName());
					if (functionName == "properties" || functionName == "keys") {
						if (function.getArguments().size() != 1U) {
							return false;
						}
						const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
								function.getArguments().front().get());
						if (reference == nullptr || reference->hasProperty() || reference->getVariableName().empty()) {
							return false;
						}
						if ((allowedVariables == nullptr || allowedVariables->contains(reference->getVariableName())) &&
							(shadowedVariables == nullptr ||
							 !shadowedVariables->contains(reference->getVariableName()))) {
							appendCompleteEntityVariable(result, reference->getVariableName());
						}
						return true;
					}
					bool complete = true;
					for (const auto &argument: function.getArguments()) {
						complete = collectNested(argument.get()) && complete;
					}
					return complete;
				}
				case ExpressionType::IN_LIST: {
					const auto &in = static_cast<const expressions::InExpression &>(expression);
					const bool valueComplete = collectNested(in.getValue());
					const bool listComplete = !in.hasDynamicList() || collectNested(in.getListExpression());
					return valueComplete && listComplete;
				}
				case ExpressionType::IS_NULL: {
					const auto &isNullExpression = static_cast<const expressions::IsNullExpression &>(expression);
					return collectNested(isNullExpression.getExpression());
				}
				case ExpressionType::MAP_PROJECTION: {
					const auto &projection = static_cast<const expressions::MapProjectionExpression &>(expression);
					if (projection.isMapLiteral()) {
						bool complete = true;
						for (const auto &item: projection.getItems()) {
							if (item.type == expressions::MapProjectionItemType::MPROP_PROPERTY ||
								item.type == expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES) {
								complete = false;
								continue;
							}
							complete = collectNested(item.expression.get()) && complete;
						}
						return complete;
					}
					if (projection.getVariable().empty()) {
						return false;
					}
					const bool variableAllowed =
							(allowedVariables == nullptr || allowedVariables->contains(projection.getVariable())) &&
							(shadowedVariables == nullptr || !shadowedVariables->contains(projection.getVariable()));
					bool complete = true;
					for (const auto &item: projection.getItems()) {
						if (item.type == expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES) {
							if (variableAllowed) {
								appendCompleteEntityVariable(result, projection.getVariable());
							}
							continue;
						}
						if (item.type == expressions::MapProjectionItemType::MPROP_LITERAL) {
							complete = collectNested(item.expression.get()) && complete;
						}
					}
					return complete;
				}
				case ExpressionType::CASE_EXPRESSION: {
					const auto &caseExpression = static_cast<const expressions::CaseExpression &>(expression);
					bool complete = collectNested(caseExpression.getComparisonExpression());
					for (const auto &branch: caseExpression.getBranches()) {
						const bool whenComplete = collectNested(branch.whenExpression.get());
						const bool thenComplete = collectNested(branch.thenExpression.get());
						complete = whenComplete && thenComplete && complete;
					}
					return collectNested(caseExpression.getElseExpression()) && complete;
				}
				case ExpressionType::LIST_SLICE: {
					const auto &slice = static_cast<const expressions::ListSliceExpression &>(expression);
					const bool listComplete = collectNested(slice.getList());
					const bool startComplete = collectNested(slice.getStart());
					const bool endComplete = collectNested(slice.getEnd());
					return listComplete && startComplete && endComplete;
				}
				case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
					const auto &pattern = static_cast<const expressions::PatternComprehensionExpression &>(expression);
					if (pattern.getMapExpression() == nullptr)
						return false;
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					if (!pattern.getTargetVar().empty())
						scopedVariables.insert(pattern.getTargetVar());
					if (!pattern.getRelationshipVar().empty())
						scopedVariables.insert(pattern.getRelationshipVar());
					const bool whereComplete = pattern.getWhereExpression() == nullptr ||
											   collectCompleteEntityVariables(*pattern.getWhereExpression(), result,
																			  allowedVariables, &scopedVariables);
					const bool mapComplete = collectCompleteEntityVariables(*pattern.getMapExpression(), result,
																			allowedVariables, &scopedVariables);
					return whereComplete && mapComplete;
				}
				case ExpressionType::EXPR_EXISTS: {
					const auto &exists = static_cast<const expressions::ExistsExpression &>(expression);
					auto scopedVariables =
							shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
					if (!exists.getTargetVar().empty())
						scopedVariables.insert(exists.getTargetVar());
					return exists.getWhereExpression() == nullptr ||
						   collectCompleteEntityVariables(*exists.getWhereExpression(), result, allowedVariables,
														  &scopedVariables);
				}
			}
			return false;
		}

		// Keep complete-Entity prefetch for safe expression fragments when another
		// fragment owns a nested graph scope that this outer-row collector cannot
		// analyze. The recursive collector never enters those scopes, so partial
		// discoveries from supported siblings remain safe to batch.
		[[nodiscard]] std::vector<std::string>
		selectBatchableCompleteEntityVariables(const std::vector<const expressions::Expression *> &candidates,
											   const std::unordered_set<std::string> *allowedVariables = nullptr) {
			std::vector<std::string> result;
			for (const expressions::Expression *candidate: candidates) {
				if (candidate == nullptr) {
					return {};
				}
				std::vector<std::string> expressionVariables;
				static_cast<void>(collectCompleteEntityVariables(*candidate, expressionVariables, allowedVariables));
				for (const std::string &variable: expressionVariables) {
					appendCompleteEntityVariable(result, variable);
				}
			}
			return result;
		}

		[[nodiscard]] bool collectBatchableEntityPropertyPredicates(
				const expressions::Expression &expression, std::vector<BatchableEntityPropertyPredicate> &result,
				const std::unordered_set<std::string> *allowedVariables = nullptr,
				std::vector<BatchableEntityMetadataPredicate> *metadataResult = nullptr,
				const std::unordered_set<std::string> *shadowedVariables = nullptr) {
			using ExpressionType = expressions::ExpressionType;
			const auto collectNested = [&](const expressions::Expression *nested) {
				return nested == nullptr || collectBatchableEntityPropertyPredicates(*nested, result, allowedVariables,
																					 metadataResult, shadowedVariables);
			};
			if (expression.getExpressionType() == ExpressionType::PROPERTY_ACCESS) {
				const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(expression);
				if (!reference.hasProperty() || reference.getVariableName().empty() ||
					reference.getPropertyName().empty()) {
					return false;
				}
				if ((allowedVariables != nullptr && !allowedVariables->contains(reference.getVariableName())) ||
					(shadowedVariables != nullptr && shadowedVariables->contains(reference.getVariableName()))) {
					// Pattern-local target/relationship bindings are populated only while
					// visiting a candidate. They remain on the semantic evaluator path;
					// outer-row properties can still be prefetched safely.
					return true;
				}
				appendBatchableEntityPropertyPredicate(result,
													   {reference.getVariableName(), reference.getPropertyName()});
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::VARIABLE_REFERENCE) {
				// A scalar binding does not require a storage read. Treat it as a
				// batchable leaf so comprehensions can still prefetch outer entity
				// properties used alongside the scoped scalar.
				const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(expression);
				return !reference.hasProperty();
			}
			if (expression.getExpressionType() == ExpressionType::SHORTEST_PATH)
				return true;
			if (expression.getExpressionType() == ExpressionType::LITERAL ||
				expression.getExpressionType() == ExpressionType::PARAMETER) {
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_LIST_LITERAL) {
				const auto &list = static_cast<const expressions::ListLiteralExpression &>(expression);
				if (list.isStatic()) {
					return true;
				}
				for (const auto &element: list.getElements()) {
					if (!collectNested(element.get())) {
						return false;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::LIST_COMPREHENSION) {
				const auto &comprehension = static_cast<const expressions::ListComprehensionExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				scopedVariables.insert(comprehension.getVariable());
				const auto collectScoped = [&](const expressions::Expression *nested) {
					return nested == nullptr ||
						   collectBatchableEntityPropertyPredicates(*nested, result, allowedVariables, metadataResult,
																	&scopedVariables);
				};
				return collectNested(comprehension.getListExpr()) && collectScoped(comprehension.getWhereExpr()) &&
					   collectScoped(comprehension.getMapExpr());
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_REDUCE) {
				const auto &reduce = static_cast<const expressions::ReduceExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				scopedVariables.insert(reduce.getVariable());
				scopedVariables.insert(reduce.getAccumulator());
				return collectNested(reduce.getInitialExpr()) && collectNested(reduce.getListExpr()) &&
					   (reduce.getBodyExpr() == nullptr ||
						collectBatchableEntityPropertyPredicates(*reduce.getBodyExpr(), result, allowedVariables,
																 metadataResult, &scopedVariables));
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_QUANTIFIER_FUNCTION) {
				const auto &quantifier = static_cast<const expressions::QuantifierFunctionExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				scopedVariables.insert(quantifier.getVariable());
				return collectNested(quantifier.getListExpression()) &&
					   (quantifier.getWhereExpression() == nullptr ||
						collectBatchableEntityPropertyPredicates(*quantifier.getWhereExpression(), result,
																 allowedVariables, metadataResult, &scopedVariables));
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_PATTERN_COMPREHENSION) {
				const auto &pattern = static_cast<const expressions::PatternComprehensionExpression &>(expression);
				if (pattern.getMapExpression() == nullptr)
					return false;
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				if (!pattern.getTargetVar().empty())
					scopedVariables.insert(pattern.getTargetVar());
				if (!pattern.getRelationshipVar().empty())
					scopedVariables.insert(pattern.getRelationshipVar());
				const auto collectScoped = [&](const expressions::Expression *nested) {
					return nested == nullptr ||
						   collectBatchableEntityPropertyPredicates(*nested, result, allowedVariables, metadataResult,
																	&scopedVariables);
				};
				return collectScoped(pattern.getWhereExpression()) && collectScoped(pattern.getMapExpression());
			}
			if (expression.getExpressionType() == ExpressionType::EXPR_EXISTS) {
				const auto &exists = static_cast<const expressions::ExistsExpression &>(expression);
				auto scopedVariables =
						shadowedVariables == nullptr ? std::unordered_set<std::string>{} : *shadowedVariables;
				if (!exists.getTargetVar().empty())
					scopedVariables.insert(exists.getTargetVar());
				return exists.getWhereExpression() == nullptr ||
					   collectBatchableEntityPropertyPredicates(*exists.getWhereExpression(), result, allowedVariables,
																metadataResult, &scopedVariables);
			}
			if (expression.getExpressionType() == ExpressionType::BINARY_OP) {
				const auto &binary = static_cast<const expressions::BinaryOpExpression &>(expression);
				return binary.getLeft() != nullptr && binary.getRight() != nullptr && collectNested(binary.getLeft()) &&
					   collectNested(binary.getRight());
			}
			if (expression.getExpressionType() == ExpressionType::UNARY_OP) {
				const auto &unary = static_cast<const expressions::UnaryOpExpression &>(expression);
				return unary.getOperand() != nullptr && collectNested(unary.getOperand());
			}
			if (expression.getExpressionType() == ExpressionType::FUNCTION_CALL) {
				const auto &function = static_cast<const expressions::FunctionCallExpression &>(expression);
				const std::string functionName = lowerCase(function.getFunctionName());
				if (metadataResult != nullptr && (functionName == "labels" || functionName == "type") &&
					function.getArguments().size() == 1U) {
					const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
							function.getArguments().front().get());
					if (reference != nullptr && !reference->hasProperty() && !reference->getVariableName().empty() &&
						(allowedVariables == nullptr || allowedVariables->contains(reference->getVariableName())) &&
						(shadowedVariables == nullptr || !shadowedVariables->contains(reference->getVariableName()))) {
						appendBatchableEntityMetadataPredicate(*metadataResult,
															   {reference->getVariableName(), functionName});
					}
				}
				// `properties(entity)` and `keys(entity)` observe the complete map;
				// a selected-column pattern shape must not silently turn them into a
				// partial map.  Other scalar/entity metadata functions are safe because
				// pattern reads always retain labels and relationship type metadata.
				if (allowedVariables != nullptr && (functionName == "properties" || functionName == "keys") &&
					function.getArguments().size() == 1U) {
					const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(
							function.getArguments().front().get());
					if (reference != nullptr && !reference->hasProperty() &&
						allowedVariables->contains(reference->getVariableName()) &&
						(shadowedVariables == nullptr || !shadowedVariables->contains(reference->getVariableName()))) {
						return false;
					}
				}
				for (const auto &argument: function.getArguments()) {
					if (!collectNested(argument.get())) {
						return false;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::IN_LIST) {
				const auto &in = static_cast<const expressions::InExpression &>(expression);
				if (!collectNested(in.getValue())) {
					return false;
				}
				return !in.hasDynamicList() ||
					   (in.getListExpression() != nullptr && collectNested(in.getListExpression()));
			}
			if (expression.getExpressionType() == ExpressionType::IS_NULL) {
				const auto &isNullExpression = static_cast<const expressions::IsNullExpression &>(expression);
				return collectNested(isNullExpression.getExpression());
			}
			if (expression.getExpressionType() == ExpressionType::MAP_PROJECTION) {
				const auto &projection = static_cast<const expressions::MapProjectionExpression &>(expression);
				if (projection.isMapLiteral()) {
					for (const auto &item: projection.getItems()) {
						if (item.type == expressions::MapProjectionItemType::MPROP_PROPERTY ||
							item.type == expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES) {
							// A map literal cannot select entity properties. Returning false
							// preserves the normal evaluator's validation and NULL semantics.
							return false;
						}
						if (!collectNested(item.expression.get())) {
							return false;
						}
					}
					return true;
				}
				if (projection.getVariable().empty()) {
					return false;
				}
				const bool variableIsBatchable =
						(allowedVariables == nullptr || allowedVariables->contains(projection.getVariable())) &&
						(shadowedVariables == nullptr || !shadowedVariables->contains(projection.getVariable()));
				for (const auto &item: projection.getItems()) {
					switch (item.type) {
						case expressions::MapProjectionItemType::MPROP_PROPERTY:
							if (item.key.empty()) {
								return false;
							}
							if (variableIsBatchable) {
								appendBatchableEntityPropertyPredicate(result, {projection.getVariable(), item.key});
							}
							break;
						case expressions::MapProjectionItemType::MPROP_LITERAL:
							if (!collectNested(item.expression.get())) {
								return false;
							}
							break;
						case expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES:
							// `.*` requires the complete Property map and cannot use a
							// selected-column batch without changing its contract.
							if (variableIsBatchable) {
								return false;
							}
							break;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::CASE_EXPRESSION) {
				const auto &caseExpression = static_cast<const expressions::CaseExpression &>(expression);
				if (!collectNested(caseExpression.getComparisonExpression())) {
					return false;
				}
				for (const auto &branch: caseExpression.getBranches()) {
					if (!collectNested(branch.whenExpression.get()) || !collectNested(branch.thenExpression.get())) {
						return false;
					}
				}
				return collectNested(caseExpression.getElseExpression());
			}
			if (expression.getExpressionType() == ExpressionType::LIST_SLICE) {
				const auto &slice = static_cast<const expressions::ListSliceExpression &>(expression);
				return collectNested(slice.getList()) && collectNested(slice.getStart()) &&
					   collectNested(slice.getEnd());
			}
			return false;
		}

		// A logical operator may combine storage-backed expressions with shapes that
		// intentionally require complete entities or scoped path evaluation.  Keep
		// the safe subset batched instead of disabling batching for the whole operator.
		[[nodiscard]] std::vector<const expressions::Expression *>
		selectBatchableExpressions(const std::vector<const expressions::Expression *> &expressions) {
			std::vector<const expressions::Expression *> selected;
			selected.reserve(expressions.size());
			for (const expressions::Expression *expression: expressions) {
				if (expression == nullptr) {
					continue;
				}
				std::vector<BatchableEntityPropertyPredicate> properties;
				std::vector<BatchableEntityMetadataPredicate> metadata;
				if (collectBatchableEntityPropertyPredicates(*expression, properties, nullptr, &metadata) &&
					(!properties.empty() || !metadata.empty())) {
					selected.push_back(expression);
				}
			}
			return selected;
		}

		// A read-only correlated subquery executes once for each outer row, but a
		// simple unary body can still share its imported Entity Property reads.  Walk
		// only the expression-bearing unary operators here; anything more complex
		// keeps the normal per-row evaluator so nested scope and cardinality semantics
		// remain authoritative.
		[[nodiscard]] std::vector<const expressions::Expression *>
		collectCorrelatedSubqueryExpressions(const logical::LogicalOperator &body) {
			std::vector<const expressions::Expression *> expressions;
			const logical::LogicalOperator *current = &body;
			while (current != nullptr) {
				switch (current->getType()) {
					case logical::LogicalOpType::LOP_PROJECT: {
						const auto &project = dynamic_cast<const logical::LogicalProject &>(*current);
						for (const logical::LogicalProjectItem &item: project.getItems()) {
							expressions.push_back(item.expression.get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_FILTER: {
						const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*current);
						expressions.push_back(filter.getPredicate().get());
						break;
					}
					case logical::LogicalOpType::LOP_SORT: {
						const auto &sort = dynamic_cast<const logical::LogicalSort &>(*current);
						for (const logical::LogicalSortItem &item: sort.getSortItems()) {
							expressions.push_back(item.expression.get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_AGGREGATE: {
						const auto &aggregate = dynamic_cast<const logical::LogicalAggregate &>(*current);
						for (const auto &expression: aggregate.getGroupByExprs()) {
							expressions.push_back(expression.get());
						}
						for (const logical::LogicalAggItem &item: aggregate.getAggregations()) {
							expressions.push_back(item.argument.get());
							expressions.push_back(item.extraArg.get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_SKIP:
					case logical::LogicalOpType::LOP_LIMIT:
						break;
					case logical::LogicalOpType::LOP_UNWIND: {
						const auto &unwind = dynamic_cast<const logical::LogicalUnwind &>(*current);
						if (!unwind.hasLiteralList() && unwind.getListExpr() != nullptr) {
							expressions.push_back(unwind.getListExpr().get());
						}
						break;
					}
					case logical::LogicalOpType::LOP_SINGLE_ROW:
						return expressions;
					default:
						return {};
				}
				const std::vector<logical::LogicalOperator *> children = current->getChildren();
				if (children.size() != 1U || children.front() == nullptr) {
					return {};
				}
				current = children.front();
			}
			return {};
		}

		// Collect direct property accesses from side-effect-free expression shapes;
		// the original evaluator remains authoritative after the batch is filled.
		[[nodiscard]] std::optional<PatternBindingReadShape>
		inspectPatternBindingReadShape(const expressions::Expression *whereExpression,
									   const expressions::Expression *mapExpression, std::string_view targetVariable,
									   std::string_view relationshipVariable,
									   storage::LogicalGraphReadSnapshot &snapshot) {
			std::unordered_set<std::string> allowedVariables;
			if (!targetVariable.empty()) {
				allowedVariables.emplace(targetVariable);
			}
			if (!relationshipVariable.empty()) {
				allowedVariables.emplace(relationshipVariable);
			}
			std::vector<std::string> completeVariables;
			for (const auto *expression: {whereExpression, mapExpression}) {
				if (expression != nullptr &&
					!collectCompleteEntityVariables(*expression, completeVariables, &allowedVariables)) {
					return std::nullopt;
				}
			}
			auto selectedVariables = allowedVariables;
			for (const auto &variable: completeVariables)
				selectedVariables.erase(variable);
			std::vector<BatchableEntityPropertyPredicate> requests;
			for (const auto *expression: {whereExpression, mapExpression}) {
				if (expression != nullptr &&
					!collectBatchableEntityPropertyPredicates(*expression, requests, &selectedVariables)) {
					return std::nullopt;
				}
			}
			PatternBindingReadShape shape;
			shape.target.include_labels = !targetVariable.empty();
			shape.relationship.include_type = !relationshipVariable.empty();
			shape.target.all_properties =
					std::ranges::find(completeVariables, targetVariable) != completeVariables.end();
			shape.relationship.all_properties =
					std::ranges::find(completeVariables, relationshipVariable) != completeVariables.end();
			for (const BatchableEntityPropertyPredicate &request: requests) {
				const std::optional<uint32_t> token = snapshot.findPropertyToken(request.key);
				storage::EntityReadShape *destination = nullptr;
				std::vector<std::string> *namedProperties = nullptr;
				if (request.variable == targetVariable) {
					destination = &shape.target;
					namedProperties = &shape.target_named_properties;
				} else if (request.variable == relationshipVariable) {
					destination = &shape.relationship;
					namedProperties = &shape.relationship_named_properties;
				} else {
					throw std::logic_error("logical-plan pattern shape resolved an unknown scoped Entity variable");
				}
				if (token)
					destination->property_tokens.push_back(*token);
				else
					namedProperties->push_back(request.key);
			}
			const auto normalize = [](storage::EntityReadShape &entityShape) {
				std::ranges::sort(entityShape.property_tokens);
				entityShape.property_tokens.erase(
						std::unique(entityShape.property_tokens.begin(), entityShape.property_tokens.end()),
						entityShape.property_tokens.end());
				validateEntityReadShape(entityShape);
			};
			normalize(shape.target);
			normalize(shape.relationship);
			for (auto *keys: {&shape.target_named_properties, &shape.relationship_named_properties}) {
				std::ranges::sort(*keys);
				keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
			}
			return shape;
		}

		// Prefetch selected Properties for any storage-neutral row producer.  Keeping
		// this boundary independent of a particular logical operator lets filters,
		// projections, and aggregates share the same bounded page-grouped read path.
		[[nodiscard]] std::optional<bool>
		tryVisitBatchedPropertyExpressions(const logical::LogicalOperator &child,
										   const std::vector<const expressions::Expression *> &expressions,
										   ExecutionState &state, const InternalVisitor &visitor,
										   const BatchSelectionPredicate &selectionPredicate = {},
										   const std::vector<std::string> &completeEntityVariables = {},
										   const ChargedRowVisitor *chargedReceiver = nullptr,
										   bool borrowRepeatedNodeColumns = false) {
			if (!state.shared.allow_deferred_entity_refs ||
				(!state.shared.snapshot.supportsNativeNodeRefScans() && !state.shared.snapshot.supportsNativeEdgeRefScans())) {
				return std::nullopt;
			}
			std::vector<BatchableEntityPropertyPredicate> requests;
			std::vector<BatchableEntityMetadataPredicate> metadataRequests;
			for (const expressions::Expression *expression: expressions) {
				if (expression == nullptr) {
					return std::nullopt;
				}
				std::vector<BatchableEntityPropertyPredicate> nestedRequests;
				if (!collectBatchableEntityPropertyPredicates(*expression, nestedRequests, nullptr,
															  &metadataRequests)) {
					return std::nullopt;
				}
				for (const auto &candidate: nestedRequests) {
					if (!std::ranges::any_of(requests, [&](const auto &existing) {
							return existing.variable == candidate.variable && existing.key == candidate.key;
						})) {
						requests.push_back(candidate);
					}
				}
			}
			if (requests.empty() && metadataRequests.empty() && completeEntityVariables.empty()) {
				return std::nullopt;
			}
			// A property expression can outlive the entity binding that produced it
			// (for example, an outer projection over `WITH DISTINCT n.value`).  In
			// that shape the child row intentionally contains only the projected
			// scalar, so this batch helper must defer to the normal evaluator rather
			// than treating the missing binding as a corrupt storage result.
			const std::vector<std::string> childOutputs = child.getOutputVariables();
			for (const BatchableEntityPropertyPredicate &request: requests) {
				if (std::ranges::find(childOutputs, request.variable) == childOutputs.end()) {
					return std::nullopt;
				}
			}
			for (const BatchableEntityMetadataPredicate &request: metadataRequests) {
				if (std::ranges::find(childOutputs, request.variable) == childOutputs.end()) {
					return std::nullopt;
				}
			}
			for (const std::string &variable: completeEntityVariables) {
				if (std::ranges::find(childOutputs, variable) == childOutputs.end()) {
					return std::nullopt;
				}
			}

			struct PropertyGroup {
				std::string variable;
				std::vector<std::string> keys;
			};
			std::vector<PropertyGroup> groups;
			for (const auto &request: requests) {
				auto group = std::ranges::find(groups, request.variable, &PropertyGroup::variable);
				if (group == groups.end()) {
					groups.push_back({request.variable, {request.key}});
				} else if (std::ranges::find(group->keys, request.key) == group->keys.end()) {
					group->keys.push_back(request.key);
				}
			}
			struct MetadataGroup {
				std::string variable;
				bool labels = false;
				bool type = false;
			};
			std::vector<MetadataGroup> metadataGroups;
			for (const BatchableEntityMetadataPredicate &request: metadataRequests) {
				auto group = std::ranges::find(metadataGroups, request.variable, &MetadataGroup::variable);
				if (group == metadataGroups.end()) {
					metadataGroups.push_back(
							{request.variable, request.function == "labels", request.function == "type"});
				} else if (request.function == "labels") {
					group->labels = true;
				} else if (request.function == "type") {
					group->type = true;
				}
			}

			// Reserve only a fraction of the working allowance for pending row
            // carriers. Split retries must retain their unprocessed siblings;
            // filling a tiny budget with carriers can starve even one payload.
            const size_t carrierCapacity = std::max<size_t>(1U,
                state.shared.materialized_budget.available() / 8U / sizeof(LogicalPlanReadRow));
            const size_t batchCapacity = std::min({DEFAULT_PROPERTY_BATCH_ROWS,
                state.shared.bounds.maximum_rows, carrierCapacity});
			if (batchCapacity == 0U) {
				return std::nullopt;
			}
			const bool coordinatedStorage = state.shared.context && state.shared.context->resource_account;
            MaterializedByteReservation legacyBufferedRowsBytes(&state.shared.materialized_budget);
            detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
            BoundedBatch<LogicalPlanReadRow> rows(
                batchCapacity, state.shared.bounds.maximum_materialized_bytes,
                [&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
                coordinatedStorage ? &state.shared.materialized_budget : nullptr, true, sizeof(LogicalPlanReadRow));
            auto &bufferedRowsBytes = coordinatedStorage ? rows.enrichmentReservation() : legacyBufferedRowsBytes;
            rows.reserve(batchCapacity);
			bool stoppedByVisitor = false;
			size_t largestInputRowBytes = 0U;
			std::function<bool()> flush;
			struct PrefetchBytePressure {};
			const auto putCacheValue = [&](auto &cache, const std::string &variable, const std::string &key,
										   auto &&value) {
				const size_t previousBytes = bufferedRowsBytes.bytes();
				size_t nextBytes = previousBytes;
				const auto alias = cache.find(variable);
				if (alias == cache.end()) {
					nextBytes =
							checkedSizeAdd(nextBytes, variable.size(), "logical-plan prefetch cache size overflowed");
				}
				if (alias != cache.end() && alias->second.contains(key)) {
					nextBytes -= retainedExpressionValueBytes(alias->second.at(key), state);
				} else {
					nextBytes = checkedSizeAdd(nextBytes, key.size(), "logical-plan prefetch cache size overflowed");
				}
				const size_t requestedPayloadBytes = retainedExpressionValueBytes(value, state);
				nextBytes = checkedSizeAdd(nextBytes, requestedPayloadBytes,
										   "logical-plan prefetch cache size overflowed");
				const size_t withSelection =
						checkedSizeAdd(nextBytes, SelectionVector::requiredStorageBytes(rows.size()),
									   "logical-plan prefetch selection size overflowed");
				if (nextBytes > state.shared.bounds.maximum_materialized_bytes ||
					bufferedRowsBytes.wouldExceed(withSelection)) {
					throw PrefetchBytePressure{};
				}
				bufferedRowsBytes.set(nextBytes);
				try {
					if constexpr (std::is_lvalue_reference_v<decltype(value)>) {
						PropertyValue copy = detail::copyPropertyValueGuarded(value, [&] { checkGuardNow(state); });
						cache[variable].insert_or_assign(key, std::move(copy));
					} else {
						cache[variable].insert_or_assign(key, std::forward<decltype(value)>(value));
					}
				} catch (...) {
					bufferedRowsBytes.set(previousBytes);
					throw;
				}
				// Copying a container can change its capacity. Reconcile the owned
				// value before a pressure-driven split transfers charges to rows.
				const size_t retainedBytes = retainedExpressionValueBytes(cache.at(variable).at(key), state);
				const size_t actualBytes = checkedSizeAdd(nextBytes - requestedPayloadBytes, retainedBytes,
					"logical-plan copied cache size overflowed");
				// If a copy unexpectedly grows beyond admission, fail closed rather
				// than entering a split with an undercharged, already-mutated row.
				bufferedRowsBytes.set(actualBytes);
				if (debug::PerfTrace::areValuesEnabled()) {
					// Diagnose row-cache scatter separately from storage reads. A diagnostic
					// allocation failure must not reject an already admitted cache value.
					try {
						constexpr bool copied = std::is_lvalue_reference_v<decltype(value)>;
						debug::PerfTrace::addValue(copied ? "logical_plan.prefetch.copied_values" : "logical_plan.prefetch.moved_values", 1);
						debug::PerfTrace::addValue(copied ? "logical_plan.prefetch.copied_retained_bytes" : "logical_plan.prefetch.moved_retained_bytes",
							static_cast<int64_t>(std::min(retainedBytes,static_cast<size_t>((std::numeric_limits<int64_t>::max)()))));
					} catch (...) {}
				}
			};
			const auto hasMetadata = [](const LogicalPlanReadRow &row, const std::string &variable,
										const std::string &key) {
				const auto found = row.entity_metadata_cache.find(variable);
				return found != row.entity_metadata_cache.end() && found->second.contains(key);
			};
			const auto splitBufferedRowsAndFlush = [&]() {
				rows.recalculateBytes();
				MaterializedByteReservation pendingBytes(std::move(bufferedRowsBytes));
				bufferedRowsBytes = MaterializedByteReservation(&state.shared.materialized_budget);
				auto bufferedRows = std::move(rows.values());
				rows.clear();
				const size_t splitCapacity = std::max<size_t>(1U, bufferedRows.size() / 2U);
				std::reverse(bufferedRows.begin(), bufferedRows.end());
				while (!bufferedRows.empty()) {
					if (!rows.empty() && rows.wouldExceedBytes(bufferedRows.back()) && !flush())
						return false;
					const size_t rowBytes = logicalReadRowBytes(bufferedRows.back(), state.shared.context);
					LogicalPlanReadRow row = std::move(bufferedRows.back());
					bufferedRows.pop_back();
					MaterializedByteReservation rowCharge = coordinatedStorage
                        ? pendingBytes.splitOffAccounting(rowBytes, sizeof(LogicalPlanReadRow))
                        : pendingBytes.splitOff(rowBytes);
                    if (coordinatedStorage) rows.appendCharged(std::move(row), std::move(rowCharge));
                    else {
                        rows.append(std::move(row));
                        bufferedRowsBytes.absorb(std::move(rowCharge));
                    }
					if (rows.size() == splitCapacity && !flush())
						return false;
				}
				return rows.empty() || flush();
			};
			flush = [&]() {
				if (rows.empty()) {
					return true;
				}
				checkGuard(state);
				try {
					std::optional<ScopedPropertyColumns> ownedColumns;
					MaterializedByteReservation ownedColumnBytes(&state.shared.materialized_budget);
					if (!completeEntityVariables.empty()) {
						MaterializedByteReservation referenceWorkspaceBytes(&state.shared.materialized_budget);
						size_t maximumNodeReferences = 0U;
						size_t maximumEdgeReferences = 0U;
						for (const LogicalPlanReadRow &row: rows.values()) {
							for (const std::string &variable: completeEntityVariables) {
								const auto found = row.values.find(variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan complete Entity batch lost its binding");
								}
								if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
									validateEntityRef(*reference);
									size_t &count = reference->is_edge ? maximumEdgeReferences : maximumNodeReferences;
									count = checkedSizeAdd(count, 1U,
												  "logical-plan complete Entity reference count overflowed");
								}
							}
						}
						const auto referenceBytes = [](size_t nodeCount, size_t edgeCount) {
							return checkedSizeAdd(
								checkedSizeMul(nodeCount, sizeof(storage::EntityRef),
											  "logical-plan complete Entity reference buffer size overflowed"),
								checkedSizeMul(edgeCount, sizeof(storage::EntityRef),
											  "logical-plan complete Entity reference buffer size overflowed"),
								"logical-plan complete Entity reference buffer size overflowed");
						};
						const size_t maximumReferenceBytes =
							referenceBytes(maximumNodeReferences, maximumEdgeReferences);
						if (referenceWorkspaceBytes.wouldExceed(maximumReferenceBytes))
							throw PrefetchBytePressure{};
						referenceWorkspaceBytes.set(maximumReferenceBytes);
						std::vector<storage::EntityRef> nodeReferences;
						std::vector<storage::EntityRef> edgeReferences;
						nodeReferences.reserve(maximumNodeReferences);
						edgeReferences.reserve(maximumEdgeReferences);
						const size_t actualReferenceBytes =
							referenceBytes(nodeReferences.capacity(), edgeReferences.capacity());
						if (referenceWorkspaceBytes.wouldExceed(actualReferenceBytes))
							throw PrefetchBytePressure{};
						referenceWorkspaceBytes.set(actualReferenceBytes);
						for (const LogicalPlanReadRow &row: rows.values()) {
							for (const std::string &variable: completeEntityVariables) {
								const auto found = row.values.find(variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan complete Entity batch lost its binding");
								}
								if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
									validateEntityRef(*reference);
									(reference->is_edge ? edgeReferences : nodeReferences).push_back(*reference);
								}
							}
						}
						const auto materializeForPrefetch = [](auto &&materialize) {
							try {
								materialize();
							} catch (const std::runtime_error &error) {
								if (std::string_view(error.what()) != "shared materialization exceeded its byte bound")
									throw;
								throw PrefetchBytePressure{};
							}
						};
						if (!nodeReferences.empty()) {
							materializeForPrefetch([&] {
								static_cast<void>(materializeNodeReferences(nodeReferences, state, false));
							});
						}
						if (!edgeReferences.empty()) {
							materializeForPrefetch([&] {
								static_cast<void>(materializeEdgeReferences(edgeReferences, state, false));
							});
						}
					}
					if (!metadataGroups.empty()) {
						MaterializedByteReservation metadataWorkspaceBytes(&state.shared.materialized_budget);
						size_t maximumNodeIds = 0U;
						size_t maximumEdgeIds = 0U;
						for (const MetadataGroup &group: metadataGroups) {
							for (const LogicalPlanReadRow &row: rows.values()) {
								const auto found = row.values.find(group.variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan metadata batch lost its Entity binding");
								}
								const auto *reference = std::get_if<storage::EntityRef>(&found->second);
								if (reference == nullptr)
									continue;
								validateEntityRef(*reference);
								const auto cached = row.entity_metadata_cache.find(group.variable);
								if (!reference->is_edge && group.labels &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("labels")) &&
									!state.shared.node_cache.contains(reference->entity_id)) {
									maximumNodeIds = checkedSizeAdd(maximumNodeIds, 1U,
																	"logical-plan metadata Node ID count overflowed");
								}
								if (reference->is_edge && group.type &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("type")) &&
									!state.shared.edge_cache.contains(reference->entity_id)) {
									maximumEdgeIds = checkedSizeAdd(maximumEdgeIds, 1U,
																	"logical-plan metadata Edge ID count overflowed");
								}
							}
						}
						const size_t maximumIdBytes =
								checkedSizeAdd(checkedSizeMul(maximumNodeIds, sizeof(uint64_t),
															  "logical-plan metadata ID scratch size overflowed"),
											   checkedSizeMul(maximumEdgeIds, sizeof(uint64_t),
															  "logical-plan metadata ID scratch size overflowed"),
											   "logical-plan metadata ID scratch size overflowed");
						metadataWorkspaceBytes.set(maximumIdBytes);
						std::vector<uint64_t> nodeIds;
						std::vector<uint64_t> edgeIds;
						nodeIds.reserve(maximumNodeIds);
						edgeIds.reserve(maximumEdgeIds);
						metadataWorkspaceBytes.set(
								checkedSizeAdd(checkedSizeMul(nodeIds.capacity(), sizeof(uint64_t),
															  "logical-plan metadata Node ID capacity overflowed"),
											   checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
															  "logical-plan metadata Edge ID capacity overflowed"),
											   "logical-plan metadata ID capacity overflowed"));
						for (const MetadataGroup &group: metadataGroups) {
							for (const LogicalPlanReadRow &row: rows.values()) {
								const auto found = row.values.find(group.variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan metadata batch lost its Entity binding");
								}
								const auto cached = row.entity_metadata_cache.find(group.variable);
								const auto *reference = std::get_if<storage::EntityRef>(&found->second);
								if (reference == nullptr) {
									continue;
								}
								validateEntityRef(*reference);
								if (!reference->is_edge && group.labels &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("labels"))) {
									if (!state.shared.node_cache.contains(reference->entity_id)) {
										nodeIds.push_back(reference->entity_id);
									}
								}
								if (reference->is_edge && group.type &&
									(cached == row.entity_metadata_cache.end() || !cached->second.contains("type"))) {
									if (!state.shared.edge_cache.contains(reference->entity_id)) {
										edgeIds.push_back(reference->entity_id);
									}
								}
							}
						}
						std::ranges::sort(nodeIds);
						nodeIds.erase(std::unique(nodeIds.begin(), nodeIds.end()), nodeIds.end());
						std::ranges::sort(edgeIds);
						edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());
						if (nodeIds.size() > state.shared.bounds.maximum_node_records ||
							edgeIds.size() > state.shared.bounds.maximum_edge_records) {
							throw std::runtime_error("logical-plan metadata batch exceeded its Entity bound");
						}
						std::vector<storage::LogicalGraphNode> loadedNodes;
						std::vector<storage::LogicalGraphEdge> loadedEdges;
						const auto metadataFrameBytes = [&](const size_t nodeCapacity, const size_t edgeCapacity) {
							return checkedSizeAdd(
									checkedSizeAdd(checkedSizeMul(nodeIds.capacity(), sizeof(uint64_t),
																  "logical-plan metadata ID capacity overflowed"),
												   checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
																  "logical-plan metadata ID capacity overflowed"),
												   "logical-plan metadata ID capacity overflowed"),
									checkedSizeAdd(checkedSizeMul(nodeCapacity, sizeof(storage::LogicalGraphNode),
																  "logical-plan metadata Node frame size overflowed"),
												   checkedSizeMul(edgeCapacity, sizeof(storage::LogicalGraphEdge),
																  "logical-plan metadata Edge frame size overflowed"),
												   "logical-plan metadata record frame size overflowed"),
									"logical-plan metadata workspace frame size overflowed");
						};
						metadataWorkspaceBytes.set(metadataFrameBytes(nodeIds.size(), edgeIds.size()));
						loadedNodes.reserve(nodeIds.size());
						loadedEdges.reserve(edgeIds.size());
						metadataWorkspaceBytes.set(metadataFrameBytes(loadedNodes.capacity(), loadedEdges.capacity()));
						for (size_t offset = 0U; offset < nodeIds.size();) {
							checkGuard(state);
							const size_t end = std::min(nodeIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							std::vector<storage::LogicalGraphNode> batch = readNodesShapedSortedBudgeted(
									state, std::span<const uint64_t>(nodeIds).subspan(offset, end - offset),
									{.include_labels = true}, &sourceBytes);
							if (batch.size() != end - offset) {
								throw std::runtime_error(
										"logical graph backend changed metadata Node batch cardinality");
							}
							for (size_t index = 0U; index < batch.size(); ++index) {
								validateNode(batch[index], state);
								if (batch[index].node_id != nodeIds[offset + index]) {
									throw std::runtime_error(
											"logical graph metadata Node batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							size_t batchPayloadBytes = 0U;
							for (const storage::LogicalGraphNode &node: batch) {
								batchPayloadBytes =
										checkedSizeAdd(batchPayloadBytes, logicalNodeCacheBytes(node, state.shared.context) - sizeof(node),
													   "logical-plan metadata Node payload size overflowed");
							}
							for (storage::LogicalGraphNode &node: batch)
								loadedNodes.push_back(std::move(node));
							metadataWorkspaceBytes.absorb(sourceBytes.splitOff(batchPayloadBytes));
							offset = end;
						}
						for (size_t offset = 0U; offset < edgeIds.size();) {
							checkGuard(state);
							const size_t end = std::min(edgeIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
							MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
							std::vector<storage::LogicalGraphEdge> batch = readEdgesShapedSortedBudgeted(
									state, std::span<const uint64_t>(edgeIds).subspan(offset, end - offset),
									{.include_type = true}, &sourceBytes);
							if (batch.size() != end - offset) {
								throw std::runtime_error(
										"logical graph backend changed metadata Edge batch cardinality");
							}
							for (size_t index = 0U; index < batch.size(); ++index) {
								validateEdge(batch[index]);
								if (batch[index].edge_id != edgeIds[offset + index]) {
									throw std::runtime_error(
											"logical graph metadata Edge batch changed logical ID order");
								}
							}
							retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
							size_t batchPayloadBytes = 0U;
							for (const storage::LogicalGraphEdge &edge: batch) {
								batchPayloadBytes =
										checkedSizeAdd(batchPayloadBytes, logicalEdgeCacheBytes(edge, state.shared.context) - sizeof(edge),
													   "logical-plan metadata Edge payload size overflowed");
							}
							for (storage::LogicalGraphEdge &edge: batch)
								loadedEdges.push_back(std::move(edge));
							metadataWorkspaceBytes.absorb(sourceBytes.splitOff(batchPayloadBytes));
							offset = end;
						}
						const auto putLabels = [&](LogicalPlanReadRow &row, const std::string &variable,
												   const std::vector<std::string> &labels) {
							std::vector<PropertyValue> values;
							values.reserve(labels.size());
							for (const std::string &label: labels) {
								values.emplace_back(label);
							}
							putCacheValue(row.entity_metadata_cache, variable, "labels",
										  PropertyValue(std::move(values)));
						};
						const auto putType = [&](LogicalPlanReadRow &row, const std::string &variable,
												 const std::string &type) {
							putCacheValue(row.entity_metadata_cache, variable, "type", PropertyValue(type));
						};
						for (LogicalPlanReadRow &row: rows.values()) {
							for (const MetadataGroup &group: metadataGroups) {
								const auto found = row.values.find(group.variable);
								if (found == row.values.end()) {
									throw std::runtime_error("logical-plan metadata batch lost its Entity binding");
								}
								if (const auto *node = std::get_if<storage::LogicalGraphNode>(&found->second)) {
									if (group.labels && !hasMetadata(row, group.variable, "labels")) {
										putLabels(row, group.variable, node->labels);
									}
									continue;
								}
								if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&found->second)) {
									if (group.type && !hasMetadata(row, group.variable, "type")) {
										putType(row, group.variable, edge->type);
									}
									continue;
								}
								const auto *reference = std::get_if<storage::EntityRef>(&found->second);
								if (reference == nullptr) {
									continue;
								}
								validateEntityRef(*reference);
								if (!reference->is_edge && group.labels &&
									!hasMetadata(row, group.variable, "labels")) {
									const auto position = std::ranges::lower_bound(
											loadedNodes, reference->entity_id, {}, &storage::LogicalGraphNode::node_id);
									if (position != loadedNodes.end() && position->node_id == reference->entity_id) {
										putLabels(row, group.variable, position->labels);
									} else {
										const auto cached = state.shared.node_cache.find(reference->entity_id);
										if (cached == state.shared.node_cache.end()) {
											throw std::runtime_error(
													"logical-plan metadata batch lost a requested Node");
										}
										putLabels(row, group.variable, cached->second.labels);
									}
								}
								if (reference->is_edge && group.type && !hasMetadata(row, group.variable, "type")) {
									const auto position = std::ranges::lower_bound(
											loadedEdges, reference->entity_id, {}, &storage::LogicalGraphEdge::edge_id);
									if (position != loadedEdges.end() && position->edge_id == reference->entity_id) {
										validateEdgeReferenceRecord(*reference, *position, state,
																	"logical-plan metadata batch relationship");
										putType(row, group.variable, position->type);
									} else {
										const auto cached = state.shared.edge_cache.find(reference->entity_id);
										if (cached == state.shared.edge_cache.end()) {
											throw std::runtime_error(
													"logical-plan metadata batch lost a requested relationship");
										}
										validateEdgeReferenceRecord(*reference, cached->second, state,
																	"logical-plan metadata batch relationship");
										putType(row, group.variable, cached->second.type);
									}
								}
							}
						}
					}
					for (const PropertyGroup &group: groups) {
						// IDs can contain at most one binding per buffered row. Reserve that
						// temporary gather space before either vector starts growing.
						MaterializedByteReservation propertyReadBytes(&state.shared.materialized_budget);
						const size_t maximumIdScratch = checkedSizeMul(
								checkedSizeMul(rows.size(), 2U, "logical-plan Property ID scratch size overflowed"),
								sizeof(uint64_t), "logical-plan Property ID scratch size overflowed");
						const size_t maximumKeyViewScratch =
								checkedSizeMul(group.keys.size(), sizeof(std::string_view),
											   "logical-plan Property key-view size overflowed");
						propertyReadBytes.set(checkedSizeAdd(maximumIdScratch, maximumKeyViewScratch,
															 "logical-plan Property gather scratch size overflowed"));
						std::vector<uint64_t> nodeIds;
						std::vector<uint64_t> edgeIds;
						for (LogicalPlanReadRow &row: rows.values()) {
							const auto found = row.values.find(group.variable);
							if (found == row.values.end()) {
								throw std::runtime_error("logical-plan Property batch lost its Entity binding");
							}
							if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
								validateEntityRef(*reference);
								if (reference->is_edge) {
									if (const auto cached = state.shared.edge_cache.find(reference->entity_id);
										cached != state.shared.edge_cache.end()) {
										for (const std::string &key: group.keys) {
											const auto property =
													std::ranges::find(cached->second.properties, key,
																	  &storage::LogicalGraphPropertyEntry::key);
											const PropertyValue *value = property == cached->second.properties.end()
																				 ? nullptr
																				 : &property->value;
											if (value == nullptr)
												putCacheValue(row.property_cache, group.variable, key, PropertyValue());
											else
												putCacheValue(row.property_cache, group.variable, key, *value);
										}
										continue;
									}
								} else if (const auto cached = state.shared.node_cache.find(reference->entity_id);
										   cached != state.shared.node_cache.end()) {
									for (const std::string &key: group.keys) {
										const auto property =
												std::ranges::find(cached->second.properties, key,
																  &storage::LogicalGraphPropertyEntry::key);
										const PropertyValue *value = property == cached->second.properties.end()
																			 ? nullptr
																			 : &property->value;
										if (value == nullptr)
											putCacheValue(row.property_cache, group.variable, key, PropertyValue());
										else
											putCacheValue(row.property_cache, group.variable, key, *value);
									}
									continue;
								}
								const auto cached = row.property_cache.find(group.variable);
								const bool complete = cached != row.property_cache.end() &&
													  std::ranges::all_of(group.keys, [&](const std::string &key) {
														  return cached->second.contains(key);
													  });
								if (!complete) {
									(reference->is_edge ? edgeIds : nodeIds).push_back(reference->entity_id);
								}
							}
						}
						std::ranges::sort(nodeIds);
						nodeIds.erase(std::unique(nodeIds.begin(), nodeIds.end()), nodeIds.end());
						std::ranges::sort(edgeIds);
						edgeIds.erase(std::unique(edgeIds.begin(), edgeIds.end()), edgeIds.end());
						std::vector<std::string_view> keys;
						keys.reserve(group.keys.size());
						for (const std::string &key: group.keys) {
							keys.push_back(key);
						}
						const auto columnFrameBytes = [&](const size_t idCount) {
							if (idCount == 0U)
								return size_t{0U};
							return checkedSizeAdd(
									checkedSizeMul(keys.size(), sizeof(std::vector<PropertyValue>),
												   "logical-plan Property column frame size overflowed"),
									checkedSizeMul(checkedSizeMul(keys.size(), idCount,
																  "logical-plan Property column frame size overflowed"),
												   sizeof(PropertyValue),
												   "logical-plan Property column frame size overflowed"),
									"logical-plan Property column frame size overflowed");
						};
						const size_t idStorageBytes =
								checkedSizeAdd(checkedSizeMul(nodeIds.capacity(), sizeof(uint64_t),
															  "logical-plan Property ID storage size overflowed"),
											   checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
															  "logical-plan Property ID storage size overflowed"),
											   "logical-plan Property ID storage size overflowed");
						const size_t columnStructures = checkedSizeAdd(
								checkedSizeAdd(idStorageBytes,
											   checkedSizeMul(keys.capacity(), sizeof(std::string_view),
															  "logical-plan Property key-view size overflowed"),
											   "logical-plan Property gather structure size overflowed"),
								checkedSizeAdd(columnFrameBytes(nodeIds.size()), columnFrameBytes(edgeIds.size()),
											   "logical-plan Property column frame size overflowed"),
								"logical-plan Property gather structure size overflowed");
						// IDs and key views now have their actual capacities. Keep their
						// charge while replacing the conservative gather reservation with
						// the exact column-frame reservation before storage allocates results.
						propertyReadBytes.set(columnStructures);
						const std::function<void(size_t)> admitProperty = [&](size_t bytes) {
							const size_t next = checkedSizeAdd(propertyReadBytes.bytes(), bytes,
															   "logical-plan Property source size overflowed");
							if (propertyReadBytes.wouldExceed(next))
								throw PrefetchBytePressure{};
							propertyReadBytes.set(next);
							debug::PerfTrace::addValue(
									"logical_plan.property_source.admitted_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						const bool sourceAdmission = state.shared.snapshot.supportsPropertyReadAdmission();
						auto nodeColumns = nodeIds.empty() ? std::vector<std::vector<PropertyValue>>{}
										   : sourceAdmission
												   ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															 nodeIds, keys, admitProperty)
												   : state.shared.snapshot.readNodePropertyColumnsSorted(nodeIds, keys);
						auto edgeColumns = edgeIds.empty() ? std::vector<std::vector<PropertyValue>>{}
										   : sourceAdmission
												   ? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
															 edgeIds, keys, admitProperty)
												   : state.shared.snapshot.readEdgePropertyColumnsSorted(edgeIds, keys);
						// Invalid storage output must fail before budget retries. Otherwise
						// smaller reads could conceal corruption in the original batch.
						const auto validateColumns = [&](const std::vector<uint64_t> &ids,
														 const std::vector<std::vector<PropertyValue>> &columns) {
							const size_t expectedColumns = ids.empty() ? 0U : group.keys.size();
							if (columns.size() != expectedColumns ||
								std::ranges::any_of(columns,
													[&](const auto &column) { return column.size() != ids.size(); })) {
								throw std::runtime_error("logical graph backend changed Property batch cardinality");
							}
						};
						validateColumns(nodeIds, nodeColumns);
						validateColumns(edgeIds, edgeColumns);
						const auto columnPayloadBytes = [&](const auto &columns, const char *message) {
							size_t bytes = 0U;
							for (const auto &column: columns) {
								for (const PropertyValue &value: column) {
									bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value), message);
								}
							}
							return bytes;
						};
						const size_t nodeColumnBytes =
								columnPayloadBytes(nodeColumns, "logical-plan Node Property batch size overflowed");
						const size_t edgeColumnBytes =
								columnPayloadBytes(edgeColumns, "logical-plan Edge Property batch size overflowed");
						const size_t retainedColumns = checkedSizeAdd(nodeColumnBytes, edgeColumnBytes,
																	  "logical-plan Property batch size overflowed");
						const size_t retainedWorkingSet = checkedSizeAdd(
								columnStructures, retainedColumns, "logical-plan Property working set size overflowed");
						if (retainedWorkingSet > state.shared.bounds.maximum_materialized_bytes) {
							if (rows.size() > 1U) {
								throw PrefetchBytePressure{};
							}
							throw std::runtime_error("logical-plan Property batch exceeded its byte bound");
						}
						// Rebase source wire admission to the existing retained-column
						// estimate once the complete shape has been authenticated. Readers
						// without source admission only gain this post-read reservation.
						if (propertyReadBytes.wouldExceed(retainedWorkingSet))
							throw PrefetchBytePressure{};
						propertyReadBytes.set(retainedWorkingSet);
						if (borrowRepeatedNodeColumns && groups.size() == 1U && metadataGroups.empty() &&
							completeEntityVariables.empty() && edgeIds.empty() && !nodeIds.empty() && nodeIds.size() < rows.size()) {
							ownedColumns.emplace(ScopedPropertyColumns{&group.variable, &group.keys, std::move(nodeIds), std::move(nodeColumns)});
							ownedColumnBytes.absorb(std::move(propertyReadBytes));
						} else {
							const auto fill = [&](const bool isEdge, const std::vector<uint64_t> &ids,
												  std::vector<std::vector<PropertyValue>> &columns) {
								if (ids.empty()) {
									return;
								}
								// Earlier duplicate bindings keep independent copies; the final
								// consumer takes ownership of the decoded column payload.
								std::vector<size_t> lastRow(ids.size(), rows.size());
								for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
									const auto found = rows.values()[rowIndex].values.find(group.variable);
									if (found == rows.values()[rowIndex].values.end())
										continue;
									const auto *reference = std::get_if<storage::EntityRef>(&found->second);
									if (reference == nullptr || reference->is_edge != isEdge)
										continue;
									const auto position = std::ranges::lower_bound(ids, reference->entity_id);
									if (position != ids.end() && *position == reference->entity_id) {
										lastRow[static_cast<size_t>(position - ids.begin())] = rowIndex;
									}
								}
								for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
									LogicalPlanReadRow &row = rows.values()[rowIndex];
									const auto found = row.values.find(group.variable);
									if (found == row.values.end()) {
										throw std::runtime_error("logical-plan Property batch lost its Entity binding");
									}
									const auto *reference = std::get_if<storage::EntityRef>(&found->second);
									if (reference == nullptr || reference->is_edge != isEdge) {
										continue;
									}
									const auto position = std::ranges::lower_bound(ids, reference->entity_id);
									if (position == ids.end() || *position != reference->entity_id) {
										continue;
									}
									const size_t ordinal = static_cast<size_t>(position - ids.begin());
									for (size_t column = 0U; column < group.keys.size(); ++column) {
										if (lastRow[ordinal] == rowIndex) {
											const size_t previous = propertyReadBytes.bytes();
											const size_t transferred =
													property_utils::getPropertyValueSize(columns[column][ordinal]);
											propertyReadBytes.set(previous - transferred);
											try {
												putCacheValue(row.property_cache, group.variable, group.keys[column],
															  std::move(columns[column][ordinal]));
											} catch (...) {
												propertyReadBytes.set(previous);
												throw;
											}
										} else {
											putCacheValue(row.property_cache, group.variable, group.keys[column],
														  std::as_const(columns[column][ordinal]));
										}
									}
								}
							};
							fill(false, nodeIds, nodeColumns);
							fill(true, edgeIds, edgeColumns);
							// The output rows now own the selected values. Drop the temporary
							// column matrices and gather vectors before charging downstream row
							// enrichment; retaining their empty/moved-from slots would inflate the
							// true peak and could reject a batch that fits after transfer.
							std::vector<std::vector<PropertyValue>>().swap(nodeColumns);
							std::vector<std::vector<PropertyValue>>().swap(edgeColumns);
							std::vector<uint64_t>().swap(nodeIds);
							std::vector<uint64_t>().swap(edgeIds);
							std::vector<std::string_view>().swap(keys);
							propertyReadBytes.set(0U);
						}
					}
					// Selected columns are attached to the buffered rows during the flush;
					// recompute the byte budget after that enrichment so LargeValue payloads
					// are covered by the same bound as the input rows.
					const size_t bitmapBytes = SelectionVector::requiredStorageBytes(rows.size());
					const size_t sharedAvailable =
							checkedSizeAdd(state.shared.materialized_budget.available(), bufferedRowsBytes.bytes(),
										   "logical-plan prefetch allowance overflowed");
					const size_t enrichedLimit = std::min(state.shared.bounds.maximum_materialized_bytes,
														  sharedAvailable - std::min(sharedAvailable, bitmapBytes));
					size_t enrichedBytes = 0U;
					size_t largestEnrichedRowBytes = 0U;
					for (const LogicalPlanReadRow &row: rows.values()) {
						const size_t rowBytes = logicalReadRowBytes(row, state.shared.context);
						if (rowBytes > enrichedLimit - std::min(enrichedBytes, enrichedLimit)) {
							if (rows.size() > 1U) {
								throw PrefetchBytePressure{};
							}
							throw std::runtime_error("bounded batch exceeded its byte limit");
						}
						enrichedBytes += rowBytes;
						largestEnrichedRowBytes = std::max(largestEnrichedRowBytes, rowBytes);
					}
					rows.recalculateBytes();
					debug::PerfTrace::addValue("logical_plan.batch.flushes", 1);
					debug::PerfTrace::addValue("logical_plan.batch.rows", static_cast<int64_t>(rows.size()));
					debug::PerfTrace::addValue("logical_plan.batch.materialized_bytes",
											   static_cast<int64_t>(rows.bytes()));
					bufferedRowsBytes.set(rows.bytes());
					// Borrowed columns remain resident during downstream callbacks. Keep
					// the same two-row headroom used by input admission after enrichment,
					// before publishing any row; split pressure must never replay effects.
					if (ownedColumns && rows.size() > 1U) {
						const size_t handoffHeadroom = checkedSizeAdd(bitmapBytes,
							checkedSizeMul(largestEnrichedRowBytes, 2U, "logical-plan prefetch handoff headroom overflowed"),
							"logical-plan prefetch handoff headroom overflowed");
						if (handoffHeadroom > state.shared.materialized_budget.available())
							throw PrefetchBytePressure{};
					}
					struct ColumnScope {
						ExecutionState &state; const ScopedPropertyColumns *previous;
						~ColumnScope() { state.task.property_columns = previous; }
					} columnScope{state, state.task.property_columns};
					if (ownedColumns) state.task.property_columns = &*ownedColumns;
					SelectionVector selection(rows.size(), &selectionMemory);
					if (selectionPredicate) {
						for (size_t index = 0U; index < rows.size(); ++index) {
							checkGuard(state);
							if (!selectionPredicate(rows.values()[index], state)) {
								selection.reject(index);
							}
						}
					}
					debug::PerfTrace::addValue("logical_plan.selection.selected_rows",
											   static_cast<int64_t>(selection.selectedCount()));
					debug::PerfTrace::addValue("logical_plan.selection.rejected_rows",
											   static_cast<int64_t>(rows.size() - selection.selectedCount()));
					if (ownedColumns && selectionPredicate) {
						// Filter rows may outlive this flush in downstream batching. Keep
						// independent caches for selected rows only; rejected rows never
						// receive the repeated property payload. Release columns before
						// handing any row to a downstream owner.
						auto &columns = *ownedColumns;
						const size_t scratchBytes = checkedSizeMul(columns.ids.size(), sizeof(size_t),
							"logical-plan selected scatter size overflowed");
						if (scratchBytes > state.shared.materialized_budget.available()) throw PrefetchBytePressure{};
						detail::BudgetMemoryResource scatterMemory(state.shared.materialized_budget);
						std::pmr::vector<size_t> lastSelected(columns.ids.size(), rows.size(), &scatterMemory);
						const auto ordinalFor = [&](const LogicalPlanReadRow &row) -> std::optional<size_t> {
							const auto *reference = std::get_if<storage::EntityRef>(&row.values.at(*columns.variable));
							if (reference == nullptr || reference->is_edge) return std::nullopt;
							const auto position = std::ranges::lower_bound(columns.ids, reference->entity_id);
							if (position == columns.ids.end() || *position != reference->entity_id)
								throw std::runtime_error("logical-plan selected column lost its Node binding");
							return static_cast<size_t>(position - columns.ids.begin());
						};
						for (size_t index = 0U; index < rows.size(); ++index) {
							if (!selection.includes(index)) continue;
							if (const auto ordinal = ordinalFor(rows.values()[index])) lastSelected[*ordinal] = index;
						}
						for (size_t index = 0U; index < rows.size(); ++index) {
							if (!selection.includes(index)) continue;
							auto &row = rows.values()[index];
							const auto selectedOrdinal = ordinalFor(row);
							if (!selectedOrdinal) continue;
							const size_t ordinal = *selectedOrdinal;
							for (size_t column = 0U; column < columns.keys->size(); ++column) {
								if (lastSelected[ordinal] == index) {
									const size_t previous = ownedColumnBytes.bytes();
									const size_t transferred = property_utils::getPropertyValueSize(columns.columns[column][ordinal]);
									ownedColumnBytes.set(previous - transferred);
									try {
										putCacheValue(row.property_cache, *columns.variable, (*columns.keys)[column],
											std::move(columns.columns[column][ordinal]));
									} catch (...) { ownedColumnBytes.set(previous); throw; }
								} else {
									putCacheValue(row.property_cache, *columns.variable, (*columns.keys)[column],
										std::as_const(columns.columns[column][ordinal]));
								}
							}
						}
						state.task.property_columns = columnScope.previous;
						ownedColumns.reset();
						ownedColumnBytes.reset();
					}
					// Predicates can populate additional caches. Reconcile those before
					// removing rows so each handoff refunds the current payload size.
					rows.recalculateBytes();
					bufferedRowsBytes.set(rows.bytes());
					std::reverse(rows.values().begin(), rows.values().end());
					bool keepGoing = true;
					const size_t rowCount = rows.size();
					for (size_t index = 0U; index < rowCount; ++index) {
						checkGuard(state);
                        if (coordinatedStorage) {
                            if (!selection.includes(index)) { rows.discardBack(); continue; }
                            auto charged = rows.popBackCharged();
                            const bool accepted = chargedReceiver
                                ? (*chargedReceiver)(std::move(charged.value), std::move(charged.reservation))
                                : visitor(std::move(charged.value));
                            if (!accepted) { keepGoing = false; break; }
                            continue;
                        }
						LogicalPlanReadRow row = rows.popBack();
						if (selection.includes(index)) {
							if (chargedReceiver != nullptr) {
								MaterializedByteReservation rowCharge =
										bufferedRowsBytes.splitOff(logicalReadRowBytes(row, state.shared.context));
								if (!(*chargedReceiver)(std::move(row), std::move(rowCharge))) {
									keepGoing = false;
									break;
								}
							} else {
								bufferedRowsBytes.set(rows.bytes());
								if (!visitor(std::move(row))) {
									keepGoing = false;
									break;
								}
							}
						} else {
							bufferedRowsBytes.set(rows.bytes());
						}
					}
					rows.clear();
					bufferedRowsBytes.reset();
					largestInputRowBytes = 0U;
					return keepGoing;
				} catch (const PrefetchBytePressure &) {
					if (rows.size() > 1U)
						return splitBufferedRowsAndFlush();
					throw std::runtime_error("bounded batch exceeded its byte limit");
				}
			};

			const auto acceptInput = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
				// Leave room for cache enrichment rather than filling the entire
				// allowance with input references before their payload sizes are known.
				const size_t rowBytes = logicalReadRowBytes(row, state.shared.context);
				const size_t batchAllowance =
						checkedSizeAdd(checkedSizeAdd(state.shared.materialized_budget.available(), bufferedRowsBytes.bytes(),
													  "logical-plan prefetch input allowance overflowed"),
									   charge.bytes(), "logical-plan prefetch input allowance overflowed");
				const size_t headroom = checkedSizeMul(std::max(largestInputRowBytes, rowBytes), 2U,
													   "logical-plan prefetch input headroom overflowed");
				const size_t inputAllowance = batchAllowance - std::min(batchAllowance, headroom);
				if (!rows.empty() && (rows.wouldExceedBytes(row) ||
									  rowBytes > inputAllowance - std::min(rows.bytes(), inputAllowance))) {
					if (!flush()) {
						stoppedByVisitor = true;
						return false;
					}
				}
                if (coordinatedStorage) {
                    if (charge.bytes() == 0U) rows.append(std::move(row));
                    else rows.appendCharged(std::move(row), std::move(charge));
                } else {
                    rows.append(std::move(row));
                    if (charge.bytes() == 0U) bufferedRowsBytes.set(rows.bytes());
                    else bufferedRowsBytes.absorb(std::move(charge));
                }
				largestInputRowBytes = std::max(largestInputRowBytes, rowBytes);
				// A first cache-enriched row can already consume the headroom. Flush
				// it now so the upstream source can admit the next selected payload.
				if (rows.size() == batchCapacity || rows.bytes() > inputAllowance) {
					const bool keepGoing = flush();
					if (!keepGoing) {
						stoppedByVisitor = true;
					}
					return keepGoing;
				}
				return true;
			};
			const ChargedRowVisitor chargedInput = acceptInput;
			const bool childCompleted = visitOperator(
					child, state,
					[&](LogicalPlanReadRow row) {
						return acceptInput(std::move(row), MaterializedByteReservation(&state.shared.materialized_budget));
					},
					supportsChargedRowHandoff(child.getType()) ? &chargedInput : nullptr);
			if (stoppedByVisitor || !flush()) {
				return false;
			}
			return childCompleted;
		}

		// Extract only the semantics-preserving subset of an outer Filter that can
		// be used to drive the global topology cursor.  The complete expression is
		// still evaluated by the normal Filter visitor, so this is an acceleration
		// hint rather than a second predicate implementation.  We intentionally
		// accept conjunctions of edge-property/literal equalities only; OR, dynamic
		// expressions, and comparisons with unknown values retain the generic path.
		// Views are valid for this synchronous execution: the logical plan owns
		// literal payloads and inline predicate values throughout the call.
		struct DirectEdgePredicateView {
			std::string_view key;
			const PropertyValue *value = nullptr;
			const std::string *text = nullptr;
			PropertyValue scalar;

			[[nodiscard]] bool matches(const PropertyValue &actual) const {
				if (text != nullptr) {
					const auto *actualText = std::get_if<std::string>(&actual.getVariant());
					return actualText != nullptr && *actualText == *text;
				}
				return actual == (value == nullptr ? scalar : *value);
			}
			[[nodiscard]] bool sameValue(const DirectEdgePredicateView &other) const {
				if (text == nullptr)
					return other.matches(value == nullptr ? scalar : *value);
				if (other.text != nullptr)
					return *text == *other.text;
				const auto &actual = other.value == nullptr ? other.scalar : *other.value;
				const auto *otherText = std::get_if<std::string>(&actual.getVariant());
				return otherText != nullptr && *text == *otherText;
			}
			[[nodiscard]] PropertyValue copyValue() const {
				return text != nullptr ? PropertyValue(*text) : value == nullptr ? scalar : *value;
			}
		};

		[[nodiscard]] std::optional<std::vector<DirectEdgePredicateView>>
		inspectDirectEdgePropertyFilterViews(const expressions::Expression &expression,
											 const std::string &edgeVariable) {
			if (edgeVariable.empty()) {
				return std::nullopt;
			}
			std::vector<DirectEdgePredicateView> result;
			const auto visit = [&](const auto &self, const expressions::Expression &current) -> bool {
				using ExpressionType = expressions::ExpressionType;
				if (current.getExpressionType() == ExpressionType::BINARY_OP) {
					const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(&current);
					if (binary == nullptr || binary->getLeft() == nullptr || binary->getRight() == nullptr) {
						return false;
					}
					if (binary->getOperator() == expressions::BinaryOperatorType::BOP_AND) {
						return self(self, *binary->getLeft()) && self(self, *binary->getRight());
					}
					if (binary->getOperator() != expressions::BinaryOperatorType::BOP_EQUAL) {
						return false;
					}
					const auto *left =
							dynamic_cast<const expressions::VariableReferenceExpression *>(binary->getLeft());
					const auto *right =
							dynamic_cast<const expressions::VariableReferenceExpression *>(binary->getRight());
					const expressions::VariableReferenceExpression *property = nullptr;
					const expressions::Expression *literal = nullptr;
					if (left != nullptr && left->hasProperty() && left->getVariableName() == edgeVariable) {
						property = left;
						literal = binary->getRight();
					} else if (right != nullptr && right->hasProperty() && right->getVariableName() == edgeVariable) {
						property = right;
						literal = binary->getLeft();
					}
					if (property == nullptr || literal == nullptr ||
						literal->getExpressionType() != ExpressionType::LITERAL ||
						property->getPropertyName().empty()) {
						return false;
					}
					const auto &constant = dynamic_cast<const expressions::LiteralExpression &>(*literal);
					DirectEdgePredicateView predicate{property->getPropertyName(), nullptr, nullptr, PropertyValue()};
					if (constant.isNull())
						return false;
					if (constant.isPropertyValue()) {
						predicate.value = &constant.getPropertyValue();
						if (isNull(*predicate.value))
							return false;
					} else if (constant.isString())
						predicate.text = &constant.getStringValue();
					else if (constant.isBoolean())
						predicate.scalar = PropertyValue(constant.getBooleanValue());
					else if (constant.isInteger())
						predicate.scalar = PropertyValue(constant.getIntegerValue());
					else
						predicate.scalar = PropertyValue(constant.getDoubleValue());
					const auto existing = std::ranges::find(result, predicate.key, &DirectEdgePredicateView::key);
					if (existing != result.end())
						return existing->sameValue(predicate);
					result.push_back(std::move(predicate));
					return true;
				}
				return false;
			};
			if (!visit(visit, expression) || result.empty()) {
				return std::nullopt;
			}
			return result;
		}

		[[nodiscard]] std::optional<std::vector<std::pair<std::string, PropertyValue>>>
		inspectDirectEdgePropertyFilter(const expressions::Expression &expression, const std::string &edgeVariable,
										ExecutionState &) {
			const auto views = inspectDirectEdgePropertyFilterViews(expression, edgeVariable);
			if (!views.has_value())
				return std::nullopt;
			std::vector<std::pair<std::string, PropertyValue>> result;
			result.reserve(views->size());
			for (const auto &view: *views)
				result.emplace_back(std::string(view.key), view.copyValue());
			return result;
		}

		// COUNT(*) and COUNT(n) over a simple Node scan need only logical IDs and,
		// when present, label tokens. Keep this a logical-shape path so every
		// equivalent plan can reuse it without manufacturing per-Node rows.
		[[nodiscard]] std::optional<bool> tryVisitDirectNodeCount(const logical::LogicalAggregate &aggregate,
																  ExecutionState &state,
																  const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.shared.snapshot.supportsNativeNodeRefScans()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg) {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> children = aggregate.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &scan = dynamic_cast<const logical::LogicalNodeScan &>(*children.front());
			if (!scan.getChildren().empty() || scan.getVariable().empty() || !scan.getPropertyPredicates().empty() ||
				!scan.getRangePredicates().empty() || scan.getCompositeEquality().has_value()) {
				return std::nullopt;
			}
			// Parameter predicates are resolved at execution time rather than stored in
			// getPropertyPredicates(). A count fast path is valid only when that
			// resolved set is empty; otherwise it would silently count rows that the
			// ordinary Node scan filters out.
			const auto parameterPredicates = resolveParameterPropertyPredicates(scan, state);
			if (!parameterPredicates.has_value()) {
				LogicalPlanReadRow result;
				result.values.emplace(count.alias, PropertyValue(int64_t{0}));
				return visitor(std::move(result));
			}
			if (!parameterPredicates->empty()) {
				return std::nullopt;
			}
			if (count.argument != nullptr) {
				const auto *variable =
						dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() ||
					variable->getVariableName() != scan.getVariable()) {
					return std::nullopt;
				}
			}

			debug::ScopedPerfTimer timer("logical_plan.direct_node_count");
			size_t inputRows = 0U;
			uint64_t previousNodeId = 0U;
			auto visitReference = [&](const storage::EntityRef &reference, bool labelsMatch) {
				checkGuard(state);
				validateNodeRef(reference,
								"logical graph direct Node count returned an invalid or non-monotonic Node reference");
				if (reference.entity_id <= previousNodeId) {
					throw std::runtime_error(
							"logical graph direct Node count returned an invalid or non-monotonic Node reference");
				}
				previousNodeId = reference.entity_id;
				incrementBounded(state.task.stats.nodes_scanned, "logical-plan direct Node count counter overflowed");
				if (!labelsMatch) {
					return true;
				}
				if (inputRows == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
				return true;
			};
			if (scan.getLabels().empty()) {
				state.shared.snapshot.visitNodeRefs(state.shared.bounds.maximum_node_records,
											 [&visitReference](const storage::EntityRef &reference) {
												 return visitReference(reference, true);
											 });
			} else {
				state.shared.snapshot.visitNodeRefsForLabelPredicate(
						state.shared.bounds.maximum_node_records, scan.getLabels(),
						[&visitReference](const storage::EntityRef &reference, bool labelsMatch) {
							return visitReference(reference, labelsMatch);
						});
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		// An anchored one-hop COUNT(v) can use adjacency topology when the Edge
		// predicate is empty. The source scan and any target predicates still use the
		// ordinary bounded/index-aware runtime path.
		// A nested anchored COUNT is handled as two bounded global Edge passes. This
		// is deliberately expressed in terms of logical operator shape rather than a
		// Cypher string: it removes one B+Tree root descent per intermediate source
		// while preserving path multiplicity and all query resource bounds.
		[[nodiscard]] std::optional<bool> tryVisitBatchedVariableLengthCount(const logical::LogicalAggregate &aggregate,
																			 ExecutionState &state,
																			 const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.shared.snapshot.supportsNativeBatchAdjacentEdgeRefs() ||
				!state.shared.snapshot.supportsNodeExistenceProbes()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal =
					dynamic_cast<const logical::LogicalVarLengthTraversal &>(*aggregateChildren.front());
			if (traversal.getSourceVar().empty() || traversal.getTargetVar().empty() ||
				traversal.getSourceVar() == traversal.getTargetVar() || !traversal.getEdgeVar().empty() ||
				traversal.getMinHops() < 0 || traversal.getMaxHops() < traversal.getMinHops() ||
				(traversal.getDirection() != "out" && traversal.getDirection() != "in" &&
				 traversal.getDirection() != "both")) {
				return std::nullopt;
			}
			if (count.argument != nullptr) {
				const auto *variable =
						dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() ||
					variable->getVariableName() != traversal.getTargetVar()) {
					return std::nullopt;
				}
			}
			const std::vector<logical::LogicalOperator *> traversalChildren = traversal.getChildren();
			if (traversalChildren.size() != 1U || traversalChildren.front() == nullptr ||
				traversalChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &sourceScan = dynamic_cast<const logical::LogicalNodeScan &>(*traversalChildren.front());
			if (sourceScan.getVariable() != traversal.getSourceVar() || !sourceScan.getChildren().empty()) {
				return std::nullopt;
			}

			int maximumHops = traversal.getMaxHops();
			if (state.shared.context != nullptr && state.shared.context->maxVarLengthDepth.has_value()) {
				if (*state.shared.context->maxVarLengthDepth < 0) {
					throw std::invalid_argument("logical-plan variable-length count has a negative query depth bound");
				}
				maximumHops = std::min(maximumHops, *state.shared.context->maxVarLengthDepth);
			}
			if (static_cast<uint64_t>(maximumHops) > state.shared.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan variable-length count exceeds its configured depth bound");
			}

			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty()) {
				if (!state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
					return std::nullopt;
				}
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
			}

			storage::EntityReadShape targetShape;
			targetShape.include_labels = !traversal.getTargetLabels().empty();
			for (const auto &[key, expected]: traversal.getTargetProperties()) {
				static_cast<void>(expected);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan variable-length target predicate has an empty key");
				}
				const std::optional<uint32_t> token = state.shared.snapshot.findPropertyToken(key);
				if (!token.has_value()) {
					// A storage-neutral null result cannot distinguish an absent key from
					// a backend without native Property tokens. Preserve correctness by
					// retaining the ordinary traversal in either case.
					return std::nullopt;
				}
				targetShape.property_tokens.push_back(*token);
			}
			std::ranges::sort(targetShape.property_tokens);
			targetShape.property_tokens.erase(
					std::unique(targetShape.property_tokens.begin(), targetShape.property_tokens.end()),
					targetShape.property_tokens.end());
			validateEntityReadShape(targetShape);

			struct FrontierPath {
				uint64_t current_node_id = 0U;
				std::vector<uint64_t> visited_edge_ids;
			};
			struct AdjacentRecord {
				uint64_t source_node_id = 0U;
				uint64_t target_node_id = 0U;
				uint64_t edge_id = 0U;

				bool operator==(const AdjacentRecord &) const = default;
			};
			const auto pathBytes = [](const FrontierPath &path) {
				return checkedSizeMul(path.visited_edge_ids.capacity(), sizeof(uint64_t),
									  "logical-plan variable-length frontier size overflowed");
			};
			const auto incrementResult = [&](uint64_t &resultCount) {
				if (resultCount == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				if (resultCount == static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
					throw std::overflow_error("logical-plan variable-length count exceeds integer result range");
				}
				++resultCount;
			};

			debug::ScopedPerfTimer timer("logical_plan.batched_variable_length_count");
			std::vector<FrontierPath> frontier;
			size_t frontierBytes = 0U;
			uint64_t resultCount = 0U;
			static_cast<void>(visitOperator(sourceScan, state, [&](LogicalPlanReadRow row) {
				checkGuard(state);
				const LogicalPlanReadValue &binding = requireBinding(row, traversal.getSourceVar());
				const uint64_t sourceId = nodeBindingId(binding, state);
				if (sourceId == 0U || frontier.size() == state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length count exceeded its source Node bound");
				}
				if (traversal.getMinHops() == 0 && maximumHops >= 0) {
					storage::LogicalGraphNode source;
					if (traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
						source = patternSourceIdentity(state, binding);
					} else {
						const std::array<uint64_t, 1U> ids{sourceId};
						const auto batch = readNodesShapedSortedBudgeted(state, ids, targetShape);
						if (batch.size() != 1U || batch.front().node_id != sourceId) {
							throw std::runtime_error("logical graph zero-hop target changed cardinality or identity");
						}
						source = batch.front();
						validateNode(source, state);
						if (source.properties.size() > state.shared.bounds.maximum_property_entries ||
							logicalNodeCacheBytes(source, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error("logical-plan zero-hop target exceeded its materialized bound");
						}
					}
					if (nodeMatches(state.shared.snapshot, source, traversal.getTargetLabels(),
									traversal.getTargetProperties(), state)) {
						incrementResult(resultCount);
					}
				}
				FrontierPath path;
				path.current_node_id = sourceId;
				path.visited_edge_ids.reserve(1U);
				// Relationship identity, rather than Node identity, constrains a trail.
				// Retain one bounded slot for the first expansion; no source Node is marked.
				const size_t pathPayloadBytes = pathBytes(path);
				const size_t previousCapacity = frontier.capacity();
				frontier.push_back(std::move(path));
				const size_t addedSlotBytes =
						checkedSizeMul(frontier.capacity() - previousCapacity, sizeof(FrontierPath),
									   "logical-plan variable-length frontier size overflowed");
				frontierBytes = checkedSizeAdd(frontierBytes,
											   checkedSizeAdd(pathPayloadBytes, addedSlotBytes,
															  "logical-plan variable-length frontier size overflowed"),
											   "logical-plan variable-length frontier size overflowed");
				if (frontierBytes > state.shared.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}
				return true;
			}));
			std::ranges::sort(frontier, {}, &FrontierPath::current_node_id);
			if (std::ranges::adjacent_find(frontier, {}, &FrontierPath::current_node_id) != frontier.end()) {
				throw std::runtime_error("logical-plan variable-length source scan returned a duplicate Node");
			}

			size_t scannedEdges = 0U;
			for (int depth = 1; depth <= maximumHops && !frontier.empty(); ++depth) {
				checkGuard(state);
				std::vector<uint64_t> currentNodeIds;
				currentNodeIds.reserve(frontier.size());
				for (const FrontierPath &path: frontier) {
					currentNodeIds.push_back(path.current_node_id);
				}
				std::ranges::sort(currentNodeIds);
				currentNodeIds.erase(std::unique(currentNodeIds.begin(), currentNodeIds.end()), currentNodeIds.end());
				if (currentNodeIds.size() > state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length frontier exceeded its Node bound");
				}

				std::vector<size_t> edgeCounts(currentNodeIds.size(), 0U);
				const size_t layerFixedBytes =
						checkedSizeAdd(checkedSizeMul(currentNodeIds.capacity(), sizeof(uint64_t),
													  "logical-plan variable-length layer size overflowed"),
									   checkedSizeMul(edgeCounts.capacity(), sizeof(size_t),
													  "logical-plan variable-length layer size overflowed"),
									   "logical-plan variable-length layer size overflowed");
				const size_t frontierAndLayerBytes = checkedSizeAdd(
						frontierBytes, layerFixedBytes, "logical-plan variable-length layer size overflowed");
				if (frontierAndLayerBytes >= state.shared.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}
				const size_t maximumAdjacencyRecords =
						(state.shared.bounds.maximum_materialized_bytes - frontierAndLayerBytes) / sizeof(AdjacentRecord);
				std::vector<AdjacentRecord> adjacency;
				const size_t estimatedAdjacency =
						std::min(state.shared.bounds.maximum_edge_records,
								 checkedSizeMul(currentNodeIds.size(), size_t{4U},
												"logical-plan variable-length adjacency capacity overflowed"));
				adjacency.reserve(std::min(estimatedAdjacency, maximumAdjacencyRecords));
				const auto appendAdjacency = [&](const AdjacentRecord record) {
					if (adjacency.size() == maximumAdjacencyRecords) {
						throw std::runtime_error(
								"logical-plan variable-length count exceeded its materialized byte bound");
					}
					if (adjacency.size() == adjacency.capacity()) {
						const size_t doubled = adjacency.capacity() == 0U
													   ? 1U
													   : (adjacency.capacity() > maximumAdjacencyRecords / 2U
																  ? maximumAdjacencyRecords
																  : adjacency.capacity() * 2U);
						adjacency.reserve(std::min(maximumAdjacencyRecords, doubled));
					}
					adjacency.push_back(record);
				};
				const auto visitDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
					if (scannedEdges == state.shared.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan variable-length count exceeded its Edge bound");
					}
					const size_t remaining = state.shared.bounds.maximum_edge_records - scannedEdges;
					const size_t edgeVisitBound =
							checkedSizeAdd(remaining, 1U, "logical-plan variable-length Edge bound overflowed");
					state.shared.snapshot.visitAdjacentEdgeRefsForNodes(
							currentNodeIds, adjacencyDirection, edgeVisitBound,
							[&](const storage::EntityRef &reference) {
								checkGuard(state);
								validateEdgeRef(
										reference,
										"logical graph batch adjacency returned an invalid or non-Edge reference");
								if (scannedEdges == state.shared.bounds.maximum_edge_records) {
									throw std::runtime_error(
											"logical-plan variable-length count exceeded its Edge bound");
								}
								++scannedEdges;
								incrementBounded(state.task.stats.edges_scanned,
												 "logical-plan variable-length Edge counter overflowed");
								const uint64_t sourceId =
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.source_node_id
												: reference.target_node_id;
								const uint64_t targetId =
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.target_node_id
												: reference.source_node_id;
								const auto source = std::ranges::lower_bound(currentNodeIds, sourceId);
								if (source == currentNodeIds.end() || *source != sourceId) {
									throw std::runtime_error(
											"logical graph batch adjacency returned an inconsistent endpoint");
								}
								const bool duplicateSelfLoop =
										traversal.getDirection() == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										reference.source_node_id == reference.target_node_id;
								if (duplicateSelfLoop) {
									return true;
								}
								const size_t sourceOrdinal = static_cast<size_t>(source - currentNodeIds.begin());
								if (edgeCounts[sourceOrdinal] == state.shared.bounds.maximum_edges_per_node) {
									throw std::runtime_error(
											"logical-plan variable-length count exceeded its per-Node Edge bound");
								}
								++edgeCounts[sourceOrdinal];
								if (relationshipTypeToken.has_value() &&
									reference.type_token != *relationshipTypeToken) {
									return true;
								}
								appendAdjacency({sourceId, targetId, reference.entity_id});
								return true;
							});
				};
				if (relationshipTypeToken.has_value() || traversal.getEdgeType().empty()) {
					if (traversal.getDirection() == "out" || traversal.getDirection() == "both") {
						visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
					}
					if (traversal.getDirection() == "in" || traversal.getDirection() == "both") {
						visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
					}
				}
				std::ranges::sort(adjacency, [](const AdjacentRecord &left, const AdjacentRecord &right) {
					return std::tie(left.source_node_id, left.edge_id, left.target_node_id) <
						   std::tie(right.source_node_id, right.edge_id, right.target_node_id);
				});
				if (std::ranges::adjacent_find(adjacency) != adjacency.end()) {
					throw std::runtime_error("logical graph batch adjacency returned a duplicate Edge reference");
				}
				if (adjacency.empty()) {
					break;
				}
				const size_t adjacencyBytes = checkedSizeMul(adjacency.capacity(), sizeof(AdjacentRecord),
															 "logical-plan variable-length adjacency size overflowed");
				const size_t layerBytes = checkedSizeAdd(layerFixedBytes, adjacencyBytes,
														 "logical-plan variable-length layer size overflowed");
				const size_t retainedLayerBytes =
						checkedSizeAdd(frontierBytes, layerBytes, "logical-plan variable-length layer size overflowed");
				const size_t targetVectorBytes =
						checkedSizeMul(adjacency.size(), sizeof(uint64_t),
									   "logical-plan variable-length target vector size overflowed");
				if (targetVectorBytes > state.shared.bounds.maximum_materialized_bytes -
												std::min(retainedLayerBytes, state.shared.bounds.maximum_materialized_bytes)) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}

				std::vector<uint64_t> targetNodeIds;
				targetNodeIds.reserve(adjacency.size());
				for (const AdjacentRecord &record: adjacency) {
					targetNodeIds.push_back(record.target_node_id);
				}
				std::ranges::sort(targetNodeIds);
				targetNodeIds.erase(std::unique(targetNodeIds.begin(), targetNodeIds.end()), targetNodeIds.end());
				if (targetNodeIds.size() > state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length endpoints exceeded the Node bound");
				}
				std::vector<uint8_t> targetMatches(targetNodeIds.size(), uint8_t{0U});
				if (traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetNodeIds);
					if (present.size() != targetNodeIds.size() ||
						std::ranges::any_of(present, [](const bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph batch adjacency returned an Edge with a missing endpoint");
					}
					std::fill(targetMatches.begin(), targetMatches.end(), uint8_t{1U});
				} else {
					const std::vector<storage::LogicalGraphNode> targets =
							readNodesShapedSortedBudgeted(state, targetNodeIds, targetShape);
					if (targets.size() != targetNodeIds.size()) {
						throw std::runtime_error("logical graph backend changed variable-length target cardinality");
					}
					size_t targetBytes = 0U;
					for (size_t index = 0U; index < targets.size(); ++index) {
						if (targets[index].node_id != targetNodeIds[index]) {
							throw std::runtime_error("logical graph backend changed variable-length target identity");
						}
						targetBytes = checkedSizeAdd(targetBytes, logicalNodeCacheBytes(targets[index], state.shared.context),
													 "logical-plan variable-length target batch size overflowed");
						const size_t stagedBytes = checkedSizeAdd(
								retainedLayerBytes,
								checkedSizeAdd(targetVectorBytes, targetBytes,
											   "logical-plan variable-length target batch size overflowed"),
								"logical-plan variable-length target batch size overflowed");
						if (stagedBytes > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan variable-length count exceeded its materialized byte bound");
						}
						targetMatches[index] = nodeMatches(state.shared.snapshot, targets[index], traversal.getTargetLabels(),
														   traversal.getTargetProperties(), state);
					}
				}

				std::vector<FrontierPath> nextFrontier;
				size_t nextFrontierBytes = 0U;
				const size_t retainedBytes =
						checkedSizeAdd(retainedLayerBytes,
									   checkedSizeAdd(targetVectorBytes, targetMatches.capacity() * sizeof(uint8_t),
													  "logical-plan variable-length layer size overflowed"),
									   "logical-plan variable-length layer size overflowed");
				for (const FrontierPath &path: frontier) {
					checkGuard(state);
					const auto first = std::ranges::lower_bound(adjacency, path.current_node_id, {},
																&AdjacentRecord::source_node_id);
					const auto last = std::ranges::upper_bound(adjacency, path.current_node_id, {},
															   &AdjacentRecord::source_node_id);
					for (auto candidate = first; candidate != last; ++candidate) {
						if (std::ranges::find(path.visited_edge_ids, candidate->edge_id) !=
							path.visited_edge_ids.end()) {
							continue;
						}
						const auto target = std::ranges::lower_bound(targetNodeIds, candidate->target_node_id);
						if (target == targetNodeIds.end() || *target != candidate->target_node_id) {
							throw std::runtime_error("logical-plan variable-length target lookup lost an endpoint");
						}
						if (depth >= traversal.getMinHops() &&
							targetMatches[static_cast<size_t>(target - targetNodeIds.begin())]) {
							incrementResult(resultCount);
						}
						if (depth == maximumHops) {
							continue;
						}
						FrontierPath next;
						next.current_node_id = candidate->target_node_id;
						next.visited_edge_ids.reserve(path.visited_edge_ids.size() + 1U);
						next.visited_edge_ids.insert(next.visited_edge_ids.end(), path.visited_edge_ids.begin(),
													 path.visited_edge_ids.end());
						next.visited_edge_ids.push_back(candidate->edge_id);
						const size_t nextPathBytes = pathBytes(next);
						const size_t previousCapacity = nextFrontier.capacity();
						nextFrontier.push_back(std::move(next));
						const size_t addedSlotBytes =
								checkedSizeMul(nextFrontier.capacity() - previousCapacity, sizeof(FrontierPath),
											   "logical-plan variable-length frontier size overflowed");
						nextFrontierBytes =
								checkedSizeAdd(nextFrontierBytes,
											   checkedSizeAdd(nextPathBytes, addedSlotBytes,
															  "logical-plan variable-length frontier size overflowed"),
											   "logical-plan variable-length frontier size overflowed");
						if (checkedSizeAdd(retainedBytes, nextFrontierBytes,
										   "logical-plan variable-length frontier size overflowed") >
							state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan variable-length count exceeded its materialized byte bound");
						}
					}
				}
				frontier = std::move(nextFrontier);
				frontierBytes = nextFrontierBytes;
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(resultCount)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool>
		tryVisitAnchoredTwoHopRelationshipCount(const logical::LogicalAggregate &aggregate, ExecutionState &state,
												const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.shared.snapshot.supportsNativeEdgeRefScans() || !state.shared.snapshot.supportsNodeExistenceProbes()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg ||
				!count.argument) {
				return std::nullopt;
			}
			const auto *countVariable =
					dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
			if (countVariable == nullptr || countVariable->hasProperty()) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (debug::PerfTrace::areValuesEnabled() && !aggregateChildren.empty() &&
				aggregateChildren.front() != nullptr) {
				debug::PerfTrace::addValue("logical_plan.two_hop.aggregate_child_type",
										   static_cast<int64_t>(aggregateChildren.front()->getType()));
			}
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &outer = dynamic_cast<const logical::LogicalTraversal &>(*aggregateChildren.front());
			if (outer.getTargetVar().empty() || countVariable->getVariableName() != outer.getTargetVar() ||
				!outer.getEdgeProperties().empty() || !outer.getTargetProperties().empty() ||
				(outer.getDirection() != "out" && outer.getDirection() != "in" && outer.getDirection() != "both")) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> outerChildren = outer.getChildren();
			if (outerChildren.size() != 1U || outerChildren.front() == nullptr ||
				outerChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &inner = dynamic_cast<const logical::LogicalTraversal &>(*outerChildren.front());
			if (inner.getTargetVar().empty() || inner.getTargetVar() != outer.getSourceVar() ||
				!inner.getEdgeProperties().empty() || !inner.getTargetProperties().empty() ||
				!inner.getTargetProperties().empty() ||
				(inner.getDirection() != "out" && inner.getDirection() != "in" && inner.getDirection() != "both")) {
				return std::nullopt;
			}
			const std::vector<logical::LogicalOperator *> innerChildren = inner.getChildren();
			if (innerChildren.size() != 1U || innerChildren.front() == nullptr ||
				innerChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &seed = dynamic_cast<const logical::LogicalNodeScan &>(*innerChildren.front());
			if (seed.getVariable() != inner.getSourceVar()) {
				return std::nullopt;
			}
			if (state.shared.bounds.maximum_rows == std::numeric_limits<size_t>::max() ||
				state.shared.bounds.maximum_edge_records == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("logical-plan two-hop relationship count bound overflowed");
			}
			std::optional<uint64_t> innerType;
			if (!inner.getEdgeType().empty()) {
				if (!state.shared.snapshot.supportsNativeRelationshipTypeTokens(inner.getEdgeType())) {
					return std::nullopt;
				}
				innerType = state.shared.snapshot.findRelationshipTypeToken(inner.getEdgeType());
				if (!innerType.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			std::optional<uint64_t> outerType;
			if (!outer.getEdgeType().empty()) {
				if (!state.shared.snapshot.supportsNativeRelationshipTypeTokens(outer.getEdgeType())) {
					return std::nullopt;
				}
				outerType = state.shared.snapshot.findRelationshipTypeToken(outer.getEdgeType());
				if (!outerType.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}

			detail::BudgetMemoryResource intermediateMemory(state.shared.materialized_budget);
			const auto bitBytes = [](size_t capacity) { return capacity / 8U + (capacity % 8U != 0U); };
			const auto visitBatchRefs = [&](std::span<const uint64_t> sources,
				storage::LogicalGraphReadDirection direction, size_t bound,
				const std::function<bool(const storage::EntityRef &)> &consume) {
				if (state.shared.snapshot.supportsBatchAdjacencyReferenceBufferAdmission()) {
					MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
					state.shared.snapshot.visitAdjacentEdgeRefsForNodesWithBufferAdmission(sources, direction, bound, consume,
						[&](size_t bytes) {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							if (sourceBytes.wouldExceed(bytes)) throw storage::LogicalGraphBufferPressure();
							sourceBytes.set(bytes);
						});
				} else {
					state.shared.snapshot.visitAdjacentEdgeRefsForNodes(sources, direction, bound, consume);
				}
			};

			std::pmr::vector<uint64_t> seedIds(&intermediateMemory);
			static_cast<void>(visitOperator(seed, state, [&](LogicalPlanReadRow row) {
				const uint64_t id = nodeBindingId(requireBinding(row, seed.getVariable()), state);
				if (id == 0U || seedIds.size() == state.shared.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its source Node bound");
				}
				seedIds.push_back(id);
				return true;
			}));
			detail::sortUniqueIdsGuarded(seedIds, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});

			using Multiplicity = uint64_t;
			const auto expand = [&](const std::pmr::vector<uint64_t> &sources, const std::string &direction,
									const std::optional<uint64_t> &type,
									std::pmr::unordered_map<uint64_t, Multiplicity> *next) {
				std::pmr::unordered_set<uint64_t> sourceSet(&intermediateMemory);
				sourceSet.reserve(sources.size());
				for (const uint64_t id: sources) {
					checkGuard(state);
					sourceSet.insert(id);
				}
				std::pmr::unordered_map<uint64_t, size_t> sourceEdgeCounts(&intermediateMemory);
				sourceEdgeCounts.reserve(sources.size());
				std::pmr::vector<uint64_t> targetIds(&intermediateMemory);
				size_t scanned = 0U;
				const auto consume = [&](const storage::EntityRef &reference) {
					checkGuard(state);
					validateEdgeRef(reference,
									"logical-plan two-hop relationship count received an invalid or Node reference");
					if (scanned == state.shared.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
					}
					++scanned;
					incrementBounded(state.task.stats.edges_scanned, "logical-plan two-hop Edge counter overflowed");
					const bool outgoing = sourceSet.contains(reference.source_node_id);
					const bool incoming = sourceSet.contains(reference.target_node_id);
					const auto consumeOne = [&](const uint64_t sourceId, const uint64_t targetId) {
						if (sourceEdgeCounts[sourceId] == state.shared.bounds.maximum_edges_per_node) {
							throw std::runtime_error(
									"logical-plan two-hop relationship count exceeded its per-Node Edge bound");
						}
						++sourceEdgeCounts[sourceId];
						if (type.has_value() && reference.type_token != *type) {
							return;
						}
						if (next != nullptr) {
							const Multiplicity previous = next->contains(targetId) ? next->at(targetId) : 0U;
							const Multiplicity increment = sources.empty() ? 0U : 1U;
							if (previous > std::numeric_limits<Multiplicity>::max() - increment) {
								throw std::overflow_error("logical-plan two-hop path multiplicity overflowed");
							}
							next->insert_or_assign(targetId, previous + increment);
							// Probe each distinct endpoint once; multiplicity stays in the map.
							if (previous == 0U) targetIds.push_back(targetId);
						}
					};
					if (direction == "out" || direction == "both") {
						if (outgoing)
							consumeOne(reference.source_node_id, reference.target_node_id);
					}
					if ((direction == "in" || direction == "both") && incoming &&
						!(outgoing && reference.source_node_id == reference.target_node_id)) {
						consumeOne(reference.target_node_id, reference.source_node_id);
					}
					return true;
				};
				const auto visitDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
					if (scanned == state.shared.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
					}
					const size_t remaining = state.shared.bounds.maximum_edge_records - scanned;
					const size_t edgeVisitBound =
							checkedSizeAdd(remaining, 1U, "logical-plan two-hop Edge bound overflowed");
					visitBatchRefs(sources, adjacencyDirection, edgeVisitBound, consume);
				};
				if (direction == "out" || direction == "both") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
				}
				if (direction == "in" || direction == "both") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
				}
				if (next != nullptr) {
					if (next->size() > state.shared.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan two-hop relationship count exceeded its intermediate Node bound");
					}
					detail::sortUniqueIdsGuarded(targetIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					MaterializedByteReservation presentBytes(&state.shared.materialized_budget);
					presentBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
						"logical-plan two-hop bit slots overflowed"));
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetIds);
					presentBytes.set(bitBytes(present.capacity()));
					checkGuard(state);
					if (present.size() != targetIds.size() ||
						std::ranges::any_of(present, [](bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph backend returned an Edge topology with a missing endpoint");
					}
					const std::vector<std::string> &labels = inner.getTargetLabels();
					if (!labels.empty()) {
						MaterializedByteReservation matchesBytes(&state.shared.materialized_budget);
						matchesBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
							"logical-plan two-hop bit slots overflowed"));
						const std::vector<bool> matches = state.shared.snapshot.readNodeLabelMatchesSorted(targetIds, labels);
						matchesBytes.set(bitBytes(matches.capacity()));
						checkGuard(state);
						if (matches.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed two-hop intermediate label cardinality");
						}
						for (size_t index = 0U; index < targetIds.size(); ++index) {
							checkGuard(state);
							if (!matches[index]) {
								next->erase(targetIds[index]);
							}
						}
					}
				}
			};
			std::pmr::unordered_map<uint64_t, Multiplicity> intermediate(&intermediateMemory);
			if (!seedIds.empty()) {
				expand(seedIds, inner.getDirection(), innerType, &intermediate);
			}
			std::pmr::vector<uint64_t> intermediateIds(&intermediateMemory);
			intermediateIds.reserve(intermediate.size());
			for (const auto &[id, multiplicity]: intermediate) {
				checkGuard(state);
				static_cast<void>(multiplicity);
				intermediateIds.push_back(id);
			}
			detail::sortUniqueIdsGuarded(intermediateIds, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
			uint64_t resultCount = 0U;
			// The second pass retains multiplicity from the first hop.  It uses the same
			// endpoint/type checks but does not materialize intermediate rows.
			std::pmr::unordered_set<uint64_t> sourceSet(&intermediateMemory);
			sourceSet.reserve(intermediate.size());
			for (const uint64_t id: intermediateIds) {
				checkGuard(state);
				sourceSet.insert(id);
			}
			std::pmr::unordered_map<uint64_t, size_t> edgeCounts(&intermediateMemory);
			std::pmr::unordered_map<uint64_t, Multiplicity> finalCandidates(&intermediateMemory);
			std::pmr::vector<uint64_t> finalTargetIds(&intermediateMemory);
			size_t scanned = 0U;
			const auto consumeOuterReference = [&](const storage::EntityRef &reference) {
				checkGuard(state);
				validateEdgeRef(reference,
								"logical-plan two-hop relationship count received an invalid or Node reference");
				if (scanned == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
				}
				++scanned;
				incrementBounded(state.task.stats.edges_scanned, "logical-plan two-hop Edge counter overflowed");
				const bool outgoing = sourceSet.contains(reference.source_node_id);
				const bool incoming = sourceSet.contains(reference.target_node_id);
				const auto consume = [&](uint64_t sourceId, uint64_t targetId) {
					if (edgeCounts[sourceId] == state.shared.bounds.maximum_edges_per_node) {
						throw std::runtime_error(
								"logical-plan two-hop relationship count exceeded its per-Node Edge bound");
					}
					++edgeCounts[sourceId];
					if ((outerType.has_value() && reference.type_token != *outerType))
						return;
					const uint64_t multiplicity = intermediate.at(sourceId);
					const auto [entry, inserted] = finalCandidates.try_emplace(targetId, 0U);
					if (entry->second > std::numeric_limits<uint64_t>::max() - multiplicity) {
						throw std::overflow_error("logical-plan two-hop relationship count overflowed");
					}
					entry->second += multiplicity;
					if (inserted) finalTargetIds.push_back(targetId);
				};
				if (outer.getDirection() == "out" || outer.getDirection() == "both") {
					if (outgoing)
						consume(reference.source_node_id, reference.target_node_id);
				}
				if ((outer.getDirection() == "in" || outer.getDirection() == "both") && incoming &&
					!(outgoing && reference.source_node_id == reference.target_node_id)) {
					consume(reference.target_node_id, reference.source_node_id);
				}
				return true;
			};
			const auto visitOuterDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
				if (scanned == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
				}
				const size_t remaining = state.shared.bounds.maximum_edge_records - scanned;
				const size_t edgeVisitBound =
						checkedSizeAdd(remaining, 1U, "logical-plan two-hop Edge bound overflowed");
				visitBatchRefs(intermediateIds, adjacencyDirection, edgeVisitBound,
															 consumeOuterReference);
			};
			if (outer.getDirection() == "out" || outer.getDirection() == "both") {
				visitOuterDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
			}
			if (outer.getDirection() == "in" || outer.getDirection() == "both") {
				visitOuterDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
			}
			detail::sortUniqueIdsGuarded(finalTargetIds, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
			MaterializedByteReservation finalPresentBytes(&state.shared.materialized_budget);
			finalPresentBytes.set(checkedSizeAdd(bitBytes(finalTargetIds.size()), sizeof(size_t),
				"logical-plan two-hop bit slots overflowed"));
			const std::vector<bool> finalPresent = state.shared.snapshot.containsNodesSorted(finalTargetIds);
			finalPresentBytes.set(bitBytes(finalPresent.capacity()));
			checkGuard(state);
			if (finalPresent.size() != finalTargetIds.size() ||
				std::ranges::any_of(finalPresent, [](bool value) { return !value; })) {
				throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
			}
			MaterializedByteReservation finalLabelBytes(&state.shared.materialized_budget);
			std::vector<bool> finalLabels;
			if (!outer.getTargetLabels().empty()) {
				finalLabelBytes.set(checkedSizeAdd(bitBytes(finalTargetIds.size()), sizeof(size_t),
					"logical-plan two-hop label bit slots overflowed"));
				finalLabels = state.shared.snapshot.readNodeLabelMatchesSorted(finalTargetIds, outer.getTargetLabels());
				finalLabelBytes.set(bitBytes(finalLabels.capacity()));
				checkGuard(state);
				if (finalLabels.size() != finalTargetIds.size()) {
					throw std::runtime_error("logical graph backend changed two-hop target label cardinality");
				}
			}
			for (const auto &[targetId, multiplicity]: finalCandidates) {
				checkGuard(state);
				const auto position = std::ranges::lower_bound(finalTargetIds, targetId);
				if (position == finalTargetIds.end() ||
					(!finalLabels.empty() && !finalLabels[static_cast<size_t>(position - finalTargetIds.begin())])) {
					continue;
				}
				if (resultCount > std::numeric_limits<uint64_t>::max() - multiplicity) {
					throw std::overflow_error("logical-plan two-hop relationship count overflowed");
				}
				resultCount += multiplicity;
				if (resultCount > state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
			}
			LogicalPlanReadRow result;
			if (resultCount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical-plan two-hop relationship count exceeds integer result range");
			}
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(resultCount)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitAnchoredRelationshipCount(const logical::LogicalAggregate &aggregate,
																			ExecutionState &state,
																			const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg ||
				!count.argument) {
				return std::nullopt;
			}
			const auto *countVariable =
					dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
			if (countVariable == nullptr || countVariable->hasProperty()) {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(*aggregateChildren.front());
			if (traversal.getEdgeVar().empty() || traversal.getTargetVar().empty() ||
				countVariable->getVariableName() != traversal.getTargetVar() ||
				!traversal.getEdgeProperties().empty()) {
				return std::nullopt;
			}
			if (traversal.getDirection() != "out" && traversal.getDirection() != "in" &&
				traversal.getDirection() != "both") {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> traversalChildren = traversal.getChildren();
			if (traversalChildren.size() != 1U || traversalChildren.front() == nullptr ||
				traversalChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &seed = dynamic_cast<const logical::LogicalNodeScan &>(*traversalChildren.front());
			if (seed.getVariable() != traversal.getSourceVar()) {
				return std::nullopt;
			}
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() &&
				state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			const bool useNativeAdjacencyRefs = state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
												(traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
			// Nullable columns need a presence probe to distinguish stored NULL
			// from an absent Property without loading unrelated values.
			const bool targetPropertiesCanBeSelected =
					useNativeAdjacencyRefs && !traversal.getTargetProperties().empty() &&
					(state.shared.snapshot.supportsPropertyPresenceProbes() ||
					 !std::ranges::any_of(traversal.getTargetProperties(),
						[](const auto &predicate) { return isNull(predicate.second); }));

			debug::ScopedPerfTimer timer("logical_plan.anchored_relationship_count");
			size_t inputRows = 0U;
			const bool supportsExistenceProbes = state.shared.snapshot.supportsNodeExistenceProbes();
			const auto visitSource = [&](LogicalPlanReadRow row) {
				const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
				// The topology cursor needs only the stable logical ID. Keep a native
				// deferred source as a reference instead of reopening its full Node record;
				// source labels and predicates were already validated by the scan.
				const uint64_t sourceNodeId = nodeBindingId(sourceBinding, state);
				size_t sourceEdges = 0U;
				struct PendingTarget {
					uint64_t edge_id = 0U;
					uint64_t source_node_id = 0U;
					uint64_t target_node_id = 0U;
					bool type_matches = false;
				};
				MaterializedByteReservation pendingBytes(&state.shared.materialized_budget);
				std::vector<PendingTarget> pendingTargets;
				using LabelMatch = std::pair<uint64_t, bool>;
				MaterializedByteReservation labelCacheBytes(&state.shared.materialized_budget);
				std::vector<LabelMatch> targetLabelCache;
				const auto cachedLabel = [&](uint64_t id) { return std::ranges::find(targetLabelCache, id, &LabelMatch::first); };
				// Target Property values are stable for the pinned snapshot.  Keep a
				// bounded per-source cache so a super-node whose adjacency spans many
				// pages does not reread the same target on every page flush.
				using PropertyCacheEntry = std::pair<uint64_t, std::vector<PropertyValue>>;
				MaterializedByteReservation propertyCacheSlots(&state.shared.materialized_budget);
				MaterializedByteReservation targetPropertyCacheReservation(&state.shared.materialized_budget);
				std::vector<PropertyCacheEntry> targetPropertyCache;
				const auto cachedProperties = [&](uint64_t id) {
					return std::ranges::find(targetPropertyCache, id, &PropertyCacheEntry::first);
				};
				size_t targetPropertyCacheBytes = 0U;
				const size_t targetBatchCapacity = std::max<size_t>(
						1U, std::min(state.shared.bounds.maximum_adjacency_page_records, state.shared.bounds.maximum_edges_per_node));
				const auto visitDirection = [&](storage::LogicalGraphReadDirection direction) {
					uint64_t previousEdgeId = 0U;
					const auto flushTargets = [&]() {
						if (pendingTargets.empty()) {
							return true;
						}
						// Both native references and ordinary topology pages can use the
						// same bounded target validation batch.  The latter used to fall
						// through to one existence/label read per Edge even though COUNT
						// has no visitor early-termination boundary.
						MaterializedByteReservation targetIdsBytes(&state.shared.materialized_budget);
						std::vector<uint64_t> targetIds;
						if (useNativeAdjacencyRefs ||
							(traversal.getTargetProperties().empty() &&
							 (!traversal.getTargetLabels().empty() || supportsExistenceProbes))) {
							targetIdsBytes.set(checkedSizeMul(pendingTargets.size(), sizeof(uint64_t),
								"logical-plan anchored endpoint slots overflowed"));
							targetIds.reserve(pendingTargets.size());
							targetIdsBytes.set(checkedSizeMul(targetIds.capacity(), sizeof(uint64_t),
								"logical-plan anchored endpoint capacity overflowed"));
							for (const PendingTarget &candidate: pendingTargets) {
								targetIds.push_back(candidate.target_node_id);
							}
							detail::sortUniqueIdsGuarded(targetIds, [&] {
								if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
							});
						}
						const auto bitBytes = [](size_t capacity) { return capacity / 8U + (capacity % 8U != 0U); };
						MaterializedByteReservation labelBitsBytes(&state.shared.materialized_budget);
						std::vector<bool> labelsMatch;
						if (!traversal.getTargetLabels().empty()) {
							labelBitsBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
								"logical-plan anchored label bit slots overflowed"));
							labelsMatch.resize(targetIds.size());
							labelBitsBytes.set(bitBytes(labelsMatch.capacity()));
							MaterializedByteReservation uncachedIdsBytes(&state.shared.materialized_budget);
							uncachedIdsBytes.set(checkedSizeMul(targetIds.size(), sizeof(uint64_t),
								"logical-plan anchored cold label slots overflowed"));
							std::vector<uint64_t> uncachedIds;
							uncachedIds.reserve(targetIds.size());
							uncachedIdsBytes.set(checkedSizeMul(uncachedIds.capacity(), sizeof(uint64_t),
								"logical-plan anchored cold label capacity overflowed"));
							for (size_t index = 0U; index < targetIds.size(); ++index) {
								const auto cached = cachedLabel(targetIds[index]);
								if (cached == targetLabelCache.end()) uncachedIds.push_back(targetIds[index]);
								else labelsMatch[index] = cached->second;
							}
							if (!uncachedIds.empty()) {
								MaterializedByteReservation resolvedBitsBytes(&state.shared.materialized_budget);
								resolvedBitsBytes.set(checkedSizeAdd(bitBytes(uncachedIds.size()), sizeof(size_t),
									"logical-plan anchored resolved bit slots overflowed"));
								const std::vector<bool> resolved = state.shared.snapshot.readNodeLabelMatchesSorted(
										uncachedIds, traversal.getTargetLabels());
								resolvedBitsBytes.set(bitBytes(resolved.capacity()));
								checkGuard(state);
								if (resolved.size() != uncachedIds.size()) {
									throw std::runtime_error(
											"logical graph backend changed anchored target label-match cardinality");
								}
								const size_t cacheLimit = std::min(state.shared.bounds.maximum_edges_per_node, size_t{64U});
								for (size_t index = 0U; index < uncachedIds.size(); ++index) {
									const auto position = std::ranges::lower_bound(targetIds, uncachedIds[index]);
									labelsMatch[static_cast<size_t>(position - targetIds.begin())] = resolved[index];
									if (targetLabelCache.size() == cacheLimit) continue;
									if (targetLabelCache.size() == targetLabelCache.capacity()) {
										const size_t old = targetLabelCache.capacity();
										const size_t next = old == 0U ? std::min(cacheLimit, size_t{8U}) : old + std::min(old, cacheLimit - old);
										const size_t peak = (old + next) * sizeof(LabelMatch);
										if (labelCacheBytes.wouldExceed(peak)) continue;
										labelCacheBytes.set(peak);
										targetLabelCache.reserve(next);
										labelCacheBytes.set(targetLabelCache.capacity() * sizeof(LabelMatch));
										debug::PerfTrace::addValue("logical_plan.anchored_label_cache_capacity", static_cast<int64_t>(targetLabelCache.capacity()));
									}
									targetLabelCache.emplace_back(uncachedIds[index], resolved[index]);
								}
							}
						}
						MaterializedByteReservation keySlotsBytes(&state.shared.materialized_budget);
						MaterializedByteReservation coldIdsBytes(&state.shared.materialized_budget);
						MaterializedByteReservation targetPropertyReadBytes(&state.shared.materialized_budget);
						std::vector<std::string_view> targetPropertyKeys;
						std::vector<std::vector<PropertyValue>> resolvedPropertyColumns;
						std::vector<uint64_t> uncachedPropertyIds;
						size_t targetPropertyStructureBytes = 0U;
						if (targetPropertiesCanBeSelected) {
							// Keep the current flush complete even when the cross-page cache
							// reaches its byte budget. Oversized values are used once and are
							// deliberately not retained for later adjacency pages.
							for (const auto &predicate: traversal.getTargetProperties())
								if (predicate.first.empty()) throw std::invalid_argument("logical-plan target Node Property predicate has an empty key");
							keySlotsBytes.set(checkedSizeMul(traversal.getTargetProperties().size(), sizeof(std::string_view),
								"logical-plan anchored key view slots overflowed"));
							targetPropertyKeys.reserve(traversal.getTargetProperties().size());
							keySlotsBytes.set(checkedSizeMul(targetPropertyKeys.capacity(), sizeof(std::string_view),
								"logical-plan anchored key view capacity overflowed"));
							for (const auto &[key, expected]: traversal.getTargetProperties()) {
								static_cast<void>(expected);
								if (key.empty()) {
									throw std::invalid_argument(
											"logical-plan target Node Property predicate has an empty key");
								}
								if (std::ranges::find(targetPropertyKeys, key) == targetPropertyKeys.end()) {
									targetPropertyKeys.push_back(key);
								}
							}
							const auto &keyViews = targetPropertyKeys; // Borrow stable plan-owned names.
							const size_t coldCount = static_cast<size_t>(std::ranges::count_if(targetIds,
								[&](uint64_t id) { return cachedProperties(id) == targetPropertyCache.end(); }));
							coldIdsBytes.set(checkedSizeMul(coldCount, sizeof(uint64_t), "logical-plan anchored cold owner slots overflowed"));
							uncachedPropertyIds.reserve(coldCount);
							coldIdsBytes.set(checkedSizeMul(uncachedPropertyIds.capacity(), sizeof(uint64_t),
								"logical-plan anchored cold owner capacity overflowed"));
							for (const uint64_t targetId: targetIds) {
								if (cachedProperties(targetId) == targetPropertyCache.end()) {
									uncachedPropertyIds.push_back(targetId);
								}
							}
							if (!uncachedPropertyIds.empty()) {
								targetPropertyStructureBytes = checkedSizeAdd(
										checkedSizeMul(
												keyViews.size(), sizeof(std::vector<PropertyValue>),
												"logical-plan anchored target Property column structure overflowed"),
										checkedSizeMul(
												checkedSizeMul(keyViews.size(), uncachedPropertyIds.size(),
															   "logical-plan anchored target Property column structure "
															   "overflowed"),
												sizeof(PropertyValue),
												"logical-plan anchored target Property column structure overflowed"),
										"logical-plan anchored target Property column structure overflowed");
								targetPropertyReadBytes.set(targetPropertyStructureBytes);
								const std::function<void(size_t)> admission = [&](size_t bytes) {
									checkGuard(state);
									targetPropertyReadBytes.set(checkedSizeAdd(targetPropertyReadBytes.bytes(), bytes,
										"logical-plan anchored target Property batch size overflowed"));
								};
								resolvedPropertyColumns = state.shared.snapshot.supportsPropertyReadAdmission()
									? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(uncachedPropertyIds, keyViews, admission)
									: state.shared.snapshot.readNodePropertyColumnsSorted(uncachedPropertyIds, keyViews);
								if (resolvedPropertyColumns.size() != targetPropertyKeys.size())
									throw std::runtime_error("logical graph backend changed anchored target Property column cardinality");
								for (const auto &column: resolvedPropertyColumns)
									if (column.size() != uncachedPropertyIds.size())
										throw std::runtime_error("logical graph backend changed anchored target Property result cardinality");
							}
							size_t actualColumnsBytes = checkedSizeMul(resolvedPropertyColumns.capacity(), sizeof(std::vector<PropertyValue>),
								"logical-plan anchored column capacity overflowed");
							for (const auto &column: resolvedPropertyColumns) {
								actualColumnsBytes = checkedSizeAdd(actualColumnsBytes, checkedSizeMul(column.capacity(), sizeof(PropertyValue),
									"logical-plan anchored column value capacity overflowed"), "logical-plan anchored column size overflowed");
								for (const auto &value: column) actualColumnsBytes = checkedSizeAdd(actualColumnsBytes,
									property_utils::getPropertyValueSize(value), "logical-plan anchored column payload overflowed");
							}
							targetPropertyReadBytes.set(actualColumnsBytes);
							checkGuard(state);
							const size_t cacheLimit = std::min(state.shared.bounds.maximum_edges_per_node, size_t{64U});
							for (size_t ordinal = 0U; ordinal < uncachedPropertyIds.size() && targetPropertyCache.size() < cacheLimit; ++ordinal) {
								size_t payloadBytes = 0U;
								for (const auto &column: resolvedPropertyColumns)
									payloadBytes = checkedSizeAdd(payloadBytes, property_utils::getPropertyValueSize(column[ordinal]),
										"logical-plan anchored Property cache payload overflowed");
								const size_t valueBytes = checkedSizeAdd(payloadBytes,
									checkedSizeMul(targetPropertyKeys.size(), sizeof(PropertyValue), "logical-plan anchored Property cache slots overflowed"),
									"logical-plan anchored Property cache size overflowed");
								const size_t nextValues = checkedSizeAdd(targetPropertyCacheBytes, valueBytes,
									"logical-plan anchored Property cache size overflowed");
								const size_t old = targetPropertyCache.capacity();
								const bool grow = targetPropertyCache.size() == old;
								const size_t next = grow ? (old == 0U ? std::min(cacheLimit, size_t{8U}) : old + std::min(old, cacheLimit - old)) : old;
								const size_t extraSlots = grow ? next * sizeof(PropertyCacheEntry) : 0U;
								const size_t combined = checkedSizeAdd(nextValues, extraSlots, "logical-plan anchored Property cache peak overflowed");
								if (checkedSizeAdd(combined, propertyCacheSlots.bytes(), "logical-plan anchored Property cache limit overflowed")
									> MAXIMUM_MATERIALIZED_CACHE_BYTES || targetPropertyCacheReservation.wouldExceed(combined)) continue;
								targetPropertyCacheReservation.set(nextValues);
								if (grow) {
									propertyCacheSlots.set((old + next) * sizeof(PropertyCacheEntry));
									targetPropertyCache.reserve(next);
									propertyCacheSlots.set(targetPropertyCache.capacity() * sizeof(PropertyCacheEntry));
								}
								std::vector<PropertyValue> values;
								values.reserve(targetPropertyKeys.size());
								const size_t actualBytes = checkedSizeAdd(payloadBytes,
									checkedSizeMul(values.capacity(), sizeof(PropertyValue), "logical-plan anchored Property cache capacity overflowed"),
									"logical-plan anchored Property cache capacity overflowed");
								targetPropertyCacheReservation.set(checkedSizeAdd(targetPropertyCacheBytes, actualBytes,
									"logical-plan anchored Property cache actual size overflowed"));
								for (const auto &column: resolvedPropertyColumns) values.push_back(column[ordinal]);
								targetPropertyCache.emplace_back(uncachedPropertyIds[ordinal], std::move(values));
								targetPropertyCacheBytes += actualBytes;
							}
						}
						MaterializedByteReservation existenceBytes(&state.shared.materialized_budget);
						std::vector<bool> targetExists;
						if (state.shared.snapshot.supportsNodeExistenceProbes()) {
							existenceBytes.set(checkedSizeAdd(bitBytes(targetIds.size()), sizeof(size_t),
								"logical-plan anchored existence bit slots overflowed"));
							targetExists = state.shared.snapshot.containsNodesSorted(targetIds);
							existenceBytes.set(bitBytes(targetExists.capacity()));
							checkGuard(state);
							if (targetExists.size() != targetIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed anchored target existence cardinality");
							}
						}
						for (const PendingTarget &candidate: pendingTargets) {
							checkGuard(state);
							if (!candidate.type_matches) {
								continue;
							}
							const auto position = std::ranges::lower_bound(targetIds, candidate.target_node_id);
							if (position == targetIds.end() || *position != candidate.target_node_id) {
								throw std::runtime_error("logical graph backend lost an anchored target Node");
							}
							const size_t ordinal = static_cast<size_t>(position - targetIds.begin());
							if (!targetExists.empty() && !targetExists[ordinal]) {
								throw std::runtime_error(
										"logical graph backend returned an Edge topology with a missing endpoint");
							}
							if (!labelsMatch.empty() && !labelsMatch[ordinal]) {
								continue;
							}
							bool matches = true;
							if (targetPropertiesCanBeSelected) {
								for (size_t index = 0U; index < traversal.getTargetProperties().size(); ++index) {
									const auto &[key, expected] = traversal.getTargetProperties()[index];
									const auto keyPosition = std::ranges::find(targetPropertyKeys, key);
									if (keyPosition == targetPropertyKeys.end()) {
										throw std::logic_error(
												"logical-plan anchored target Property column is missing");
									}
									const size_t columnIndex =
											static_cast<size_t>(keyPosition - targetPropertyKeys.begin());
									const auto cached = cachedProperties(candidate.target_node_id);
									const PropertyValue *actual = nullptr;
									if (cached != targetPropertyCache.end()) {
										actual = &cached->second[columnIndex];
									} else {
										const auto propertyRow =
												std::ranges::lower_bound(uncachedPropertyIds, candidate.target_node_id);
										if (propertyRow == uncachedPropertyIds.end() ||
											*propertyRow != candidate.target_node_id) {
											throw std::runtime_error(
													"logical graph backend lost anchored target Property values");
										}
										actual = &resolvedPropertyColumns[columnIndex][static_cast<size_t>(
												propertyRow - uncachedPropertyIds.begin())];
									}
									if (actual == nullptr || *actual != expected) {
										matches = false;
										break;
									}
									if (isNull(*actual)) {
										checkGuardNow(state);
										const auto present = state.shared.snapshot.nodePropertyExists(candidate.target_node_id, key);
										if (!present) throw std::runtime_error("anchored count Property presence probe is unavailable");
										if (!*present) { matches = false; break; }
									}
								}
							}
							if (!matches) {
								continue;
							}
							if (inputRows == state.shared.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
							}
							incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						}
						pendingTargets.clear();
						return true;
					};
					const auto consumeTopology = [&](const uint64_t edgeId, const uint64_t edgeSourceNodeId,
													 const uint64_t edgeTargetNodeId, const bool typeMatches) {
						checkGuard(state);
						if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
							throw std::runtime_error(
									"logical-plan anchored relationship count exceeded its per-Node Edge bound");
						}
						validateAdjacentEdgeOrder(edgeId, previousEdgeId, "anchored relationship count");
						const bool endpointMatches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
															 ? edgeSourceNodeId == sourceNodeId
															 : edgeTargetNodeId == sourceNodeId;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid anchored relationship topology");
						}
						++sourceEdges;
						incrementBounded(state.task.stats.edges_scanned,
										 "logical-plan anchored relationship Edge counter overflowed");
						if (traversal.getDirection() == "both" &&
							direction == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							edgeSourceNodeId == sourceNodeId) {
							return true;
						}
						if (!typeMatches) {
							return true;
						}
						const uint64_t targetId = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														  ? edgeTargetNodeId
														  : edgeSourceNodeId;
						// Native reference pages already expose the complete endpoint set for
						// this bounded batch. Keep endpoint liveness in that same batch even
						// when no target label/property predicate is present.
						const bool requiresBatchedTargetRead =
								(useNativeAdjacencyRefs && targetPropertiesCanBeSelected) ||
								(traversal.getTargetProperties().empty() &&
								 (!traversal.getTargetLabels().empty() || supportsExistenceProbes));
						if (!requiresBatchedTargetRead) {
							if (!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty()) {
								const storage::LogicalGraphNode target = readNodeCached(state, targetId);
								if (!nodeMatches(state.shared.snapshot, target, traversal.getTargetLabels(),
												 traversal.getTargetProperties(), state)) {
									return true;
								}
							} else if (supportsExistenceProbes) {
								if (!state.shared.snapshot.containsNode(targetId)) {
									throw std::runtime_error(
											"logical graph backend returned an Edge topology with a missing endpoint");
								}
							} else {
								validateNodeIdentity(state, targetId);
							}
						}
						// A native reference batch validates all target endpoints in
						// flushTargets().  Do not probe this endpoint eagerly here: doing
						// so turns a page-bounded batch into one Directory lookup per Edge.
						if (requiresBatchedTargetRead) {
							if (pendingTargets.size() == pendingTargets.capacity()) {
								const size_t old = pendingTargets.capacity();
								size_t next = old == 0U ? std::min(targetBatchCapacity, size_t{16U})
									: old + std::min(old, targetBatchCapacity - old);
								size_t peak = checkedSizeMul(old + next, sizeof(PendingTarget),
									"logical-plan anchored pending capacity overflowed");
								if (pendingBytes.wouldExceed(peak)) {
									if (old != 0U) {
										if (!flushTargets()) return false;
									} else {
										next = 1U;
										peak = sizeof(PendingTarget);
									}
								}
								if (pendingTargets.size() == pendingTargets.capacity()) {
									pendingBytes.set(peak);
									pendingTargets.reserve(next);
									debug::PerfTrace::addValue("logical_plan.anchored_pending_capacity",
										static_cast<int64_t>(std::min(pendingTargets.capacity(), static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
									pendingBytes.set(checkedSizeMul(pendingTargets.capacity(), sizeof(PendingTarget),
										"logical-plan anchored pending capacity overflowed"));
								}
							}
							pendingTargets.push_back({edgeId, edgeSourceNodeId, edgeTargetNodeId, typeMatches});
							if (pendingTargets.size() >= targetBatchCapacity) {
								return flushTargets();
							}
							return true;
						}
						if (inputRows == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
						}
						incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						return true;
					};
					if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
						// Ask for one bounded probe so an additional Edge is rejected rather
						// than silently truncating the aggregate input.
						if (useNativeAdjacencyRefs) {
							MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
							const auto page = readReferencePageBudgeted(state, sourceNodeId, direction, std::nullopt, 1U, referenceBytes);
							if (!page.edges.empty()) {
								throw std::runtime_error(
										"logical-plan anchored relationship count exceeded its per-Node Edge bound");
							}
						} else {
							state.shared.snapshot.visitAdjacentEdgeTopologies(
									sourceNodeId, direction, 1U, [](const storage::LogicalGraphEdgeTopology &) -> bool {
										throw std::runtime_error("logical-plan anchored relationship count exceeded "
																 "its per-Node Edge bound");
									});
						}
						return;
					}
					const size_t directionLimit = state.shared.bounds.maximum_edges_per_node - sourceEdges;
					if (useNativeAdjacencyRefs) {
						std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
						for (;;) {
							if (sourceEdges == state.shared.bounds.maximum_edges_per_node) {
								throw std::runtime_error(
										"logical-plan anchored relationship count exceeded its per-Node Edge bound");
							}
							const size_t pageRecords = std::min(state.shared.bounds.maximum_edges_per_node - sourceEdges,
																state.shared.bounds.maximum_adjacency_page_records);
							MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
							const auto page = readReferencePageBudgeted(state, sourceNodeId, direction, continuation, pageRecords, referenceBytes);
							if (page.edges.size() > pageRecords ||
								(page.edges.empty() && page.continuation.has_value()) ||
								(continuation.has_value() &&
								 (page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
								throw std::runtime_error("logical graph backend returned an invalid anchored "
														 "relationship reference page");
							}
							for (const storage::EntityRef &reference: page.edges) {
								validateEdgeRef(reference, "logical graph backend returned an invalid or Node "
														   "reference from an anchored relationship page");
								const bool typeMatches = !relationshipTypeToken.has_value() ||
														 reference.type_token == *relationshipTypeToken;
								if (!consumeTopology(reference.entity_id, reference.source_node_id,
													 reference.target_node_id, typeMatches)) {
									return;
								}
							}
							if (!page.continuation.has_value()) {
								if (!flushTargets()) {
									return;
								}
								return;
							}
							if (page.edges.empty() ||
								page.continuation->previous_edge_id != page.edges.back().entity_id ||
								page.continuation->next_edge_id == 0U ||
								page.continuation->previous_edge_id == page.continuation->next_edge_id) {
								throw std::runtime_error("logical graph backend returned an invalid anchored "
														 "relationship reference continuation");
							}
							continuation = page.continuation;
						}
					}
					state.shared.snapshot.visitAdjacentEdgeTopologies(
							sourceNodeId, direction, directionLimit,
							[&](const storage::LogicalGraphEdgeTopology &edge) {
								return consumeTopology(edge.edge_id, edge.source_node_id, edge.target_node_id,
													   traversal.getEdgeType().empty() ||
															   edge.type == traversal.getEdgeType());
							});
					flushTargets();
				};
				if (traversal.getDirection() == "out" || traversal.getDirection() == "both") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
				}
				if (traversal.getDirection() == "both" || traversal.getDirection() == "in") {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
				}
				return true;
			};
			const InternalVisitor sourceVisitor = visitSource;
			const bool canUseGlobalSourceSet = traversal.getTargetProperties().empty() && supportsExistenceProbes &&
											   state.shared.snapshot.supportsNativeBatchAdjacentEdgeRefs() &&
											   (traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
			if (!canUseGlobalSourceSet) {
				static_cast<void>(visitOperator(seed, state, sourceVisitor));
			} else {
				std::vector<uint64_t> sourceIds;
				sourceIds.reserve(std::min(state.shared.bounds.maximum_node_records,
										   state.shared.bounds.maximum_adjacency_page_records * size_t{2U}));
				static_cast<void>(visitOperator(seed, state, [&](LogicalPlanReadRow row) {
					const uint64_t sourceId = nodeBindingId(requireBinding(row, traversal.getSourceVar()), state);
					if (sourceId == 0U) {
						throw std::runtime_error(
								"logical-plan anchored relationship count returned an invalid source Node");
					}
					if (sourceIds.size() == state.shared.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan anchored relationship count exceeded its source Node bound");
					}
					sourceIds.push_back(sourceId);
					return true;
				}));
				std::ranges::sort(sourceIds);
				sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
				const size_t globalSourceThreshold = std::max<size_t>(1U, state.shared.bounds.maximum_adjacency_page_records);
				if (sourceIds.size() < globalSourceThreshold) {
					for (const uint64_t sourceId: sourceIds) {
						LogicalPlanReadRow row;
						row.values.emplace(traversal.getSourceVar(),
										   LogicalPlanReadValue(storage::EntityRef{sourceId, false, 0U, 0U, 0U}));
						static_cast<void>(visitSource(std::move(row)));
					}
				} else {
					struct PendingGlobalEdge {
						uint64_t target_node_id = 0U;
					};
					std::unordered_set<uint64_t> sourceSet;
					sourceSet.reserve(sourceIds.size());
					for (const uint64_t sourceId: sourceIds) {
						static_cast<void>(sourceSet.insert(sourceId));
					}
					std::unordered_map<uint64_t, size_t> sourceEdgeCounts;
					sourceEdgeCounts.reserve(sourceIds.size());
					size_t globalEdgesScanned = 0U;
					std::vector<PendingGlobalEdge> pending;
					const size_t globalBatchCapacity =
							std::max<size_t>(1U, std::min(state.shared.bounds.maximum_adjacency_page_records,
														  state.shared.bounds.maximum_edge_records));
					pending.reserve(globalBatchCapacity);
					const auto flushGlobal = [&]() {
						if (pending.empty()) {
							return;
						}
						std::vector<uint64_t> targetIds;
						targetIds.reserve(pending.size());
						for (const PendingGlobalEdge &candidate: pending) {
							targetIds.push_back(candidate.target_node_id);
						}
						detail::sortUniqueIdsGuarded(targetIds, [&] {
							if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
						});
						const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(targetIds);
						if (present.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed anchored global target existence cardinality");
						}
						const std::vector<bool> labelsMatch = traversal.getTargetLabels().empty()
																	  ? std::vector<bool>(targetIds.size(), true)
																	  : state.shared.snapshot.readNodeLabelMatchesSorted(
																				targetIds, traversal.getTargetLabels());
						if (labelsMatch.size() != targetIds.size()) {
							throw std::runtime_error(
									"logical graph backend changed anchored global target label cardinality");
						}
						for (const PendingGlobalEdge &candidate: pending) {
							const auto position = std::ranges::lower_bound(targetIds, candidate.target_node_id);
							if (position == targetIds.end() || *position != candidate.target_node_id ||
								!present[static_cast<size_t>(position - targetIds.begin())]) {
								throw std::runtime_error(
										"logical graph backend returned an Edge topology with a missing endpoint");
							}
							if (!labelsMatch[static_cast<size_t>(position - targetIds.begin())]) {
								continue;
							}
							if (inputRows == state.shared.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
							}
							incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						}
						pending.clear();
					};
					const auto visitGlobalDirection = [&](const storage::LogicalGraphReadDirection direction) {
						if (globalEdgesScanned == state.shared.bounds.maximum_edge_records) {
							throw std::runtime_error(
									"logical-plan anchored global relationship count exceeded its Edge bound");
						}
						const size_t remaining = state.shared.bounds.maximum_edge_records - globalEdgesScanned;
						const size_t edgeVisitBound =
								checkedSizeAdd(remaining, 1U, "logical-plan anchored global Edge bound overflowed");
						state.shared.snapshot.visitAdjacentEdgeRefsForNodes(
								sourceIds, direction, edgeVisitBound, [&](const storage::EntityRef &reference) {
									checkGuard(state);
									validateEdgeRef(reference, "logical-plan anchored global relationship count "
															   "received an invalid or Node reference");
									if (globalEdgesScanned == state.shared.bounds.maximum_edge_records) {
										throw std::runtime_error("logical-plan anchored global relationship count "
																 "exceeded its Edge bound");
									}
									incrementBounded(globalEdgesScanned,
													 "logical-plan anchored global Edge counter overflowed");
									incrementBounded(state.task.stats.edges_scanned,
													 "logical-plan anchored global Edge statistics overflowed");
									const bool outgoing =
											direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING;
									const uint64_t sourceId =
											outgoing ? reference.source_node_id : reference.target_node_id;
									if (!sourceSet.contains(sourceId)) {
										return true;
									}
									const auto countSourceEdge = [&](const uint64_t sourceId) {
										const size_t count = sourceEdgeCounts[sourceId];
										if (count == state.shared.bounds.maximum_edges_per_node) {
											throw std::runtime_error("logical-plan anchored relationship count "
																	 "exceeded its per-Node Edge bound");
										}
										sourceEdgeCounts[sourceId] = count + 1U;
									};
									countSourceEdge(sourceId);
									// BOTH visits the incoming and outgoing indexes independently. A
									// self-loop is one adjacency row for its source and must not be
									// emitted again by the incoming pass.
									if (traversal.getDirection() == "both" && !outgoing &&
										reference.source_node_id == reference.target_node_id) {
										return true;
									}
									if (relationshipTypeToken.has_value() &&
										reference.type_token != *relationshipTypeToken) {
										return true;
									}
									pending.push_back({outgoing ? reference.target_node_id : reference.source_node_id});
									if (pending.size() >= globalBatchCapacity) {
										flushGlobal();
									}
									return true;
								});
					};
					if (traversal.getDirection() == "out" || traversal.getDirection() == "both") {
						visitGlobalDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
					}
					if (traversal.getDirection() == "in" || traversal.getDirection() == "both") {
						visitGlobalDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
					}
					flushGlobal();
				}
			}
			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitDirectRelationshipCount(const logical::LogicalAggregate &aggregate,
																		  ExecutionState &state,
																		  const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg) {
				return std::nullopt;
			}

			const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr) {
				return std::nullopt;
			}
			const logical::LogicalOperator *traversalOperator = aggregateChildren.front();
			MaterializedByteReservation predicateViewBytes(&state.shared.materialized_budget);
			std::vector<DirectEdgePredicateView> pushedEdgePredicates;
			if (traversalOperator->getType() == logical::LogicalOpType::LOP_FILTER) {
				const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*traversalOperator);
				if (!filter.getPredicate()) {
					return std::nullopt;
				}
				const logical::LogicalOperator &filterChild = requireOnlyChild(filter);
				if (filterChild.getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
					return std::nullopt;
				}
				const auto &filteredTraversal = dynamic_cast<const logical::LogicalTraversal &>(filterChild);
				auto inspected =
						inspectDirectEdgePropertyFilterViews(*filter.getPredicate(), filteredTraversal.getEdgeVar());
				if (!inspected.has_value()) {
					return std::nullopt;
				}
				pushedEdgePredicates = std::move(*inspected);
				traversalOperator = &filterChild;
			}
			if (traversalOperator->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(*traversalOperator);
			if (traversal.getDirection() != "out" || traversal.getEdgeVar().empty() ||
				!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty()) {
				return std::nullopt;
			}
			if (count.argument) {
				const auto *variable =
						dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() ||
					variable->getVariableName() != traversal.getEdgeVar()) {
					return std::nullopt;
				}
			}

			const std::vector<logical::LogicalOperator *> traversalChildren = traversal.getChildren();
			if (traversalChildren.size() != 1U || traversalChildren.front() == nullptr ||
				traversalChildren.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &seed = dynamic_cast<const logical::LogicalNodeScan &>(*traversalChildren.front());
			if (seed.getVariable() != traversal.getSourceVar() || !seed.getLabels().empty() ||
				!seed.getPropertyPredicates().empty() || !seed.getRangePredicates().empty() ||
				seed.getCompositeEquality().has_value()) {
				return std::nullopt;
			}

			if (state.shared.bounds.maximum_rows == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("logical-plan direct relationship count input bound overflowed");
			}
			debug::ScopedPerfTimer timer("logical_plan.direct_relationship_count");
			if (debug::PerfTrace::areValuesEnabled()) {
				debug::PerfTrace::addValue("logical_plan.direct_relationship_count", 1);
			}
			// Defer endpoint validation until the scan completes. The same bounded
			// endpoint set was previously probed once per Property batch, which
			// restarted the Entity Directory cursor hundreds of times on large scans.
			// No result is emitted before this set is authenticated.
			const size_t endpointCacheCapacity = state.shared.bounds.maximum_node_records;
			MaterializedByteReservation endpointBytes(&state.shared.materialized_budget);
			std::unordered_set<uint64_t> endpointsToValidate;
			// Charge the key plus conservative node/bucket links before inserting.
			// Do not reserve from the adjacency page bound: a large configured page
			// may otherwise allocate an endpoint table before the shared byte budget
			// has admitted any endpoint.
			constexpr size_t endpointEntryBytes = sizeof(uint64_t) + 2U * sizeof(void *);
			const bool supportsExistenceProbes = state.shared.snapshot.supportsNodeExistenceProbes();
			const auto validateEndpoints = [&]() {
				MaterializedByteReservation endpointScratch(&state.shared.materialized_budget);
				endpointScratch.set(checkedSizeMul(endpointsToValidate.size(), sizeof(uint64_t),
												   "logical-plan count endpoint scratch overflowed"));
				std::vector<uint64_t> endpointIds(endpointsToValidate.begin(), endpointsToValidate.end());
				if (endpointIds.empty()) {
					return;
				}
				std::ranges::sort(endpointIds);
				if (supportsExistenceProbes) {
					endpointScratch.set(
							checkedSizeAdd(endpointScratch.bytes(),
										   endpointIds.size() / 8U + static_cast<size_t>(endpointIds.size() % 8U != 0U),
										   "logical-plan count endpoint presence scratch overflowed"));
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(endpointIds);
					if (present.size() != endpointIds.size()) {
						throw std::runtime_error("logical graph backend changed Node existence batch cardinality");
					}
					for (size_t index = 0U; index < endpointIds.size(); ++index) {
						if (!present[index]) {
							throw std::runtime_error(
									"logical graph backend returned an Edge topology with a missing endpoint");
						}
					}
				} else {
					for (const uint64_t nodeId: endpointIds) {
						validateNodeIdentity(state, nodeId);
					}
				}
			};

			std::vector<DirectEdgePredicateView> edgePredicates;
			edgePredicates.reserve(traversal.getEdgeProperties().size() + pushedEdgePredicates.size());
			for (const auto &[key, expected]: traversal.getEdgeProperties()) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
				}
				if (isNull(expected)) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
				edgePredicates.push_back({key, &expected, nullptr, PropertyValue()});
			}
			std::ranges::sort(edgePredicates, {}, &DirectEdgePredicateView::key);
			for (const auto &predicate: pushedEdgePredicates) {
				const auto existing = std::ranges::find(edgePredicates, predicate.key, &DirectEdgePredicateView::key);
				if (existing == edgePredicates.end())
					edgePredicates.push_back(predicate);
				else if (!existing->sameValue(predicate)) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			std::vector<std::string_view> edgePropertyKeys;
			edgePropertyKeys.reserve(edgePredicates.size());
			for (const auto &predicate: edgePredicates) {
				edgePropertyKeys.push_back(predicate.key);
			}
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() &&
				state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}

			// Only view/scalar slots are retained here; payloads stay in the plan.
			predicateViewBytes.set(
					checkedSizeAdd(checkedSizeMul(checkedSizeAdd(edgePredicates.size(), pushedEdgePredicates.size(),
																 "logical-plan count predicate count overflowed"),
												  sizeof(DirectEdgePredicateView),
												  "logical-plan count predicate view size overflowed"),
								   checkedSizeMul(edgePropertyKeys.size(), sizeof(std::string_view),
												  "logical-plan count predicate key view size overflowed"),
								   "logical-plan count predicate scratch overflowed"));
			size_t inputRows = 0U;
			size_t edgeRecords = 0U;
			// Keep topology-only counts batched as well.  A batch of one was useful
			// for an early prototype, but it turns every topology page into a
			// per-edge callback and prevents the same bounded execution substrate
			// from amortizing validation and selection work when no Property column
			// is requested.
			const size_t batchCapacity = std::min(
					{DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_edge_records, state.shared.bounds.maximum_rows});
			BoundedBatch<storage::LogicalGraphEdgeTopology> pending(
					batchCapacity, state.shared.bounds.maximum_materialized_bytes,
					[](const auto &edge) {
						return checkedSizeAdd(sizeof(storage::LogicalGraphEdgeTopology), edge.type.size(),
											  "logical-plan count topology size overflowed");
					},
					&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(storage::LogicalGraphEdgeTopology));
			pending.reserve(batchCapacity);
			const auto countTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
				if (inputRows == state.shared.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
			};
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return;
				}
				for (const auto &edge: pending.values()) {
					for (const uint64_t endpointId: {edge.source_node_id, edge.target_node_id}) {
						if (!endpointsToValidate.contains(endpointId) &&
							endpointsToValidate.size() == endpointCacheCapacity) {
							throw std::runtime_error(
									"logical-plan direct relationship count exceeded its endpoint Node bound");
						}
						if (!endpointsToValidate.contains(endpointId)) {
							const size_t previous = endpointBytes.bytes();
							endpointBytes.set(checkedSizeAdd(previous, endpointEntryBytes,
															 "logical-plan count endpoint size overflowed"));
							try {
								endpointsToValidate.insert(endpointId);
							} catch (...) {
								endpointBytes.set(previous);
								throw;
							}
						}
					}
				}
				if (edgePredicates.empty()) {
					for (const storage::LogicalGraphEdgeTopology &edge: pending.values()) {
						countTopology(edge);
					}
					pending.clear();
					return;
				}
				MaterializedByteReservation ordinalBytes(&state.shared.materialized_budget);
				ordinalBytes.set(
						checkedSizeMul(pending.size(), sizeof(size_t), "logical-plan count ordinal size overflowed"));
				std::vector<size_t> sortedOrdinals(pending.size());
				for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
					sortedOrdinals[index] = index;
				}
				std::ranges::sort(sortedOrdinals, [&pending](const size_t left, const size_t right) {
					return pending.values()[left].edge_id < pending.values()[right].edge_id;
				});
				detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
				SelectionVector selection(pending.size(), &selectionMemory);
				struct CountPropertyBytePressure {};
				std::function<void(size_t, size_t)> selectRange;
				selectRange = [&](size_t begin, size_t end) {
					try {
						MaterializedByteReservation idBytes(&state.shared.materialized_budget);
						const size_t requestedIds =
								checkedSizeMul(end - begin, sizeof(uint64_t), "logical-plan count ID size overflowed");
						if (idBytes.wouldExceed(requestedIds))
							throw CountPropertyBytePressure{};
						idBytes.set(requestedIds);
						std::vector<uint64_t> edgeIds;
						edgeIds.reserve(end - begin);
						for (size_t index = begin; index < end; ++index) {
							const uint64_t id = pending.values()[sortedOrdinals[index]].edge_id;
							if (!edgeIds.empty() && edgeIds.back() == id) {
								throw std::runtime_error(
										"logical graph direct relationship count returned duplicate Edge IDs");
							}
							edgeIds.push_back(id);
						}
						MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next = checkedSizeAdd(columnBytes.bytes(), bytes,
															   "logical-plan count source size overflowed");
							if (columnBytes.wouldExceed(next))
								throw CountPropertyBytePressure{};
							columnBytes.set(next);
							debug::PerfTrace::addValue(
									"logical_plan.direct_relationship_count_source_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						const auto sortedColumns =
								state.shared.snapshot.supportsPropertyReadAdmission()
										? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
												  edgeIds, edgePropertyKeys, admission)
										: state.shared.snapshot.readEdgePropertyColumnsSorted(edgeIds, edgePropertyKeys);
						if (sortedColumns.size() != edgePredicates.size()) {
							throw std::runtime_error("logical graph backend changed Edge Property column cardinality");
						}
						for (const auto &column: sortedColumns) {
							if (column.size() != end - begin) {
								throw std::runtime_error(
										"logical graph backend changed sorted Edge Property result cardinality");
							}
						}
						const size_t retained =
								propertyColumnBytes(sortedColumns, edgePropertyKeys,
													"logical-plan direct Edge Property batch size overflowed", state.shared.context);
						if (columnBytes.wouldExceed(retained))
							throw CountPropertyBytePressure{};
						columnBytes.set(retained);
						for (size_t property = 0U; property < edgePredicates.size(); ++property) {
							for (size_t index = 0U; index < end - begin; ++index) {
								const size_t original = sortedOrdinals[begin + index];
								if (selection.includes(original) &&
									(isNull(sortedColumns[property][index]) ||
									 !edgePredicates[property].matches(sortedColumns[property][index])))
									selection.reject(original);
							}
						}
						return;
					} catch (const CountPropertyBytePressure &) {
						// Destroy columns and refund their lease before issuing a smaller read.
						if (end - begin == 1U) {
							throw std::runtime_error("logical-plan direct Edge Property batch exceeded its byte bound");
						}
					}
					debug::PerfTrace::addValue("logical_plan.direct_relationship_count_split_batches", 1);
					const size_t middle = begin + (end - begin) / 2U;
					selectRange(begin, middle);
					selectRange(middle, end);
				};
				selectRange(0U, pending.size());
				for (size_t index = 0U; index < pending.size(); ++index) {
					if (!selection.includes(index)) {
						continue;
					}
					countTopology(pending.values()[index]);
				}
				pending.clear();
			};
			const auto consumeTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
				checkGuard(state);
				if (edgeRecords == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan direct relationship count exceeded its Edge record bound");
				}
				incrementBounded(edgeRecords, "logical-plan direct relationship count Edge counter overflowed");
				incrementBounded(state.task.stats.edges_scanned, "logical-plan read runtime Edge counter overflowed");
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
					throw std::runtime_error(
							"logical graph backend returned an Edge topology with invalid identity or endpoints");
				}
				if (!relationshipTypeToken.has_value() && !traversal.getEdgeType().empty() &&
					edge.type != traversal.getEdgeType()) {
					return true;
				}
				if (!pending.empty() && pending.wouldExceedBytes(edge))
					flushPending();
				pending.append(edge);
				if (pending.size() == batchCapacity) {
					flushPending();
				}
				return true;
			};
			// The default Edge-reference implementation projects complete Edge records.
			// Use it only when the snapshot explicitly provides the native cheap path;
			// generic readers retain their topology-only projection.
			const size_t edgeVisitBound =
					checkedSizeAdd(state.shared.bounds.maximum_edge_records, 1U,
								   "logical-plan direct relationship count Edge bound overflowed");
			if (state.shared.snapshot.supportsNativeEdgeRefScans() &&
				(traversal.getEdgeType().empty() || relationshipTypeToken.has_value())) {
				state.shared.snapshot.visitEdgeRefsForTypePredicate(edgeVisitBound, relationshipTypeToken, [&consumeTopology, &relationshipTypeToken](
																	 const storage::EntityRef &reference) {
					validateEdgeRef(reference,
									"logical-plan direct relationship count received an invalid or Node reference");
					if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
						return true;
					}
					return consumeTopology(
							{reference.entity_id, reference.source_node_id, reference.target_node_id, {}});
				});
			} else {
				state.shared.snapshot.visitEdgeTopologies(edgeVisitBound, consumeTopology);
			}
			flushPending();
			validateEndpoints();
			endpointsToValidate.clear();
			endpointBytes.reset();
			std::vector<DirectEdgePredicateView>{}.swap(edgePredicates);
			std::vector<DirectEdgePredicateView>{}.swap(pushedEdgePredicates);
			std::vector<std::string_view>{}.swap(edgePropertyKeys);
			predicateViewBytes.reset();
			if (debug::PerfTrace::areValuesEnabled()) {
				debug::PerfTrace::addValue("logical_plan.direct_relationship_count_edge_records",
										   static_cast<int64_t>(edgeRecords));
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitDirectTraversal(
				const logical::LogicalTraversal &traversal, ExecutionState &state, const InternalVisitor &visitor,
				const std::vector<std::pair<std::string, PropertyValue>> *additionalEdgeProperties = nullptr) {
			if (!state.shared.allow_deferred_entity_refs || !state.shared.snapshot.supportsNativeNodeRefScans() ||
				!state.shared.snapshot.supportsNativeEdgeRefScans() ||
				(traversal.getDirection() != "out" && traversal.getDirection() != "in")) {
				return std::nullopt;
			}
			const bool hasAdditionalEdgeProperties =
					additionalEdgeProperties != nullptr && !additionalEdgeProperties->empty();
			const std::vector<logical::LogicalOperator *> children = traversal.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &sourceScan = dynamic_cast<const logical::LogicalNodeScan &>(*children.front());
			// An unanchored topology traversal is cheaper as one global Edge scan;
			// constrained sources retain the adjacency cursor for locality.
			const bool sourceConstrained =
					!sourceScan.getLabels().empty() || !sourceScan.getPropertyPredicates().empty() ||
					!sourceScan.getRangePredicates().empty() || sourceScan.getCompositeEquality().has_value() ||
					!sourceScan.getParameterPropertyPredicates().empty();
			if (sourceConstrained && state.shared.snapshot.supportsNativeAdjacentEdgeRefScans()) {
				return std::nullopt;
			}
			if (sourceScan.getVariable() != traversal.getSourceVar() ||
				((!traversal.getEdgeProperties().empty() || hasAdditionalEdgeProperties) &&
				 traversal.getEdgeVar().empty()) ||
				(!traversal.getTargetProperties().empty() && traversal.getTargetVar().empty())) {
				return std::nullopt;
			}

			const auto parameterPredicates = resolveParameterPropertyPredicates(sourceScan, state);
			if (!parameterPredicates.has_value()) {
				return true;
			}
			struct SourceRow {
				uint64_t node_id = 0U;
				// A source Property predicate may already have selected fields in its
				// row cache. Retain only that useful state; label-only and unfiltered
				// sources need just their stable EntityId until an Edge survives.
				std::optional<LogicalPlanReadRow> cached_row;
				size_t edges_seen = 0U;
			};
			std::vector<SourceRow> sources;
			sources.reserve(std::min(state.shared.bounds.maximum_node_records, size_t{4'096U}));
			// An unconstrained source has no predicate that can reduce the Edge
			// stream.  Keep source bindings lazy and let the global Edge cursor drive
			// the pipeline; scanning every Node first would add an unrelated O(|V|)
			// pass to an O(|E|) operation.  The map is query-local and bounded by the
			// same source-record guard used by the old scan path.
			const bool globalEdgePath = !sourceConstrained;
			std::unordered_map<uint64_t, size_t> sourceOrdinals;
			if (globalEdgePath) {
				sourceOrdinals.reserve(std::min(state.shared.bounds.maximum_node_records, size_t{4'096U}));
			} else {
				state.shared.snapshot.visitNodeRefsForLabelPredicate(
						state.shared.bounds.maximum_node_records, sourceScan.getLabels(),
						[&](const storage::EntityRef &reference, const bool labelsMatch) {
							checkGuard(state);
							if (reference.is_edge || reference.entity_id == 0U) {
								throw std::runtime_error(
										"logical graph direct traversal returned an invalid source Node reference");
							}
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan direct traversal Node counter overflowed");
							if (labelsMatch) {
								sources.push_back({reference.entity_id, std::nullopt, 0U});
							}
							return true;
						});
				if (sources.empty()) {
					return true;
				}
				std::ranges::sort(sources, {}, &SourceRow::node_id);
				if (const auto duplicate = std::ranges::adjacent_find(sources, {}, &SourceRow::node_id);
					duplicate != sources.end()) {
					throw std::runtime_error(
							"logical graph direct traversal returned duplicate source Node references");
				}
			}

			std::vector<std::pair<std::string, PropertyValue>> sourceEqualities = sourceScan.getPropertyPredicates();
			sourceEqualities.insert(sourceEqualities.end(), parameterPredicates->begin(), parameterPredicates->end());
			std::vector<std::string> sourcePropertyKeys;
			sourcePropertyKeys.reserve(sourceEqualities.size() + sourceScan.getRangePredicates().size());
			bool impossibleSourcePredicate = false;
			const auto appendSourcePropertyKey = [&sourcePropertyKeys](const std::string &key) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				if (std::ranges::find(sourcePropertyKeys, key) == sourcePropertyKeys.end()) {
					sourcePropertyKeys.push_back(key);
				}
			};
			for (const auto &[key, expected]: sourceEqualities) {
				appendSourcePropertyKey(key);
				impossibleSourcePredicate = impossibleSourcePredicate || isNull(expected);
			}
			for (const logical::RangePredicate &range: sourceScan.getRangePredicates()) {
				appendSourcePropertyKey(range.key);
			}
			if (const auto &composite = sourceScan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					appendSourcePropertyKey(composite->keys[index]);
					impossibleSourcePredicate = impossibleSourcePredicate || isNull(composite->values[index]);
				}
			}
			if (impossibleSourcePredicate) {
				return true;
			}
			if (!sourcePropertyKeys.empty()) {
				std::vector<uint64_t> sourceIds;
				sourceIds.reserve(sources.size());
				for (const SourceRow &source: sources) {
					sourceIds.push_back(source.node_id);
				}
				struct SourcePropertyColumn {
					std::string key;
					std::vector<PropertyValue> values;
				};
				std::vector<SourcePropertyColumn> sourceColumns;
				sourceColumns.reserve(sourcePropertyKeys.size());
				std::vector<std::string_view> sourcePropertyKeyViews;
				sourcePropertyKeyViews.reserve(sourcePropertyKeys.size());
				for (const std::string &key: sourcePropertyKeys) {
					sourcePropertyKeyViews.push_back(key);
				}
				MaterializedByteReservation sourcePropertyBytes(&state.shared.materialized_budget);
				const size_t sourceFrames = checkedSizeAdd(
						checkedSizeMul(sourcePropertyKeyViews.size(), sizeof(std::vector<PropertyValue>),
									   "logical-plan source Property frame size overflowed"),
						checkedSizeMul(checkedSizeMul(sourcePropertyKeyViews.size(), sourceIds.size(),
													  "logical-plan source Property frame size overflowed"),
									   sizeof(PropertyValue), "logical-plan source Property frame size overflowed"),
						"logical-plan source Property frame size overflowed");
				const size_t sourceIdBytes = checkedSizeMul(sourceIds.capacity(), sizeof(uint64_t),
															"logical-plan source Property ID size overflowed");
				sourcePropertyBytes.set(checkedSizeAdd(sourceFrames, sourceIdBytes,
													   "logical-plan source Property batch size overflowed"));
				const std::function<void(size_t)> admitSourceProperty = [&](size_t bytes) {
					checkGuard(state);
					sourcePropertyBytes.set(checkedSizeAdd(sourcePropertyBytes.bytes(), bytes,
														   "logical-plan source Property payload size overflowed"));
				};
				const bool supportsSourceAdmission = state.shared.snapshot.supportsPropertyReadAdmission();
				std::vector<std::vector<PropertyValue>> sourceValues =
						supportsSourceAdmission
								? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
										  sourceIds, sourcePropertyKeyViews, admitSourceProperty)
								: state.shared.snapshot.readNodePropertyColumnsSorted(sourceIds, sourcePropertyKeyViews);
				if (sourceValues.size() != sourcePropertyKeys.size()) {
					throw std::runtime_error("logical graph backend changed source Node Property column cardinality");
				}
				for (size_t column = 0U; column < sourcePropertyKeys.size(); ++column) {
					if (sourceValues[column].size() != sources.size()) {
						throw std::runtime_error(
								"logical graph backend changed sorted Node Property result cardinality");
					}
				}
				const size_t retainedSourceBytes = propertyColumnBytes(
						sourceValues, sourcePropertyKeyViews, "logical-plan source Property batch size overflowed", state.shared.context);
				const size_t retainedSourceBatchBytes =
						checkedSizeAdd(sourceIdBytes, std::max(sourceFrames, retainedSourceBytes),
									   "logical-plan source Property batch size overflowed");
				if (sourcePropertyBytes.wouldExceed(retainedSourceBatchBytes)) {
					throw std::runtime_error("shared materialization exceeded its byte bound");
				}
				sourcePropertyBytes.set(retainedSourceBatchBytes);
				for (size_t column = 0U; column < sourcePropertyKeys.size(); ++column) {
					sourceColumns.push_back({sourcePropertyKeys[column], std::move(sourceValues[column])});
				}
				const auto sourceValue = [&sourceColumns](const size_t sourceIndex,
														  const std::string &key) -> const PropertyValue & {
					const auto found = std::ranges::find(sourceColumns, key, &SourcePropertyColumn::key);
					if (found == sourceColumns.end()) { // ZYX_COV_EXCL_LINE: keys are collected before column reads.
						throw std::logic_error("logical-plan source Property column is missing its requested key");
					}
					return found->values[sourceIndex];
				};
				std::vector<SourceRow> selectedSources;
				selectedSources.reserve(sources.size());
				for (size_t sourceIndex = 0U; sourceIndex < sources.size(); ++sourceIndex) {
					checkGuard(state);
					bool matches = true;
					for (const auto &[key, expected]: sourceEqualities) {
						const PropertyValue &actual = sourceValue(sourceIndex, key);
						if (isNull(actual) || actual != expected) {
							matches = false;
							break;
						}
					}
					for (const logical::RangePredicate &range: sourceScan.getRangePredicates()) {
						if (!matches) {
							break;
						}
						const PropertyValue &actual = sourceValue(sourceIndex, range.key);
						if (isNull(actual) ||
							(!isNull(range.minValue) &&
							 (range.minInclusive ? actual < range.minValue : actual <= range.minValue)) ||
							(!isNull(range.maxValue) &&
							 (range.maxInclusive ? actual > range.maxValue : actual >= range.maxValue))) {
							matches = false;
						}
					}
					if (const auto &composite = sourceScan.getCompositeEquality(); matches && composite.has_value()) {
						for (size_t index = 0U; index < composite->keys.size(); ++index) {
							const PropertyValue &actual = sourceValue(sourceIndex, composite->keys[index]);
							if (isNull(actual) || actual != composite->values[index]) {
								matches = false;
								break;
							}
						}
					}
					if (!matches) {
						continue;
					}
					SourceRow selected = std::move(sources[sourceIndex]);
					LogicalPlanReadRow row;
					row.values.emplace(sourceScan.getVariable(),
									   storage::EntityRef{selected.node_id, false, 0U, 0U, 0U});
					for (SourcePropertyColumn &column: sourceColumns) {
						// The temporary sorted columns have no later consumer. Transfer
						// each surviving value into the row cache instead of keeping a
						// second owned copy alive until the whole source batch is done.
						row.property_cache[sourceScan.getVariable()].insert_or_assign(
								column.key, std::move(column.values[sourceIndex]));
					}
					selected.cached_row.emplace(std::move(row));
					selectedSources.push_back(std::move(selected));
				}
				sources = std::move(selectedSources);
				if (sources.empty()) {
					return true;
				}
			}

			std::vector<std::pair<std::string, PropertyValue>> edgePredicates;
			edgePredicates.reserve(traversal.getEdgeProperties().size() +
								   (hasAdditionalEdgeProperties ? additionalEdgeProperties->size() : 0U));
			for (const auto &[key, expected]: traversal.getEdgeProperties()) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
				}
				edgePredicates.emplace_back(key, expected);
			}
			// Inline relationship properties originate in an unordered map; keep
			// their request deterministic.  Predicates extracted from an outer
			// expression are appended below in expression order.
			std::ranges::sort(edgePredicates, {}, &std::pair<std::string, PropertyValue>::first);
			if (hasAdditionalEdgeProperties) {
				for (const auto &[key, expected]: *additionalEdgeProperties) {
					if (key.empty()) {
						throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
					}
					const auto existing =
							std::ranges::find(edgePredicates, key, &std::pair<std::string, PropertyValue>::first);
					if (existing != edgePredicates.end()) {
						if (existing->second != expected) {
							// A contradictory pushed hint can never match.  Retain the
							// generic path so the original filter owns its semantics.
							return std::nullopt;
						}
						continue;
					}
					edgePredicates.emplace_back(key, expected);
				}
			}
			std::vector<std::string_view> edgePropertyKeys;
			edgePropertyKeys.reserve(edgePredicates.size());
			for (const auto &[key, expected]: edgePredicates) {
				static_cast<void>(expected);
				edgePropertyKeys.push_back(key);
			}
			const std::vector<std::pair<std::string, PropertyValue>> &targetPredicates =
					traversal.getTargetProperties();
			validateTraversalTargetProperties(traversal);
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() &&
				state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
				relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					return true;
				}
			}

			debug::ScopedPerfTimer timer("logical_plan.direct_edge_scan");
			debug::PerfTrace::addValue("logical_plan.direct_edge_scan", 1);
			bool continueVisitor = true;
			size_t edgeRecords = 0U;
			struct PendingEdge {
				storage::LogicalGraphEdgeTopology topology;
				// Preserve the snapshot-local relationship type token when the input
				// came from a native reference cursor.  A topology projection may not
				// carry this token, so zero means that the type identity is unknown.
				uint64_t type_token = 0U;
				size_t source_ordinal = 0U;
			};
			// Property roots are owner-keyed and benefit from a sorted multi-lookup.
			// Keep the batch bounded and retain the record-at-a-time path for
			// topology-only traversals, whose callers may stop after one result.
			// Property-bearing traversals use bounded batches; topology-only rows
			// remain one-at-a-time so a visitor can stop before the next physical
			// Edge is fetched (an observable early-termination contract).
			const size_t desiredEdgeBatchCapacity =
					edgePredicates.empty() && targetPredicates.empty()
							? size_t{1U}
							: std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_edge_records,
										state.shared.bounds.maximum_rows});
			const size_t edgeBatchCapacity = std::min(desiredEdgeBatchCapacity,
					std::max<size_t>(1U, state.shared.materialized_budget.available() / (8U * sizeof(PendingEdge))));
			detail::BudgetMemoryResource pendingMemory(state.shared.materialized_budget);
			std::pmr::vector<PendingEdge> pending{&pendingMemory};
			pending.reserve(edgeBatchCapacity);
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return continueVisitor;
				}
				// Property-bearing global traversals already admit a bounded batch.
				// Validate all newly observed source endpoints in that same batch so
				// one Entity Directory descent is not paid per Edge.  Topology-only
				// traversal keeps edgeBatchCapacity == 1 and therefore retains its
				// strict visitor early-termination contract.
				if (globalEdgePath && state.shared.snapshot.supportsNodeExistenceProbes()) {
					std::vector<uint64_t> sourceIds;
					sourceIds.reserve(pending.size());
					for (const PendingEdge &candidate: pending) {
						if (candidate.source_ordinal >= sources.size()) {
							throw std::runtime_error("logical graph direct traversal lost a source ordinal");
						}
						sourceIds.push_back(sources[candidate.source_ordinal].node_id);
					}
					std::ranges::sort(sourceIds);
					sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
					debug::PerfTrace::addValue("logical_plan.direct_source_existence_batch_requests", 1);
					debug::PerfTrace::addValue("logical_plan.direct_source_existence_batch_ids",
											   static_cast<int64_t>(sourceIds.size()));
					const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(sourceIds);
					if (present.size() != sourceIds.size()) {
						throw std::runtime_error("logical graph backend changed direct traversal source cardinality");
					}
					if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph backend returned an Edge topology with a missing source Node");
					}
				}
				detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
				SelectionVector selection(pending.size(), &selectionMemory);
				MaterializedByteReservation selectedPropertyBytes(&state.shared.materialized_budget);
				MaterializedByteReservation reorderedPropertyBytes(&state.shared.materialized_budget);
				std::vector<std::vector<PropertyValue>> selectedProperties;
				if (!edgePredicates.empty()) {
					MaterializedByteReservation orderingBytes(&state.shared.materialized_budget);
					orderingBytes.set(checkedSizeMul(pending.size(), 2U * sizeof(size_t) + sizeof(uint64_t),
						"logical-plan direct traversal ordering size overflowed"));
					std::vector<size_t> sortedOrdinals(pending.size());
					for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
						sortedOrdinals[index] = index;
					}
					std::ranges::sort(sortedOrdinals, [&pending](const size_t left, const size_t right) {
						return pending[left].topology.edge_id < pending[right].topology.edge_id;
					});
					std::vector<uint64_t> edgeIds;
					edgeIds.reserve(pending.size());
					std::vector<size_t> uniqueOrdinals(pending.size());
					orderingBytes.set(checkedSizeAdd(
						checkedSizeMul(sortedOrdinals.capacity() + uniqueOrdinals.capacity(), sizeof(size_t),
							"logical-plan direct traversal ordering capacity overflowed"),
						checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t),
							"logical-plan direct traversal Edge ID capacity overflowed"),
						"logical-plan direct traversal ordering capacity overflowed"));
					for (const size_t ordinal: sortedOrdinals) {
						const uint64_t edgeId = pending[ordinal].topology.edge_id;
						if (edgeIds.empty() || edgeId != edgeIds.back()) {
							edgeIds.push_back(edgeId);
						}
						uniqueOrdinals[ordinal] = edgeIds.size() - 1U;
					}
					if (std::ranges::any_of(edgePredicates,
											[](const auto &predicate) { return isNull(predicate.second); })) {
						selection.rejectAll();
					} else {
						size_t keyBytes = 0U;
						for (const std::string_view key: edgePropertyKeys) {
							keyBytes =
									checkedSizeAdd(keyBytes, key.size(),
												   "logical-plan direct traversal Edge Property key size overflowed");
						}
						if (selectedPropertyBytes.wouldExceed(keyBytes)) {
							throw std::runtime_error(
									"logical-plan direct traversal Edge Property batch exceeded its byte bound");
						}
						selectedPropertyBytes.set(keyBytes);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next =
									checkedSizeAdd(selectedPropertyBytes.bytes(), bytes,
												   "logical-plan direct traversal Edge Property batch size overflowed");
							if (selectedPropertyBytes.wouldExceed(next)) {
								throw std::runtime_error("logical-plan direct traversal Edge Property batch "
														 "exceeded its byte bound");
							}
							selectedPropertyBytes.set(next);
						};
						auto sortedColumns =
								state.shared.snapshot.supportsPropertyReadAdmission()
										? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
												  edgeIds, edgePropertyKeys, admission)
										: state.shared.snapshot.readEdgePropertyColumnsSorted(edgeIds, edgePropertyKeys);
						const size_t selectedEdgeBytes = propertyColumnBytes(
								sortedColumns, edgePropertyKeys,
								"logical-plan direct traversal Edge Property batch size overflowed", state.shared.context);
						if (selectedPropertyBytes.wouldExceed(selectedEdgeBytes)) {
							throw std::runtime_error(
									"logical-plan direct traversal Edge Property batch exceeded its byte bound");
						}
						selectedPropertyBytes.set(selectedEdgeBytes);
						if (sortedColumns.size() != edgePredicates.size()) {
							throw std::runtime_error("logical graph backend changed Edge Property column cardinality");
						}
						reorderedPropertyBytes.set(checkedSizeMul(edgePredicates.size(), sizeof(std::vector<PropertyValue>),
							"logical-plan reordered Property column size overflowed"));
						selectedProperties.resize(edgePredicates.size());
						reorderedPropertyBytes.set(checkedSizeMul(selectedProperties.capacity(), sizeof(std::vector<PropertyValue>),
							"logical-plan reordered Property column size overflowed"));
						for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
							const std::vector<PropertyValue> &sortedValues = sortedColumns[propertyIndex];
							if (sortedValues.size() != edgeIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed sorted Edge Property result cardinality");
							}
							const size_t previousBytes = reorderedPropertyBytes.bytes();
							reorderedPropertyBytes.set(checkedSizeAdd(previousBytes,
								checkedSizeMul(pending.size(), sizeof(PropertyValue), "logical-plan reordered Property slot size overflowed"),
								"logical-plan reordered Property size overflowed"));
							std::vector<PropertyValue> values(pending.size());
							reorderedPropertyBytes.set(checkedSizeAdd(previousBytes,
								checkedSizeMul(values.capacity(), sizeof(PropertyValue), "logical-plan reordered Property slot size overflowed"),
								"logical-plan reordered Property size overflowed"));
							for (const size_t ordinal: sortedOrdinals) {
								const auto checkpoint = [&] { checkGuardNow(state); };
								const PropertyValue &source = sortedValues[uniqueOrdinals[ordinal]];
								const size_t beforeCopy = reorderedPropertyBytes.bytes();
								reorderedPropertyBytes.set(checkedSizeAdd(beforeCopy,
									detail::retainedPropertyValueBytes(source, checkpoint), "logical-plan reordered Property payload size overflowed"));
								values[ordinal] = detail::copyPropertyValueGuarded(source, checkpoint);
								reorderedPropertyBytes.set(checkedSizeAdd(beforeCopy,
									detail::retainedPropertyValueBytes(values[ordinal], checkpoint), "logical-plan reordered Property payload size overflowed"));
							}
							selection.forEachSelected([&](const size_t index) {
								if (isNull(values[index]) || values[index] != edgePredicates[propertyIndex].second) {
									selection.reject(index);
								}
							});
							selectedProperties[propertyIndex] = std::move(values);
						}
					}
				}

				// Sorted source columns have been destroyed; reordered copies retain their own reservation.
				selectedPropertyBytes.reset();

				struct TargetCandidate {
					LogicalPlanReadRow row;
					uint64_t target_id = 0U;
					size_t property_ordinal = 0U;
				};
				// Resolve labels before Property columns so a label-rejected target does
				// not consume a selected-Property read. Binding is checked first because
				// a repeated variable can reject a non-self traversal before any target
				// access is observable.
				MaterializedByteReservation targetCopiedPropertyBytes(&state.shared.materialized_budget);
				std::vector<TargetCandidate> targets;
				targets.reserve(pending.size());
				selection.forEachSelected([&](const size_t index) {
					checkGuard(state);
					const PendingEdge &candidate = pending[index];
					SourceRow &source = sources[candidate.source_ordinal];
					const uint64_t targetId = traversal.getDirection() == "out" ? candidate.topology.target_node_id
																				: candidate.topology.source_node_id;
					LogicalPlanReadRow expanded;
					if (source.cached_row.has_value()) {
						const size_t previous = targetCopiedPropertyBytes.bytes();
						targetCopiedPropertyBytes.set(checkedSizeAdd(previous, logicalReadRowBytes(*source.cached_row, state.shared.context),
							"logical-plan expanded row size overflowed"));
						expanded = detail::copyLogicalReadRowGuarded(*source.cached_row, [&] { checkGuardNow(state); });
						targetCopiedPropertyBytes.set(checkedSizeAdd(previous, logicalReadRowBytes(expanded, state.shared.context),
							"logical-plan expanded row size overflowed"));
					}
					if (!source.cached_row.has_value()) {
						expanded.values.emplace(sourceScan.getVariable(),
												storage::EntityRef{source.node_id, false, 0U, 0U, 0U});
					}
					const storage::EntityRef edgeRef{candidate.topology.edge_id, true,
													 candidate.topology.source_node_id,
													 candidate.topology.target_node_id, candidate.type_token};
					const storage::EntityRef targetRef{targetId, false, 0U, 0U, 0U};
					if (!bindValue(expanded, traversal.getEdgeVar(), edgeRef) ||
						!bindValue(expanded, traversal.getTargetVar(), targetRef)) {
						return;
					}
					for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
						expanded.property_cache[traversal.getEdgeVar()].insert_or_assign(
								edgePredicates[propertyIndex].first, copyReservedPropertyValue(
							selectedProperties[propertyIndex][index], state, targetCopiedPropertyBytes));
					}
					targets.push_back({std::move(expanded), targetId, 0U});
				});

				// Target labels are a predicate on the whole pending batch, not on the
				// Edge callback. Resolve the fixed predicate once and retain the original
				// Edge order while applying the sorted result.
				if (!targets.empty() && !traversal.getTargetLabels().empty()) {
					std::vector<uint64_t> targetIds;
					targetIds.reserve(targets.size());
					for (const TargetCandidate &target: targets) {
						if (targetIds.empty() || targetIds.back() != target.target_id) {
							targetIds.push_back(target.target_id);
						}
					}
					detail::sortUniqueIdsGuarded(targetIds, [&] {
						if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					});
					const std::vector<bool> labelsMatch =
							state.shared.snapshot.readNodeLabelMatchesSorted(targetIds, traversal.getTargetLabels());
					if (labelsMatch.size() != targetIds.size()) {
						throw std::runtime_error("logical graph backend changed target label-match cardinality");
					}
					std::vector<TargetCandidate> labelMatched;
					labelMatched.reserve(targets.size());
					for (TargetCandidate &target: targets) {
						const auto position = std::ranges::lower_bound(targetIds, target.target_id);
						const size_t ordinal = static_cast<size_t>(position - targetIds.begin());
						if (position != targetIds.end() && labelsMatch[ordinal]) {
							labelMatched.push_back(std::move(target));
						}
					}
					targets = std::move(labelMatched);
				}

				if (!targets.empty() && !targetPredicates.empty()) {
					std::vector<std::string> targetPropertyKeys;
					targetPropertyKeys.reserve(targetPredicates.size());
					bool impossibleTargetPredicate = false;
					for (const auto &[key, expected]: targetPredicates) {
						if (key.empty()) {
							throw std::invalid_argument("logical-plan Node Property predicate has an empty key");
						}
						if (std::ranges::find(targetPropertyKeys, key) == targetPropertyKeys.end()) {
							targetPropertyKeys.push_back(key);
						}
						impossibleTargetPredicate = impossibleTargetPredicate || isNull(expected);
					}
					if (impossibleTargetPredicate) {
						targets.clear();
					} else {
						std::vector<size_t> sortedOrdinals(targets.size());
						for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
							sortedOrdinals[index] = index;
						}
						std::ranges::sort(sortedOrdinals, [&targets](const size_t left, const size_t right) {
							return targets[left].target_id < targets[right].target_id;
						});
						std::vector<uint64_t> targetIds;
						targetIds.reserve(targets.size());
						for (const size_t ordinal: sortedOrdinals) {
							if (targetIds.empty() || targets[ordinal].target_id != targetIds.back()) {
								targetIds.push_back(targets[ordinal].target_id);
							}
							targets[ordinal].property_ordinal = targetIds.size() - 1U;
						}
						struct TargetPropertyColumn {
							std::string key;
							std::vector<PropertyValue> values;
						};
						std::vector<std::string_view> targetPropertyKeyViews;
						targetPropertyKeyViews.reserve(targetPropertyKeys.size());
						for (const std::string &key: targetPropertyKeys) {
							targetPropertyKeyViews.push_back(key);
						}
						MaterializedByteReservation targetPropertyBytes(&state.shared.materialized_budget);
						size_t targetKeyBytes = 0U;
						for (const std::string_view key: targetPropertyKeyViews) {
							targetKeyBytes = checkedSizeAdd(targetKeyBytes, key.size(),
															"logical-plan direct target Property key size overflowed");
						}
						if (targetPropertyBytes.wouldExceed(targetKeyBytes)) {
							throw std::runtime_error(
									"logical-plan direct target Property batch exceeded its byte bound");
						}
						targetPropertyBytes.set(targetKeyBytes);
						const std::function<void(size_t)> targetAdmission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next =
									checkedSizeAdd(targetPropertyBytes.bytes(), bytes,
												   "logical-plan direct target Property batch size overflowed");
							if (targetPropertyBytes.wouldExceed(next)) {
								throw std::runtime_error(
										"logical-plan direct target Property batch exceeded its byte bound");
							}
							targetPropertyBytes.set(next);
						};
						auto targetValues = state.shared.snapshot.supportsPropertyReadAdmission()
													? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															  targetIds, targetPropertyKeyViews, targetAdmission)
													: state.shared.snapshot.readNodePropertyColumnsSorted(
															  targetIds, targetPropertyKeyViews);
						const size_t selectedTargetPayloadBytes =
								propertyColumnBytes(targetValues, targetPropertyKeyViews,
													"logical-plan direct target Property batch size overflowed", state.shared.context);
						if (targetPropertyBytes.wouldExceed(selectedTargetPayloadBytes)) {
							throw std::runtime_error(
									"logical-plan direct target Property batch exceeded its byte bound");
						}
						targetPropertyBytes.set(selectedTargetPayloadBytes);
						if (targetValues.size() != targetPropertyKeys.size()) {
							throw std::runtime_error(
									"logical graph backend changed target Node Property column cardinality");
						}
						std::vector<TargetPropertyColumn> targetColumns;
						targetColumns.reserve(targetPropertyKeys.size());
						for (size_t column = 0U; column < targetPropertyKeys.size(); ++column) {
							if (targetValues[column].size() != targetIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed sorted target Node Property result cardinality");
							}
							targetColumns.push_back({targetPropertyKeys[column], std::move(targetValues[column])});
						}
						for (TargetCandidate &target: targets) {
							checkGuard(state);
							bool matchesTarget = true;
							for (const auto &[key, expected]: targetPredicates) {
								const auto column = std::ranges::find(targetColumns, key, &TargetPropertyColumn::key);
								if (column ==
									targetColumns.end()) { // ZYX_COV_EXCL_LINE: keys are collected before column reads.
									throw std::logic_error(
											"logical-plan target Property column is missing its requested key");
								}
								const PropertyValue &actual = column->values[target.property_ordinal];
								if (isNull(actual) || actual != expected) {
									matchesTarget = false;
									break;
								}
							}
							if (!matchesTarget) {
								continue;
							}
							for (const TargetPropertyColumn &column: targetColumns) {
								target.row.property_cache[traversal.getTargetVar()].insert_or_assign(
										column.key, column.values[target.property_ordinal]);
							}
							if (continueVisitor) {
								continueVisitor = visitor(std::move(target.row));
							}
						}
					}
				} else {
					for (TargetCandidate &target: targets) {
						checkGuard(state);
						if (!continueVisitor) {
							break;
						}
						continueVisitor = visitor(std::move(target.row));
					}
				}
				pending.clear();
				return continueVisitor;
			};
			const auto consumeTopology = [&](const storage::LogicalGraphEdgeTopology &edge,
											 const uint64_t typeToken = 0U) {
				checkGuard(state);
				validateEdge(edge);
				if (edgeRecords == state.shared.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan direct traversal exceeded its global Edge bound");
				}
				incrementBounded(edgeRecords, "logical-plan direct traversal Edge counter overflowed");
				incrementBounded(state.task.stats.edges_scanned, "logical-plan read runtime Edge counter overflowed");

				const uint64_t sourceId = traversal.getDirection() == "out" ? edge.source_node_id : edge.target_node_id;
				size_t sourceOrdinal = 0U;
				if (globalEdgePath) {
					if (sourceId == 0U) {
						throw std::runtime_error("logical graph direct traversal returned a zero source Node ID");
					}
					const auto existing = sourceOrdinals.find(sourceId);
					if (existing == sourceOrdinals.end()) {
						if (sources.size() == state.shared.bounds.maximum_node_records) {
							throw std::runtime_error(
									"logical-plan direct traversal exceeded its source Node record bound");
						}
						// Property-bearing batches validate source liveness in flushPending().
						// Keep the scalar check only for topology-only batches, whose capacity is
						// intentionally one to preserve strict early termination.
						if (state.shared.snapshot.supportsNodeExistenceProbes() && edgeBatchCapacity == 1U &&
							!state.shared.snapshot.containsNode(sourceId)) {
							return true;
						}
						sourceOrdinal = sources.size();
						sourceOrdinals.emplace(sourceId, sourceOrdinal);
						sources.push_back({sourceId, std::nullopt, 0U});
					} else {
						sourceOrdinal = existing->second;
					}
				} else {
					auto source = std::ranges::lower_bound(sources, sourceId, {}, &SourceRow::node_id);
					if (source == sources.end() || source->node_id != sourceId ||
						source->edges_seen == state.shared.bounds.maximum_edges_per_node) {
						return true;
					}
					incrementBounded(source->edges_seen,
									 "logical-plan direct traversal per-Node Edge counter overflowed");
					sourceOrdinal = static_cast<size_t>(source - sources.begin());
				}
				if (globalEdgePath) {
					SourceRow &source = sources[sourceOrdinal];
					if (source.edges_seen == state.shared.bounds.maximum_edges_per_node) {
						return true;
					}
					incrementBounded(source.edges_seen,
									 "logical-plan direct traversal per-Node Edge counter overflowed");
				}
				if (!relationshipTypeToken.has_value() && !traversal.getEdgeType().empty() &&
					edge.type != traversal.getEdgeType()) {
					return true;
				}
				pending.push_back({{edge.edge_id, edge.source_node_id, edge.target_node_id, {}}, typeToken, sourceOrdinal});
				return pending.size() != edgeBatchCapacity || flushPending();
			};
			const size_t edgeVisitBound = checkedSizeAdd(state.shared.bounds.maximum_edge_records, 1U,
														 "logical-plan direct traversal Edge bound overflowed");
			if (traversal.getEdgeType().empty() || relationshipTypeToken.has_value()) {
				state.shared.snapshot.visitEdgeRefsForTypePredicate(edgeVisitBound, relationshipTypeToken, [&consumeTopology, &relationshipTypeToken](
																	 const storage::EntityRef &reference) {
					validateEdgeRef(reference, "logical-plan direct traversal received an invalid or Node reference");
					if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
						return true;
					}
					return consumeTopology(
							{reference.entity_id, reference.source_node_id, reference.target_node_id, {}},
							reference.type_token);
				});
			} else {
				state.shared.snapshot.visitEdgeTopologies(edgeVisitBound, consumeTopology);
			}
			if (continueVisitor) {
				static_cast<void>(flushPending());
			}
			const int64_t tracedEdgeRecords = edgeRecords > static_cast<size_t>(std::numeric_limits<int64_t>::max())
													  ? std::numeric_limits<int64_t>::max()
													  : static_cast<int64_t>(edgeRecords);
			debug::PerfTrace::addValue("logical_plan.direct_edge_records", tracedEdgeRecords);
			return continueVisitor;
		}

		// Page-local native Node scans do not promise logical-ID order. Batch selected
		// Property columns therefore sort only the bounded lookup request, then restore
		// the scan order before exposing rows to the logical runtime.
		[[nodiscard]] std::optional<bool>
		tryVisitBatchedNativeNodeRefScan(const logical::LogicalNodeScan &scan,
										 const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
										 ExecutionState &state, const InternalVisitor &visitor,
										 const NodeColumnReceiver *columnReceiver = nullptr) {
			if (!state.shared.allow_deferred_entity_refs || !state.shared.snapshot.supportsNativeNodeRefScans()) {
				return std::nullopt;
			}

			std::vector<std::string> propertyKeys;
			propertyKeys.reserve(scan.getPropertyPredicates().size() + parameterPredicates.size() +
								 scan.getRangePredicates().size());
			const auto appendKey = [&propertyKeys](std::string_view key) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Node Property predicate has an empty key");
				}
				if (std::ranges::find(propertyKeys, key) == propertyKeys.end()) {
					propertyKeys.emplace_back(key);
				}
			};
			const auto collectEqualityKeys = [&](const auto &predicates) {
				for (const auto &[key, expected]: predicates) {
					if (isNull(expected))
						return false;
					appendKey(key);
				}
				return true;
			};
			if (!collectEqualityKeys(scan.getPropertyPredicates()) || !collectEqualityKeys(parameterPredicates)) {
				return std::nullopt;
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				appendKey(range.key);
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					if (isNull(composite->values[index])) {
						return std::nullopt;
					}
					appendKey(composite->keys[index]);
				}
			}
			if (columnReceiver) {
				for (const auto key : columnReceiver->keys) appendKey(key);
			}
			if (propertyKeys.empty()) {
				return std::nullopt;
			}
			MaterializedByteReservation propertyKeyBytes(&state.shared.materialized_budget);
			if (columnReceiver) {
				size_t bytes = checkedSizeMul(propertyKeys.capacity(), sizeof(std::string),
					"logical-plan Node column key capacity overflowed");
				for (const auto &key : propertyKeys) bytes = checkedSizeAdd(bytes,
					detail::dynamicStringBufferBytes(key), "logical-plan Node column key buffers overflowed");
				if (propertyKeyBytes.wouldExceed(bytes)) return std::nullopt;
				propertyKeyBytes.set(bytes);
			}

			struct PendingNodeRef {
				storage::EntityRef reference;
			};
			// Keep reference buffering below a quarter of the available allowance so
			// IDs, ordinal mappings, selected values and a row can coexist at flush.
			const size_t byteBatchCapacity =
					std::max<size_t>(1U, state.shared.materialized_budget.available() / sizeof(PendingNodeRef) / 4U);
			const size_t batchCapacity = std::min({DEFAULT_PROPERTY_BATCH_ROWS, byteBatchCapacity,
												   state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_rows});
			BoundedBatch<PendingNodeRef> pending(
					batchCapacity, state.shared.bounds.maximum_materialized_bytes,
					[](const auto &) { return sizeof(PendingNodeRef); }, &state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(PendingNodeRef));
			pending.reserve(batchCapacity);
			bool continueVisitor = true;
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return continueVisitor;
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(propertyKeys.size());
				for (const auto &key: propertyKeys)
					keyViews.push_back(key);
				struct NodeColumnBytePressure {};
				std::function<bool(size_t, size_t)> processRange;
				processRange = [&](size_t begin, size_t end) -> bool {
					try {
						MaterializedByteReservation lookupBytes(&state.shared.materialized_budget);
						const size_t scratch = checkedSizeMul(end - begin, sizeof(uint64_t) + 2U * sizeof(size_t),
															  "logical-plan Node column lookup size overflowed");
						if (lookupBytes.wouldExceed(scratch))
							throw NodeColumnBytePressure{};
						lookupBytes.set(scratch);
						std::vector<size_t> sortedOrdinals(end - begin);
						std::iota(sortedOrdinals.begin(), sortedOrdinals.end(), begin);
						std::ranges::sort(sortedOrdinals, [&](size_t left, size_t right) {
							return pending.values()[left].reference.entity_id <
								   pending.values()[right].reference.entity_id;
						});
						std::vector<uint64_t> nodeIds;
						nodeIds.reserve(end - begin);
						std::vector<size_t> lookupOrdinals(end - begin);
						for (size_t sorted = 0U; sorted < sortedOrdinals.size(); ++sorted) {
							const size_t original = sortedOrdinals[sorted];
							const uint64_t id = pending.values()[original].reference.entity_id;
							if (!nodeIds.empty() && nodeIds.back() == id) {
								throw std::runtime_error(
										"logical graph native Node reference scan returned duplicate Node IDs");
							}
							nodeIds.push_back(id);
							lookupOrdinals[original - begin] = sorted;
						}
						if (columnReceiver && columnReceiver->consumeDirect &&
							state.shared.snapshot.supportsNodePropertyCellBatches()) {
							std::optional<detail::DirectPropertyColumns> direct;
							std::optional<detail::BoundColumnLayout> directLayout;
							std::optional<detail::BatchEnvelope> directEnvelope;
							detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
							std::optional<SelectionVector> selected;
							bool guardFailed = false;
							const auto directGuard = [&] {
								try { checkGuardNow(state); } catch (...) { guardFailed = true; throw; }
							};
							try {
								direct.emplace(state.shared.snapshot, nodeIds.size(), keyViews.size(), state.shared.materialized_budget);
								selected.emplace(nodeIds.size(), &selectionMemory);
								detail::BoundNodeColumnPredicates predicates(scan, parameterPredicates, propertyKeys, state.shared.materialized_budget);
								MaterializedByteReservation sourceScratch(&state.shared.materialized_budget);
								state.shared.snapshot.readNodePropertyCellsSortedWithAdmission(nodeIds, keyViews,
									[&](size_t row, size_t column, std::optional<PropertyValue> value) {
										direct->append(row, column, std::move(value), directGuard);
									}, [&](size_t bytes) {
										directGuard();
										sourceScratch.set(checkedSizeAdd(sourceScratch.bytes(), bytes, "direct source bytes overflowed"));
									});
								direct->seal(); sourceScratch.reset();
								(void)predicates.filter(*direct, *selected, directGuard);
								if (state.shared.bound_program) {
									try {
										directLayout.emplace(propertyKeys, state.shared.materialized_budget,
											std::pmr::get_default_resource(), directGuard);
										detail::BudgetMemoryResource metadataMemory(state.shared.materialized_budget);
										std::pmr::vector<detail::BatchColumnSource> sources(&metadataMemory);
										std::pmr::vector<uint32_t> selectedRows(&metadataMemory);
										std::pmr::vector<uint64_t> originalOrdinals(nodeIds.size(), 0U, &metadataMemory);
										for (size_t c = 0; c < propertyKeys.size(); ++c) sources.push_back(detail::BatchColumnSource::direct(*direct, c));
										for (size_t original = 0; original < lookupOrdinals.size(); ++original) {
											directGuard(); const auto sorted = lookupOrdinals[original]; originalOrdinals[sorted] = original;
											if (selected->includes(sorted)) selectedRows.push_back(static_cast<uint32_t>(sorted));
										}
										directEnvelope.emplace(state.shared.bound_program->root().program, *directLayout,
											state.shared.snapshot, nodeIds.size(), state.shared.materialized_budget);
										directEnvelope->publish(propertyKeys, sources, nodeIds.size(), selectedRows, originalOrdinals, directGuard);
										debug::PerfTrace::addValue("logical_plan.batch_envelope.published", 1);
									} catch (const MaterializedByteLimitExceeded &) {
										if (guardFailed) throw;
										directEnvelope.reset(); directLayout.reset();
										debug::PerfTrace::addValue("logical_plan.batch_envelope.admission_fallback", 1);
									}
								}
							} catch (const MaterializedByteLimitExceeded &) {
								if (guardFailed) throw;
								// Private construction only: there have been no consumer effects.
								throw NodeColumnBytePressure{};
							}
							debug::PerfTrace::addValue("logical_plan.direct_property.batches", 1);
							debug::PerfTrace::addValue("logical_plan.direct_property.rows", static_cast<int64_t>(direct->size()));
							debug::PerfTrace::addValue("logical_plan.direct_property.rejected", static_cast<int64_t>(direct->size() - selected->selectedCount()));
							// Consumer exceptions are outside preparation fallback and never replay.
							return columnReceiver->consumeDirect(propertyKeys, *direct, *selected, lookupOrdinals);
						}
						MaterializedByteReservation rowBytes(&state.shared.materialized_budget);
						LogicalPlanReadRow row;
						const auto prepareRow = [&](const storage::EntityRef &reference) {
							row.values.emplace(scan.getVariable(), reference);
							for (const auto &key: propertyKeys)
								row.property_cache[scan.getVariable()].emplace(key, PropertyValue());
						};
						if (!columnReceiver) prepareRow(pending.values()[begin].reference);
						size_t structuralBytes = columnReceiver ? 0U : logicalReadRowBytes(row, state.shared.context);
						if (rowBytes.wouldExceed(structuralBytes))
							throw NodeColumnBytePressure{};
						rowBytes.set(structuralBytes);
						MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next = checkedSizeAdd(columnBytes.bytes(), bytes,
															   "logical-plan Node source size overflowed");
							if (columnBytes.wouldExceed(next))
								throw NodeColumnBytePressure{};
							columnBytes.set(next);
							graph::debug::PerfTrace::addValue(
									"logical_plan.node_predicate_source_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						auto columns = state.shared.snapshot.supportsPropertyReadAdmission()
											   ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
														 nodeIds, keyViews, admission)
											   : state.shared.snapshot.readNodePropertyColumnsSorted(nodeIds, keyViews);
						if (columns.size() != propertyKeys.size()) {
							throw std::runtime_error("logical graph backend changed Node Property column cardinality");
						}
						for (const auto &column: columns) {
							if (column.size() != end - begin) {
								throw std::runtime_error(
										"logical graph backend changed sorted Node Property result cardinality");
							}
						}
						const size_t retained =
								propertyColumnBytes(columns, keyViews, "logical-plan Node selected size overflowed", state.shared.context);
						if (columnBytes.wouldExceed(retained))
							throw NodeColumnBytePressure{};
						columnBytes.set(retained);
						std::optional<detail::BoundNodeColumnPredicates> boundPredicates;
						try {
							boundPredicates.emplace(scan, parameterPredicates, propertyKeys, state.shared.materialized_budget);
						} catch (const MaterializedByteLimitExceeded &) {
							// Optional metadata is admitted only after the source and row
							// owners. Declining it preserves the existing scalar path.
						}
						detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
						std::optional<SelectionVector> selection;
						const size_t bitmapBytes = SelectionVector::requiredStorageBytes(end - begin);
						if (boundPredicates && bitmapBytes <= state.shared.materialized_budget.available()) {
							try { selection.emplace(end - begin, &selectionMemory); }
							catch (const MaterializedByteLimitExceeded &) {
								// Optional bitmap admission can decline only before filtering.
							}
						}
						std::optional<detail::BoundNodeScanBatch> scanBatch;
						if (selection && state.shared.bound_program) {
							try {
								scanBatch.emplace(*state.shared.bound_program, state.shared.snapshot, propertyKeys, columns, nodeIds,
									state.shared.materialized_budget, [&] { checkGuardNow(state); });
								debug::PerfTrace::addValue("logical_plan.bound_scan.published", 1);
							} catch (const MaterializedByteLimitExceeded &) {
								debug::PerfTrace::addValue("logical_plan.bound_scan.admission_fallback", 1);
							}
						}
						if (selection) {
							const auto filtered = scanBatch
								? boundPredicates->filter(scanBatch->envelope(), scanBatch->layout(), *selection, [&] { checkGuard(state); })
								: boundPredicates->filter(columns, *selection, state.shared.materialized_budget, [&] { checkGuard(state); });
							debug::PerfTrace::addValue("logical_plan.node_column_filter.typed_columns",
								static_cast<int64_t>(filtered.typed_columns));
							debug::PerfTrace::addValue("logical_plan.node_column_filter.scalar_columns",
								static_cast<int64_t>(filtered.scalar_columns));
							debug::PerfTrace::addValue("logical_plan.node_column_filter.rejected_before_rows",
								static_cast<int64_t>(selection->size() - selection->selectedCount()));
							if (scanBatch) {
								try { scanBatch->publishSelection(*selection, lookupOrdinals, [&] { checkGuardNow(state); }); }
								catch (const MaterializedByteLimitExceeded &) { scanBatch.reset(); }
							}
							// Payload mutation invalidates the borrowed source matrix. Release
							// every envelope before clearing rejected cells or moving rows.
							scanBatch.reset();
							// Rejected payloads cannot escape. Release them before handing a
							// survivor downstream, retaining only charged matrix slots/NULLs.
							for (size_t index = 0U; index < selection->size(); ++index) {
								if (selection->includes(index)) continue;
								for (auto &column : columns) {
									checkGuard(state);
									const size_t bytes = detail::retainedPropertyValueBytes(column[index], [&] { checkGuardNow(state); });
									column[index] = PropertyValue();
									columnBytes.set(checkedSizeAdd(columnBytes.bytes() - bytes, 1U,
										"logical-plan rejected column bytes overflowed"));
								}
							}
						}
						// Predicate instructions borrow the source only during filtering.
						// Release their capacity before downstream owning-output admission.
						boundPredicates.reset();
						if (columnReceiver && selection) {
							debug::PerfTrace::addValue("logical_plan.node_column_consumer.batches", 1);
							continueVisitor = columnReceiver->consume(propertyKeys, columns, *selection, lookupOrdinals);
							return continueVisitor;
						}
						if (columnReceiver) {
							prepareRow(pending.values()[begin].reference);
							structuralBytes = logicalReadRowBytes(row, state.shared.context);
							if (rowBytes.wouldExceed(structuralBytes)) throw NodeColumnBytePressure{};
							rowBytes.set(structuralBytes);
						}
						// No byte-pressure retry is issued after a row is delivered: only
						// pre-read/pre-delivery pressure may split this original scan range.
						for (size_t index = begin; index < end && continueVisitor; ++index) {
							checkGuard(state);
							const size_t sorted = lookupOrdinals[index - begin];
							if (selection && !selection->includes(sorted)) continue;
							if (index != begin) {
								row = LogicalPlanReadRow{};
								prepareRow(pending.values()[index].reference);
								rowBytes.set(structuralBytes);
							}
							for (size_t column = 0U; column < propertyKeys.size(); ++column) {
								const size_t valueBytes = detail::retainedPropertyValueBytes(columns[column][sorted], [&] { checkGuardNow(state); });
								columnBytes.set(columnBytes.bytes() - valueBytes);
								rowBytes.set(checkedSizeAdd(rowBytes.bytes() - 1U, valueBytes,
															"logical-plan Node row size overflowed"));
								row.property_cache.at(scan.getVariable()).at(propertyKeys[column]) =
										std::move(columns[column][sorted]);
							}
							if (selection.has_value() || nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan, parameterPredicates,
												   state, true)) {
								rowBytes.reset();
								continueVisitor = visitor(std::move(row));
							} else {
								row = LogicalPlanReadRow{};
								rowBytes.reset();
							}
						}
						return continueVisitor;
					} catch (const NodeColumnBytePressure &) {
						if (end - begin == 1U)
							throw std::runtime_error("logical-plan Node Property batch exceeded its byte bound");
					}
					graph::debug::PerfTrace::addValue("logical_plan.node_predicate_split_batches", 1);
					const size_t middle = begin + (end - begin) / 2U;
					return processRange(begin, middle) && processRange(middle, end);
				};
				continueVisitor = processRange(0U, pending.size());
				pending.clear();
				return continueVisitor;
			};

			state.shared.snapshot.visitNodeRefsForLabelPredicate(
					state.shared.bounds.maximum_node_records, scan.getLabels(),
					[&](const storage::EntityRef &reference, const bool labelsMatch) {
						checkGuard(state);
						validateNodeRef(reference,
										"logical graph backend returned an invalid or Edge reference from a Node scan");
						incrementBounded(state.task.stats.nodes_scanned,
										 "logical-plan read runtime Node counter overflowed");
						if (!labelsMatch) {
							return true;
						}
						if (!pending.empty() && pending.wouldExceedBytes(PendingNodeRef{reference}) && !flushPending())
							return false;
						pending.append(PendingNodeRef{reference});
						return pending.size() != batchCapacity || flushPending();
					});
			if (continueVisitor) {
				static_cast<void>(flushPending());
			}
			return continueVisitor;
		}

		// Consume one authenticated native index batch.  Keeping this logic in a
		// batch-local helper lets a cursor stop after the downstream LIMIT without
		// collecting the complete index result in memory.
		[[nodiscard]] bool visitIndexedCandidateBatch(
				const storage::IndexCandidateBatch &nativeCandidates, const logical::LogicalNodeScan &scan,
				const std::vector<std::pair<std::string, PropertyValue>> &equalityPredicates,
				const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates, size_t &candidateRecords,
				std::unordered_set<uint64_t> &seenCandidateIds, MaterializedByteReservation &seenCandidateBytes,
				ExecutionState &state, const InternalVisitor &visitor) {
			if (nativeCandidates.snapshot_generation == 0U) {
				throw std::runtime_error("logical graph native indexed Node candidates have no snapshot generation");
			}
			const size_t candidateBytes = logicalIndexCandidateBatchBytes(nativeCandidates, state.shared.context);
			if (candidateBytes > state.shared.bounds.maximum_candidate_bytes) {
				throw std::runtime_error("logical graph indexed Node candidate batch exceeded its byte bound");
			}
			debug::PerfTrace::addValue(
					"logical_plan.index_candidate.materialized_bytes",
					static_cast<int64_t>(
							std::min(candidateBytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
			const std::optional<uint64_t> snapshotGeneration = state.shared.snapshot.nativeSnapshotGeneration();
			if (!snapshotGeneration.has_value() || *snapshotGeneration == 0U ||
				nativeCandidates.snapshot_generation != *snapshotGeneration) {
				throw std::runtime_error(
						"logical graph native indexed Node candidates do not belong to the pinned snapshot");
			}
			if (candidateRecords > state.shared.bounds.maximum_node_records ||
				nativeCandidates.candidates.size() > state.shared.bounds.maximum_node_records - candidateRecords) {
				throw std::runtime_error("logical graph indexed Node lookup exceeded the Node scan bound");
			}
			candidateRecords += nativeCandidates.candidates.size();
			std::unordered_set<std::string> requiredCandidateKeys;
			for (const auto &[key, value]: equalityPredicates) {
				static_cast<void>(value);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan indexed Node predicate has an empty Property key");
				}
				requiredCandidateKeys.insert(key);
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan indexed Node range predicate has an empty Property key");
				}
				requiredCandidateKeys.insert(range.key);
			}
			std::unordered_map<std::string, const std::vector<PropertyValue> *> candidateColumns;
			if (nativeCandidates.predicates_verified) {
				if (!nativeCandidates.candidates.empty() && nativeCandidates.verified_property_columns.empty()) {
					throw std::runtime_error(
							"logical graph native indexed Node candidates have no verified Property columns");
				}
				for (const auto &column: nativeCandidates.verified_property_columns) {
					if (column.key.empty() || column.values.size() != nativeCandidates.candidates.size() ||
						!candidateColumns.emplace(column.key, &column.values).second) {
						throw std::runtime_error("logical graph native indexed Node candidates have mismatched "
												 "verified Property fields");
					}
				}
				for (const std::string &key: requiredCandidateKeys) {
					if (!candidateColumns.contains(key)) {
						throw std::runtime_error("logical graph native indexed Node candidates omit a requested "
												 "verified Property field");
					}
				}
			}
			// The immutable producer batch stays alive while rows are handed downstream.
			// Reserve its retained storage as well as each independent row-cache copy.
			MaterializedByteReservation verifiedCandidateBytes(&state.shared.materialized_budget);
			if (nativeCandidates.predicates_verified)
				verifiedCandidateBytes.set(candidateBytes);
			MaterializedByteReservation candidateScratchBytes(&state.shared.materialized_budget);
			const bool residualColumns = !nativeCandidates.predicates_verified && state.shared.snapshot.supportsNativeNodeRefScans() &&
				!nativeCandidates.candidates.empty();
			if (!residualColumns) {
				candidateScratchBytes.set(checkedSizeAdd(
					checkedSizeMul(nativeCandidates.candidates.size(), sizeof(uint64_t), "logical-plan candidate scratch overflowed"),
					nativeCandidates.candidates.size() / 8U + (nativeCandidates.candidates.size() % 8U != 0U),
					"logical-plan candidate scratch overflowed"));
			}
			uint64_t previousNodeId = 0U;
			std::vector<uint64_t> candidateNodeIds;
			if (!residualColumns) candidateNodeIds.reserve(nativeCandidates.candidates.size());
			for (const storage::EntityRef &candidate: nativeCandidates.candidates) {
				validateNodeRef(candidate,
								"logical graph indexed Node lookup returned invalid or non-monotonic Node references");
				if (candidate.entity_id <= previousNodeId) {
					throw std::runtime_error(
							"logical graph indexed Node lookup returned a non-monotonic candidate batch");
				}
				previousNodeId = candidate.entity_id;
				if (seenCandidateIds.contains(candidate.entity_id)) {
					throw std::runtime_error("logical graph indexed Node lookup returned duplicate Node references");
				}

				if (!residualColumns) candidateNodeIds.push_back(candidate.entity_id);
			}
			const auto rememberCandidate = [&](uint64_t id) {
				const size_t previousSeenBytes = seenCandidateBytes.bytes();
				seenCandidateBytes.set(checkedSizeAdd(previousSeenBytes, sizeof(uint64_t),
													  "logical-plan candidate seen-ID size overflowed"));
				try {
					seenCandidateIds.insert(id);
				} catch (...) {
					seenCandidateBytes.set(previousSeenBytes);
					throw;
				}
			};
			std::vector<bool> candidateLabelsMatch(residualColumns ? 0U : nativeCandidates.candidates.size(), true);
			if (!nativeCandidates.label_scope_verified && !candidateNodeIds.empty() && !scan.getLabels().empty()) {
				candidateLabelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(candidateNodeIds, scan.getLabels());
				if (candidateLabelsMatch.size() != candidateNodeIds.size()) {
					throw std::runtime_error("logical graph backend changed indexed Node label-match cardinality");
				}
				if (std::ranges::any_of(candidateLabelsMatch, [](const bool matches) { return !matches; })) {
					throw std::runtime_error(
							"logical graph native indexed Node candidates violate the requested label scope");
				}
			}
			// An index candidate stream is only an access path.  When the index does
			// not already provide verified predicate columns, validate the residual
			// predicates as one selected-column batch instead of reopening a complete
			// Node for every candidate.  Complete materialization remains limited to
			// candidates that survive the batch; deferred consumers can keep the
			// authenticated EntityRef all the way to their result boundary.
			if (residualColumns) {
				std::vector<std::string> propertyKeys;
				const auto appendPropertyKey = [&propertyKeys](const std::string &key) {
					if (key.empty()) {
						throw std::invalid_argument("logical-plan indexed Node predicate has an empty Property key");
					}
					if (std::ranges::find(propertyKeys, key) == propertyKeys.end()) {
						propertyKeys.push_back(key);
					}
				};
				for (const auto &[key, value]: scan.getPropertyPredicates()) {
					static_cast<void>(value);
					appendPropertyKey(key);
				}
				for (const auto &[key, value]: parameterPredicates) {
					static_cast<void>(value);
					appendPropertyKey(key);
				}
				for (const logical::RangePredicate &range: scan.getRangePredicates()) {
					appendPropertyKey(range.key);
				}
				if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
					if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
						throw std::invalid_argument(
								"logical-plan indexed Node composite predicate has an invalid shape");
					}
					for (const std::string &key: composite->keys) {
						appendPropertyKey(key);
					}
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(propertyKeys.size());
				for (const std::string &key: propertyKeys) {
					keyViews.push_back(key);
				}
				const size_t minimumCandidateBytes =
						checkedSizeAdd(sizeof(storage::EntityRef),
									   checkedSizeMul(propertyKeys.size(), sizeof(PropertyValue),
													  "logical-plan indexed Node batch capacity overflowed"),
									   "logical-plan indexed Node batch capacity overflowed");
				const size_t byteBatchCapacity =
						std::max<size_t>(1U, state.shared.bounds.maximum_materialized_bytes / minimumCandidateBytes);
				// Adapt the row cap before reading. Admission-capable readers reserve
				// payload bytes at the source; compatibility readers are checked on
				// return. Pre-delivery pressure can further split these ranges.
				const size_t candidateBatchCapacity =
						std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_node_records,
								  state.shared.bounds.maximum_rows, byteBatchCapacity});
				if (candidateBatchCapacity == 0U) {
					throw std::runtime_error("logical-plan indexed Node Property batch has no positive capacity");
				}
				struct IndexedColumnBytePressure {};
				std::function<bool(size_t, size_t)> processRange;
				processRange = [&](size_t offset, size_t count) -> bool {
					try {
						checkGuard(state);
						MaterializedByteReservation idBytes(&state.shared.materialized_budget);
						const size_t idSlots = checkedSizeMul(count, sizeof(uint64_t), "logical-plan residual IDs overflowed");
						if (idBytes.wouldExceed(idSlots)) throw IndexedColumnBytePressure{};
						idBytes.set(idSlots);
						std::vector<uint64_t> batchIds;
						batchIds.reserve(count);
						idBytes.set(checkedSizeMul(batchIds.capacity(), sizeof(uint64_t), "logical-plan residual IDs overflowed"));
						for (size_t i = 0U; i < count; ++i) batchIds.push_back(nativeCandidates.candidates[offset + i].entity_id);
						std::vector<bool> labelsMatch;
						if (!nativeCandidates.label_scope_verified && !scan.getLabels().empty()) {
							const size_t labelSlots = checkedSizeAdd(idSlots, count / 8U + (count % 8U != 0U), "logical-plan residual labels overflowed");
							if (idBytes.wouldExceed(labelSlots)) throw IndexedColumnBytePressure{};
							idBytes.set(labelSlots);
							labelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(batchIds, scan.getLabels());
							if (labelsMatch.size() != count) throw std::runtime_error("logical graph indexed Node labels changed cardinality");
							idBytes.set(checkedSizeAdd(idSlots, labelsMatch.capacity() / 8U + (labelsMatch.capacity() % 8U != 0U), "logical-plan residual labels overflowed"));
						}
						MaterializedByteReservation rowBytes(&state.shared.materialized_budget);
						LogicalPlanReadRow row;
						const auto prepareRow = [&](const storage::EntityRef &candidate) {
							row.values.emplace(scan.getVariable(), candidate);
							for (const auto &key: propertyKeys)
								row.property_cache[scan.getVariable()].emplace(key, PropertyValue());
						};
						prepareRow(nativeCandidates.candidates[offset]);
						const size_t structuralBytes = logicalReadRowBytes(row, state.shared.context);
						if (rowBytes.wouldExceed(structuralBytes))
							throw IndexedColumnBytePressure{};
						rowBytes.set(structuralBytes);
						MaterializedByteReservation columnBytes(&state.shared.materialized_budget);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							checkGuard(state);
							const size_t next = checkedSizeAdd(columnBytes.bytes(), bytes,
															   "logical-plan indexed Node source size overflowed");
							if (columnBytes.wouldExceed(next))
								throw IndexedColumnBytePressure{};
							columnBytes.set(next);
							debug::PerfTrace::addValue(
									"logical_plan.index_predicate_source_bytes",
									static_cast<int64_t>(
											std::min(bytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
						};
						auto columns = propertyKeys.empty() ? std::vector<std::vector<PropertyValue>>{}
									   : state.shared.snapshot.supportsPropertyReadAdmission()
											   ? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
														 batchIds, keyViews, admission)
											   : state.shared.snapshot.readNodePropertyColumnsSorted(batchIds, keyViews);
						if (columns.size() != propertyKeys.size() ||
							std::ranges::any_of(columns, [&](const auto &column) { return column.size() != count; }))
							throw std::runtime_error(
									"logical graph backend changed indexed Node Property column cardinality");
						const size_t selectedBytes = propertyColumnBytes(
								columns, keyViews, "logical-plan indexed Node Property batch size overflowed", state.shared.context);
						if (columnBytes.wouldExceed(selectedBytes))
							throw IndexedColumnBytePressure{};
						columnBytes.set(selectedBytes);
						for (size_t batchIndex = 0U; batchIndex < count; ++batchIndex) {
							checkGuard(state);
							const size_t candidateIndex = offset + batchIndex;
							const auto &candidate = nativeCandidates.candidates[candidateIndex];
							rememberCandidate(candidate.entity_id);
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan indexed Node lookup counter overflowed");
							if (batchIndex != 0U) {
								row = LogicalPlanReadRow{};
								prepareRow(candidate);
								rowBytes.set(structuralBytes);
							}
							for (size_t column = 0U; column < propertyKeys.size(); ++column) {
								const size_t valueBytes =
										detail::retainedPropertyValueBytes(columns[column][batchIndex], [&] { checkGuardNow(state); });
								columnBytes.set(columnBytes.bytes() - valueBytes);
								rowBytes.set(checkedSizeAdd(rowBytes.bytes() - 1U, valueBytes,
															"logical-plan indexed Node row size overflowed"));
								row.property_cache.at(scan.getVariable()).at(propertyKeys[column]) =
										std::move(columns[column][batchIndex]);
							}
							if (!nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan, parameterPredicates,
													state, labelsMatch.empty() || labelsMatch[batchIndex])) {
								row = LogicalPlanReadRow{};
								rowBytes.reset();
								continue;
							}
							if (!state.shared.allow_deferred_entity_refs) {
								auto node = readNodeCached(state, candidate.entity_id);
								if (!nodeScanMatches(state.shared.snapshot, node, scan, parameterPredicates, state)) {
									throw std::runtime_error(
											"logical graph indexed Node candidate changed during materialization");
								}
								row.values[scan.getVariable()] = std::move(node);
								row.property_cache.erase(scan.getVariable());
								rowBytes.set(logicalReadRowBytes(row, state.shared.context));
							}
							rowBytes.reset();
							if (!visitor(std::move(row)))
								return false;
						}
						return true;
					} catch (const IndexedColumnBytePressure &) {
						if (count == 1U)
							throw std::runtime_error(
									"logical-plan indexed Node Property batch exceeded its byte bound");
					}
					debug::PerfTrace::addValue("logical_plan.index_predicate_split_batches", 1);
					const size_t left = count / 2U;
					return processRange(offset, left) && processRange(offset + left, count - left);
				};
				for (size_t offset = 0U; offset < nativeCandidates.candidates.size(); offset += candidateBatchCapacity) {
					if (!processRange(offset, std::min(candidateBatchCapacity, nativeCandidates.candidates.size() - offset)))
						return false;
				}
				return true;
			}
			for (size_t candidateIndex = 0U; candidateIndex < nativeCandidates.candidates.size(); ++candidateIndex) {
				checkGuard(state);
				const storage::EntityRef &candidate = nativeCandidates.candidates[candidateIndex];
				rememberCandidate(candidate.entity_id);
				incrementBounded(state.task.stats.nodes_scanned, "logical-plan indexed Node lookup counter overflowed");
				if ((nativeCandidates.exact_match_verified || nativeCandidates.predicates_verified) &&
					state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans()) {
					MaterializedByteReservation rowBytes(&state.shared.materialized_budget);
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), candidate);
					rowBytes.set(logicalReadRowBytes(row, state.shared.context));
					const auto copyProperty = [&](const std::string &key, const PropertyValue &value) {
						size_t previous = 0U;
						if (const auto cache = row.property_cache.find(scan.getVariable());
							cache != row.property_cache.end()) {
							if (const auto found = cache->second.find(key); found != cache->second.end()) {
								previous =
										checkedSizeAdd(key.size(), property_utils::getPropertyValueSize(found->second),
													   "logical-plan candidate row size overflowed");
							}
						}
						const size_t next =
								checkedSizeAdd(rowBytes.bytes() - previous,
											   checkedSizeAdd(key.size(), property_utils::getPropertyValueSize(value),
															  "logical-plan candidate row size overflowed"),
											   "logical-plan candidate row size overflowed");
						rowBytes.set(next);
						row.property_cache[scan.getVariable()].insert_or_assign(key, value);
					};
					// Include the cache variable name before publishing any owned values.
					rowBytes.set(checkedSizeAdd(rowBytes.bytes(), scan.getVariable().size(),
												"logical-plan candidate row size overflowed"));
					if (nativeCandidates.predicates_verified) {
						for (const auto &column: nativeCandidates.verified_property_columns) {
							copyProperty(column.key, column.values[candidateIndex]);
						}
						if (!nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan, parameterPredicates,
												state, candidateLabelsMatch[candidateIndex])) {
							throw std::runtime_error("logical graph native indexed Node candidates violate their "
													 "verified Property predicates");
						}
					} else {
						for (const auto &[key, value]: equalityPredicates) {
							copyProperty(key, value);
						}
					}
					rowBytes.reset();
					if (!visitor(std::move(row))) {
						return false;
					}
					continue;
				}
				const storage::LogicalGraphNode node = readNodeCached(state, candidate.entity_id);
				if (nodeScanMatches(state.shared.snapshot, node, scan, parameterPredicates, state)) {
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), node);
					if (!visitor(std::move(row))) {
						return false;
					}
				}
			}
			return true;
		}

		// LIMIT may cross only cardinality-preserving projections. Keep the
		// original input ordinal as the final key so ties match stable full sort.
		[[nodiscard]] bool visitBoundedTopKSort(const logical::LogicalSort &sort, ExecutionState &state,
			const InternalVisitor &visitor, const ChargedRowVisitor *chargedReceiver, size_t maximum) {
			using Record = detail::QuerySpillRowCodec::Record;
			const auto &items = sort.getSortItems();
			MaterializedByteReservation metadata(&state.shared.materialized_budget);
			metadata.set(checkedSizeMul(items.size(), sizeof(const expressions::Expression *), "Top-K metadata overflowed"));
			std::vector<const expressions::Expression *> candidates;
			candidates.reserve(items.size());
			for (const auto &item : items) candidates.push_back(item.expression.get());
			std::optional<detail::BoundExpressionInputs> inputs;
			if (state.shared.bound_program)
				inputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(sort), candidates,
					state.shared.snapshot, state.shared.materialized_budget, [&] { checkGuardNow(state); },
					std::pmr::get_default_resource(), state.shared.context);
			const auto bytes = [&](const Record &record) {
				size_t total = checkedSizeAdd(logicalReadRowBytes(record.row, state.shared.context),
					checkedSizeMul(record.keys.capacity(), sizeof(PropertyValue), "Top-K keys overflowed"), "Top-K row overflowed");
				for (const auto &key : record.keys)
					total = checkedSizeAdd(total, retainedExpressionValueBytes(key, state), "Top-K payload overflowed");
				return total;
			};
			BoundedBatch<Record> rows(maximum, state.shared.bounds.maximum_materialized_bytes, bytes,
				&state.shared.materialized_budget, true, sizeof(LogicalPlanReadRow));
			const auto precedes = [&](const Record &left, const Record &right) {
				checkGuard(state);
				for (size_t i = 0; i < items.size(); ++i) {
					if (left.keys[i] == right.keys[i]) continue;
					if (detail::orderByPrecedes(left.keys[i], right.keys[i], items[i].ascending)) return true;
					if (detail::orderByPrecedes(right.keys[i], left.keys[i], items[i].ascending)) return false;
					return left.ordinal < right.ordinal;
				}
				return left.ordinal < right.ordinal;
			};
			const auto decorate = [&](Record &record, MaterializedByteReservation &charge) {
				charge.set(checkedSizeAdd(logicalReadRowBytes(record.row, state.shared.context),
					checkedSizeMul(items.size(), sizeof(PropertyValue), "Top-K keys overflowed"), "Top-K row overflowed"));
				record.keys.reserve(items.size());
				charge.set(bytes(record));
				for (const auto &item : items) {
					checkGuard(state);
					PropertyValue key;
					{ ScopedBoundInputs scoped(state, inputs, record.row); key = scalarValue(evaluateExpression(*item.expression, record.row, state)); }
					charge.set(checkedSizeAdd(bytes(record), retainedExpressionValueBytes(key, state), "Top-K key growth overflowed"));
					record.keys.push_back(std::move(key));
					charge.set(bytes(record));
				}
			};
			const auto retain = [&](Record record, MaterializedByteReservation charge) {
				decorate(record, charge);
				if (rows.size() == maximum) {
					if (!precedes(record, rows.values().front())) return;
					std::pop_heap(rows.values().begin(), rows.values().end(), precedes);
					{ auto removed = rows.popBackCharged(); }
				}
				rows.appendCharged(std::move(record), std::move(charge));
				std::push_heap(rows.values().begin(), rows.values().end(), precedes);
			};
			struct Pending { MaterializedByteReservation charge; Record record; };
			std::optional<Pending> first;
			size_t seen = 0U;
			const ChargedRowVisitor collect = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
				checkGuard(state);
				if (seen == state.shared.bounds.maximum_rows)
					throw std::runtime_error("logical-plan Sort exceeded its materialization row bound");
				Record record{std::move(row), {}, seen++};
				if (seen == 1U) {
					if (!charge.bytes()) charge.set(bytes(record));
					first.emplace(Pending{std::move(charge), std::move(record)});
					return true;
				}
				if (first) { retain(std::move(first->record), std::move(first->charge)); first.reset(); }
				retain(std::move(record), std::move(charge));
				return true;
			};
			const InternalVisitor uncharged = [&](LogicalPlanReadRow row) {
				return collect(std::move(row), MaterializedByteReservation(&state.shared.materialized_budget));
			};
			const auto expressions = selectBatchableExpressions(candidates);
			const auto complete = selectBatchableCompleteEntityVariables(candidates);
			const auto &child = requireOnlyChild(sort);
			const auto batched = tryVisitBatchedPropertyExpressions(child, expressions, state, uncharged, {}, complete, &collect);
			const bool completed = batched ? *batched : visitOperator(child, state, uncharged,
				supportsChargedRowHandoff(child.getType()) ? &collect : nullptr);
			if (!completed) throw std::runtime_error("Top-K could not prove its input was exhausted");
			const auto emit = [&](Record record, MaterializedByteReservation charge) {
				std::vector<PropertyValue>().swap(record.keys);
				charge.set(logicalReadRowBytes(record.row, state.shared.context));
				if (chargedReceiver) return (*chargedReceiver)(std::move(record.row), std::move(charge));
				charge.reset(); return visitor(std::move(record.row));
			};
			debug::PerfTrace::addValue("logical_plan.sort.topk_input_rows", seen);
			debug::PerfTrace::addValue("logical_plan.sort.topk_retained_rows", rows.size() + (first ? 1U : 0U));
			if (first) return emit(std::move(first->record), std::move(first->charge));
			std::sort_heap(rows.values().begin(), rows.values().end(), precedes);
			std::reverse(rows.values().begin(), rows.values().end());
			while (!rows.empty()) {
				auto row = rows.popBackCharged();
				if (!emit(std::move(row.value), std::move(row.reservation))) return false;
			}
			return true;
		}

		[[nodiscard]] bool visitSpillableSort(const logical::LogicalSort &sort,ExecutionState &state,
			const InternalVisitor &visitor,const ChargedRowVisitor *chargedReceiver,
			storage::LogicalQueryScratchProvider &provider) {
			using Record=detail::QuerySpillRowCodec::Record;
			const auto &items=sort.getSortItems();
			MaterializedByteReservation metadata(&state.shared.materialized_budget);
			metadata.set(checkedSizeMul(items.size(),4U*sizeof(const expressions::Expression *)+sizeof(uint8_t),"Sort spill metadata overflowed"));
			std::vector<const expressions::Expression *> candidates;std::vector<uint8_t> ascending;
			candidates.reserve(items.size());ascending.reserve(items.size());
			for(const auto &item:items){if(!item.expression)throw std::invalid_argument("Sort has an empty expression");candidates.push_back(item.expression.get());ascending.push_back(item.ascending?1:0);}
			std::optional<detail::BoundExpressionInputs> inputs;
			if(state.shared.bound_program)inputs.emplace(*state.shared.bound_program,state.shared.bound_program->id(sort),candidates,
				state.shared.snapshot,state.shared.materialized_budget,[&]{checkGuardNow(state);},std::pmr::get_default_resource(),state.shared.context);
			const auto fixedScalar=[](const PropertyValue &value){const auto type=value.getType();return type==PropertyType::NULL_TYPE || type==PropertyType::BOOLEAN || type==PropertyType::INTEGER || type==PropertyType::DOUBLE;};
			const auto rowBytes=[&](const LogicalPlanReadRow &row){checkGuard(state);
				const bool simple=row.property_cache.empty() && row.entity_metadata_cache.empty() && row.complete_entity_property_cache.empty() && row.path_cache.empty() &&
					std::ranges::all_of(row.values,[&](const auto &entry){const auto *value=std::get_if<PropertyValue>(&entry.second);return value?fixedScalar(*value):std::holds_alternative<storage::EntityRef>(entry.second);});
				return logicalReadRowBytes(row,simple?nullptr:state.shared.context);};
			const auto keyBytes=[&](const PropertyValue &value){if(!fixedScalar(value))return retainedExpressionValueBytes(value,state);checkGuard(state);return detail::retainedPropertyValueBytes(value,[]{});};

			const auto bytes=[&](const Record &record){size_t total=rowBytes(record.row);
				total=checkedSizeAdd(total,checkedSizeMul(record.keys.capacity(),sizeof(PropertyValue),"Sort spill keys overflowed"),"Sort spill row overflowed");
				for(const auto &key:record.keys)total=checkedSizeAdd(total,keyBytes(key),"Sort spill key payload overflowed");return total;};
			detail::ExternalQuerySort sorter(provider,state.shared.materialized_budget,state.shared.bounds.maximum_rows,
				state.shared.bounds.maximum_materialized_bytes,ascending,bytes,[&]{checkGuard(state);},128U*1024U*1024U,1024U,[&]{checkGuardNow(state);},state.shared.context?state.shared.context->resource_account:nullptr);
			struct Pending {MaterializedByteReservation charge;Record record;};std::optional<Pending> pending;bool multiple=false;
			const auto decorate=[&](Record &record,MaterializedByteReservation &charge){
				charge.set(checkedSizeAdd(rowBytes(record.row),checkedSizeMul(items.size(),sizeof(PropertyValue),"Sort spill keys overflowed"),"Sort spill row overflowed"));
				record.keys.reserve(items.size());charge.set(bytes(record));
				for(const auto &item:items){checkGuard(state);PropertyValue key;
					{ScopedBoundInputs scoped(state,inputs,record.row);key=scalarValue(evaluateExpression(*item.expression,record.row,state));}
					const auto extra=keyBytes(key);charge.set(checkedSizeAdd(bytes(record),extra,"Sort spill key growth overflowed"));record.keys.push_back(std::move(key));charge.set(bytes(record));
				}
			};
			const ChargedRowVisitor collect=[&](LogicalPlanReadRow row,MaterializedByteReservation charge){
				if(!multiple && !pending){pending.emplace(Pending{std::move(charge),Record{std::move(row),{},0U}});if(!pending->charge.bytes())pending->charge.set(bytes(pending->record));return true;}
				if(!multiple){multiple=true;decorate(pending->record,pending->charge);sorter.append(std::move(pending->record),std::move(pending->charge));pending.reset();}
				Record record{std::move(row),{},0U};decorate(record,charge);sorter.append(std::move(record),std::move(charge));return true;
			};
			const InternalVisitor uncharged=[&](LogicalPlanReadRow row){return collect(std::move(row),MaterializedByteReservation(&state.shared.materialized_budget));};
			const auto expressions=selectBatchableExpressions(candidates);const auto complete=selectBatchableCompleteEntityVariables(candidates);
			const auto &child=requireOnlyChild(sort);const auto batched=tryVisitBatchedPropertyExpressions(child,expressions,state,uncharged,{},complete,&collect);
			const bool completed=batched ? *batched : visitOperator(child,state,uncharged,supportsChargedRowHandoff(child.getType())?&collect:nullptr);
			if(!completed)throw std::runtime_error("Sort spill could not prove its input was exhausted");
			const auto emit=[&](Record record,MaterializedByteReservation charge){std::vector<PropertyValue>().swap(record.keys);charge.set(rowBytes(record.row));
				if(chargedReceiver)return (*chargedReceiver)(std::move(record.row),std::move(charge));charge.reset();return visitor(std::move(record.row));};
			if(pending)return emit(std::move(pending->record),std::move(pending->charge));
			const bool result=sorter.finish(emit);if(sorter.spilled())debug::PerfTrace::addValue("logical_plan.sort.spilled",1);return result;
		}

		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor, const ChargedRowVisitor *chargedReceiver,
										 const NodeColumnReceiver *columnReceiver) {
			checkGuard(state);
			if (state.shared.bound_program) {
				state.shared.bound_program->validateSnapshot(state.shared.snapshot);
				(void) state.shared.bound_program->view(state.shared.bound_program->id(op));
			}
			switch (op.getType()) {
				case logical::LogicalOpType::LOP_SINGLE_ROW: {
					if (!op.getChildren().empty()) { // ZYX_COV_EXCL_LINE: LogicalSingleRow is a concrete leaf and
													 // cannot own a child.
						throw std::runtime_error("logical SingleRow unexpectedly has a child");
					}
					return state.task.injected_row == nullptr ? visitor({}) :
						visitor(detail::copyLogicalReadRowGuarded(*state.task.injected_row, [&] { checkGuardNow(state); }));
				}
				case logical::LogicalOpType::LOP_NODE_SCAN: {
					const auto &scan = dynamic_cast<const logical::LogicalNodeScan &>(op);
					if (!scan.getChildren().empty() || scan.getVariable().empty()) {
						throw std::invalid_argument("logical-plan Node scan has an invalid shape or variable");
					}
					const auto parameterPredicates = resolveParameterPropertyPredicates(scan, state);
					if (!parameterPredicates.has_value()) {
						return true;
					}
					std::vector<std::pair<std::string, PropertyValue>> equalityPredicates =
							scan.getPropertyPredicates();
					equalityPredicates.insert(equalityPredicates.end(), parameterPredicates->begin(),
											  parameterPredicates->end());
					const bool exactPropertyLookup = !equalityPredicates.empty() && scan.getRangePredicates().empty() &&
													 !scan.getCompositeEquality().has_value();
					const bool rangePropertyLookup = equalityPredicates.empty() &&
													 scan.getRangePredicates().size() == 1U &&
													 !scan.getCompositeEquality().has_value();
					const bool compositeRangePropertyLookup = !equalityPredicates.empty() &&
															  scan.getRangePredicates().size() == 1U &&
															  !scan.getCompositeEquality().has_value();
					// Prefer the streaming candidate contract.  The visitor is wired
					// directly to the downstream operator, so LIMIT/SKIP can stop the
					// producer without materializing another result vector. Backends that
					// do not expose the contract continue through the defensive legacy
					// lookup branch below.
					const auto consumeCandidateStream = [&](const auto &producer) -> std::optional<bool> {
						try {
							const auto stream = producer();
							if (!stream.has_value()) {
								return std::nullopt;
							}
							return *stream;
						} catch (const std::length_error &error) {
							// Storage-neutral adapters report their byte guard as a
							// length_error. Normalize that boundary at the logical
							// runtime so native and compatibility producers expose the
							// same query-facing resource failure.
							if (std::string_view(error.what()) ==
								"logical graph candidate stream exceeded its byte bound") {
								throw std::runtime_error(
										"logical graph indexed Node candidate batch exceeded its byte bound");
							}
							throw;
						}
					};
					size_t indexedCandidateRecords = 0U;
					MaterializedByteReservation seenIndexedCandidateBytes(&state.shared.materialized_budget);
					std::unordered_set<uint64_t> seenIndexedCandidateIds;
					const auto candidateVisitor = [&](const storage::IndexCandidateBatch &batch) {
						return visitIndexedCandidateBatch(batch, scan, equalityPredicates, *parameterPredicates,
														  indexedCandidateRecords, seenIndexedCandidateIds,
														  seenIndexedCandidateBytes, state, visitor);
					};
					if (exactPropertyLookup && state.shared.snapshot.supportsNativeExactNodeCandidates()) {
						if (const auto streamed = consumeCandidateStream([&] {
								return state.shared.snapshot.visitNodeCandidatesByExactProperties(
										scan.getLabels(), equalityPredicates, state.shared.bounds.maximum_node_records,
										state.shared.bounds.maximum_property_entries,
										state.shared.snapshot.supportsPropertyReadAdmission()
												? std::min(state.shared.bounds.maximum_candidate_bytes,
														   std::max<size_t>(1U,
																			state.shared.materialized_budget.available() / 2U))
												: state.shared.bounds.maximum_candidate_bytes,
										candidateVisitor);
							});
							streamed.has_value()) {
							return *streamed;
						}
					}
					if (rangePropertyLookup && state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (const auto streamed = consumeCandidateStream([&] {
								return state.shared.snapshot.visitNodeCandidatesByPropertyRange(
										scan.getLabels(),
										{range.key, range.minValue, range.maxValue, range.minInclusive,
										 range.maxInclusive},
										state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries,
										state.shared.snapshot.supportsPropertyReadAdmission()
												? std::min(state.shared.bounds.maximum_candidate_bytes,
														   std::max<size_t>(1U,
																			state.shared.materialized_budget.available() / 2U))
												: state.shared.bounds.maximum_candidate_bytes,
										candidateVisitor);
							});
							streamed.has_value()) {
							return *streamed;
						}
					}
					if (compositeRangePropertyLookup && state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (const auto streamed = consumeCandidateStream([&] {
								return state.shared.snapshot.visitNodeCandidatesByCompositePropertyRange(
										scan.getLabels(),
										{equalityPredicates,
										 {range.key, range.minValue, range.maxValue, range.minInclusive,
										  range.maxInclusive}},
										state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries,
										state.shared.snapshot.supportsPropertyReadAdmission()
												? std::min(state.shared.bounds.maximum_candidate_bytes,
														   std::max<size_t>(1U,
																			state.shared.materialized_budget.available() / 2U))
												: state.shared.bounds.maximum_candidate_bytes,
										candidateVisitor);
							});
							streamed.has_value()) {
							return *streamed;
						}
					}
					std::optional<storage::IndexCandidateBatch> exactIndexCandidates;
					std::optional<storage::IndexCandidateBatch> rangeIndexCandidates;
					std::optional<std::vector<storage::LogicalGraphNode>> indexNodes;
					if (exactPropertyLookup && state.shared.snapshot.supportsNativeExactNodeCandidates()) {
						exactIndexCandidates = state.shared.snapshot.findNodeCandidatesByExactProperties(
								scan.getLabels(), equalityPredicates, state.shared.bounds.maximum_node_records,
								state.shared.bounds.maximum_property_entries);
					} else if (exactPropertyLookup) {
						indexNodes = state.shared.snapshot.findNodesByExactProperties(scan.getLabels(), equalityPredicates,
																			   state.shared.bounds.maximum_node_records,
																			   state.shared.bounds.maximum_property_entries);
					} else if (rangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
							rangeIndexCandidates = state.shared.snapshot.findNodeCandidatesByPropertyRange(
									scan.getLabels(),
									{range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						} else {
							indexNodes = state.shared.snapshot.findNodesByPropertyRange(
									scan.getLabels(),
									{range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						}
					} else if (compositeRangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (state.shared.snapshot.supportsNativeRangeNodeCandidates()) {
							rangeIndexCandidates = state.shared.snapshot.findNodeCandidatesByCompositePropertyRange(
									scan.getLabels(),
									{equalityPredicates,
									 {range.key, range.minValue, range.maxValue, range.minInclusive,
									  range.maxInclusive}},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						} else {
							indexNodes = state.shared.snapshot.findNodesByCompositePropertyRange(
									scan.getLabels(),
									{equalityPredicates,
									 {range.key, range.minValue, range.maxValue, range.minInclusive,
									  range.maxInclusive}},
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						}
					}
					if (exactIndexCandidates.has_value() || rangeIndexCandidates.has_value()) {
						const storage::IndexCandidateBatch &nativeCandidates =
								exactIndexCandidates.has_value() ? *exactIndexCandidates : *rangeIndexCandidates;
						return visitIndexedCandidateBatch(nativeCandidates, scan, equalityPredicates,
														  *parameterPredicates, indexedCandidateRecords,
														  seenIndexedCandidateIds, seenIndexedCandidateBytes, state,
														  visitor);
					}
					if (indexNodes.has_value()) {
						if (indexNodes->size() > state.shared.bounds.maximum_node_records) {
							throw std::runtime_error("logical graph indexed Node lookup exceeded the Node scan bound");
						}
						uint64_t previousNodeId = 0U;
						for (const storage::LogicalGraphNode &node: *indexNodes) {
							checkGuard(state);
							validateNode(node, state);
							if (node.node_id <= previousNodeId) {
								throw std::runtime_error(
										"logical graph indexed Node lookup returned non-monotonic Node IDs");
							}
							for (const std::string &label: scan.getLabels()) {
								if (std::ranges::find(node.labels, label) == node.labels.end()) {
									throw std::runtime_error("logical graph indexed Node lookup returned a Node "
															 "outside the requested label scope");
								}
							}
							previousNodeId = node.node_id;
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan indexed Node lookup counter overflowed");
							// The physical index is an acceleration only. Re-evaluate all
							// logical scan predicates before exposing an indexed candidate.
							if (nodeScanMatches(state.shared.snapshot, node, scan, *parameterPredicates, state)) {
								LogicalPlanReadRow row;
								row.values.emplace(scan.getVariable(), node);
								if (!visitor(std::move(row))) {
									return false;
								}
							}
						}
						return true;
					}
					if (const std::optional<bool> batched =
								tryVisitBatchedNativeNodeRefScan(scan, *parameterPredicates, state, visitor, columnReceiver);
						batched.has_value()) {
						return *batched;
					}
					if (state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans()) {
						bool continueVisitor = true;
						// Keep label evaluation on the page-local reference scan. The native
						// adapter resolves label tokens once and reports a match fact, avoiding
						// a second point Entity-Map read for every scanned Node.
						state.shared.snapshot.visitNodeRefsForLabelPredicate(
								state.shared.bounds.maximum_node_records, scan.getLabels(),
								[&](const storage::EntityRef &reference, const bool labelsMatch) {
									checkGuard(state);
									validateNodeRef(reference, "logical graph backend returned an invalid or Edge "
															   "reference from a Node scan");
									incrementBounded(state.task.stats.nodes_scanned,
													 "logical-plan read runtime Node counter overflowed");
									if (!labelsMatch) {
										return true;
									}
									LogicalPlanReadRow row;
									row.values.emplace(scan.getVariable(), reference);
									if (nodeRefScanMatches(state.shared.snapshot, row, scan.getVariable(), scan,
														   *parameterPredicates, state, true)) {
										continueVisitor = visitor(std::move(row));
									}
									return continueVisitor;
								});
						return continueVisitor;
					}
					uint64_t afterNodeId = 0U;
					for (;;) {
						const storage::LogicalGraphNodePage page =
								state.shared.snapshot.scanNodesAfter(afterNodeId, state.shared.bounds.maximum_node_page_records,
															  state.shared.bounds.maximum_metadata_pages_per_node_page);
						if (page.nodes.size() > state.shared.bounds.maximum_node_page_records) {
							throw std::runtime_error("logical graph backend exceeded the Node page bound");
						}
						if (page.nodes.empty()) {
							if (page.may_have_more) {
								throw std::runtime_error(
										"logical graph backend returned an empty nonterminal Node page");
							}
							return true;
						}
						uint64_t previousNodeId = afterNodeId;
						for (const storage::LogicalGraphNode &node: page.nodes) {
							checkGuard(state);
							if (node.node_id <= previousNodeId) {
								throw std::runtime_error("logical graph backend returned non-monotonic Node page IDs");
							}
							previousNodeId = node.node_id;
							incrementBounded(state.task.stats.nodes_scanned,
											 "logical-plan read runtime Node counter overflowed");
							if (nodeScanMatches(state.shared.snapshot, node, scan, *parameterPredicates, state)) {
								LogicalPlanReadRow row;
								row.values.emplace(scan.getVariable(), node);
								if (!visitor(std::move(row))) {
									return false;
								}
							}
							if (state.task.stats.nodes_scanned == state.shared.bounds.maximum_node_records) {
								return false;
							}
						}
						afterNodeId = previousNodeId;
						if (!page.may_have_more) {
							return true;
						}
					}
				}
				case logical::LogicalOpType::LOP_FILTER: {
					const auto &filter = dynamic_cast<const logical::LogicalFilter &>(op);
					if (!filter.getPredicate()) {
						throw std::invalid_argument("logical-plan Filter has no predicate");
					}
					const logical::LogicalOperator &filterChild = requireOnlyChild(filter);
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						const expressions::Expression *predicate = filter.getPredicate().get();
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op),
							std::span<const expressions::Expression *const>(&predicate, 1U), state.shared.snapshot,
							state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					const auto acceptFilteredRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						PropertyValue result;
						{ ScopedBoundInputs scopedInputs(state, boundInputs, row);
							result = evaluateScalarExpression(*filter.getPredicate(), row, state); }
						if (booleanOrNull(result) != true)
							return true;
						if (chargedReceiver != nullptr) {
							if (charge.bytes() != 0U)
								charge.set(logicalReadRowBytes(row, state.shared.context));
							return (*chargedReceiver)(std::move(row), std::move(charge));
						}
						charge.reset();
						return visitor(std::move(row));
					};
					// A WHERE clause directly above a single-hop traversal is a common
					// shape for Edge-property predicates.  Feed its safe equality
					// constraints into the same global topology path used by inline
					// relationship properties, while retaining the Filter evaluator as
					// the semantic authority.
					if (filterChild.getType() == logical::LogicalOpType::LOP_TRAVERSAL) {
						const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(filterChild);
						if (const auto edgeProperties = inspectDirectEdgePropertyFilter(*filter.getPredicate(),
																						traversal.getEdgeVar(), state);
							edgeProperties.has_value()) {
							if (const std::optional<bool> direct = tryVisitDirectTraversal(
										traversal, state,
										[&](LogicalPlanReadRow row) {
											const PropertyValue result =
													evaluateScalarExpression(*filter.getPredicate(), row, state);
											return booleanOrNull(result) == true ? visitor(std::move(row)) : true;
										},
										&*edgeProperties);
								direct.has_value()) {
								return *direct;
							}
						}
					}
					// The optimizer stores literal predicates on a NodeScan so the scan can
					// use an exact/range/composite index.  Do not let the generic property
					// prefetch path consume that scan first: it would enumerate every Node,
					// defeating the index and LIMIT early-stop contract.  Residual predicates
					// remain authoritative in the normal Filter evaluator below and can still
					// use the candidate row's selected-property cache.
					const auto *filterScan = dynamic_cast<const logical::LogicalNodeScan *>(&filterChild);
					const bool hasIndexableScanShape =
							filterScan != nullptr && (!filterScan->getPropertyPredicates().empty() ||
													  !filterScan->getRangePredicates().empty() ||
													  filterScan->getCompositeEquality().has_value());
					const bool forwardsDirectColumns = columnReceiver && columnReceiver->filterOwner == &op;
					if (!hasIndexableScanShape && !forwardsDirectColumns) {
						const std::vector<const expressions::Expression *> candidates{filter.getPredicate().get()};
						const std::vector<const expressions::Expression *> propertyExpressions =
								selectBatchableExpressions(candidates);
						const std::vector<std::string> completeEntityVariables =
								selectBatchableCompleteEntityVariables(candidates);
						if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									filterChild, propertyExpressions, state,
									[&](LogicalPlanReadRow row) { return visitor(std::move(row)); },
									[&](LogicalPlanReadRow &row, ExecutionState &predicateState) {
										const PropertyValue result =
												evaluateScalarExpression(*filter.getPredicate(), row, predicateState);
										return booleanOrNull(result) == true;
									},
									completeEntityVariables, chargedReceiver, true);
							batched.has_value()) {
							return *batched;
						}
					}
					const ChargedRowVisitor chargedInput = acceptFilteredRow;
					return visitOperator(
							filterChild, state,
							[&](LogicalPlanReadRow row) {
								return acceptFilteredRow(std::move(row),
														 MaterializedByteReservation(&state.shared.materialized_budget));
							},
							supportsChargedRowHandoff(filterChild.getType()) ? &chargedInput : nullptr,
							forwardsDirectColumns ? columnReceiver : nullptr);
				}
				case logical::LogicalOpType::LOP_PROJECT: {
					const auto &project = dynamic_cast<const logical::LogicalProject &>(op);
					const auto &items = project.getItems();
					if (items.empty()) {
						throw std::invalid_argument("logical-plan Project has no items");
					}
					std::unordered_set<std::string> aliases;
					aliases.reserve(items.size());
					for (const logical::LogicalProjectItem &item: items) {
						if (item.alias.empty() || !item.expression || !aliases.insert(item.alias).second) {
							throw std::invalid_argument("logical-plan Project has an invalid output item");
						}
					}
					detail::BudgetMemoryResource distinctMemory(state.shared.materialized_budget);
					std::pmr::unordered_set<std::pmr::string> distinctRows{&distinctMemory};
					std::optional<detail::ExternalQueryKeySet> externalDistinct;
					if (project.isDistinct() && state.shared.allow_cross_row_entity_batches &&
					        state.shared.bounds.maximum_materialized_bytes >= detail::ExternalQueryKeySet::MINIMUM_QUERY_BYTES) {
					    auto *provider = dynamic_cast<storage::LogicalQueryScratchProvider *>(&state.shared.snapshot);
					    if (provider && provider->queryScratchEnabled())
					        externalDistinct.emplace(*provider, state.shared.materialized_budget, distinctMemory,
					            state.shared.bounds.maximum_distinct_rows, state.shared.bounds.maximum_materialized_bytes / 8U,
					            [&] { checkGuard(state); }, [&] { checkGuardNow(state); },
                                128U * 1024U * 1024U, 1024U,
                                state.shared.context ? state.shared.context->resource_account : nullptr);
					}
					const auto finishDistinct = [&](bool completed) {
					    if (externalDistinct) {
					        externalDistinct->finish();
					        if (externalDistinct->spilled()) debug::PerfTrace::addValue("logical_plan.distinct.spilled", 1);
					    }
					    return completed;
					};
					const auto *identityReference =
							items.size() == 1U && !project.isDistinct()
									? dynamic_cast<const expressions::VariableReferenceExpression *>(
											  items.front().expression.get())
									: nullptr;
					const bool canForwardIdentity = identityReference != nullptr && !identityReference->hasProperty();
					const logical::LogicalOperator &projectChild = requireOnlyChild(project);
					std::optional<detail::BoundProjectionReferences> boundReferences;
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						std::pmr::vector<const expressions::Expression *> expressionInputs(&distinctMemory);
						expressionInputs.reserve(items.size());
						for (const auto &item : items) expressionInputs.push_back(item.expression.get());
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op), expressionInputs,
							state.shared.snapshot, state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					// Single-item ownership forwarding stays on its existing path. The
					// transition adapter benefits repeated direct references in wider
					// projections without charging constant-only expressions for slots.
					if (!state.shared.bound_program && items.size() > 1U && std::ranges::any_of(items, [](const auto &item) {
						return dynamic_cast<const expressions::VariableReferenceExpression *>(item.expression.get()) != nullptr;
					})) {
						MaterializedByteReservation inputNamesBytes(&state.shared.materialized_budget);
						const auto inputs = projectChild.getOutputVariables();
						size_t bytes = checkedSizeMul(inputs.capacity(), sizeof(std::string),
							"logical-plan binding input names overflowed");
						for (const auto &name : inputs) bytes = checkedSizeAdd(bytes, detail::dynamicStringBufferBytes(name),
							"logical-plan binding input name buffers overflowed");
						inputNamesBytes.set(bytes);
						boundReferences.emplace(inputs, items, state.shared.materialized_budget);
					}
					const auto projectRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						if (!project.isDistinct() && items.size() == 1U) {
							const auto *propertyReference =
									dynamic_cast<const expressions::VariableReferenceExpression *>(
											items.front().expression.get());
							if (propertyReference != nullptr && propertyReference->hasProperty()) {
								auto cachedEntity = row.property_cache.find(propertyReference->getVariableName());
								if (cachedEntity != row.property_cache.end()) {
									auto cachedValue = cachedEntity->second.find(propertyReference->getPropertyName());
									if (cachedValue != cachedEntity->second.end()) {
										PropertyValue projectedValue = std::move(cachedValue->second);
										row.values.clear();
										row.property_cache.clear();
										row.complete_entity_property_cache.clear();
										row.entity_metadata_cache.clear();
										row.path_cache.clear();
										row.values.emplace(items.front().alias, std::move(projectedValue));
										charge.set(logicalReadRowBytes(row, state.shared.context));
										if (chargedReceiver != nullptr) {
											return (*chargedReceiver)(std::move(row), std::move(charge));
										}
										charge.reset();
										return visitor(std::move(row));
									}
								}
							}
						}
						if (!project.isDistinct() && items.size() == 1U) {
							const auto *function = dynamic_cast<const expressions::FunctionCallExpression *>(
									items.front().expression.get());
							const auto *entityFunction =
									function == nullptr
											? nullptr
											: dynamic_cast<const expressions::EntityIntrospectionFunction *>(
													  expressions::FunctionRegistry::getInstance().lookupScalarFunction(
															  function->getFunctionName()));
							if (function != nullptr && entityFunction != nullptr &&
								lowerCase(function->getFunctionName()) == "id" && function->getArgumentCount() == 1U) {
								const PropertyValue identity =
										evaluateScalarExpression(*items.front().expression, row, state);
								row.values.clear();
								row.property_cache.clear();
								row.complete_entity_property_cache.clear();
								row.entity_metadata_cache.clear();
								row.path_cache.clear();
								row.values.emplace(items.front().alias, identity);
								charge.set(logicalReadRowBytes(row, state.shared.context));
								if (chargedReceiver != nullptr) {
									return (*chargedReceiver)(std::move(row), std::move(charge));
								}
								charge.reset();
								return visitor(std::move(row));
							}
						}
						if (canForwardIdentity && row.values.contains(identityReference->getVariableName())) {
							const std::string &source = identityReference->getVariableName();
							const std::string &alias = items.front().alias;
							auto forwarded = std::move(row.values.at(source));
							row.values.clear();
							row.values.emplace(alias, std::move(forwarded));
							const auto retainAndRenameAlias = [&](auto &cache) {
								auto entry = cache.find(source);
								if (entry == cache.end()) {
									cache.clear();
									return;
								}
								if (source == alias) {
									std::erase_if(cache,
												  [&](const auto &candidate) { return candidate.first != alias; });
									return;
								}
								auto retained = cache.extract(entry);
								cache.clear();
								retained.key() = alias;
								cache.insert(std::move(retained));
							};
							retainAndRenameAlias(row.property_cache);
							if (source != alias && row.complete_entity_property_cache.erase(source) != 0U) {
								row.complete_entity_property_cache.clear();
								row.complete_entity_property_cache.insert(alias);
							} else {
								std::erase_if(row.complete_entity_property_cache,
											  [&](const std::string &variable) { return variable != alias; });
							}
							retainAndRenameAlias(row.entity_metadata_cache);
							retainAndRenameAlias(row.path_cache);
							charge.set(logicalReadRowBytes(row, state.shared.context));
							if (chargedReceiver != nullptr) {
								return (*chargedReceiver)(std::move(row), std::move(charge));
							}
							charge.reset();
							return visitor(std::move(row));
						}
						const size_t inputBytes = logicalReadRowBytes(row, state.shared.context);
						if (charge.bytes() == 0U)
							charge.set(inputBytes);
						const size_t inputChargeBytes = charge.bytes();
						size_t projectedBytes = 0U;
						LogicalPlanReadRow projected;
						projected.values.reserve(items.size());
						{
							ScopedBoundInputs scopedInputs(state, boundInputs, row);
							std::optional<detail::BoundProjectionReferences::RowBinding> inputSlots;
							if (boundReferences) inputSlots.emplace(*boundReferences, row);
							size_t itemIndex = 0U;
							for (const logical::LogicalProjectItem &item: items) {
								checkGuard(state);
								const auto *instruction = boundReferences ? &boundReferences->instruction(itemIndex) : nullptr;
								const auto *value = instruction == nullptr ? nullptr : boundReferences->value(*instruction);
								if (value != nullptr && instruction->kind == detail::ProjectionReferenceKind::PRK_VALUE) {
									debug::PerfTrace::addValue("logical_plan.bound_projection.slot_reads", 1);
									projected.values.emplace(item.alias, *value);
								} else if (value != nullptr && instruction->kind == detail::ProjectionReferenceKind::PRK_PROPERTY) {
									debug::PerfTrace::addValue("logical_plan.bound_projection.property_reads", 1);
									projected.values.emplace(item.alias, readProperty(state.shared.snapshot, row,
										instruction->expression->getVariableName(), instruction->expression->getPropertyName(), state, value));
								} else {
									projected.values.emplace(item.alias, evaluateExpression(*item.expression, row, state));
								}
								++itemIndex;
								propagateProjectedEntityCaches(*item.expression, row, projected, item.alias, state);
								projectedBytes = logicalReadRowBytes(projected, state.shared.context);
								charge.set(checkedSizeAdd(inputChargeBytes, projectedBytes,
														  "logical-plan Project live row size overflowed"));
							}
						} // Slot borrows end before input destruction or downstream callbacks.
						if (project.isDistinct()) {
							auto key = distinctRowKey(projected, items, &distinctMemory);
							if (externalDistinct) {
								if (!externalDistinct->insert(std::move(key))) return true;
							} else {
								if (distinctRows.contains(key)) return true;
								if (distinctRows.size() == state.shared.bounds.maximum_distinct_rows)
									throw std::runtime_error("logical-plan DISTINCT exceeded its row bound");
								distinctRows.insert(std::move(key));
							}
						}
						row = {};
						if (chargedReceiver != nullptr) {
							charge.set(projectedBytes);
							return (*chargedReceiver)(std::move(projected), std::move(charge));
						}
						charge.reset();
						return visitor(std::move(projected));
					};
					const auto emitProjectedRow = [&](LogicalPlanReadRow row) {
						return projectRow(std::move(row), MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedProject = projectRow;

					// Fuse scalar observers across one projection boundary. The eligibility
					// check rejects every bare/path-dependent use, so the internal NULL
					// slots cannot escape or participate in entity/path comparisons.
					const auto *pathProject = dynamic_cast<const logical::LogicalProject *>(&projectChild);
					if (pathProject != nullptr && !pathProject->isDistinct()) {
						std::unordered_map<std::string, const expressions::Expression *> pathExpressions;
						std::unordered_set<std::string> childAliases;
						bool valid = !pathProject->getItems().empty();
						for (const auto &item: pathProject->getItems()) {
							valid = valid && !item.alias.empty() && item.expression &&
									childAliases.insert(item.alias).second;
							if (item.expression &&
								item.expression->getExpressionType() == expressions::ExpressionType::SHORTEST_PATH) {
								pathExpressions.emplace(item.alias, item.expression.get());
							}
						}
						std::unordered_map<std::string, unsigned> demands;
						std::function<bool(const expressions::Expression *)> inspectDemand;
						inspectDemand = [&](const expressions::Expression *expression) -> bool {
							if (expression == nullptr)
								return false;
							using expressions::ExpressionType;
							switch (expression->getExpressionType()) {
								case ExpressionType::LITERAL:
								case ExpressionType::PARAMETER:
									return true;
								case ExpressionType::VARIABLE_REFERENCE:
								case ExpressionType::PROPERTY_ACCESS: {
									const auto &reference =
											static_cast<const expressions::VariableReferenceExpression &>(*expression);
									return !pathExpressions.contains(reference.getVariableName());
								}
								case ExpressionType::FUNCTION_CALL: {
									const auto &function =
											static_cast<const expressions::FunctionCallExpression &>(*expression);
									const auto name = lowerCase(function.getFunctionName());
									if ((name == "size" || name == "length") && function.getArgumentCount() == 1U) {
										const auto *reference =
												dynamic_cast<const expressions::VariableReferenceExpression *>(
														function.getArguments().front().get());
										if (reference != nullptr && !reference->hasProperty() &&
											pathExpressions.contains(reference->getVariableName())) {
											demands[reference->getVariableName()] |= name == "size" ? 1U : 2U;
											return true;
										}
									}
									return std::ranges::all_of(function.getArguments(), [&](const auto &argument) {
										return inspectDemand(argument.get());
									});
								}
								case ExpressionType::BINARY_OP: {
									const auto &binary =
											static_cast<const expressions::BinaryOpExpression &>(*expression);
									if (binary.getOperator() == expressions::BinaryOperatorType::BOP_AND ||
										binary.getOperator() == expressions::BinaryOperatorType::BOP_OR)
										return false;
									return inspectDemand(binary.getLeft()) && inspectDemand(binary.getRight());
								}
								case ExpressionType::UNARY_OP:
									return inspectDemand(
											static_cast<const expressions::UnaryOpExpression &>(*expression)
													.getOperand());
								case ExpressionType::IS_NULL:
									return inspectDemand(static_cast<const expressions::IsNullExpression &>(*expression)
																 .getExpression());
								case ExpressionType::EXPR_LIST_LITERAL: {
									const auto &list =
											static_cast<const expressions::ListLiteralExpression &>(*expression);
									return list.isStatic() ||
										   std::ranges::all_of(list.getElements(), [&](const auto &element) {
											   return inspectDemand(element.get());
										   });
								}
								default:
									return false;
							}
						};
						for (const auto &item: items)
							valid = valid && inspectDemand(item.expression.get());
						// Dropped paths retain ordinary evaluation/error semantics.
						for (const auto &[alias, expression]: pathExpressions) {
							static_cast<void>(expression);
							valid = valid && demands.contains(alias);
						}
						if (valid && !pathExpressions.empty()) {
							const auto emitScalarPathRow = [&](LogicalPlanReadRow row) {
								LogicalPlanReadRow intermediate;
								for (const auto &item: pathProject->getItems()) {
									checkGuard(state);
									if (pathExpressions.contains(item.alias)) {
										const unsigned demand = demands.at(item.alias);
										if ((demand & 1U) != 0U)
											intermediate.entity_metadata_cache[item.alias]["$shortest_path_size"] =
													evaluateScalarExpression(*item.expression, row, state,
																			 ShortestPathResultMode::SPR_SIZE);
										if ((demand & 2U) != 0U)
											intermediate.entity_metadata_cache[item.alias]["$shortest_path_length"] =
													evaluateScalarExpression(*item.expression, row, state,
																			 ShortestPathResultMode::SPR_LENGTH);
										intermediate.values.emplace(item.alias, PropertyValue());
									} else {
										intermediate.values.emplace(item.alias,
																	evaluateExpression(*item.expression, row, state));
										propagateProjectedEntityCaches(*item.expression, row, intermediate, item.alias, state);
									}
								}
								if (logicalReadRowBytes(intermediate, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
									throw std::runtime_error(
											"logical-plan scalar path projection exceeded its materialized bound");
								}
								return emitProjectedRow(std::move(intermediate));
							};
							std::vector<const expressions::Expression *> candidates;
							for (const auto &item: pathProject->getItems())
								candidates.push_back(item.expression.get());
							const auto selected = selectBatchableExpressions(candidates);
							const auto complete = selectBatchableCompleteEntityVariables(candidates);
							const auto &input = requireOnlyChild(*pathProject);
							if (!selected.empty() || !complete.empty()) {
								if (const auto batched = tryVisitBatchedPropertyExpressions(
											input, selected, state, emitScalarPathRow, {}, complete);
									batched)
									return finishDistinct(*batched);
							}
							return finishDistinct(visitOperator(input, state, emitScalarPathRow));
						}
					}
					std::vector<const expressions::Expression *> projectionCandidates;
					projectionCandidates.reserve(items.size());
					for (const logical::LogicalProjectItem &item: items) {
						projectionCandidates.push_back(item.expression.get());
					}
					const std::vector<const expressions::Expression *> projectionExpressions =
							selectBatchableExpressions(projectionCandidates);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(projectionCandidates);
					if (!projectionExpressions.empty() || !completeEntityVariables.empty()) {
						if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									projectChild, projectionExpressions, state, emitProjectedRow, {},
									completeEntityVariables, chargedReceiver != nullptr ? &chargedProject : nullptr,
									!items.empty() && !project.isDistinct() && std::ranges::all_of(items, [](const auto &item) {
										return item.expression && item.expression->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS;
									}));
							batched.has_value()) {
							return finishDistinct(*batched);
						}
					}
					return finishDistinct(visitOperator(projectChild, state, emitProjectedRow,
										 chargedReceiver != nullptr && supportsChargedRowHandoff(projectChild.getType())
												 ? &chargedProject
												 : nullptr));
				}
				case logical::LogicalOpType::LOP_AGGREGATE: {
					const auto &aggregate = dynamic_cast<const logical::LogicalAggregate &>(op);
					if (const std::optional<bool> direct = tryVisitDirectNodeCount(aggregate, state, visitor);
						direct.has_value()) {
						return *direct;
					}
					if (const std::optional<bool> variableLength =
								tryVisitBatchedVariableLengthCount(aggregate, state, visitor);
						variableLength.has_value()) {
						return *variableLength;
					}
					if (const std::optional<bool> twoHop =
								tryVisitAnchoredTwoHopRelationshipCount(aggregate, state, visitor);
						twoHop.has_value()) {
						return *twoHop;
					}
					if (const std::optional<bool> anchored =
								tryVisitAnchoredRelationshipCount(aggregate, state, visitor);
						anchored.has_value()) {
						return *anchored;
					}
					if (const std::optional<bool> direct = tryVisitDirectRelationshipCount(aggregate, state, visitor);
						direct.has_value()) {
						return *direct;
					}
					const auto &groupExpressions = aggregate.getGroupByExprs();
					const auto &groupAliases = aggregate.getGroupByAliases();
					const auto &aggregations = aggregate.getAggregations();
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						detail::BudgetMemoryResource bindingMemory(state.shared.materialized_budget);
						std::pmr::vector<const expressions::Expression *> expressionInputs(&bindingMemory);
						for (const auto &expression : groupExpressions) expressionInputs.push_back(expression.get());
						for (const auto &item : aggregations) expressionInputs.push_back(item.argument.get());
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op), expressionInputs,
							state.shared.snapshot, state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					if (aggregations.empty() || groupAliases.size() > groupExpressions.size()) {
						throw std::invalid_argument("logical-plan Aggregate has an invalid shape");
					}
					std::vector<std::string> outputAliases;
					outputAliases.reserve(groupExpressions.size() + aggregations.size());
					for (size_t index = 0U; index < groupExpressions.size(); ++index) {
						if (!groupExpressions[index]) {
							throw std::invalid_argument("logical-plan Aggregate has an empty group expression");
						}
						const std::string alias = index < groupAliases.size() && !groupAliases[index].empty()
														  ? groupAliases[index]
														  : groupExpressions[index]->toString();
						if (alias.empty()) {
							throw std::invalid_argument("logical-plan Aggregate has an empty group output alias");
						}
						outputAliases.push_back(alias);
					}

					std::vector<AggregateFunctionType> functions;
					std::vector<double> percentiles;
					functions.reserve(aggregations.size());
					percentiles.reserve(aggregations.size());
					for (const logical::LogicalAggItem &item: aggregations) {
						const AggregateFunctionType function = aggregateFunctionType(item.functionName);
						if (item.alias.empty() || std::ranges::find(outputAliases, item.alias) != outputAliases.end() ||
							(function != AggregateFunctionType::AGG_COUNT && !item.argument) ||
							(function == AggregateFunctionType::AGG_COUNT && !item.argument && item.distinct)) {
							throw std::invalid_argument("logical-plan Aggregate has an invalid aggregate item");
						}
						outputAliases.push_back(item.alias);
						functions.push_back(function);
						percentiles.push_back(aggregatePercentile(item, function, state));
					}
					bool countOnlyAggregate = groupExpressions.empty();
					for (size_t index = 0U; countOnlyAggregate && index < aggregations.size(); ++index) {
						countOnlyAggregate =
								functions[index] == AggregateFunctionType::AGG_COUNT && !aggregations[index].distinct;
					}

					struct AggregateGroup {
						std::vector<PropertyValue> values;
						std::vector<std::unique_ptr<AggregateAccumulator>> accumulators;
						std::vector<std::pmr::unordered_set<std::pmr::string>> distinct_values;
						size_t accounted_bytes = 0U;
					};
					MaterializedByteReservation aggregateStateBytes(&state.shared.materialized_budget);
					MaterializedByteReservation groupStorageBytes(&state.shared.materialized_budget);
					detail::BudgetMemoryResource aggregateIndexMemory(state.shared.materialized_budget);
					std::vector<AggregateGroup> groups;
					std::pmr::unordered_map<std::pmr::string, size_t> groupIndexes{&aggregateIndexMemory};
					size_t aggregateMemoryBytes = 0U;
					const auto chargeAggregateBytes = [&](const size_t bytes) {
						if (bytes > state.shared.bounds.maximum_materialized_bytes -
											std::min(aggregateMemoryBytes, state.shared.bounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical-plan Aggregate exceeded its materialization byte bound");
						}
						const size_t nextBytes = checkedSizeAdd(
								aggregateMemoryBytes, bytes, "logical-plan Aggregate materialization size overflowed");
						aggregateStateBytes.set(nextBytes);
						aggregateMemoryBytes = nextBytes;
					};
					const auto createGroup = [&](std::vector<PropertyValue> values, MaterializedByteReservation *prepaid = nullptr) {
						if (groups.size() == state.shared.bounds.maximum_distinct_rows ||
							groups.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its group bound");
						}
						size_t groupBytes = 0U;
						for (const PropertyValue &value: values) {
							groupBytes = checkedSizeAdd(groupBytes, detail::retainedPropertyValueBytes(value, [&] { checkGuardNow(state); }),
														"logical-plan Aggregate group size overflowed");
						}
						AggregateGroup group;
						group.values = std::move(values);
						groupBytes = checkedSizeAdd(groupBytes, group.values.capacity() * sizeof(PropertyValue),
													"logical-plan Aggregate group size overflowed");
						groupBytes =
								checkedSizeAdd(groupBytes,
											   aggregations.size() * (sizeof(std::unique_ptr<AggregateAccumulator>) +
																	  sizeof(std::pmr::unordered_set<std::pmr::string>)),
											   "logical-plan Aggregate group size overflowed");
						const size_t prepaidBytes = prepaid ? std::min(prepaid->bytes(), groupBytes) : 0U;
						if (prepaidBytes) {
							aggregateStateBytes.absorb(prepaid->splitOff(prepaidBytes));
							aggregateMemoryBytes = checkedSizeAdd(aggregateMemoryBytes, prepaidBytes, "group prepaid size overflowed");
						}
						chargeAggregateBytes(groupBytes - prepaidBytes);
						group.accumulators.reserve(aggregations.size());
						group.distinct_values.reserve(aggregations.size());
						for (size_t index = 0U; index < aggregations.size(); ++index)
							group.distinct_values.emplace_back(&aggregateIndexMemory);
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							std::unique_ptr<AggregateAccumulator> accumulator =
									createAccumulator(functions[index], false, percentiles[index], &aggregateIndexMemory,
										[&](size_t objectBytes) {
											checkGuardNow(state);
											if (objectBytes > state.shared.materialized_budget.available())
												throw std::runtime_error("logical-plan Aggregate accumulator object exceeded its materialized byte bound");
											chargeAggregateBytes(objectBytes);
											groupBytes = checkedSizeAdd(groupBytes, objectBytes,
												"logical-plan Aggregate object size overflowed");
										});
							if (!accumulator) { // aggregateFunctionType accepts exactly the factory's supported enum
												// values.
								throw std::logic_error("logical-plan Aggregate could not create an accumulator");
							}
							group.accumulators.push_back(std::move(accumulator));
						}
						group.accounted_bytes = groupBytes;
						if (groups.size() == groups.capacity()) {
							const size_t minimumCapacity = checkedSizeAdd(
									groups.size(), 1U, "logical-plan Aggregate group capacity overflowed");
							const size_t requestedCapacity =
									groups.capacity() == 0U
											? minimumCapacity
											: std::max(minimumCapacity,
													   checkedSizeMul(
															   groups.capacity(), 2U,
															   "logical-plan Aggregate group capacity overflowed"));
							const size_t requestedStorageBytes =
									checkedSizeMul(requestedCapacity, sizeof(AggregateGroup),
												   "logical-plan Aggregate group vector size overflowed");
							if (requestedStorageBytes > state.shared.materialized_budget.available()) {
								throw std::runtime_error("logical-plan Aggregate group vector growth exceeded its "
														 "materialized byte bound");
							}
							MaterializedByteReservation groupStorageGrowthBytes(&state.shared.materialized_budget);
							groupStorageGrowthBytes.set(requestedStorageBytes);
							groups.reserve(requestedCapacity);
							const size_t actualCapacityBytes =
									checkedSizeMul(groups.capacity(), sizeof(AggregateGroup),
												   "logical-plan Aggregate group capacity overflowed");
							groupStorageGrowthBytes.set(actualCapacityBytes);
							groupStorageBytes = std::move(groupStorageGrowthBytes);
						}
						groups.push_back(std::move(group));
						return groups.size() - 1U;
					};
					if (groupExpressions.empty()) {
						static_cast<void>(createGroup({}));
					}
					const auto accountAccumulatorRetention = [&](AggregateGroup &group, size_t beforeBytes, size_t afterBytes, MaterializedByteReservation *prepaid = nullptr) {
						if (afterBytes >= beforeBytes) {
							const size_t delta = afterBytes - beforeBytes;
							const size_t transferred = prepaid ? std::min(delta, prepaid->bytes()) : 0U;
							if (transferred) {
								aggregateStateBytes.absorb(prepaid->splitOff(transferred));
								aggregateMemoryBytes = checkedSizeAdd(aggregateMemoryBytes, transferred, "aggregate prepaid size overflowed");
							}
							chargeAggregateBytes(delta - transferred);
							group.accounted_bytes = checkedSizeAdd(group.accounted_bytes, afterBytes - beforeBytes,
								"logical-plan Aggregate group size overflowed");
						} else {
							aggregateMemoryBytes -= beforeBytes - afterBytes;
							aggregateStateBytes.set(aggregateMemoryBytes);
							group.accounted_bytes -= beforeBytes - afterBytes;
						}
					};

					size_t inputRows = 0U;
					const logical::LogicalOperator &aggregateChild = requireOnlyChild(aggregate);
					std::pmr::vector<std::string_view> aggregateColumnKeys(&aggregateIndexMemory);
					std::pmr::vector<size_t> aggregateColumnBindings(&aggregateIndexMemory);
					std::optional<NodeColumnReceiver> aggregateColumnReceiver;
					bool aggregateColumnsBound = false;
					std::optional<detail::BoundColumnLayout> aggregateInputLayout;
					const size_t rowCountBinding = std::numeric_limits<size_t>::max();
					bool columnAggregateEligible = !state.shared.snapshot.supportsNodePropertyCellBatches() &&
						groupExpressions.empty() && !countOnlyAggregate &&
						state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans() &&
						aggregateChild.getType() == logical::LogicalOpType::LOP_NODE_SCAN;
					if (columnAggregateEligible) {
						const auto &scan = dynamic_cast<const logical::LogicalNodeScan &>(aggregateChild);
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							const auto &item = aggregations[index];
							const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(item.argument.get());
							if (item.distinct ||
								(item.argument && (!reference || reference->getVariableName() != scan.getVariable())) ||
								(functions[index] != AggregateFunctionType::AGG_COUNT && (!reference || !reference->hasProperty()))) {
								columnAggregateEligible = false;
								break;
							}
							// Preserve payload-free COUNT probes for fields not already
							// required by a value-consuming sibling aggregate.
							if (functions[index] == AggregateFunctionType::AGG_COUNT && reference && reference->hasProperty()) {
								const bool sharedValueInput = std::ranges::any_of(aggregations, [&](const auto &sibling) {
									const auto *other = dynamic_cast<const expressions::VariableReferenceExpression *>(sibling.argument.get());
									return aggregateFunctionType(sibling.functionName) != AggregateFunctionType::AGG_COUNT && other &&
										other->hasProperty() && other->getVariableName() == scan.getVariable() &&
										other->getPropertyName() == reference->getPropertyName();
								});
								if (!sharedValueInput) { columnAggregateEligible = false; break; }
							}
						}
						if (columnAggregateEligible) {
							try {
								aggregateColumnKeys.reserve(aggregations.size());
								aggregateColumnBindings.reserve(aggregations.size());
								for (const auto &item : aggregations) {
									const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(item.argument.get());
									if (!reference || !reference->hasProperty()) {
										aggregateColumnBindings.push_back(rowCountBinding);
										continue;
									}
									const std::string_view key = reference->getPropertyName();
									auto found = std::ranges::find(aggregateColumnKeys, key);
									if (found == aggregateColumnKeys.end()) {
										aggregateColumnKeys.push_back(key);
										found = aggregateColumnKeys.end() - 1;
									}
									aggregateColumnBindings.push_back(static_cast<size_t>(found - aggregateColumnKeys.begin()));
								}
							} catch (const MaterializedByteLimitExceeded &) {
								columnAggregateEligible = false;
							}
						}
					}
					if (columnAggregateEligible) {
						aggregateColumnReceiver.emplace(NodeColumnReceiver{aggregateColumnKeys,
							[&](std::span<const std::string> keys, std::span<const std::vector<PropertyValue>> columns,
								const SelectionVector &selection, std::span<const size_t> originalOrdinals) {
								if (columns.size() != keys.size()) throw std::logic_error("aggregate column input changed its width");
								for (const auto &column : columns) {
									checkGuard(state);
									if (column.size() != selection.size()) throw std::logic_error("aggregate column input changed its row count");
								}
								for (size_t ordinal : originalOrdinals) {
									checkGuard(state);
									if (ordinal >= selection.size()) throw std::logic_error("aggregate column input has an invalid row ordinal");
								}
								if (!aggregateColumnsBound) {
									for (auto &binding : aggregateColumnBindings) {
										if (binding == rowCountBinding) continue;
										const auto found = std::ranges::find(keys, aggregateColumnKeys[binding]);
										if (found == keys.end()) throw std::logic_error("aggregate input column was not produced");
										binding = static_cast<size_t>(found - keys.begin());
									}
									try {
										aggregateInputLayout.emplace(keys, state.shared.materialized_budget,
											std::pmr::get_default_resource(), [&] { checkGuard(state); });
									} catch (const MaterializedByteLimitExceeded &) {
										// Optional name metadata declines before any aggregate effects.
									}
									aggregateColumnsBound = true;
								}
								if (aggregateInputLayout) aggregateInputLayout->validate(keys);
								else for (size_t index = 0U; index < aggregations.size(); ++index) {
									if (aggregateColumnBindings[index] == rowCountBinding) continue;
									const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(*aggregations[index].argument);
									detail::validateColumnBinding(keys, aggregateColumnBindings[index], reference.getPropertyName());
								}
								detail::PrimitiveNumericColumns numericColumns(state.shared.materialized_budget);
								MaterializedByteReservation numericStateHeadroom(&state.shared.materialized_budget);
								size_t numericStateBytes = 0U;
								for (size_t index = 0U; index < functions.size(); ++index) {
									if (functions[index] == AggregateFunctionType::AGG_MIN || functions[index] == AggregateFunctionType::AGG_MAX) {
										const size_t retained = groups.front().accumulators[index]->retainedBytesForAdmission();
										numericStateBytes = checkedSizeAdd(numericStateBytes, sizeof(int64_t) - std::min(retained, sizeof(int64_t)),
											"logical-plan numeric aggregate state size overflowed");
									}
								}
								bool numericPrepared = false;
								// Growing collections own their allocation protocol; optional conversion
								// buffers must not compete with that state or trigger update replay.
								const bool primitiveOnly = std::ranges::all_of(functions, [](auto function) {
									return function == AggregateFunctionType::AGG_COUNT || function == AggregateFunctionType::AGG_SUM ||
										function == AggregateFunctionType::AGG_AVG || function == AggregateFunctionType::AGG_MIN ||
										function == AggregateFunctionType::AGG_MAX || function == AggregateFunctionType::AGG_STDEV ||
										function == AggregateFunctionType::AGG_STDEVP;
								});
								if (primitiveOnly && !numericStateHeadroom.wouldExceed(numericStateBytes)) {
									// Optional buffers cannot consume the scalar extremum's
									// first retained value allowance. Actual updates charge it below.
									numericStateHeadroom.set(numericStateBytes);
									numericPrepared = numericColumns.tryPrepare(columns, aggregateColumnBindings,
										[&] { checkGuard(state); });
									numericStateHeadroom.reset();
								}
								debug::PerfTrace::addValue("logical_plan.aggregate.typed_columns",
									static_cast<int64_t>(numericColumns.typedColumnCount()));
								if (numericPrepared && numericColumns.typedColumnCount() == 0U) {
									// Dynamic-only batches gain nothing from conversion metadata.
									numericColumns.release();
									numericPrepared = false;
								}
								std::optional<detail::BatchEnvelope> envelope;
								if (numericPrepared && aggregateInputLayout && state.shared.bound_program) {
									try {
										numericStateHeadroom.set(numericStateBytes);
										detail::BudgetMemoryResource metadataMemory(state.shared.materialized_budget);
										std::pmr::vector<detail::BatchColumnSource> sources(&metadataMemory);
										std::pmr::vector<uint32_t> selectedRows(&metadataMemory);
										std::pmr::vector<uint64_t> sourceOrdinals(&metadataMemory);
										for (size_t column = 0U; column < numericColumns.columnCount(); ++column) {
											checkGuard(state); sources.push_back(detail::BatchColumnSource::numeric(numericColumns, column));
										}
										for (size_t row = 0U; row < selection.size(); ++row) { checkGuard(state); sourceOrdinals.push_back(row); }
										for (size_t row : originalOrdinals) if (selection.includes(row)) {
											checkGuard(state); selectedRows.push_back(static_cast<uint32_t>(row));
										}
										envelope.emplace(state.shared.bound_program->root().program, *aggregateInputLayout, state.shared.snapshot,
											state.shared.bounds.maximum_rows, state.shared.materialized_budget);
										envelope->publish(keys, sources, selection.size(), selectedRows, sourceOrdinals, [&] { checkGuardNow(state); });
										debug::PerfTrace::addValue("logical_plan.batch_envelope.published", 1);
									} catch (const MaterializedByteLimitExceeded &) {
										envelope.reset(); debug::PerfTrace::addValue("logical_plan.batch_envelope.admission_fallback", 1);
									}
									numericStateHeadroom.reset();
								}
								const auto numericRow = [&](size_t column, size_t ordinal) {
									return envelope ? envelope->sourceRow(aggregateInputLayout->column(column), envelope->rowId(ordinal)) : numericColumns.rowId(ordinal);
								};
								for (const size_t ordinal : originalOrdinals) {
									checkGuard(state);
									if (!selection.includes(ordinal)) continue;
									if (inputRows == state.shared.bounds.maximum_rows)
										throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
									++inputRows;
									for (size_t index = 0U; index < aggregations.size(); ++index) {
										checkGuard(state);
										auto &accumulator = *groups.front().accumulators[index];
										if (functions[index] == AggregateFunctionType::AGG_COUNT || functions[index] == AggregateFunctionType::AGG_SUM ||
											functions[index] == AggregateFunctionType::AGG_AVG || functions[index] == AggregateFunctionType::AGG_STDEV ||
											functions[index] == AggregateFunctionType::AGG_STDEVP) {
											// Count and numeric sums retain no variable payload. Keep
											// their typed loop free of retention probes and copy guards.
											if (aggregateColumnBindings[index] == rowCountBinding) accumulator.updateRow();
											else if (numericPrepared) numericColumns.update(accumulator, functions[index], aggregateColumnBindings[index], numericRow(aggregateColumnBindings[index], ordinal));
											else accumulator.update(columns[aggregateColumnBindings[index]][ordinal]);
											continue;
										}
										const size_t beforeBytes = accumulator.retainedBytesForAdmission();
										MaterializedByteReservation updateScratch(&state.shared.materialized_budget);
										bool admissionRejected = false;
										const std::function<void(size_t)> admit = [&](size_t bytes) {
											checkGuardNow(state);
											try { updateScratch.set(bytes); }
											catch (const MaterializedByteLimitExceeded &) { admissionRejected = true; throw; }
										};
										try {
											if (numericPrepared) numericColumns.update(accumulator, functions[index], aggregateColumnBindings[index], numericRow(aggregateColumnBindings[index], ordinal), admit);
											else accumulator.updateAdmitted(columns[aggregateColumnBindings[index]][ordinal], admit);
										} catch (const MaterializedByteLimitExceeded &) {
											if (!admissionRejected || !numericPrepared) throw;
											// Extremum copying has not published state on admission failure.
											// Drop optional buffers and retry only this update, never rows
											// already counted or accumulated. Guard failures still escape.
											envelope.reset();
											numericColumns.release();
											numericPrepared = false;
											updateScratch.reset();
											accumulator.updateAdmitted(columns[aggregateColumnBindings[index]][ordinal], admit);
											debug::PerfTrace::addValue("logical_plan.aggregate.optional_buffers_released", 1);
										}
										updateScratch.reset();
										accountAccumulatorRetention(groups.front(), beforeBytes, accumulator.retainedBytesForAdmission());
									}
								}
								debug::PerfTrace::addValue("logical_plan.aggregate.column_input_rows",
									static_cast<int64_t>(selection.selectedCount()));
								return true;
							}});
					} else {
						std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(aggregateColumnKeys);
						std::pmr::vector<size_t>(&aggregateIndexMemory).swap(aggregateColumnBindings);
					}
					std::pmr::vector<std::string_view> directKeys(&aggregateIndexMemory);
					std::pmr::vector<size_t> directGroups(&aggregateIndexMemory), directArguments(&aggregateIndexMemory);
					struct DirectFilterTerm { size_t column; const expressions::LiteralExpression *literal; detail::ColumnPredicateOp operation; };
					std::pmr::vector<DirectFilterTerm> directTerms(&aggregateIndexMemory);
					const auto *directFilter = dynamic_cast<const logical::LogicalFilter *>(&aggregateChild);
					const auto *directInput = directFilter ? &requireOnlyChild(*directFilter) : &aggregateChild;
					bool directBound = false;
					bool directEligible = state.shared.allow_deferred_entity_refs && state.shared.snapshot.supportsNativeNodeRefScans() &&
						state.shared.snapshot.supportsNodePropertyCellBatches() && directInput->getType() == logical::LogicalOpType::LOP_NODE_SCAN;
					const auto *directScan = directEligible ? &static_cast<const logical::LogicalNodeScan &>(*directInput) : nullptr;
					const auto directReference = [&](const expressions::Expression *expression) {
						const auto *ref = dynamic_cast<const expressions::VariableReferenceExpression *>(expression);
						return ref && directScan && ref->getVariableName() == directScan->getVariable() ? ref : nullptr;
					};
					for (const auto &expression : groupExpressions) {
						const auto *ref = directReference(expression.get());
						if (!ref || !ref->hasProperty()) directEligible = false;
					}
					for (size_t index = 0; index < aggregations.size(); ++index) {
						const auto &item = aggregations[index]; const auto *ref = directReference(item.argument.get());
						if (item.argument && (!ref || (!ref->hasProperty() &&
							(functions[index] != AggregateFunctionType::AGG_COUNT || item.distinct)))) directEligible = false;
					}
					for (size_t index = 0; directEligible && index < aggregations.size(); ++index) {
						const auto &item = aggregations[index]; const auto *ref = directReference(item.argument.get());
						if (functions[index] != AggregateFunctionType::AGG_COUNT || item.distinct || !ref || !ref->hasProperty()) continue;
						const auto same = [&](const expressions::Expression *expression) {
							const auto *other = directReference(expression);
							return other && other->hasProperty() && other->getPropertyName() == ref->getPropertyName();
						};
						const bool needed = std::ranges::any_of(groupExpressions, [&](const auto &expr) { return same(expr.get()); }) ||
							std::ranges::any_of(aggregations, [&](const auto &sibling) {
								return (sibling.distinct || aggregateFunctionType(sibling.functionName) != AggregateFunctionType::AGG_COUNT) && same(sibling.argument.get());
							});
						if (!needed) directEligible = false;
					}
					// Payload-free global COUNT(property) probes remain preferable when
					// no grouping/value-consuming sibling already needs those fields.
					if (countOnlyAggregate && std::ranges::any_of(aggregations, [&](const auto &item) {
						const auto *ref = directReference(item.argument.get()); return ref && ref->hasProperty();
					})) directEligible = false;
					if (directEligible) {
						try {
							const auto bind = [&](const expressions::Expression *expression) {
								const auto *ref = directReference(expression);
								if (!ref || !ref->hasProperty()) return rowCountBinding;
								const std::string_view name = ref->getPropertyName();
								auto it = std::ranges::find(directKeys, name);
								if (it == directKeys.end()) { directKeys.push_back(name); it = directKeys.end() - 1; }
								return static_cast<size_t>(it - directKeys.begin());
							};
							if (directFilter) {
								const auto collect = [&](const auto &self, const expressions::Expression *expression) -> bool {
									checkGuard(state);
									const auto *binary = dynamic_cast<const expressions::BinaryOpExpression *>(expression);
									if (!binary || !binary->getLeft() || !binary->getRight()) return false;
									using Op = expressions::BinaryOperatorType;
									if (binary->getOperator() == Op::BOP_AND) return self(self, binary->getLeft()) && self(self, binary->getRight());
									const auto *ref = directReference(binary->getLeft());
									const auto *literal = dynamic_cast<const expressions::LiteralExpression *>(binary->getRight());
									if (!ref || !ref->hasProperty() || !literal) return false;
									detail::ColumnPredicateOp operation;
									switch (binary->getOperator()) {
										case Op::BOP_EQUAL: operation = detail::ColumnPredicateOp::CPO_EQUAL; break;
										case Op::BOP_NOT_EQUAL: operation = detail::ColumnPredicateOp::CPO_NOT_EQUAL; break;
										case Op::BOP_LESS: operation = detail::ColumnPredicateOp::CPO_LESS; break;
										case Op::BOP_LESS_EQUAL: operation = detail::ColumnPredicateOp::CPO_LESS_EQUAL; break;
										case Op::BOP_GREATER: operation = detail::ColumnPredicateOp::CPO_GREATER; break;
										case Op::BOP_GREATER_EQUAL: operation = detail::ColumnPredicateOp::CPO_GREATER_EQUAL; break;
										default: return false;
									}
									directTerms.push_back({bind(binary->getLeft()), literal, operation}); return true;
								};
								directEligible = collect(collect, directFilter->getPredicate().get());
							}
							for (const auto &expression : groupExpressions) directGroups.push_back(bind(expression.get()));
							for (const auto &item : aggregations) directArguments.push_back(bind(item.argument.get()));
						} catch (const MaterializedByteLimitExceeded &) { directEligible = false; }
					}
					if (!directEligible) {
						std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(directKeys);
						std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directGroups);
						std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directArguments);
						std::pmr::vector<DirectFilterTerm>(&aggregateIndexMemory).swap(directTerms);
					}
					if (directEligible) {
						aggregateColumnReceiver.emplace(NodeColumnReceiver{directKeys, {},
							[&](std::span<const std::string> keys, const detail::DirectPropertyColumns &columns,
								const SelectionVector &selection, std::span<const size_t> ordinals) {
								if (!directBound) {
									const auto resolve = [&](size_t &binding) {
										if (binding == rowCountBinding) return;
										const auto found = std::ranges::find(keys, directKeys.at(binding));
										if (found == keys.end()) throw std::logic_error("direct aggregate column missing");
										binding = static_cast<size_t>(found - keys.begin());
									};
									for (auto &binding : directGroups) resolve(binding);
									for (auto &binding : directArguments) resolve(binding);
									for (auto &term : directTerms) resolve(term.column);
									directBound = true;
								}
								const auto appendCellKey = [&](std::pmr::string &key, size_t column, detail::BatchRowId row) {
									const auto cell = columns.cell(column, row);
									std::visit([&](auto value) {
										using T = decltype(value);
										if constexpr (std::is_same_v<T, std::monostate>) appendDistinctPropertyValue(key, PropertyValue());
										else if constexpr (std::is_same_v<T, std::string_view>) {
											key += static_cast<char>(static_cast<uint8_t>(PropertyType::STRING)); appendDistinctString(key, value);
										} else if constexpr (std::is_same_v<T, const PropertyValue *>) appendDistinctPropertyValue(key, *value);
										else appendDistinctPropertyValue(key, PropertyValue(value));
									}, cell.data);
								};
								for (const auto ordinal : ordinals) {
									checkGuard(state);
									if (ordinal >= columns.size()) throw std::logic_error("direct aggregate ordinal changed");
									if (!selection.includes(ordinal)) continue;
									bool passesFilter = true;
									for (const auto &term : directTerms) {
										checkGuard(state); const auto cell = columns.cell(term.column, columns.rowId(ordinal));
										if (cell.state != detail::ColumnCellState::CCS_VALUE) { passesFilter = false; break; }
										const auto &literal = *term.literal; PropertyValue scalar;
										std::optional<std::string_view> text; const PropertyValue *expected = &scalar;
										if (literal.isString()) { scalar = PropertyValue(std::string{}); text = literal.getStringValue(); }
										else if (literal.isBoolean()) scalar = PropertyValue(literal.getBooleanValue());
										else if (literal.isInteger()) scalar = PropertyValue(literal.getIntegerValue());
										else if (literal.isDouble()) scalar = PropertyValue(literal.getDoubleValue());
										else if (literal.isPropertyValue()) expected = &literal.getPropertyValue();
										const detail::ColumnPredicate predicate{term.column, term.operation, expected};
										const bool match = std::visit([&](auto value) { return detail::columnPredicateMatchesBorrowed(value, predicate, text); }, cell.data);
										if (!match) { passesFilter = false; break; }
									}
									if (!passesFilter) continue;
									if (inputRows == state.shared.bounds.maximum_rows) throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
									++inputRows; const auto row = columns.rowId(ordinal);
									size_t groupIndex = 0;
									if (!directGroups.empty()) {
										std::pmr::string key{&aggregateIndexMemory};
										for (const auto column : directGroups) appendCellKey(key, column, row);
										const auto found = groupIndexes.find(key);
										if (found != groupIndexes.end()) groupIndex = found->second;
										else {
											MaterializedByteReservation copies(&state.shared.materialized_budget);
											copies.set(checkedSizeMul(directGroups.size(), sizeof(PropertyValue), "direct group slots overflowed"));
											std::vector<PropertyValue> values; values.reserve(directGroups.size());
											copies.set(checkedSizeMul(values.capacity(), sizeof(PropertyValue), "direct group capacity overflowed"));
											for (const auto column : directGroups) {
												const size_t before = copies.bytes();
												auto value = columns.copyValue(column, row, [&](size_t bytes) {
													copies.set(checkedSizeAdd(before, bytes, "direct group copy overflowed"));
												}, [&] { checkGuardNow(state); });
												copies.set(checkedSizeAdd(before, detail::propertyBufferEstimates(value, [&] { checkGuardNow(state); }).heapBytes, "direct group retained bytes overflowed"));
												values.push_back(std::move(value));
											}
											groupIndex = createGroup(std::move(values), &copies);
											groupIndexes.emplace(std::move(key), groupIndex);
										}
									}
									auto &group = groups.at(groupIndex);
									for (size_t index = 0; index < aggregations.size(); ++index) {
										checkGuard(state); auto &accumulator = *group.accumulators[index];
										const size_t column = directArguments[index];
										if (column == rowCountBinding) { accumulator.updateRow(); continue; }
										const auto cell = columns.cell(column, row);
										if (aggregations[index].distinct) {
											std::pmr::string key{&aggregateIndexMemory}; appendCellKey(key, column, row);
											if (!group.distinct_values[index].insert(std::move(key)).second) continue;
										}
										if (functions[index] == AggregateFunctionType::AGG_COUNT) {
											if (cell.state == detail::ColumnCellState::CCS_VALUE) accumulator.updateRow();
											continue;
										}
										MaterializedByteReservation valueCopy(&state.shared.materialized_budget), update(&state.shared.materialized_budget);
										auto value = columns.copyValue(column, row, [&](size_t bytes) { valueCopy.set(bytes); }, [&] { checkGuardNow(state); });
										const size_t before = accumulator.retainedBytesForAdmission();
										accumulator.updateAdmitted(value, [&](size_t bytes) { checkGuardNow(state); update.set(bytes); });
										accountAccumulatorRetention(group, before, accumulator.retainedBytesForAdmission(), &update);
									}
									debug::PerfTrace::addValue("logical_plan.aggregate.direct_input_rows", 1);
								}
								return true;
							}});
						aggregateColumnReceiver->filterOwner = directFilter;
					}
					const auto consumeAggregateRow = [&](LogicalPlanReadRow row,
														 MaterializedByteReservation inputRowBytes) {
						ScopedBoundInputs scopedInputs(state, boundInputs, row);
						if (countOnlyAggregate) {
							inputRowBytes.reset();
						} else if (inputRowBytes.bytes() == 0U) {
							admitReferenceResultBytes(
									inputRowBytes, logicalReadRowBytes(row, state.shared.context),
									"logical-plan Aggregate input row exceeded its shared byte bound");
						}
						checkGuard(state);
						if (inputRows == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
						}
						++inputRows;
						MaterializedByteReservation groupValueScratch(&state.shared.materialized_budget);
						size_t groupValueBytes = checkedSizeMul(groupExpressions.size(), sizeof(PropertyValue),
							"logical-plan Aggregate group value array overflowed");
						groupValueScratch.set(groupValueBytes);
						std::vector<PropertyValue> groupValues;
						groupValues.reserve(groupExpressions.size());
						std::pmr::string groupKey{&aggregateIndexMemory};
						for (const std::shared_ptr<expressions::Expression> &expression: groupExpressions) {
							PropertyValue value = scalarValue(evaluateExpression(*expression, row, state));
							groupValueBytes = checkedSizeAdd(groupValueBytes,
								detail::retainedPropertyValueBytes(value, [&] { checkGuardNow(state); }),
								"logical-plan Aggregate group value size overflowed");
							groupValueScratch.set(groupValueBytes);
							appendDistinctPropertyValue(groupKey, value);
							groupValues.push_back(std::move(value));
						}
						size_t groupIndex = 0U;
						if (!groupExpressions.empty()) {
							const auto found = groupIndexes.find(groupKey);
							if (found == groupIndexes.end()) {
								// Transfer the already-owned payload to permanent group accounting.
								groupValueScratch.reset();
								groupIndex = createGroup(std::move(groupValues));
								groupIndexes.emplace(std::move(groupKey), groupIndex);
							} else {
								groupIndex = found->second;
								std::vector<PropertyValue>().swap(groupValues);
								groupValueScratch.reset();
							}
						}
						AggregateGroup &group = groups[groupIndex];
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							const logical::LogicalAggItem &item = aggregations[index];
							const size_t beforeBytes = group.accumulators[index]->retainedBytesForAdmission();
							if (!item.argument) {
								group.accumulators[index]->updateRow();
							} else {
								// A non-distinct COUNT over a direct Entity Property only needs
								// non-NULL presence. Native snapshots answer from the compact
								// token directory and value header without decoding payloads, keeping
								// large properties out of the transient row and session caches.
								std::optional<bool> presence;
								if (functions[index] == AggregateFunctionType::AGG_COUNT && !item.distinct &&
									item.argument->getExpressionType() ==
											expressions::ExpressionType::PROPERTY_ACCESS) {
									const auto *property =
											dynamic_cast<const expressions::VariableReferenceExpression *>(
													item.argument.get());
									if (property != nullptr && property->hasProperty() &&
										!row.values.contains(property->toString())) {
										const auto binding = row.values.find(property->getVariableName());
										if (binding != row.values.end()) {
											if (const auto *node =
														std::get_if<storage::LogicalGraphNode>(&binding->second)) {
												presence = state.shared.snapshot.nodePropertyIsNonNull(
														node->node_id, property->getPropertyName());
											} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(
															   &binding->second)) {
												presence = state.shared.snapshot.edgePropertyIsNonNull(
														edge->edge_id, property->getPropertyName());
											} else if (const auto *reference =
															   std::get_if<storage::EntityRef>(&binding->second)) {
												validateEntityRef(*reference);
												presence = reference->is_edge ? state.shared.snapshot.edgePropertyIsNonNull(
																						reference->entity_id,
																						property->getPropertyName())
																			  : state.shared.snapshot.nodePropertyIsNonNull(
																						reference->entity_id,
																						property->getPropertyName());
											}
										}
									}
								}
								if (presence.has_value()) {
									if (*presence) {
										group.accumulators[index]->updateRow();
									}
									continue;
								}
								const PropertyValue value = scalarValue(evaluateExpression(*item.argument, row, state));
								if (item.distinct) {
									std::pmr::string key{&aggregateIndexMemory};
									appendDistinctPropertyValue(key, value);
									if (!group.distinct_values[index].insert(std::move(key)).second) continue;
								}
								MaterializedByteReservation updateScratch(&state.shared.materialized_budget);
								group.accumulators[index]->updateAdmitted(value, [&](size_t bytes) {
									checkGuardNow(state); updateScratch.set(bytes);
								});
								updateScratch.reset();
							}
							const size_t afterBytes = group.accumulators[index]->retainedBytesForAdmission();
							accountAccumulatorRetention(group, beforeBytes, afterBytes);
						}
						return true;
					};
					// Aggregate already consumes every input row. Its own all-selected batch
					// only delayed accumulation and duplicated the source's row storage.
					const auto consumeUnchargedAggregateRow = [&](LogicalPlanReadRow row) {
						return consumeAggregateRow(std::move(row),
												   MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedAggregateRow = consumeAggregateRow;
					std::vector<const expressions::Expression *> aggregateCandidates;
					aggregateCandidates.reserve(groupExpressions.size() + aggregations.size());
					for (const auto &expression: groupExpressions) {
						aggregateCandidates.push_back(expression.get());
					}
					const bool canProbeCountProperties = state.shared.snapshot.supportsPropertyNonNullProbes();
					for (size_t index = 0U; index < aggregations.size(); ++index) {
						const auto &item = aggregations[index];
						if (!item.argument)
							continue;
						// Grouping and sibling aggregates do not change COUNT's data needs.
						// Keep actual values when DISTINCT or another expression needs them;
						// unsupported snapshots retain their existing column-batch fallback.
						if (canProbeCountProperties && functions[index] == AggregateFunctionType::AGG_COUNT &&
							!item.distinct &&
							item.argument->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS) {
							continue;
						}
						aggregateCandidates.push_back(item.argument.get());
					}
					const std::vector<const expressions::Expression *> aggregatePropertyExpressions =
							selectBatchableExpressions(aggregateCandidates);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(aggregateCandidates);
					std::optional<bool> batchedProperties;
					// COUNT is cardinality-only for selected scalar Properties: it must not
					// force a potentially large payload into the bounded prefetch batch.
					// Complete-Entity expressions are different: COUNT(properties(n)) still
					// needs the complete map, so that requirement is batched even for a
					// count-only aggregate.
					if (!aggregateColumnReceiver && ((!aggregatePropertyExpressions.empty() && !countOnlyAggregate) ||
						!completeEntityVariables.empty())) {
						batchedProperties = tryVisitBatchedPropertyExpressions(
								requireOnlyChild(aggregate), aggregatePropertyExpressions, state,
								consumeUnchargedAggregateRow, {}, completeEntityVariables, &chargedAggregateRow,
								!aggregateCandidates.empty() && std::ranges::all_of(aggregateCandidates, [](const auto *expression) {
									return expression && expression->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS;
								}) && std::ranges::none_of(aggregations, [](const auto &item) { return item.distinct; }));
					}
					const bool completed = batchedProperties.has_value()
												   ? *batchedProperties
												   : visitOperator(aggregateChild, state, consumeUnchargedAggregateRow,
																   supportsChargedRowHandoff(aggregateChild.getType())
																		   ? &chargedAggregateRow
																		   : nullptr,
																   aggregateColumnReceiver ? &*aggregateColumnReceiver : nullptr);
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Aggregate could not prove that its complete input was read");
					}
					// Release lookup nodes and buckets before downstream output is
					// admitted. Per-group DISTINCT keys live until their group is consumed.
					aggregateColumnReceiver.reset();
					std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(directKeys);
					std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directGroups);
					std::pmr::vector<size_t>(&aggregateIndexMemory).swap(directArguments);
					std::pmr::vector<DirectFilterTerm>(&aggregateIndexMemory).swap(directTerms);
					aggregateInputLayout.reset();
					std::pmr::vector<std::string_view>(&aggregateIndexMemory).swap(aggregateColumnKeys);
					std::pmr::vector<size_t>(&aggregateIndexMemory).swap(aggregateColumnBindings);
					decltype(groupIndexes){&aggregateIndexMemory}.swap(groupIndexes);
					const size_t outputGroupCount = groups.size();
					for (size_t groupIndex = 0U; groupIndex < outputGroupCount; ++groupIndex) {
						AggregateGroup &group = groups[groupIndex];
						checkGuard(state);
						LogicalPlanReadRow row;
						for (size_t index = 0U; index < group.values.size(); ++index) {
							row.values.emplace(outputAliases[index], std::move(group.values[index]));
						}
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							row.values.emplace(aggregations[index].alias, group.accumulators[index]->takeResult());
						}
						// Transfer payload ownership before downstream buffers acquire it.
						// The vector's now-empty group slot remains live until this scope ends.
						const size_t releasedBytes = group.accounted_bytes;
						group = AggregateGroup{};
						group.accounted_bytes = 0U;
						aggregateMemoryBytes -= releasedBytes;
						aggregateStateBytes.set(aggregateMemoryBytes);
						if (groupIndex + 1U == outputGroupCount) {
							// The last result no longer needs any group slots. Destroy
							// their retained capacity before refunding it and admitting
							// the output; even a scalar COUNT must have this headroom.
							std::vector<AggregateGroup>{}.swap(groups);
							groupStorageBytes.reset();
						}
						const size_t outputBytes = logicalReadRowBytes(row, state.shared.context) -
								(state.shared.resident_output_batch_bytes == 0U ? 0U : sizeof(LogicalPlanReadRow));
						// Buffered output already charges its row-slot capacity. Its
						// collector admits only payload; do not charge that slot twice.
						if (outputBytes > state.shared.materialized_budget.available()) {
							throw std::runtime_error(
									"logical-plan Aggregate output exceeded the shared materialized byte bound");
						}
						if (!visitor(std::move(row))) {
							return false;
						}
					}
					return true;
				}
				case logical::LogicalOpType::LOP_JOIN: {
					const auto &join = dynamic_cast<const logical::LogicalJoin &>(op);
					const logical::LogicalOperator *left = join.getLeft();
					const logical::LogicalOperator *right = join.getRight();
					if (left == nullptr || right == nullptr) {
						throw std::invalid_argument("logical-plan Join requires two valid children");
					}
					MaterializedByteReservation leftNameBytes(&state.shared.materialized_budget), rightNameBytes(&state.shared.materialized_budget);
					const auto leftColumns = boundOutputNames(*left, state, leftNameBytes);
					const auto rightColumns = boundOutputNames(*right, state, rightNameBytes);
					detail::BudgetMemoryResource nameMemory(state.shared.materialized_budget);
					auto *nameAllocator = state.shared.bound_program ? static_cast<std::pmr::memory_resource *>(&nameMemory) : std::pmr::get_default_resource();
					const auto validateColumns = [nameAllocator](const std::vector<std::string> &columns) {
						std::pmr::unordered_set<std::string_view> declaredColumns(nameAllocator);
						declaredColumns.reserve(columns.size());
						for (const std::string &column: columns) {
							if (column.empty() || !declaredColumns.insert(column).second) {
								throw std::invalid_argument("logical-plan Join has invalid output columns");
							}
						}
					};
					validateColumns(leftColumns);
					validateColumns(rightColumns);

					BoundedBatch<LogicalPlanReadRow> rightRows(
							state.shared.bounds.maximum_rows, state.shared.bounds.maximum_materialized_bytes,
							[&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
							&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					rightRows.reserve(state.shared.bounds.maximum_rows);
					std::pmr::vector<const std::string *> sharedColumns(nameAllocator);
					sharedColumns.reserve(std::min(leftColumns.size(), rightColumns.size()));
					std::pmr::unordered_set<std::string_view> rightColumnSet(nameAllocator);
					rightColumnSet.reserve(rightColumns.size());
					for (const std::string &column: rightColumns) {
						rightColumnSet.insert(column);
					}
					for (const std::string &column: leftColumns) {
						if (rightColumnSet.contains(column)) {
							sharedColumns.push_back(&column);
						}
					}
					// Build a bounded hash index over shared bindings. A join with no
					// shared columns intentionally retains Cartesian semantics by using
					// the same candidate list for every left row.
					detail::BudgetMemoryResource rightIndexMemory(state.shared.materialized_budget);
					std::pmr::unordered_map<std::pmr::string, std::pmr::vector<size_t>> rightRowsByKey{&rightIndexMemory};
					const bool rightCompleted = visitOperator(*right, state, [&](LogicalPlanReadRow row) {
						if (rightRows.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan Join exceeded its materialized right-side row bound");
						}
						LogicalPlanReadRow normalized = normalizeOutputRow(std::move(row), rightColumns);
						const auto key = bindingKey(normalized, sharedColumns, &rightIndexMemory);
						const size_t rowBytes = logicalReadRowBytes(normalized, state.shared.context);
						if (rowBytes > state.shared.bounds.maximum_materialized_bytes -
							std::min(rightRows.bytes(), state.shared.bounds.maximum_materialized_bytes))
							throw std::runtime_error("logical-plan Join exceeded its materialized byte bound");
						rightRows.append(std::move(normalized));
						rightRowsByKey[key].push_back(rightRows.size() - 1U);
						return true;
					});
					if (!rightCompleted) {
						throw std::runtime_error(
								"logical-plan Join could not prove that its materialized right input was exhausted");
					}
					if (rightRows.empty()) {
						return true;
					}

					bool stoppedByVisitor = false;
					const bool leftCompleted = visitOperator(*left, state, [&](LogicalPlanReadRow row) {
						MaterializedByteReservation leftReservation(&state.shared.materialized_budget);
						LogicalPlanReadRow normalizedLeft = normalizeOutputRow(std::move(row), leftColumns);
						const auto key = bindingKey(normalizedLeft, sharedColumns, &rightIndexMemory);
						leftReservation.set(logicalReadRowBytes(normalizedLeft, state.shared.context));
						const auto candidates = rightRowsByKey.find(key);
						if (candidates == rightRowsByKey.end()) {
							return true;
						}
						for (const size_t rightIndex: candidates->second) {
							checkGuard(state);
							const LogicalPlanReadRow &rightRow = rightRows.values()[rightIndex];
							const bool lastCandidate = rightIndex == candidates->second.back();
							MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
							if (lastCandidate) {
								outputReservation.absorb(std::move(leftReservation));
								leftReservation = MaterializedByteReservation(&state.shared.materialized_budget);
							} else {
								outputReservation.set(logicalReadRowBytes(normalizedLeft, state.shared.context));
							}
							// Admit the right-side copy while its reusable source is still retained.
							outputReservation.set(checkedSizeAdd(outputReservation.bytes(),
								logicalReadRowBytes(rightRow, state.shared.context) - sizeof(LogicalPlanReadRow),
								"logical-plan combined row size overflowed"));
							LogicalPlanReadRow combined = lastCandidate ? std::move(normalizedLeft) : detail::copyLogicalReadRowGuarded(normalizedLeft, [&] { checkGuardNow(state); });
							if (lastCandidate) {
								normalizedLeft = {};
							}
							bool compatible = true;
							for (const auto &[column, value]: rightRow.values) {
								const auto existing = combined.values.find(column);
								if (existing != combined.values.end()) {
									if (!logicalValuesEqual(existing->second, value)) {
										compatible = false;
										break;
									}
									continue;
								}
								combined.values.emplace(column, detail::copyLogicalReadValueGuarded(value, [&] { checkGuardNow(state); }));
							}
							if (!compatible) {
								continue;
							}
							mergeCompatibleRowCaches(combined, rightRow, state);
							outputReservation.set(logicalReadRowBytes(combined, state.shared.context));
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(combined), std::move(outputReservation));
							} else {
								outputReservation.reset();
								keepGoing = visitor(std::move(combined));
							}
							if (!keepGoing) {
								stoppedByVisitor = true;
								return false;
							}
						}
						return true;
					});
					if (!leftCompleted && !stoppedByVisitor) {
						throw std::runtime_error(
								"logical-plan Join could not prove that its complete left input was read");
					}
					return leftCompleted;
				}
				case logical::LogicalOpType::LOP_OPTIONAL_MATCH: {
					const auto &optional = dynamic_cast<const logical::LogicalOptionalMatch &>(op);
					const logical::LogicalOperator *input = optional.getInput();
					const logical::LogicalOperator *pattern = optional.getOptionalPattern();
					if (input == nullptr || pattern == nullptr) {
						throw std::invalid_argument("logical-plan OptionalMatch requires two valid children");
					}
					MaterializedByteReservation inputNameBytes(&state.shared.materialized_budget), patternNameBytes(&state.shared.materialized_budget);
					const auto inputColumns = boundOutputNames(*input, state, inputNameBytes);
					const auto patternColumns = boundOutputNames(*pattern, state, patternNameBytes);
					detail::BudgetMemoryResource nameMemory(state.shared.materialized_budget);
					auto *nameAllocator = state.shared.bound_program ? static_cast<std::pmr::memory_resource *>(&nameMemory) : std::pmr::get_default_resource();
					std::pmr::unordered_set<std::string_view> inputColumnSet(nameAllocator);
					inputColumnSet.reserve(inputColumns.size());
					for (const std::string &column: inputColumns) {
						if (column.empty() || !inputColumnSet.insert(column).second) {
							throw std::invalid_argument("logical-plan OptionalMatch input has invalid output columns");
						}
					}
					std::pmr::unordered_set<std::string_view> patternColumnSet(nameAllocator);
					patternColumnSet.reserve(patternColumns.size());
					std::pmr::vector<const std::string *> sharedColumns(nameAllocator);
					sharedColumns.reserve(std::min(inputColumns.size(), patternColumns.size()));
					for (const std::string &column: patternColumns) {
						if (column.empty() || !patternColumnSet.insert(column).second) {
							throw std::invalid_argument(
									"logical-plan OptionalMatch pattern has invalid output columns");
						}
						if (inputColumnSet.contains(column)) {
							sharedColumns.push_back(&column);
						}
					}
					for (const std::string &variable: optional.getRequiredVariables()) {
						if (variable.empty() ||
							(!inputColumnSet.contains(variable) && !patternColumnSet.contains(variable))) {
							throw std::invalid_argument("logical-plan OptionalMatch has an invalid required variable");
						}
					}

					detail::BudgetMemoryResource patternIndexMemory(state.shared.materialized_budget);
					std::pmr::unordered_map<std::pmr::string, std::pmr::vector<size_t>> patternRowsByKey{&patternIndexMemory};
					BoundedBatch<LogicalPlanReadRow> patternRows(
							state.shared.bounds.maximum_rows, state.shared.bounds.maximum_materialized_bytes,
							[&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
							&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					patternRows.reserve(state.shared.bounds.maximum_rows);
					const bool patternCompleted = visitOperator(*pattern, state, [&](LogicalPlanReadRow row) {
						if (patternRows.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan OptionalMatch exceeded its materialized pattern row bound");
						}
						LogicalPlanReadRow normalized = normalizeOutputRow(std::move(row), patternColumns);
						const auto key = bindingKey(normalized, sharedColumns, &patternIndexMemory);
						const size_t rowBytes = logicalReadRowBytes(normalized, state.shared.context);
						if (rowBytes > state.shared.bounds.maximum_materialized_bytes -
							std::min(patternRows.bytes(), state.shared.bounds.maximum_materialized_bytes))
							throw std::runtime_error("logical-plan OptionalMatch exceeded its materialized byte bound");
						patternRows.append(std::move(normalized));
						patternRowsByKey[key].push_back(patternRows.size() - 1U);
						return true;
					});
					if (!patternCompleted) {
						throw std::runtime_error(
								"logical-plan OptionalMatch could not prove that its pattern input was exhausted");
					}

					bool stoppedByVisitor = false;
					const bool inputCompleted = visitOperator(*input, state, [&](LogicalPlanReadRow row) {
						MaterializedByteReservation inputReservation(&state.shared.materialized_budget);
						LogicalPlanReadRow normalizedInput = normalizeOutputRow(std::move(row), inputColumns);
						const auto key = bindingKey(normalizedInput, sharedColumns, &patternIndexMemory);
						inputReservation.set(logicalReadRowBytes(normalizedInput, state.shared.context));
						const auto matches = patternRowsByKey.find(key);
						if (matches == patternRowsByKey.end()) {
							LogicalPlanReadRow nullExtended = std::move(normalizedInput);
							normalizedInput = {};
							for (const std::string &column: patternColumns) {
								if (!inputColumnSet.contains(column)) {
									nullExtended.values.emplace(column, PropertyValue());
								}
							}
							MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
							outputReservation.set(logicalReadRowBytes(nullExtended, state.shared.context));
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(nullExtended), std::move(outputReservation));
							} else {
								outputReservation.reset();
								keepGoing = visitor(std::move(nullExtended));
							}
							if (!keepGoing) {
								stoppedByVisitor = true;
								return false;
							}
							return true;
						}
						for (const size_t matchIndex: matches->second) {
							checkGuard(state);
							const LogicalPlanReadRow &match = patternRows.values()[matchIndex];
							const bool lastMatch = matchIndex == matches->second.back();
							MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
							if (lastMatch) {
								outputReservation.absorb(std::move(inputReservation));
								inputReservation = MaterializedByteReservation(&state.shared.materialized_budget);
							} else {
								outputReservation.set(logicalReadRowBytes(normalizedInput, state.shared.context));
							}
							// Admit the right-side copy while its reusable source is still retained.
							outputReservation.set(checkedSizeAdd(outputReservation.bytes(),
								logicalReadRowBytesIncluding(match, [&](const std::string &alias) { return !inputColumnSet.contains(alias); }, state.shared.context) - sizeof(LogicalPlanReadRow),
								"logical-plan combined row size overflowed"));
							LogicalPlanReadRow combined = lastMatch ? std::move(normalizedInput) : detail::copyLogicalReadRowGuarded(normalizedInput, [&] { checkGuardNow(state); });
							if (lastMatch) {
								normalizedInput = {};
							}
							for (const auto &[column, value]: match.values) {
								if (!inputColumnSet.contains(column)) {
									combined.values.emplace(column, detail::copyLogicalReadValueGuarded(value, [&] { checkGuardNow(state); }));
								}
							}
							mergeCompatibleRowCaches(combined, match, state, &inputColumnSet);
							outputReservation.set(logicalReadRowBytes(combined, state.shared.context));
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(combined), std::move(outputReservation));
							} else {
								outputReservation.reset();
								keepGoing = visitor(std::move(combined));
							}
							if (!keepGoing) {
								stoppedByVisitor = true;
								return false;
							}
						}
						return true;
					});
					if (!inputCompleted && !stoppedByVisitor) {
						throw std::runtime_error(
								"logical-plan OptionalMatch could not prove that its complete input was read");
					}
					return inputCompleted;
				}
				case logical::LogicalOpType::LOP_CALL_SUBQUERY: {
					const auto &subquery = dynamic_cast<const logical::LogicalCallSubquery &>(op);
					const logical::LogicalOperator *input = subquery.getInput();
					const logical::LogicalOperator *body = subquery.getSubquery();
					if (body == nullptr || subquery.isInTransactions() || subquery.getBatchSize() != 0) {
						throw std::invalid_argument("logical-plan CallSubquery has an unsupported or invalid shape");
					}
					const std::vector<std::string> returnedVariables = subquery.getReturnedVars();
					std::unordered_set<std::string> returnedSet;
					returnedSet.reserve(returnedVariables.size());
					for (const std::string &variable: returnedVariables) {
						if (variable.empty() || !returnedSet.insert(variable).second) {
							throw std::invalid_argument("logical-plan CallSubquery has invalid returned variables");
						}
					}
					const auto executeForInput = [&](LogicalPlanReadRow inputRow) {
						MaterializedByteReservation outerReservation(&state.shared.materialized_budget);
						MaterializedByteReservation importedReservation(&state.shared.materialized_budget);
						LogicalPlanReadRow retainedInput = std::move(inputRow);
						outerReservation.set(logicalReadRowBytes(retainedInput, state.shared.context));
						const std::vector<std::string> &importedVars = subquery.getImportedVars();
						for (size_t index = 0U; index < importedVars.size(); ++index) {
							const std::string &variable = importedVars[index];
							if (variable.empty()) {
								throw std::invalid_argument("logical-plan CallSubquery has an empty imported variable");
							}
							if (!retainedInput.values.contains(variable) ||
								std::ranges::find(importedVars.begin(), importedVars.begin() + index, variable) !=
										importedVars.begin() + index) {
								throw std::runtime_error(
										"logical-plan CallSubquery could not resolve a unique imported variable");
							}
						}
						const size_t importedBytes =
								logicalReadRowBytesIncluding(retainedInput, [&](const std::string &name) {
									return std::ranges::find(importedVars, name) != importedVars.end();
								}, state.shared.context);
						admitReferenceResultBytes(importedReservation, importedBytes,
												  "logical-plan CallSubquery imports exceeded its shared byte bound");
						LogicalPlanReadRow injected;
						injected.values.reserve(importedVars.size());
						injected.property_cache.reserve(importedVars.size());
						for (const std::string &variable: importedVars) {
							const auto value = retainedInput.values.find(variable);
							injected.values.emplace(variable, detail::copyLogicalReadValueGuarded(value->second, [&] { checkGuardNow(state); }));
							if (const auto properties = retainedInput.property_cache.find(variable);
								properties != retainedInput.property_cache.end()) {
								injected.property_cache.emplace(variable, detail::copyPropertyMapGuarded(properties->second, [&] { checkGuardNow(state); }));
								if (retainedInput.complete_entity_property_cache.contains(variable)) {
									injected.complete_entity_property_cache.insert(variable);
								}
							}
							if (const auto metadata = retainedInput.entity_metadata_cache.find(variable);
								metadata != retainedInput.entity_metadata_cache.end()) {
								injected.entity_metadata_cache.emplace(variable, detail::copyPropertyMapGuarded(metadata->second, [&] { checkGuardNow(state); }));
							}
							if (const auto path = retainedInput.path_cache.find(variable);
								path != retainedInput.path_cache.end()) {
								injected.path_cache.emplace(variable, detail::copyLogicalReadPathGuarded(path->second, [&] { checkGuardNow(state); }));
							}
						}
						struct InjectionScope final {
							ExecutionState &state;
							const LogicalPlanReadRow *previous;

							InjectionScope(ExecutionState &executionState, const LogicalPlanReadRow &row) :
								state(executionState), previous(executionState.task.injected_row) {
								state.task.injected_row = &row;
							}

							~InjectionScope() { state.task.injected_row = previous; }
						};
						admitReferenceResultBytes(importedReservation, logicalReadRowBytes(injected, state.shared.context),
								"logical-plan CallSubquery imports exceeded its shared byte bound");
						bool emitted = false;
						bool completed = false;
						{
							InjectionScope injection(state, injected);
							const auto acceptBodyRow = [&](LogicalPlanReadRow subqueryRow,
														   MaterializedByteReservation bodyReservation) {
								if (bodyReservation.bytes() == 0U) {
									admitReferenceResultBytes(
											bodyReservation, logicalReadRowBytes(subqueryRow, state.shared.context),
											"logical-plan CallSubquery body row exceeded its shared byte bound");
								}
								LogicalPlanReadRow normalized =
										normalizeOutputRow(std::move(subqueryRow), returnedVariables);
								admitReferenceResultBytes(
										bodyReservation, logicalReadRowBytes(normalized, state.shared.context),
										"logical-plan CallSubquery body row exceeded its shared byte bound");
								MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
								admitReferenceResultBytes(
										outputReservation, logicalReadRowBytes(retainedInput, state.shared.context),
										"logical-plan CallSubquery output exceeded its shared byte bound");
								LogicalPlanReadRow combined = detail::copyLogicalReadRowGuarded(retainedInput, [&] { checkGuardNow(state); });
								for (const auto &[variable, value]: normalized.values) {
									if (combined.values.contains(variable)) {
										throw std::runtime_error(
												"logical-plan CallSubquery attempted to rebind an outer variable");
									}
								}
								combined.values.merge(normalized.values);
								mergeOwnedRowCaches(combined, std::move(normalized));
								outputReservation.absorb(std::move(bodyReservation));
								admitReferenceResultBytes(
										outputReservation, logicalReadRowBytes(combined, state.shared.context),
										"logical-plan CallSubquery output exceeded its shared byte bound");
								emitted = true;
								if (chargedReceiver != nullptr) {
									return (*chargedReceiver)(std::move(combined), std::move(outputReservation));
								}
								outputReservation.reset();
								return visitor(std::move(combined));
							};
							const ChargedRowVisitor chargedBodyRow = acceptBodyRow;
							completed = visitOperator(
									*body, state,
									[&](LogicalPlanReadRow row) {
										return acceptBodyRow(std::move(row),
															 MaterializedByteReservation(&state.shared.materialized_budget));
									},
									supportsChargedRowHandoff(body->getType()) ? &chargedBodyRow : nullptr);
						}
						injected = {};
						importedReservation.reset();
						if (!completed) {
							return false;
						}
						if (!emitted && returnedVariables.empty()) {
							LogicalPlanReadRow output = std::move(retainedInput);
							retainedInput = {};
							if (chargedReceiver != nullptr) {
								return (*chargedReceiver)(std::move(output), std::move(outerReservation));
							}
							outerReservation.reset();
							return visitor(std::move(output));
						}
						return true;
					};
					if (input == nullptr) {
						return executeForInput({});
					}
					// Read-only correlated subqueries commonly consist of a unary
					// expression pipeline over the imported row. Prefetch both selected
					// Properties and complete-Entity requirements across outer rows before
					// invoking that pipeline; the body still evaluates each row normally,
					// so filtering, cardinality, and cancellation semantics do not change.
					const std::vector<const expressions::Expression *> subqueryExpressions =
							collectCorrelatedSubqueryExpressions(*body);
					const std::vector<const expressions::Expression *> batchableSubqueryExpressions =
							selectBatchableExpressions(subqueryExpressions);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(subqueryExpressions);
					if (!batchableSubqueryExpressions.empty() || !completeEntityVariables.empty()) {
						if (const std::optional<bool> batched =
									tryVisitBatchedPropertyExpressions(*input, batchableSubqueryExpressions, state,
																	   executeForInput, {}, completeEntityVariables);
							batched.has_value()) {
							return *batched;
						}
					}
					return visitOperator(*input, state, executeForInput);
				}
				case logical::LogicalOpType::LOP_NAMED_PATH: {
					const auto &namedPath = dynamic_cast<const logical::LogicalNamedPath &>(op);
					const auto &nodeVariables = namedPath.getNodeVariables();
					const auto &edgeVariables = namedPath.getEdgeVariables();
					const bool capturedVariableLengthPath = edgeVariables.empty() && nodeVariables.size() == 2U;
					if (namedPath.getPathVariable().empty() || nodeVariables.empty() ||
						(!capturedVariableLengthPath && edgeVariables.size() + 1U != nodeVariables.size())) {
						throw std::invalid_argument("logical-plan NamedPath has an invalid path shape");
					}
					for (const std::string &variable: nodeVariables) {
						if (variable.empty()) {
							throw std::invalid_argument("logical-plan NamedPath has an anonymous Node component");
						}
					}
					for (const std::string &variable: edgeVariables) {
						if (variable.empty()) {
							throw std::invalid_argument(
									"logical-plan NamedPath has an anonymous relationship component");
						}
					}
					const auto processNamedPathRow = [&](LogicalPlanReadRow row,
														 MaterializedByteReservation inputRowBytes) {
						if (inputRowBytes.bytes() == 0U) {
							admitReferenceResultBytes(
									inputRowBytes, logicalReadRowBytes(row, state.shared.context),
									"logical-plan NamedPath input row exceeded its shared byte bound");
						}
						MaterializedByteReservation boundNodeSlots(&state.shared.materialized_budget);
						MaterializedByteReservation boundNodePointerSlots(&state.shared.materialized_budget);
						MaterializedByteReservation deferredBoundNodeSlots(&state.shared.materialized_budget);
						MaterializedByteReservation nodePointerSlots(&state.shared.materialized_budget);
						MaterializedByteReservation edgePointerSlots(&state.shared.materialized_budget);
						MaterializedByteReservation boundEdgeSlots(&state.shared.materialized_budget);
						MaterializedByteReservation boundNodeResultBytes(&state.shared.materialized_budget);
						MaterializedByteReservation capturedNodeResultBytes(&state.shared.materialized_budget);
						MaterializedByteReservation edgeResultBytes(&state.shared.materialized_budget);
						MaterializedByteReservation inlineNodePayloadBytes(&state.shared.materialized_budget);
						MaterializedByteReservation inlineEdgePayloadBytes(&state.shared.materialized_budget);
						std::vector<storage::LogicalGraphNode> boundNodeRecords;
						size_t boundNodeCapacity = nodeVariables.size();
						if (capturedVariableLengthPath) {
							const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured != row.path_cache.end()) {
								const size_t deferredCount = static_cast<size_t>(std::ranges::count_if(
										captured->second.node_refs,
										[](const storage::EntityRef &reference) { return reference.entity_id != 0U; }));
								boundNodeCapacity = checkedSizeAdd(boundNodeCapacity, deferredCount,
																   "logical-plan NamedPath Node capacity overflowed");
							}
						}
						reserveNamedPathSlots(boundNodeRecords, boundNodeCapacity, boundNodeSlots);
						std::vector<const storage::LogicalGraphNode *> boundNodes;
						reserveNamedPathSlots(boundNodes, nodeVariables.size(), boundNodePointerSlots);
						std::vector<storage::EntityRef> deferredBoundNodes;
						for (const std::string &variable: nodeVariables) {
							const LogicalPlanReadValue &value = requireBinding(row, variable);
							if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
								validateNodeRef(*reference, "logical-plan NamedPath received an invalid or Edge "
															"reference where a Node was required");
								appendNamedPathReference(deferredBoundNodes, *reference, nodeVariables.size(),
														 deferredBoundNodeSlots);
							} else {
								const auto *node = std::get_if<storage::LogicalGraphNode>(&value);
								if (node == nullptr)
									throw std::runtime_error("logical-plan expected a Node variable");
								validateNode(*node, state);
								const size_t previousBytes = inlineNodePayloadBytes.bytes();
								admitReferenceResultBytes(
										inlineNodePayloadBytes,
										checkedSizeAdd(previousBytes, logicalNodeCacheBytes(*node, state.shared.context) - sizeof(*node),
													   "logical-plan NamedPath inline Node payload overflowed"),
										"logical-plan NamedPath inline Node payload exceeded its shared byte bound");
								try {
									boundNodeRecords.push_back(*node);
								} catch (...) {
									inlineNodePayloadBytes.set(previousBytes);
									throw;
								}
							}
						}
						std::vector<storage::LogicalGraphNode> materializedBoundNodes =
								materializeNodeReferences(deferredBoundNodes, state, true, &boundNodeResultBytes);
						for (storage::LogicalGraphNode &node: materializedBoundNodes) {
							boundNodeRecords.push_back(std::move(node));
						}
						for (const std::string &variable: nodeVariables) {
							const LogicalPlanReadValue &value = requireBinding(row, variable);
							const uint64_t id = nodeBindingId(value, state);
							const auto position =
									std::ranges::find(boundNodeRecords, id, &storage::LogicalGraphNode::node_id);
							if (position == boundNodeRecords.end()) {
								throw std::runtime_error("logical-plan NamedPath lost a Node binding");
							}
							boundNodes.push_back(&*position);
						}
						std::vector<const storage::LogicalGraphNode *> nodes;
						if (!capturedVariableLengthPath) {
							nodes = std::move(boundNodes);
							nodePointerSlots = std::move(boundNodePointerSlots);
						}
						std::vector<const storage::LogicalGraphEdge *> edges;
						std::vector<storage::LogicalGraphEdge> boundEdgeRecords;
						if (capturedVariableLengthPath) {
							const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured == row.path_cache.end() || captured->second.nodes.size() < 2U ||
								captured->second.nodes.size() !=
										captured->second.edges.size() + captured->second.edge_refs.size() + 1U ||
								(!captured->second.node_refs.empty() &&
								 captured->second.node_refs.size() != captured->second.nodes.size()) ||
								(captured->second.edges.size() != 0U && !captured->second.edge_refs.empty()) ||
								captured->second.nodes.front().node_id != boundNodes.front()->node_id ||
								captured->second.nodes.back().node_id != boundNodes.back()->node_id) {
								throw std::runtime_error(
										"logical-plan NamedPath lost its variable-length traversal route");
							}
							nodes.clear();
							reserveNamedPathSlots(nodes, captured->second.nodes.size(), nodePointerSlots);
							MaterializedByteReservation deferredCapturedNodeSlots(&state.shared.materialized_budget);
							std::vector<storage::EntityRef> deferredCapturedNodes;
							if (!captured->second.node_refs.empty()) {
								for (const storage::EntityRef &reference: captured->second.node_refs) {
									if (reference.entity_id != 0U) {
										appendNamedPathReference(deferredCapturedNodes, reference,
																 captured->second.node_refs.size(),
																 deferredCapturedNodeSlots);
									}
								}
							}
							std::vector<storage::LogicalGraphNode> materializedCapturedNodes =
									materializeNodeReferences(deferredCapturedNodes, state, true,
															  &capturedNodeResultBytes);
							size_t materializedCapturedOrdinal = 0U;
							for (size_t nodeIndex = 0U; nodeIndex < captured->second.nodes.size(); ++nodeIndex) {
								const storage::LogicalGraphNode *node = &captured->second.nodes[nodeIndex];
								if (!captured->second.node_refs.empty() &&
									captured->second.node_refs[nodeIndex].entity_id != 0U) {
									const storage::EntityRef &reference = captured->second.node_refs[nodeIndex];
									validateNodeRef(reference,
													"logical-plan NamedPath has an invalid deferred Node reference");
									if (reference.entity_id != node->node_id) {
										throw std::runtime_error(
												"logical-plan NamedPath has an invalid deferred Node reference");
									}
									if (materializedCapturedOrdinal >= materializedCapturedNodes.size()) {
										throw std::runtime_error("logical-plan NamedPath lost a deferred Node during "
																 "batch materialization");
									}
									if (materializedCapturedNodes[materializedCapturedOrdinal].node_id !=
										reference.entity_id) {
										throw std::runtime_error("logical-plan NamedPath changed deferred Node "
																 "identity during batch materialization");
									}
									boundNodeRecords.push_back(
											std::move(materializedCapturedNodes[materializedCapturedOrdinal++]));
									node = &boundNodeRecords.back();
								} else {
									validateNode(*node, state);
								}
								nodes.push_back(node);
							}
							const size_t edgeCapacity =
									checkedSizeAdd(captured->second.edges.size(), captured->second.edge_refs.size(),
												   "logical-plan NamedPath Edge capacity overflowed");
							reserveNamedPathSlots(boundEdgeRecords, captured->second.edge_refs.size(), boundEdgeSlots);
							reserveNamedPathSlots(edges, edgeCapacity, edgePointerSlots);
							for (const storage::LogicalGraphEdge &edge: captured->second.edges) {
								validateEdge(edge);
								edges.push_back(&edge);
							}
							std::vector<storage::LogicalGraphEdge> materializedReferences = materializeEdgeReferences(
									captured->second.edge_refs, state, true, &edgeResultBytes);
							for (storage::LogicalGraphEdge &edge: materializedReferences) {
								boundEdgeRecords.push_back(std::move(edge));
								edges.push_back(&boundEdgeRecords.back());
							}
						} else {
							reserveNamedPathSlots(boundEdgeRecords, edgeVariables.size(), boundEdgeSlots);
							reserveNamedPathSlots(edges, edgeVariables.size(), edgePointerSlots);
							struct PendingNamedEdge {
								std::optional<storage::EntityRef> reference;
								std::optional<storage::LogicalGraphEdge> complete;
							};
							MaterializedByteReservation pendingEdgeSlots(&state.shared.materialized_budget);
							std::vector<PendingNamedEdge> pendingEdges;
							reserveNamedPathSlots(pendingEdges, edgeVariables.size(), pendingEdgeSlots);
							MaterializedByteReservation deferredEdgeSlots(&state.shared.materialized_budget);
							std::vector<storage::EntityRef> deferredReferences;
							for (const std::string &variable: edgeVariables) {
								const LogicalPlanReadValue &value = requireBinding(row, variable);
								if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
									validateEdgeRef(*reference, "logical-plan NamedPath received an invalid or Node "
																"reference where an Edge was required");
									pendingEdges.push_back({*reference, std::nullopt});
									appendNamedPathReference(deferredReferences, *reference, edgeVariables.size(),
															 deferredEdgeSlots);
								} else {
									const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value);
									if (edge == nullptr)
										throw std::runtime_error("logical-plan expected a relationship variable");
									validateEdge(*edge);
									const size_t previousBytes = inlineEdgePayloadBytes.bytes();
									admitReferenceResultBytes(
											inlineEdgePayloadBytes,
											checkedSizeAdd(previousBytes, logicalEdgeCacheBytes(*edge, state.shared.context) - sizeof(*edge),
														   "logical-plan NamedPath inline Edge payload overflowed"),
											"logical-plan NamedPath inline Edge payload exceeded its shared byte "
											"bound");
									try {
										pendingEdges.push_back({std::nullopt, *edge});
									} catch (...) {
										inlineEdgePayloadBytes.set(previousBytes);
										throw;
									}
								}
							}
							std::vector<storage::LogicalGraphEdge> materializedReferences =
									materializeEdgeReferences(deferredReferences, state, true, &edgeResultBytes);
							size_t deferredOrdinal = 0U;
							for (PendingNamedEdge &pending: pendingEdges) {
								if (pending.reference.has_value()) {
									if (deferredOrdinal >= materializedReferences.size()) {
										throw std::runtime_error("logical-plan NamedPath lost a deferred Edge during "
																 "batch materialization");
									}
									pending.complete = std::move(materializedReferences[deferredOrdinal++]);
								}
								if (!pending.complete.has_value()) {
									throw std::runtime_error("logical-plan NamedPath has an empty Edge binding");
								}
								boundEdgeRecords.push_back(std::move(*pending.complete));
								edges.push_back(&boundEdgeRecords.back());
							}
						}
						for (size_t index = 0U; index < edges.size(); ++index) {
							const uint64_t first = nodes[index]->node_id;
							const uint64_t second = nodes[index + 1U]->node_id;
							const storage::LogicalGraphEdge &edge = *edges[index];
							const bool connects = (edge.source_node_id == first && edge.target_node_id == second) ||
												  (edge.source_node_id == second && edge.target_node_id == first);
							if (!connects) {
								throw std::runtime_error(
										"logical-plan NamedPath relationship does not connect its adjacent Nodes");
							}
						}

						MaterializedByteReservation sourcePropertyBytes(&state.shared.materialized_budget);
						detail::BudgetMemoryResource propertyCacheMemory(state.shared.materialized_budget);
						MaterializedPropertyCache materialized(&propertyCacheMemory);
						const auto pathIdValue = [](uint64_t id) {
							if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
								throw std::overflow_error(
										"logical-plan NamedPath cannot represent an entity ID in a Property value");
							}
							return PropertyValue(static_cast<int64_t>(id));
						};
						MaterializedByteReservation pathBytes(&state.shared.materialized_budget);
						const auto admitPathBytes = [&](size_t nextBytes) {
							if (pathBytes.wouldExceed(nextBytes)) {
								throw std::runtime_error(
										"logical-plan NamedPath result exceeded its shared byte bound");
							}
							pathBytes.set(nextBytes);
						};
						std::vector<PropertyValue> path;
						const size_t pathCapacity = checkedSizeAdd(nodes.size(), edges.size(),
																   "logical-plan NamedPath result capacity overflowed");
						admitPathBytes(checkedSizeMul(pathCapacity, sizeof(PropertyValue),
													  "logical-plan NamedPath result capacity overflowed"));
						path.reserve(pathCapacity);
						admitPathBytes(checkedSizeMul(path.capacity(), sizeof(PropertyValue),
													  "logical-plan NamedPath result capacity overflowed"));
						const auto appendEntity = [&](std::string_view type, uint64_t id,
													  const PropertyMap &properties) {
							const PropertyValue typeValue{std::string(type)};
							const PropertyValue idValue = pathIdValue(id);
							const bool hasType = properties.contains("_type");
							const bool hasId = properties.contains("_id");
							const size_t propertyCount = checkedSizeAdd(
									properties.size(), static_cast<size_t>(!hasType) + static_cast<size_t>(!hasId),
									"logical-plan NamedPath Property count overflowed");
							if (propertyCount > state.shared.bounds.maximum_property_entries) {
								throw std::runtime_error(
										"logical-plan NamedPath exceeded the configured Property bound");
							}
							constexpr const char *message = "logical-plan NamedPath result size overflowed";
							size_t mapBytes = checkedSizeAdd(
									sizeof(uint32_t),
									checkedSizeMul(propertyCount,
												   sizeof(PropertyValue::MapType::value_type) + sizeof(void *),
												   message),
									message);
							const auto addProperty = [&](std::string_view key, const PropertyValue &value) {
								mapBytes = checkedSizeAdd(
										mapBytes,
										checkedSizeAdd(checkedSizeAdd(key.size(), sizeof(size_t), message),
													   property_utils::getPropertyValueSize(value), message),
										message);
							};
							if (!hasType)
								addProperty("_type", typeValue);
							if (!hasId)
								addProperty("_id", idValue);
							for (const auto &[key, value]: properties)
								addProperty(key, value);
							admitPathBytes(checkedSizeAdd(pathBytes.bytes(), mapBytes, message));
							PropertyValue::MapType entityMap;
							entityMap.reserve(propertyCount);
							entityMap.emplace("_type", typeValue);
							entityMap.emplace("_id", idValue);
							for (const auto &[key, value]: properties)
								entityMap.insert_or_assign(key, value);
							path.emplace_back(std::move(entityMap));
						};
						for (size_t index = 0U; index < nodes.size(); ++index) {
							const storage::LogicalGraphNode &node = *nodes[index];
							appendEntity(
									"node", node.node_id,
									materializeEntityProperties(row, node, state, materialized, &sourcePropertyBytes));
							if (index == edges.size()) {
								continue;
							}
							const storage::LogicalGraphEdge &edge = *edges[index];
							appendEntity(
									"relationship", edge.edge_id,
									materializeEntityProperties(row, edge, state, materialized, &sourcePropertyBytes));
						}
						if (!bindValue(row, namedPath.getPathVariable(), PropertyValue(std::move(path)))) {
							throw std::runtime_error("logical-plan NamedPath attempted to rebind its path variable");
						}
						row.property_cache.erase(namedPath.getPathVariable());
						row.complete_entity_property_cache.erase(namedPath.getPathVariable());
						return visitor(std::move(row));
					};
					if (!state.shared.allow_cross_row_entity_batches) {
						return visitOperator(requireOnlyChild(namedPath), state, [&](LogicalPlanReadRow row) {
							return processNamedPathRow(std::move(row),
													   MaterializedByteReservation(&state.shared.materialized_budget));
						});
					}
					const size_t batchCapacity = std::min(DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_rows);
					if (batchCapacity == 0U) {
						throw std::runtime_error("logical-plan NamedPath has no positive batch capacity");
					}
					BoundedBatch<LogicalPlanReadRow> rows(
							batchCapacity, state.shared.bounds.maximum_materialized_bytes,
							[&state](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row, state.shared.context); },
							&state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					rows.reserve(batchCapacity);
					const auto flushNamedPathRows = [&]() {
						if (rows.empty()) {
							return true;
						}
						prefetchEntityRefsInRows(rows.values(), state, true);
						std::reverse(rows.values().begin(), rows.values().end());
						while (!rows.empty()) {
							auto charged = rows.popBackCharged();
							if (!processNamedPathRow(std::move(charged.value), std::move(charged.reservation))) {
								rows.clear();
								return false;
							}
						}
						rows.clear();
						return true;
					};
					const bool childCompleted =
							visitOperator(requireOnlyChild(namedPath), state, [&](LogicalPlanReadRow row) {
								rows.append(std::move(row));
								return rows.size() != batchCapacity || flushNamedPathRows();
							});
					if (!flushNamedPathRows()) {
						return false;
					}
					return childCompleted;
				}
				case logical::LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL: {
					const auto &traversal = dynamic_cast<const logical::LogicalVarLengthTraversal &>(op);
					if (traversal.getSourceVar().empty() || !traversal.getEdgeVar().empty() ||
						traversal.getMinHops() < 0 || traversal.getMaxHops() < traversal.getMinHops()) {
						throw std::invalid_argument(
								"logical-plan variable-length Traversal has an unsupported or invalid shape");
					}
					const std::string &direction = traversal.getDirection();
					if (direction != "out" && direction != "in" && direction != "both") {
						throw std::invalid_argument("logical-plan variable-length Traversal has an unknown direction");
					}
					int maximumHops = traversal.getMaxHops();
					if (state.shared.context != nullptr && state.shared.context->maxVarLengthDepth.has_value()) {
						if (*state.shared.context->maxVarLengthDepth < 0) {
							throw std::invalid_argument(
									"logical-plan variable-length Traversal has a negative query depth bound");
						}
						maximumHops = std::min(maximumHops, *state.shared.context->maxVarLengthDepth);
					}
					if (static_cast<uint64_t>(maximumHops) > state.shared.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan variable-length Traversal exceeds its configured depth bound");
					}
					if (maximumHops != 0 &&
						state.shared.bounds.maximum_edges_per_node >
								std::numeric_limits<size_t>::max() / static_cast<size_t>(maximumHops)) {
						throw std::overflow_error("logical-plan variable-length Traversal edge budget overflowed");
					}
					storage::EntityReadShape targetReadShape;
					targetReadShape.include_labels = !traversal.getTargetLabels().empty();
					for (const auto &[key, expected]: traversal.getTargetProperties()) {
						static_cast<void>(expected);
						if (key.empty())
							throw std::invalid_argument("logical-plan Property predicate has an empty key");
						const auto token = state.shared.snapshot.findPropertyToken(key);
						if (!token) {
							targetReadShape.all_properties = true;
						} else {
							targetReadShape.property_tokens.push_back(*token);
						}
					}
					if (targetReadShape.all_properties)
						targetReadShape.property_tokens.clear();
					std::ranges::sort(targetReadShape.property_tokens);
					targetReadShape.property_tokens.erase(
							std::unique(targetReadShape.property_tokens.begin(), targetReadShape.property_tokens.end()),
							targetReadShape.property_tokens.end());
					validateEntityReadShape(targetReadShape);
					// Name columns are also valid for staged/absent keys without a catalog
					// token. Missing and stored NULL both fail non-null equality predicates.
					// Nullable values additionally require a cheap presence probe.
					const bool useNamedPredicateColumns = targetReadShape.all_properties &&
						state.shared.snapshot.supportsPropertyReadAdmission() &&
						(state.shared.snapshot.supportsPropertyPresenceProbes() ||
						 !std::ranges::any_of(traversal.getTargetProperties(),
							[](const auto &predicate) { return isNull(predicate.second); }));
					const auto readNamedPredicateNodes = [&](const std::vector<uint64_t> &ids,
						MaterializedByteReservation &retainedTargets) {
						std::vector<std::string_view> keys;
						MaterializedByteReservation columnsBytes(&state.shared.materialized_budget);
						columnsBytes.set(checkedSizeMul(traversal.getTargetProperties().size(), sizeof(std::string_view),
							"traversal named key slots overflowed"));
						keys.reserve(traversal.getTargetProperties().size());
						for (const auto &[key, expected]: traversal.getTargetProperties()) {
							static_cast<void>(expected);
							if (std::ranges::find(keys, key) == keys.end()) keys.push_back(key);
						}
						if (keys.size() > state.shared.bounds.maximum_property_entries)
							throw std::runtime_error("traversal named columns exceeded their Property bound");
						auto nodes = readNodesShapedSortedBudgeted(state, ids,
							{.include_labels = targetReadShape.include_labels}, &retainedTargets);
						if (nodes.size() != ids.size()) throw std::runtime_error("traversal named targets changed cardinality");
						retainShapedBatchBytes(nodes, retainedTargets, state.shared.context);
						const size_t frames = checkedSizeAdd(checkedSizeMul(keys.capacity(), sizeof(std::string_view),
							"traversal named key capacity overflowed"), checkedSizeMul(keys.size(),
								checkedSizeAdd(sizeof(std::vector<PropertyValue>), checkedSizeMul(ids.size(), sizeof(PropertyValue),
									"traversal named column slots overflowed"), "traversal named column frames overflowed"),
								"traversal named column frames overflowed"), "traversal named frames overflowed");
						columnsBytes.set(frames);
						auto columns = state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(ids, keys, [&](size_t bytes) {
							checkGuard(state);
							columnsBytes.set(checkedSizeAdd(columnsBytes.bytes(), bytes, "traversal named source overflowed"));
						});
						if (columns.size() != keys.size()) throw std::runtime_error("traversal named columns changed cardinality");
						for (const auto &column: columns)
							if (column.size() != ids.size()) throw std::runtime_error("traversal named column changed row count");
						columnsBytes.set(checkedSizeAdd(keys.capacity() * sizeof(std::string_view),
							propertyColumnBytes(columns, keys, "traversal named columns overflowed", state.shared.context),
							"traversal named columns overflowed"));
						for (size_t column = 0U; column < keys.size(); ++column) for (size_t row = 0U; row < ids.size(); ++row) {
							checkGuard(state);
							validateNode(nodes[row], state);
							if (nodes[row].node_id != ids[row]) throw std::runtime_error("traversal named target changed identity");
							if (isNull(columns[column][row])) {
								const bool nullable = std::ranges::any_of(traversal.getTargetProperties(),
									[&](const auto &predicate) { return predicate.first == keys[column] && isNull(predicate.second); });
								if (!nullable) continue;
								const auto present = state.shared.snapshot.nodePropertyExists(ids[row], keys[column]);
								if (!present) throw std::runtime_error("traversal Property presence probe is unavailable");
								if (!*present) continue;
							}
							const size_t payload = detail::retainedPropertyValueBytes(columns[column][row], [&] { checkGuard(state); });
							columnsBytes.set(columnsBytes.bytes() - payload);
							retainedTargets.set(checkedSizeAdd(retainedTargets.bytes(),
								checkedSizeAdd(payload, sizeof(storage::LogicalGraphPropertyEntry) + keys[column].size(),
									"traversal named entry overflowed"), "traversal named handoff overflowed"));
							nodes[row].properties.push_back({std::string(keys[column]), std::move(columns[column][row])});
							retainShapedBatchBytes(nodes, retainedTargets, state.shared.context);
						}
						return nodes;
					};
					const auto readPredicateNode = [&](uint64_t id, MaterializedByteReservation &retainedTarget) {
						checkGuard(state);
						if (useNamedPredicateColumns) {
							auto nodes = readNamedPredicateNodes(std::vector<uint64_t>{id}, retainedTarget);
							return std::move(nodes.front());
						}
						auto node = state.shared.snapshot.readNodeShaped(id, targetReadShape);
						validateNode(node, state);
						if (node.node_id != id)
							throw std::runtime_error("logical graph traversal changed target identity");
						if (node.properties.size() > state.shared.bounds.maximum_property_entries ||
							logicalNodeCacheBytes(node, state.shared.context) > state.shared.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan traversal predicate exceeded its materialized bound");
						}
						const size_t targetBytes = logicalNodeCacheBytes(node, state.shared.context);
						if (retainedTarget.wouldExceed(targetBytes)) {
							throw std::runtime_error(
									"logical-plan traversal predicate target exceeded its shared byte bound");
						}
						retainedTarget.set(targetBytes);
						return node;
					};
					const auto readPredicateNodes = [&](const std::vector<uint64_t> &ids,
														MaterializedByteReservation &retainedTargets) {
						checkGuard(state);
						if (ids.size() > state.shared.bounds.maximum_node_records) {
							throw std::runtime_error("logical-plan traversal predicate batch exceeded its Node bound");
						}
						if (useNamedPredicateColumns && !ids.empty()) return readNamedPredicateNodes(ids, retainedTargets);
						auto nodes = ids.empty() ? std::vector<storage::LogicalGraphNode>{}
												 : readNodesShapedSortedBudgeted(state, ids, targetReadShape,
																				 &retainedTargets);
						if (nodes.size() != ids.size()) {
							throw std::runtime_error("logical graph traversal changed target batch cardinality");
						}
						size_t bytes = checkedSizeMul(ids.capacity(), sizeof(uint64_t),
													  "logical-plan traversal predicate batch size overflowed");
						for (size_t index = 0U; index < nodes.size(); ++index) {
							checkGuard(state);
							validateNode(nodes[index], state);
							if (nodes[index].node_id != ids[index]) {
								throw std::runtime_error("logical graph traversal changed target batch identity");
							}
							bytes = checkedSizeAdd(bytes, logicalNodeCacheBytes(nodes[index], state.shared.context),
												   "logical-plan traversal predicate batch size overflowed");
							if (nodes[index].properties.size() > state.shared.bounds.maximum_property_entries ||
								bytes > state.shared.bounds.maximum_materialized_bytes) {
								throw std::runtime_error(
										"logical-plan traversal predicate batch exceeded its materialized bound");
							}
						}
						retainShapedBatchBytes(nodes, retainedTargets, state.shared.context);
						return nodes;
					};
					const bool batchPredicateTargets =
							state.shared.allow_cross_row_entity_batches &&
							(!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty());
					const size_t maximumEdgeVisits =
							maximumHops == 0 ? 0U
											 : state.shared.bounds.maximum_edges_per_node * static_cast<size_t>(maximumHops);
					// An exact target lookup can build one reverse reachability envelope for
					// the whole operator. The forward DFS still owns path semantics; this
					// index-driven envelope only removes branches that cannot reach a target.
					bool reversePruningEnabled = false;
					std::unordered_map<uint64_t, int> reverseDistance;
					MaterializedByteReservation reverseEnvelopeBytes(&state.shared.materialized_budget);
					if (state.shared.snapshot.supportsNativeBatchAdjacentEdgeRefs() &&
						!traversal.getTargetProperties().empty()) {
						// Reverse pruning needs only seed IDs. Native candidates already validate
						// the target predicates; avoid hydrating unrelated target Properties.
						const auto readCandidateTargets = [&]() -> std::optional<std::vector<storage::LogicalGraphNode>> {
							if (!state.shared.snapshot.supportsNativeExactNodeCandidates()) return std::nullopt;
							std::vector<storage::LogicalGraphNode> targets;
							MaterializedByteReservation targetSlots(&state.shared.materialized_budget);
							bool verified = true;
							const size_t sourceBytes = std::min(state.shared.bounds.maximum_candidate_bytes,
									std::max<size_t>(1U, state.shared.materialized_budget.available() / 2U));
							const auto streamed = state.shared.snapshot.visitNodeCandidatesByExactProperties(
									traversal.getTargetLabels(), traversal.getTargetProperties(),
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries, sourceBytes,
									[&](const storage::IndexCandidateBatch &batch) {
										checkGuard(state);
										if (batch.snapshot_generation == 0U || state.shared.snapshot.nativeSnapshotGeneration() != batch.snapshot_generation)
											throw std::runtime_error("reverse target candidates do not belong to the pinned snapshot");
										if (!batch.exact_match_verified || !batch.predicates_verified || !batch.label_scope_verified) { verified = false; return false; }
										const size_t bytes = logicalIndexCandidateBatchBytes(batch, state.shared.context);
										if (bytes > sourceBytes) throw std::runtime_error("reverse target candidate batch exceeded its byte bound");
										MaterializedByteReservation candidateSlots(&state.shared.materialized_budget);
										candidateSlots.set(bytes);
										if (batch.candidates.size() > state.shared.bounds.maximum_node_records - targets.size())
											throw std::runtime_error("reverse target candidates exceeded the Node bound");
										const size_t required = targets.size() + batch.candidates.size();
										if (required > targets.capacity()) {
											const size_t grown = targets.capacity() > state.shared.bounds.maximum_node_records / 2U
													? state.shared.bounds.maximum_node_records : targets.capacity() * 2U;
											const size_t capacity = std::max(required, grown);
											targetSlots.set(checkedSizeMul(checkedSizeAdd(targets.capacity(), capacity,
													"reverse target capacity overflowed"), sizeof(storage::LogicalGraphNode), "reverse target slots overflowed"));
											targets.reserve(capacity);
											targetSlots.set(checkedSizeMul(targets.capacity(), sizeof(storage::LogicalGraphNode), "reverse target slots overflowed"));
										}
										for (const auto &reference: batch.candidates) {
											validateNodeRef(reference, "reverse target candidates contain an invalid Node reference");
											storage::LogicalGraphNode target; target.node_id = reference.entity_id;
											targets.push_back(std::move(target));
										}
										debug::PerfTrace::addValue("logical_plan.reverse_target_candidate_rows", static_cast<int64_t>(batch.candidates.size()));
										return true;
									});
							if (!streamed.has_value() || !verified) return std::nullopt;
							if (!*streamed) throw std::runtime_error("reverse target candidate stream stopped before completion");
							return targets;
						};
						auto targetNodes = readCandidateTargets();
						if (!targetNodes.has_value()) {
							targetNodes = state.shared.snapshot.findNodesByExactProperties(
									traversal.getTargetLabels(), traversal.getTargetProperties(),
									state.shared.bounds.maximum_node_records, state.shared.bounds.maximum_property_entries);
						}
						if (targetNodes.has_value()) {
							reversePruningEnabled = true;
							if (targetNodes->size() > state.shared.bounds.maximum_node_records) {
								throw std::runtime_error(
										"logical graph exact target lookup exceeded the variable-length Node bound");
							}
							size_t targetBytes =
									checkedSizeMul(targetNodes->capacity(), sizeof(storage::LogicalGraphNode),
												   "logical-plan reverse reachability target storage overflowed");
							for (const storage::LogicalGraphNode &target: *targetNodes) {
								targetBytes =
										checkedSizeAdd(targetBytes, logicalNodeCacheBytes(target, state.shared.context),
													   "logical-plan reverse reachability target storage overflowed");
							}
							const auto reverseMapBytes = [](size_t entries) {
								// Account each hash node plus conservative link and bucket-pointer
								// overhead so the shared byte limit covers the retained envelope.
								return checkedSizeMul(entries,
													  sizeof(std::pair<const uint64_t, int>) + 3U * sizeof(void *),
													  "logical-plan reverse reachability map size overflowed");
							};
							std::vector<uint64_t> frontier;
							const size_t initialFrontierBytes =
									checkedSizeMul(targetNodes->size(), sizeof(uint64_t),
												   "logical-plan reverse reachability frontier size overflowed");
							reverseEnvelopeBytes.set(checkedSizeAdd(
									targetBytes,
									checkedSizeAdd(reverseMapBytes(targetNodes->size()), initialFrontierBytes,
												   "logical-plan reverse reachability storage overflowed"),
									"logical-plan reverse reachability storage overflowed"));
							frontier.reserve(targetNodes->size());
							reverseDistance.reserve(targetNodes->size());
							reverseEnvelopeBytes.set(checkedSizeAdd(
									targetBytes,
									checkedSizeAdd(
											reverseMapBytes(targetNodes->size()),
											checkedSizeMul(
													frontier.capacity(), sizeof(uint64_t),
													"logical-plan reverse reachability frontier size overflowed"),
											"logical-plan reverse reachability storage overflowed"),
									"logical-plan reverse reachability storage overflowed"));
							for (const storage::LogicalGraphNode &target: *targetNodes) {
								validateNode(target, state);
								if (!reverseDistance.emplace(target.node_id, 0).second) {
									throw std::runtime_error(
											"logical graph exact target lookup returned a duplicate Node");
								}
								frontier.push_back(target.node_id);
							}
							std::ranges::sort(frontier);
							const std::optional<uint64_t> reverseTypeToken =
									traversal.getEdgeType().empty()
											? std::nullopt
											: state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
							size_t reverseEdgeVisits = 0U;
							const auto reverseDirection = [&](const std::span<const uint64_t> currentFrontier,
															  const storage::LogicalGraphReadDirection
																	  adjacencyDirection,
															  std::vector<uint64_t> &nextFrontier) {
								if (reverseEdgeVisits == state.shared.bounds.maximum_edge_records) {
									throw std::runtime_error(
											"logical-plan reverse reachability exceeded its Edge bound");
								}
								const size_t remaining = state.shared.bounds.maximum_edge_records - reverseEdgeVisits;
								const size_t edgeVisitBound = checkedSizeAdd(
										remaining, 1U, "logical-plan reverse reachability Edge bound overflowed");
								state.shared.snapshot.visitAdjacentEdgeRefsForNodes(
										currentFrontier, adjacencyDirection, edgeVisitBound,
										[&](const storage::EntityRef &reference) {
											checkGuard(state);
											validateEdgeRef(reference, "logical graph reverse reachability returned an "
																	   "invalid or non-Edge reference");
											if (reverseEdgeVisits == state.shared.bounds.maximum_edge_records) {
												throw std::runtime_error(
														"logical-plan reverse reachability exceeded its Edge bound");
											}
											++reverseEdgeVisits;
											incrementBounded(
													state.task.stats.edges_scanned,
													"logical-plan reverse reachability Edge counter overflowed");
											if (reverseTypeToken.has_value() &&
												reference.type_token != *reverseTypeToken) {
												return true;
											}
											const uint64_t currentId =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_INCOMING
															? reference.target_node_id
															: reference.source_node_id;
											const uint64_t predecessor =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_INCOMING
															? reference.source_node_id
															: reference.target_node_id;
											if (!std::ranges::binary_search(currentFrontier, currentId) ||
												predecessor == 0U) {
												throw std::runtime_error("logical graph reverse reachability returned "
																		 "an inconsistent endpoint");
											}
											const auto current = reverseDistance.find(currentId);
											if (current == reverseDistance.end() || current->second >= maximumHops) {
												return true;
											}
											if (!reverseDistance.contains(predecessor)) {
												const size_t nextSize = checkedSizeAdd(
														nextFrontier.size(), 1U,
														"logical-plan reverse reachability frontier size overflowed");
												const size_t nextCapacity =
														nextSize > nextFrontier.capacity()
																? std::max(nextSize,
																		   nextFrontier.capacity() == 0U
																				   ? 1U
																				   : checkedSizeMul(
																							 nextFrontier.capacity(),
																							 2U,
																							 "logical-plan reverse "
																							 "reachability frontier "
																							 "size overflowed"))
																: nextFrontier.capacity();
												const size_t nextBytes = checkedSizeAdd(
														targetBytes,
														checkedSizeAdd(
																reverseMapBytes(checkedSizeAdd(
																		reverseDistance.size(), 1U,
																		"logical-plan reverse reachability map size "
																		"overflowed")),
																checkedSizeMul(
																		checkedSizeAdd(
																				frontier.capacity(), nextCapacity,
																				"logical-plan reverse reachability "
																				"frontier size overflowed"),
																		sizeof(uint64_t),
																		"logical-plan reverse reachability frontier "
																		"size overflowed"),
																"logical-plan reverse reachability storage overflowed"),
														"logical-plan reverse reachability storage overflowed");
												reverseEnvelopeBytes.set(nextBytes);
												if (nextCapacity > nextFrontier.capacity())
													nextFrontier.reserve(nextCapacity);
												reverseDistance.emplace(predecessor, current->second + 1);
												nextFrontier.push_back(predecessor);
											}
											return true;
										});
							};
							const bool canExpandReverse =
									traversal.getEdgeType().empty() || reverseTypeToken.has_value();
							for (int depth = 0; canExpandReverse && depth < maximumHops && !frontier.empty(); ++depth) {
								std::vector<uint64_t> nextFrontier;
								if (direction == "out" || direction == "both") {
									reverseDirection(frontier, storage::LogicalGraphReadDirection::LGRD_INCOMING,
													 nextFrontier);
								}
								if (direction == "in" || direction == "both") {
									reverseDirection(frontier, storage::LogicalGraphReadDirection::LGRD_OUTGOING,
													 nextFrontier);
								}
								std::ranges::sort(nextFrontier);
								nextFrontier.erase(std::unique(nextFrontier.begin(), nextFrontier.end()),
												   nextFrontier.end());
								frontier = std::move(nextFrontier);
								reverseEnvelopeBytes.set(checkedSizeAdd(
										targetBytes,
										checkedSizeAdd(
												reverseMapBytes(reverseDistance.size()),
												checkedSizeMul(
														frontier.capacity(), sizeof(uint64_t),
														"logical-plan reverse reachability frontier size overflowed"),
												"logical-plan reverse reachability storage overflowed"),
										"logical-plan reverse reachability storage overflowed"));
							}
							std::vector<storage::LogicalGraphNode>().swap(*targetNodes);
							reverseEnvelopeBytes.set(checkedSizeAdd(
									reverseMapBytes(reverseDistance.size()),
									checkedSizeMul(frontier.capacity(), sizeof(uint64_t),
												   "logical-plan reverse reachability frontier size overflowed"),
									"logical-plan reverse reachability storage overflowed"));
						}
					}
					if (reversePruningEnabled && reverseDistance.empty()) {
						return true;
					}

					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						const uint64_t sourceId = nodeBindingId(sourceBinding, state);
						if (reversePruningEnabled) {
							const auto reachable = reverseDistance.find(sourceId);
							if (reachable == reverseDistance.end() || reachable->second > maximumHops) {
								return true;
							}
						}
						const bool sourceNeedsPredicate =
								traversal.getMinHops() == 0 &&
								(!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty());
						MaterializedByteReservation sourcePredicateBytes(&state.shared.materialized_budget);
						const storage::LogicalGraphNode source =
								sourceNeedsPredicate ? readPredicateNode(sourceId, sourcePredicateBytes)
													 : patternSourceIdentity(state, sourceBinding);
						detail::BudgetMemoryResource pathIdentityMemory(state.shared.materialized_budget);
						std::pmr::unordered_set<uint64_t> pathEdges(&pathIdentityMemory);
						std::vector<storage::LogicalGraphNode> pathNodeRecords{source};
						// A variable-length topology traversal does not need a complete
						// endpoint Node when no target label/Property predicate exists. Keep
						// an aligned reference marker so output and NamedPath can materialize
						// the endpoint only at their respective boundaries.
						std::vector<storage::EntityRef> pathNodeRefs;
						pathNodeRefs.push_back({sourceId, false, 0U, 0U, 0U});
						std::vector<storage::LogicalGraphEdge> pathEdgeRecords;
						std::vector<storage::EntityRef> pathEdgeRefs;
						size_t edgeVisits = 0U;
						struct AdjacentEdge {
							storage::LogicalGraphEdgeTopology topology;
							std::optional<storage::EntityRef> reference;
						};

						const auto visitAdjacent = [&](uint64_t nodeId,
													   const std::function<bool(const AdjacentEdge &,
																				const storage::LogicalGraphNode &)>
															   &onEdge) {
							std::optional<uint64_t> relationshipTypeToken;
							const bool useNativeAdjacencyReferences =
									state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
									(traversal.getEdgeType().empty() ||
									 state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType()));
							if (useNativeAdjacencyReferences && !traversal.getEdgeType().empty()) {
								relationshipTypeToken =
										state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
								if (!relationshipTypeToken.has_value()) {
									return true;
								}
							}
							const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
								std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
								uint64_t previousEdgeId = 0U;
								if (useNativeAdjacencyReferences) {
									for (;;) {
										if (edgeVisits == maximumEdgeVisits) {
											throw std::runtime_error("logical-plan variable-length Traversal exceeded "
																	 "its edge exploration bound");
										}
										const size_t pageRecords =
												std::min(maximumEdgeVisits - edgeVisits,
														 state.shared.bounds.maximum_adjacency_page_records);
										MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
										const auto page = readReferencePageBudgeted(state, nodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
										if (page.edges.size() > pageRecords ||
											(page.edges.empty() && page.continuation.has_value()) ||
											(continuation.has_value() &&
											 (page.edges.empty() ||
											  page.edges.front().entity_id != continuation->next_edge_id))) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "variable-length adjacency reference page");
										}
										const bool deferPageTargets = traversal.getTargetLabels().empty() &&
																	  traversal.getTargetProperties().empty() &&
																	  state.shared.snapshot.supportsNodeExistenceProbes();
										MaterializedByteReservation pageEndpointBytes(&state.shared.materialized_budget);
										std::vector<uint64_t> pageEndpointIds;
										if (deferPageTargets || batchPredicateTargets) {
											pageEndpointBytes.set(checkedSizeMul(
													page.edges.size(), sizeof(uint64_t),
													"logical-plan traversal predicate endpoint slots overflowed"));
											pageEndpointIds.reserve(page.edges.size());
											pageEndpointBytes.set(checkedSizeMul(
													pageEndpointIds.capacity(), sizeof(uint64_t),
													"logical-plan traversal predicate endpoint capacity overflowed"));
											for (const storage::EntityRef &candidate: page.edges) {
												validateEdgeRef(candidate, "logical graph backend returned an invalid "
																		   "variable-length adjacency reference");
												if (direction == "both" &&
													adjacencyDirection ==
															storage::LogicalGraphReadDirection::LGRD_INCOMING &&
													candidate.source_node_id == nodeId) {
													continue;
												}
												if (relationshipTypeToken.has_value() &&
													candidate.type_token != *relationshipTypeToken) {
													continue;
												}
												pageEndpointIds.push_back(
														adjacencyDirection == storage::LogicalGraphReadDirection::
																					  LGRD_OUTGOING
																? candidate.target_node_id
																: candidate.source_node_id);
											}
											std::ranges::sort(pageEndpointIds);
											pageEndpointIds.erase(
													std::unique(pageEndpointIds.begin(), pageEndpointIds.end()),
													pageEndpointIds.end());
										}
										const std::vector<bool> pageEndpointsPresent =
												deferPageTargets ? state.shared.snapshot.containsNodesSorted(pageEndpointIds)
																 : std::vector<bool>{};
										MaterializedByteReservation predicateTargetBytes(&state.shared.materialized_budget);
										const auto predicateTargets =
												batchPredicateTargets
														? readPredicateNodes(pageEndpointIds, predicateTargetBytes)
														: std::vector<storage::LogicalGraphNode>{};
										const auto predicateTarget =
												[&](uint64_t id) -> const storage::LogicalGraphNode & {
											const auto found = std::ranges::lower_bound(
													predicateTargets, id, {}, &storage::LogicalGraphNode::node_id);
											if (found == predicateTargets.end() || found->node_id != id) {
												throw std::runtime_error(
														"logical graph traversal lost a batched target");
											}
											return *found;
										};
										if (deferPageTargets && pageEndpointsPresent.size() != pageEndpointIds.size()) {
											throw std::runtime_error("logical graph backend changed variable-length "
																	 "endpoint cardinality");
										}
										for (const storage::EntityRef &reference: page.edges) {
											checkGuard(state);
											validateEdgeRef(reference, "logical graph backend returned an invalid "
																	   "variable-length adjacency reference");
											if (reference.entity_id <= previousEdgeId) {
												throw std::runtime_error("logical graph backend returned an invalid "
																		 "variable-length adjacency reference");
											}
											previousEdgeId = reference.entity_id;
											const bool endpointMatches =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? reference.source_node_id == nodeId
															: reference.target_node_id == nodeId;
											if (!endpointMatches) {
												throw std::runtime_error("logical graph backend returned an invalid "
																		 "variable-length Edge reference");
											}
											++edgeVisits;
											incrementBounded(state.task.stats.edges_scanned,
															 "logical-plan variable-length Edge counter overflowed");
											if (direction == "both" &&
												adjacencyDirection ==
														storage::LogicalGraphReadDirection::LGRD_INCOMING &&
												reference.source_node_id == nodeId) {
												continue;
											}
											if (relationshipTypeToken.has_value() &&
												reference.type_token != *relationshipTypeToken) {
												continue;
											}
											const uint64_t targetId =
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? reference.target_node_id
															: reference.source_node_id;
											const bool deferTargetNode = traversal.getTargetLabels().empty() &&
																		 traversal.getTargetProperties().empty();
											if (deferTargetNode) {
												if (state.shared.snapshot.supportsNodeExistenceProbes()) {
													const auto endpoint =
															std::ranges::lower_bound(pageEndpointIds, targetId);
													if (endpoint == pageEndpointIds.end() || *endpoint != targetId ||
														!pageEndpointsPresent[static_cast<size_t>(
																endpoint - pageEndpointIds.begin())]) {
														throw std::runtime_error(
																"logical graph backend returned a variable-length Edge "
																"with a missing endpoint");
													}
												} else {
													// Authenticate the endpoint with an identity read when no
													// existence probe is available; its Properties are not needed.
													validateNodeIdentity(state, targetId);
												}
											}
											MaterializedByteReservation localTargetBytes(&state.shared.materialized_budget);
											std::optional<storage::LogicalGraphNode> localTarget;
											const storage::LogicalGraphNode &target =
													deferTargetNode			? localTarget.emplace(
																					  storage::LogicalGraphNode{targetId, {}})
													: batchPredicateTargets ? predicateTarget(targetId)
																			: localTarget.emplace(readPredicateNode(
																					  targetId, localTargetBytes));
											const storage::LogicalGraphEdgeTopology topology{reference.entity_id,
																							 reference.source_node_id,
																							 reference.target_node_id,
																							 {}};
											if (!onEdge({topology, reference}, target)) {
												return false;
											}
										}
										if (!page.continuation.has_value()) {
											return true;
										}
										if (page.edges.empty() ||
											page.continuation->previous_edge_id != page.edges.back().entity_id ||
											page.continuation->next_edge_id == 0U ||
											page.continuation->previous_edge_id == page.continuation->next_edge_id) {
											throw std::runtime_error(
													"logical graph backend returned an invalid variable-length "
													"adjacency reference continuation");
										}
										continuation = page.continuation;
									}
								}
								for (;;) {
									if (edgeVisits == maximumEdgeVisits) {
										throw std::runtime_error("logical-plan variable-length Traversal exceeded its "
																 "edge exploration bound");
									}
									const size_t remaining = maximumEdgeVisits - edgeVisits;
									const size_t pageRecords =
											std::min(remaining, state.shared.bounds.maximum_adjacency_page_records);
									// Variable-length traversal never binds an Edge value or reads Edge
									// Properties. Use the topology cursor even when reference pages are
									// unavailable; target Nodes remain materialized only when predicates
									// require them.
									MaterializedByteReservation topologyBytes(&state.shared.materialized_budget);
									const auto page = readTopologyPageBudgeted(state, nodeId, adjacencyDirection,
										continuation, pageRecords, topologyBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "variable-length adjacency page");
									}
									const bool deferPageTargets = traversal.getTargetLabels().empty() &&
																  traversal.getTargetProperties().empty() &&
																  state.shared.snapshot.supportsNodeExistenceProbes();
									MaterializedByteReservation pageEndpointBytes(&state.shared.materialized_budget);
									std::vector<uint64_t> pageEndpointIds;
									if (deferPageTargets || batchPredicateTargets) {
										pageEndpointBytes.set(checkedSizeMul(
												page.edges.size(), sizeof(uint64_t),
												"logical-plan traversal predicate endpoint slots overflowed"));
										pageEndpointIds.reserve(page.edges.size());
										pageEndpointBytes.set(checkedSizeMul(
												pageEndpointIds.capacity(), sizeof(uint64_t),
												"logical-plan traversal predicate endpoint capacity overflowed"));
										for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
											const bool selfLoopDuplicate =
													direction == "both" &&
													adjacencyDirection ==
															storage::LogicalGraphReadDirection::LGRD_INCOMING &&
													candidate.source_node_id == nodeId;
											if (!selfLoopDuplicate && (traversal.getEdgeType().empty() ||
																	   candidate.type == traversal.getEdgeType())) {
												pageEndpointIds.push_back(
														adjacencyDirection == storage::LogicalGraphReadDirection::
																					  LGRD_OUTGOING
																? candidate.target_node_id
																: candidate.source_node_id);
											}
										}
										std::ranges::sort(pageEndpointIds);
										pageEndpointIds.erase(
												std::unique(pageEndpointIds.begin(), pageEndpointIds.end()),
												pageEndpointIds.end());
									}
									const std::vector<bool> pageEndpointsPresent =
											deferPageTargets ? state.shared.snapshot.containsNodesSorted(pageEndpointIds)
															 : std::vector<bool>{};
									MaterializedByteReservation predicateTargetBytes(&state.shared.materialized_budget);
									const auto predicateTargets =
											batchPredicateTargets
													? readPredicateNodes(pageEndpointIds, predicateTargetBytes)
													: std::vector<storage::LogicalGraphNode>{};
									const auto predicateTarget = [&](uint64_t id) -> const storage::LogicalGraphNode & {
										const auto found = std::ranges::lower_bound(
												predicateTargets, id, {}, &storage::LogicalGraphNode::node_id);
										if (found == predicateTargets.end() || found->node_id != id) {
											throw std::runtime_error("logical graph traversal lost a batched target");
										}
										return *found;
									};
									if (deferPageTargets && pageEndpointsPresent.size() != pageEndpointIds.size()) {
										throw std::runtime_error(
												"logical graph backend changed variable-length endpoint cardinality");
									}
									for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
										checkGuard(state);
										validateEdge(edge);
										validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "variable-length");
										const bool endpointMatches =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? edge.source_node_id == nodeId
														: edge.target_node_id == nodeId;
										if (!endpointMatches) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "variable-length adjacent Edge");
										}
										++edgeVisits;
										incrementBounded(state.task.stats.edges_scanned,
														 "logical-plan variable-length Edge counter overflowed");
										if (direction == "both" &&
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
											edge.source_node_id == nodeId) {
											continue;
										}
										if (!traversal.getEdgeType().empty() && edge.type != traversal.getEdgeType()) {
											continue;
										}
										const uint64_t targetId =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? edge.target_node_id
														: edge.source_node_id;
										const bool deferTargetNode = traversal.getTargetLabels().empty() &&
																	 traversal.getTargetProperties().empty();
										if (deferTargetNode) {
											if (state.shared.snapshot.supportsNodeExistenceProbes()) {
												const auto endpoint =
														std::ranges::lower_bound(pageEndpointIds, targetId);
												if (endpoint == pageEndpointIds.end() || *endpoint != targetId ||
													!pageEndpointsPresent[static_cast<size_t>(
															endpoint - pageEndpointIds.begin())]) {
													throw std::runtime_error(
															"logical graph backend returned a variable-length Edge "
															"with a missing endpoint");
												}
											} else {
												validateNodeIdentity(state, targetId);
											}
										}
										MaterializedByteReservation localTargetBytes(&state.shared.materialized_budget);
										std::optional<storage::LogicalGraphNode> localTarget;
										const storage::LogicalGraphNode &target =
												deferTargetNode
														? localTarget.emplace(storage::LogicalGraphNode{targetId, {}})
												: batchPredicateTargets ? predicateTarget(targetId)
																		: localTarget.emplace(readPredicateNode(
																				  targetId, localTargetBytes));
										if (!onEdge({{edge.edge_id, edge.source_node_id, edge.target_node_id,
													  edge.type},
													 std::nullopt},
													target)) {
											return false;
										}
									}
									if (!page.continuation.has_value()) {
										return true;
									}
									if (page.continuation->previous_edge_id != page.edges.back().edge_id ||
										page.continuation->next_edge_id == 0U ||
										page.continuation->previous_edge_id == page.continuation->next_edge_id) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "variable-length adjacency continuation");
									}
									continuation = page.continuation;
								}
							};
							if ((direction == "out" || direction == "both") &&
								!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
								return false;
							}
							if (direction == "in" || direction == "both") {
								return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
							}
							return true;
						};
						std::function<bool(const storage::LogicalGraphNode &, int)> visitPath;
						visitPath = [&](const storage::LogicalGraphNode &node, int depth) {
							checkGuard(state);
							if (depth >= traversal.getMinHops() && depth <= maximumHops &&
								nodeMatches(state.shared.snapshot, node, traversal.getTargetLabels(),
											traversal.getTargetProperties(), state)) {
								const bool deferredNode = !pathNodeRefs.empty() && pathNodeRefs.back().entity_id != 0U;
								constexpr const char *outputMessage = "logical-plan path output size overflowed";
								size_t predictedBytes = logicalReadRowBytes(row, state.shared.context);
								predictedBytes = checkedSizeAdd(predictedBytes,
									checkedSizeMul(traversal.getTargetVar().size(), 2U, outputMessage), outputMessage);
								predictedBytes = checkedSizeAdd(predictedBytes, sizeof(LogicalPlanReadValue), outputMessage);
								predictedBytes = checkedSizeAdd(predictedBytes,
									deferredNode ? sizeof(storage::EntityRef) : logicalNodeCacheBytes(node, state.shared.context), outputMessage);
								for (const auto &record : pathNodeRecords)
									predictedBytes = checkedSizeAdd(predictedBytes, logicalNodeCacheBytes(record, state.shared.context), outputMessage);
								for (const auto &record : pathEdgeRecords)
									predictedBytes = checkedSizeAdd(predictedBytes, logicalEdgeCacheBytes(record, state.shared.context), outputMessage);
								predictedBytes = checkedSizeAdd(predictedBytes,
									checkedSizeMul(checkedSizeAdd(pathNodeRefs.size(), pathEdgeRefs.size(), outputMessage),
										sizeof(storage::EntityRef), outputMessage), outputMessage);
								MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
								outputReservation.set(predictedBytes);
								LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(row, [&] { checkGuardNow(state); });
								LogicalPlanReadValue targetValue =
										deferredNode ? LogicalPlanReadValue(pathNodeRefs.back())
													 : LogicalPlanReadValue(detail::copyLogicalEntityGuarded(node, [&] { checkGuardNow(state); }));
								if (bindValue(expanded, traversal.getTargetVar(), std::move(targetValue))) {
									expanded.path_cache.insert_or_assign(
											traversal.getTargetVar(),
											detail::copyLogicalReadPathGuarded(pathNodeRecords, pathEdgeRecords, pathNodeRefs,
												pathEdgeRefs, [&] { checkGuardNow(state); }));
									outputReservation.set(logicalReadRowBytes(expanded, state.shared.context));
									bool keepGoing = true;
									if (chargedReceiver != nullptr) {
										keepGoing = (*chargedReceiver)(std::move(expanded), std::move(outputReservation));
									} else {
										outputReservation.reset();
										keepGoing = visitor(std::move(expanded));
									}
									if (!keepGoing) return false;
								}
							}
							if (depth == maximumHops) {
								return true;
							}
							return visitAdjacent(node.node_id, [&](const AdjacentEdge &edge,
																   const storage::LogicalGraphNode &target) {
								if (reversePruningEnabled) {
									const auto reachable = reverseDistance.find(target.node_id);
									if (reachable == reverseDistance.end() ||
										reachable->second > maximumHops - (depth + 1)) {
										return true;
									}
								}
								if (pathEdges.contains(edge.topology.edge_id)) {
									return true;
								}
								pathEdges.insert(edge.topology.edge_id);
								if (edge.reference.has_value()) {
									pathEdgeRefs.push_back(*edge.reference);
								} else {
									pathEdgeRecords.push_back({edge.topology.edge_id, edge.topology.source_node_id,
															   edge.topology.target_node_id, edge.topology.type});
								}
								pathNodeRecords.push_back(target);
								pathNodeRefs.push_back({target.node_id, false, 0U, 0U, 0U});
								const bool completed = visitPath(target, depth + 1);
								pathNodeRecords.pop_back();
								pathNodeRefs.pop_back();
								if (edge.reference.has_value()) {
									pathEdgeRefs.pop_back();
								} else {
									pathEdgeRecords.pop_back();
								}
								pathEdges.erase(edge.topology.edge_id);
								return completed;
							});
						};
						return visitPath(source, 0);
					});
				}
				case logical::LogicalOpType::LOP_UNWIND: {
					const auto &unwind = dynamic_cast<const logical::LogicalUnwind &>(op);
					if (unwind.getAlias().empty()) {
						throw std::invalid_argument("logical-plan UNWIND has an empty alias");
					}
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program && unwind.getListExpr()) {
						const expressions::Expression *candidate = unwind.getListExpr().get();
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op),
							std::span<const expressions::Expression *const>(&candidate, 1U), state.shared.snapshot,
							state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					const auto expandRows = [&](LogicalPlanReadRow input, MaterializedByteReservation inputCharge) {
						// One input may produce many output rows. Keep its existing charge
						// until expansion ends; the receiving buffer admits each new row.
						if (inputCharge.bytes() == 0U) {
							const size_t inputBytes = logicalReadRowBytes(input, state.shared.context);
							if (inputBytes > sizeof(LogicalPlanReadRow)) {
								// The empty row frame is stack-owned. Charge retained bindings
								// when an upstream operator cannot transfer a row reservation.
								inputCharge.set(inputBytes - sizeof(LogicalPlanReadRow));
							}
						}
						// Literal lists live with the plan; expression results live through
						// this invocation. Iterate either directly instead of copying the list.
						MaterializedByteReservation evaluatedBytes(&state.shared.materialized_budget);
						LogicalPlanReadValue evaluated;
						std::span<const PropertyValue> values;
						if (unwind.hasLiteralList()) {
							values = unwind.getListValues();
						} else {
							if (!unwind.getListExpr()) {
								throw std::invalid_argument("logical-plan UNWIND has no list expression");
							}
							const PropertyValue *scalar = nullptr;
							bool ownsEvaluated = false;
							if (const auto *parameter = dynamic_cast<const expressions::ParameterExpression *>(
										unwind.getListExpr().get());
								parameter != nullptr) {
								if (state.shared.context == nullptr) {
									throw std::runtime_error(
											"logical-plan parameter evaluation requires a QueryContext");
								}
								const auto found = state.shared.context->parameters.find(parameter->getParameterName());
								if (found == state.shared.context->parameters.end()) {
									throw std::runtime_error(
											"logical-plan read runtime could not resolve a query parameter");
								}
								scalar = &found->second;
							} else if (const auto *list = dynamic_cast<const expressions::ListLiteralExpression *>(
											   unwind.getListExpr().get());
									   list != nullptr && list->isStatic()) {
								scalar = &list->getValue();
							} else {
								ownsEvaluated = true;
								{ ScopedBoundInputs scopedInputs(state, boundInputs, input);
								evaluated = evaluateExpression(*unwind.getListExpr(), input, state); }
								scalar = std::get_if<PropertyValue>(&evaluated);
								if (scalar == nullptr) {
									// Entity expressions retain their existing scalar-ID conversion.
									evaluated = scalarValue(evaluated);
									scalar = std::get_if<PropertyValue>(&evaluated);
								}
							}
							if (ownsEvaluated) {
								evaluatedBytes.set(scalar->getType() == PropertyType::LIST
														   ? materializedListStorageBytes(*scalar, state)
														   : property_utils::getPropertyValueSize(*scalar));
							}
							if (scalar->getType() == PropertyType::LIST) {
								values = scalar->getList();
							}
							if (scalar->getType() != PropertyType::LIST && unwind.scalarAsSingleRow()) {
								// Match the established pipeline UNWIND contract: a scalar
								// expression contributes one row for each input row.
								values = std::span<const PropertyValue>(scalar, 1U);
							}
						}
						if (values.size() > state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan UNWIND exceeded its input list bound");
						}
						for (const PropertyValue &value: values) {
							checkGuard(state);
							MaterializedByteReservation outputBytes(&state.shared.materialized_budget);
							outputBytes.set(checkedSizeAdd(logicalReadRowBytes(input, state.shared.context),
								checkedSizeAdd(unwind.getAlias().size(), sizeof(LogicalPlanReadValue),
									"logical-plan UNWIND output size overflowed"), "logical-plan UNWIND output size overflowed"));
							LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(input, [&] { checkGuardNow(state); });
							expanded.values.insert_or_assign(unwind.getAlias(), copyReservedPropertyValue(value, state, outputBytes));
							expanded.property_cache.erase(unwind.getAlias());
							expanded.complete_entity_property_cache.erase(unwind.getAlias());
							expanded.entity_metadata_cache.erase(unwind.getAlias());
							expanded.path_cache.erase(unwind.getAlias());
							const size_t rowBytes = logicalReadRowBytes(expanded, state.shared.context);
							if (rowBytes > state.shared.bounds.maximum_materialized_bytes) {
								throw std::runtime_error("logical-plan UNWIND exceeded its materialized byte bound");
							}
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								outputBytes.set(rowBytes);
								keepGoing = (*chargedReceiver)(std::move(expanded), std::move(outputBytes));
							} else {
								outputBytes.reset();
								keepGoing = visitor(std::move(expanded));
							}
							if (!keepGoing) {
								return false;
							}
						}
						return true;
					};
					const auto emitUnwindRows = [&](LogicalPlanReadRow input) {
						return expandRows(std::move(input), MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedInput = expandRows;
					const logical::LogicalOperator &unwindChild = requireOnlyChild(unwind);
					if (state.shared.allow_cross_row_entity_batches && !unwind.hasLiteralList() &&
						unwind.getListExpr() != nullptr) {
						const std::vector<const expressions::Expression *> listExpressions{unwind.getListExpr().get()};
						if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									unwindChild, listExpressions, state, emitUnwindRows, {}, {},
									chargedReceiver != nullptr ? &chargedInput : nullptr);
							batched.has_value()) {
							return *batched;
						}
					}
					return visitOperator(unwindChild, state, emitUnwindRows,
										 chargedReceiver != nullptr && supportsChargedRowHandoff(unwindChild.getType())
												 ? &chargedInput
												 : nullptr);
				}
				case logical::LogicalOpType::LOP_LOAD_CSV: {
					const auto &loadCsv = dynamic_cast<const logical::LogicalLoadCsv &>(op);
					if (state.shared.context == nullptr) {
						throw std::runtime_error(
								"logical LOAD CSV requires an execution context with an import policy");
					}
					if (loadCsv.getRowVariable().empty() || !loadCsv.getUrlExpr()) {
						throw std::invalid_argument("logical LOAD CSV requires a URI expression and row variable");
					}
					std::optional<detail::BoundExpressionInputs> boundInputs;
					if (state.shared.bound_program) {
						const expressions::Expression *candidate = loadCsv.getUrlExpr().get();
						boundInputs.emplace(*state.shared.bound_program, state.shared.bound_program->id(op),
							std::span<const expressions::Expression *const>(&candidate, 1U), state.shared.snapshot,
							state.shared.materialized_budget, [&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
					}
					const std::string &delimiter = loadCsv.getFieldTerminator();
					if (delimiter.empty() || delimiter.size() > 8U ||
						delimiter.find_first_of("\r\n\"") != std::string::npos) {
						throw std::invalid_argument("logical LOAD CSV has an invalid field terminator");
					}
					const auto streamRows = [&](LogicalPlanReadRow input, MaterializedByteReservation inputCharge) {
						checkGuard(state);
						if (input.values.contains(loadCsv.getRowVariable())) {
							throw std::runtime_error("logical LOAD CSV attempted to rebind its row variable");
						}
						const size_t inputBytes = logicalReadRowBytes(input, state.shared.context);
						MaterializedByteReservation retainedSource(&state.shared.materialized_budget);
						if (inputCharge.bytes() == 0U && inputBytes >= state.shared.materialized_budget.available()) {
							throw std::runtime_error("logical LOAD CSV input exhausted its materialized byte bound");
						}
						if (inputCharge.bytes() != 0U) {
							retainedSource = std::move(inputCharge);
						} else {
							retainedSource.set(inputBytes);
						}
						PropertyValue uri;
						{ ScopedBoundInputs scopedInputs(state, boundInputs, input);
							uri = scalarValue(evaluateExpression(*loadCsv.getUrlExpr(), input, state)); }
						const size_t uriBytes = property_utils::getPropertyValueSize(uri);
						const size_t inputAndUriBytes = checkedSizeAdd(retainedSource.bytes(), uriBytes,
																	   "logical LOAD CSV retained size overflowed");
						retainedSource.set(inputAndUriBytes);
						size_t pathBytesEstimate = 0U;
						if (uri.getType() == PropertyType::STRING) {
							const size_t uriCharacters = checkedSizeAdd(std::get<std::string>(uri.getVariant()).size(),
																		1U, "logical LOAD CSV path size overflowed");
							pathBytesEstimate = checkedSizeMul(uriCharacters, sizeof(std::filesystem::path::value_type),
															   "logical LOAD CSV path size overflowed");
							retainedSource.set(checkedSizeAdd(inputAndUriBytes, pathBytesEstimate,
															  "logical LOAD CSV retained size overflowed"));
						}
						const std::filesystem::path csvPath = resolveCsvImportFile(uri, *state.shared.context);
						const auto &nativePath = csvPath.native();
						const size_t pathBytes =
								checkedSizeMul(nativePath.capacity(), sizeof(std::filesystem::path::value_type),
											   "logical LOAD CSV path storage size overflowed");
						retainedSource.set(checkedSizeAdd(checkedSizeAdd(inputAndUriBytes, pathBytes,
																		 "logical LOAD CSV retained size overflowed"),
														  pathBytesEstimate,
														  "logical LOAD CSV retained size overflowed"));
						const std::string sourcePath = csvPath.string();
						const size_t sourcePathBytes = csvStringCapacityBytes(sourcePath);
						const size_t retainedInputSourcePathBytes = checkedSizeAdd(
								inputAndUriBytes,
								checkedSizeAdd(pathBytes, sourcePathBytes, "logical LOAD CSV retained size overflowed"),
								"logical LOAD CSV retained size overflowed");
						retainedSource.set(retainedInputSourcePathBytes);
						CsvReader reader(sourcePath, delimiter,
										 CsvReaderLimits{state.shared.context->csv_import_policy.maximum_record_bytes,
														 state.shared.context->csv_import_policy.maximum_columns,
														 state.shared.context->csv_import_policy.maximum_field_bytes,
														 state.shared.materialized_budget.available()},
										 [&state] { state.shared.context->checkGuardNow(); });

						std::vector<std::string> headers;
						size_t headerBytes = 0U;
						if (loadCsv.isWithHeaders() && reader.hasNext()) {
							headers = reader.nextRow();
							headerBytes = checkedSizeMul(headers.capacity(), sizeof(std::string),
														 "logical LOAD CSV header capacity overflowed");
							for (const auto &header: headers) {
								headerBytes = checkedSizeAdd(headerBytes, csvStringCapacityBytes(header),
															 "logical LOAD CSV header capacity overflowed");
								if (header.empty()) {
									throw std::runtime_error("logical LOAD CSV has empty or duplicate header names");
								}
							}
							// Borrow names for duplicate detection rather than allocating a
							// second set of strings and unaccounted hash buckets/nodes.
							const size_t validationBytes =
									checkedSizeAdd(headerBytes,
												   checkedSizeMul(headers.size(), sizeof(std::string_view),
																  "logical LOAD CSV header validation size overflowed"),
												   "logical LOAD CSV header validation size overflowed");
							if (validationBytes > state.shared.materialized_budget.available()) {
								throw std::runtime_error(
										"logical LOAD CSV header validation exceeded its materialized byte bound");
							}
							state.shared.context->checkGuardNow();
							std::vector<std::string_view> sortedHeaders;
							sortedHeaders.reserve(headers.size());
							const size_t actualValidationBytes = checkedSizeAdd(
									headerBytes,
									checkedSizeMul(sortedHeaders.capacity(), sizeof(std::string_view),
												   "logical LOAD CSV header validation capacity overflowed"),
									"logical LOAD CSV header validation capacity overflowed");
							if (actualValidationBytes > state.shared.materialized_budget.available()) {
								throw std::runtime_error(
										"logical LOAD CSV header validation exceeded its materialized byte bound");
							}
							for (const auto &header: headers)
								sortedHeaders.emplace_back(header);
							std::sort(sortedHeaders.begin(), sortedHeaders.end());
							state.shared.context->checkGuardNow();
							if (std::adjacent_find(sortedHeaders.begin(), sortedHeaders.end()) != sortedHeaders.end()) {
								throw std::runtime_error("logical LOAD CSV has empty or duplicate header names");
							}
						}

						const size_t retainedBytes = checkedSizeAdd(retainedSource.bytes(), headerBytes,
																	"logical LOAD CSV retained size overflowed");
						if (headerBytes >= state.shared.materialized_budget.available()) {
							throw std::runtime_error("logical LOAD CSV headers exhausted its materialized byte bound");
						}
						retainedSource.set(retainedBytes);
						size_t loadedRows = 0U;
						while (true) {
							if (reader.atEnd())
								break;
							// The row limit only needs an EOF probe to determine whether
							// another row exists. Do not parse or allocate that forbidden row.
							if (loadedRows == state.shared.context->csv_import_policy.maximum_rows) {
								throw std::runtime_error("logical LOAD CSV exceeded its row bound");
							}
							reader.setMaximumBufferedBytes(state.shared.materialized_budget.available());
							if (!reader.hasNext())
								break;
							checkGuard(state);
							std::vector<std::string> fields = reader.nextRow();
							// Predict conversion storage before reserve/emplace. Field strings are
							// moved, but their vector slots coexist with the output container.
							size_t fieldBytes = checkedSizeMul(fields.capacity(), sizeof(std::string),
															   "logical LOAD CSV field capacity overflowed");
							for (const auto &field: fields) {
								fieldBytes = checkedSizeAdd(fieldBytes, csvStringCapacityBytes(field),
															"logical LOAD CSV field capacity overflowed");
							}
							MaterializedByteReservation rowReservation(&state.shared.materialized_budget);
							admitReferenceResultBytes(rowReservation, fieldBytes,
													  "logical LOAD CSV fields exceeded their materialized byte bound");
							size_t constructionBytes = checkedSizeAdd(sizeof(PropertyValue), fieldBytes,
																	  "logical LOAD CSV conversion size overflowed");
							const size_t valueCount =
									loadCsv.isWithHeaders() ? std::min(headers.size(), fields.size()) : fields.size();
							size_t mapNodeStorageBytes = 0U;
							if (loadCsv.isWithHeaders()) {
								// Include the link, a possible cached hash, and alignment slack in
								// addition to the key/value object. The bucket array is charged after
								// reserve using the implementation's reported bucket count.
								constexpr size_t mapNodeEstimate = sizeof(PropertyValue::MapType::value_type) +
																   2U * sizeof(void *) +
																   alignof(PropertyValue::MapType::value_type) - 1U;
								mapNodeStorageBytes = checkedSizeMul(valueCount, mapNodeEstimate,
																	 "logical LOAD CSV map node storage overflowed");
								constructionBytes = checkedSizeAdd(constructionBytes, mapNodeStorageBytes,
																   "logical LOAD CSV conversion size overflowed");
							} else {
								constructionBytes =
										checkedSizeAdd(constructionBytes,
													   checkedSizeMul(valueCount, sizeof(PropertyValue),
																	  "logical LOAD CSV list slot size overflowed"),
													   "logical LOAD CSV conversion size overflowed");
							}
							size_t valueBytes = sizeof(uint32_t);
							for (size_t index = 0U; index < valueCount; ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
								valueBytes = checkedSizeAdd(valueBytes, sizeof(size_t),
															"logical LOAD CSV value size overflowed");
								valueBytes = checkedSizeAdd(valueBytes, fields[index].size(),
															"logical LOAD CSV value size overflowed");
								if (loadCsv.isWithHeaders()) {
									constructionBytes =
											checkedSizeAdd(constructionBytes, csvStringCapacityBytes(headers[index]),
														   "logical LOAD CSV header-key capacity overflowed");
									valueBytes = checkedSizeAdd(valueBytes, sizeof(size_t),
																"logical LOAD CSV value size overflowed");
									valueBytes = checkedSizeAdd(valueBytes, headers[index].size(),
																"logical LOAD CSV value size overflowed");
								}
							}
							size_t outputContainerBytes = 0U;
							size_t outputBytes = checkedSizeAdd(inputBytes, loadCsv.getRowVariable().size(),
																"logical LOAD CSV output size overflowed");
							outputBytes = checkedSizeAdd(outputBytes, sizeof(LogicalPlanReadValue),
														 "logical LOAD CSV output size overflowed");
							outputBytes =
									checkedSizeAdd(outputBytes, valueBytes, "logical LOAD CSV output size overflowed");
							outputBytes = checkedSizeAdd(outputBytes, sizeof(PropertyValue),
														 "logical LOAD CSV handoff size overflowed");
							admitReferenceResultBytes(
									rowReservation, constructionBytes,
									"logical LOAD CSV conversion exceeded its materialized byte bound");
							PropertyValue rowValue;
							if (loadCsv.isWithHeaders()) {
								PropertyValue::MapType values;
								values.reserve(valueCount);
								const size_t requestedBucketBytes = checkedSizeMul(
										valueCount, sizeof(void *), "logical LOAD CSV map bucket estimate overflowed");
								const size_t actualBucketBytes =
										checkedSizeMul(values.bucket_count(), sizeof(void *),
													   "logical LOAD CSV map bucket capacity overflowed");
								if (actualBucketBytes > requestedBucketBytes) {
									constructionBytes =
											checkedSizeAdd(constructionBytes, actualBucketBytes - requestedBucketBytes,
														   "logical LOAD CSV map bucket capacity overflowed");
									admitReferenceResultBytes(
											rowReservation, constructionBytes,
											"logical LOAD CSV conversion exceeded its materialized byte bound");
								}
								outputContainerBytes = checkedSizeAdd(mapNodeStorageBytes, actualBucketBytes,
																	  "logical LOAD CSV map storage overflowed");
								for (size_t index = 0U; index < valueCount; ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									const size_t headerCapacity = csvStringCapacityBytes(headers[index]);
									if (headerCapacity > headers[index].size()) {
										outputContainerBytes = checkedSizeAdd(
												outputContainerBytes, headerCapacity - headers[index].size(),
												"logical LOAD CSV output key capacity overflowed");
									}
									const size_t fieldCapacity = csvStringCapacityBytes(fields[index]);
									if (fieldCapacity > fields[index].size()) {
										outputContainerBytes = checkedSizeAdd(
												outputContainerBytes, fieldCapacity - fields[index].size(),
												"logical LOAD CSV output value capacity overflowed");
									}
								}
								for (size_t index = 0U; index < headers.size() && index < fields.size(); ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									values.emplace(headers[index], PropertyValue(std::move(fields[index])));
								}
								rowValue = PropertyValue(std::move(values));
							} else {
								std::vector<PropertyValue> values;
								values.reserve(fields.size());
								const size_t requestedValueSlots =
										checkedSizeMul(valueCount, sizeof(PropertyValue),
													   "logical LOAD CSV value slot estimate overflowed");
								const size_t actualValueSlots =
										checkedSizeMul(values.capacity(), sizeof(PropertyValue),
													   "logical LOAD CSV value slot capacity overflowed");
								if (actualValueSlots > requestedValueSlots) {
									constructionBytes =
											checkedSizeAdd(constructionBytes, actualValueSlots - requestedValueSlots,
														   "logical LOAD CSV value slot capacity overflowed");
									admitReferenceResultBytes(
											rowReservation, constructionBytes,
											"logical LOAD CSV conversion exceeded its materialized byte bound");
								}
								outputContainerBytes = actualValueSlots;
								for (size_t index = 0U; index < valueCount; ++index) {
									if ((index & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									const size_t fieldCapacity = csvStringCapacityBytes(fields[index]);
									if (fieldCapacity > fields[index].size()) {
										outputContainerBytes = checkedSizeAdd(
												outputContainerBytes, fieldCapacity - fields[index].size(),
												"logical LOAD CSV output value capacity overflowed");
									}
								}
								size_t convertedFields = 0U;
								for (std::string &field: fields) {
									if ((convertedFields++ & 255U) == 0U && state.shared.context != nullptr) state.shared.context->checkGuardNow();
									values.emplace_back(std::move(field));
								}
								rowValue = PropertyValue(std::move(values));
							}
							outputBytes = checkedSizeAdd(outputBytes, outputContainerBytes,
														 "logical LOAD CSV output container capacity overflowed");
							// Drop moved-from slots and any extra columns before copying the input.
							std::vector<std::string>().swap(fields);
							admitReferenceResultBytes(
									rowReservation, outputBytes,
									"logical LOAD CSV conversion exceeded its materialized byte bound");
							++loadedRows;
							LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(input, [&] { checkGuardNow(state); });
							if (!bindValue(expanded, loadCsv.getRowVariable(), std::move(rowValue))) {
								throw std::runtime_error("logical LOAD CSV attempted to rebind its row variable");
							}
							expanded.property_cache.erase(loadCsv.getRowVariable());
							expanded.complete_entity_property_cache.erase(loadCsv.getRowVariable());
							expanded.entity_metadata_cache.erase(loadCsv.getRowVariable());
							const size_t rowBytes = logicalReadRowBytes(expanded, state.shared.context);
							admitReferenceResultBytes(rowReservation, std::max(rowBytes, outputBytes),
													  "logical LOAD CSV exceeded its materialized byte bound");
							// BoundedBatch requires its exact logical row charge. Preserve CSV's
							// additional capacity separately until the receiver returns.
							auto csvCapacityReservation = rowReservation.splitOff(
								outputBytes > rowBytes ? outputBytes - rowBytes : 0U);
							bool keepGoing = true;
							if (chargedReceiver != nullptr) {
								keepGoing = (*chargedReceiver)(std::move(expanded), std::move(rowReservation));
							} else {
								rowReservation.reset();
								keepGoing = visitor(std::move(expanded));
							}
							if (!keepGoing) {
								return false;
							}
						}
						return true;
					};

					const std::vector<logical::LogicalOperator *> children = loadCsv.getChildren();
					if (children.empty()) {
						return streamRows({}, MaterializedByteReservation(&state.shared.materialized_budget));
					}
					if (children.size() != 1U || children.front() == nullptr) {
						throw std::invalid_argument("logical LOAD CSV has an invalid input pipeline");
					}
					const auto streamUnchargedRow = [&](LogicalPlanReadRow input) {
						return streamRows(std::move(input), MaterializedByteReservation(&state.shared.materialized_budget));
					};
					const ChargedRowVisitor chargedStreamRows = streamRows;
					return visitOperator(*children.front(), state, streamUnchargedRow,
										 supportsChargedRowHandoff(children.front()->getType()) ? &chargedStreamRows
																								: nullptr);
				}
				case logical::LogicalOpType::LOP_UNION: {
					const auto &unionOperator = dynamic_cast<const logical::LogicalUnion &>(op);
					const logical::LogicalOperator *left = unionOperator.getLeft();
					const logical::LogicalOperator *right = unionOperator.getRight();
					if (left == nullptr || right == nullptr) {
						throw std::invalid_argument("logical-plan UNION requires two valid children");
					}
					MaterializedByteReservation leftNameBytes(&state.shared.materialized_budget), rightNameBytes(&state.shared.materialized_budget);
					const auto columns = boundOutputNames(*left, state, leftNameBytes);
					const auto rightColumns = boundOutputNames(*right, state, rightNameBytes);
					if (columns.empty() || columns != rightColumns) {
						throw std::invalid_argument(
								"logical-plan UNION children must declare the same nonempty output columns");
					}
					detail::BudgetMemoryResource nameMemory(state.shared.materialized_budget);
					std::pmr::unordered_set<std::string_view> uniqueColumns(state.shared.bound_program
						? static_cast<std::pmr::memory_resource *>(&nameMemory) : std::pmr::get_default_resource());
					uniqueColumns.reserve(columns.size());
					for (const std::string &column: columns) {
						if (column.empty() || !uniqueColumns.insert(column).second) {
							throw std::invalid_argument("logical-plan UNION has invalid output columns");
						}
					}

					detail::BudgetMemoryResource distinctMemory(state.shared.materialized_budget);
					std::pmr::unordered_set<std::pmr::string> distinctRows{&distinctMemory};
					std::optional<detail::ExternalQueryKeySet> externalDistinct;
					if (!unionOperator.isAll() && state.shared.allow_cross_row_entity_batches &&
					        state.shared.bounds.maximum_materialized_bytes >= detail::ExternalQueryKeySet::MINIMUM_QUERY_BYTES) {
					    auto *provider = dynamic_cast<storage::LogicalQueryScratchProvider *>(&state.shared.snapshot);
					    if (provider && provider->queryScratchEnabled())
					        externalDistinct.emplace(*provider, state.shared.materialized_budget, distinctMemory,
					            state.shared.bounds.maximum_distinct_rows, state.shared.bounds.maximum_materialized_bytes / 8U,
					            [&] { checkGuard(state); }, [&] { checkGuardNow(state); },
                                128U * 1024U * 1024U, 1024U,
                                state.shared.context ? state.shared.context->resource_account : nullptr);
					}
					const auto finishDistinct = [&](bool completed) {
					    if (externalDistinct) {
					        externalDistinct->finish();
					        if (externalDistinct->spilled()) debug::PerfTrace::addValue("logical_plan.distinct.spilled", 1);
					    }
					    return completed;
					};
					const auto visitChild = [&](const logical::LogicalOperator &child) {
						const auto acceptRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
							LogicalPlanReadRow output = normalizeOutputRow(std::move(row), columns);
							if (!unionOperator.isAll()) {
								auto key = distinctUnionRowKey<std::pmr::string>(output, columns, &distinctMemory);
								if (externalDistinct) {
									if (!externalDistinct->insert(std::move(key))) return true;
								} else {
									if (distinctRows.contains(key)) return true;
									if (distinctRows.size() == state.shared.bounds.maximum_distinct_rows)
										throw std::runtime_error("logical-plan UNION DISTINCT exceeded its row bound");
									distinctRows.insert(std::move(key));
								}
							}
							if (chargedReceiver != nullptr) {
								if (charge.bytes() != 0U)
									charge.set(logicalReadRowBytes(output, state.shared.context));
								return (*chargedReceiver)(std::move(output), std::move(charge));
							}
							charge.reset();
							return visitor(std::move(output));
						};
						const ChargedRowVisitor chargedInput = acceptRow;
						return visitOperator(
								child, state,
								[&](LogicalPlanReadRow row) {
									return acceptRow(std::move(row),
													 MaterializedByteReservation(&state.shared.materialized_budget));
								},
								chargedReceiver != nullptr && supportsChargedRowHandoff(child.getType()) ? &chargedInput
																										 : nullptr);
					};
					return finishDistinct(visitChild(*left) && visitChild(*right));
				}
				case logical::LogicalOpType::LOP_SORT: {
					const auto &sort = dynamic_cast<const logical::LogicalSort &>(op);
					const auto &items = sort.getSortItems();
					if (items.empty()) {
						throw std::invalid_argument("logical-plan Sort has no sort items");
					}
					for (const logical::LogicalSortItem &item: items) {
						if (!item.expression) {
							throw std::invalid_argument("logical-plan Sort has an empty sort expression");
						}
					}
					if (state.task.topk_sort == &sort && state.task.topk_rows != 0U)
						return visitBoundedTopKSort(sort, state, visitor, chargedReceiver, state.task.topk_rows);
					if(state.shared.allow_cross_row_entity_batches && state.shared.bounds.maximum_materialized_bytes>=detail::ExternalQuerySort::MINIMUM_QUERY_BYTES)
						if(auto *provider=dynamic_cast<storage::LogicalQueryScratchProvider *>(&state.shared.snapshot);provider && provider->queryScratchEnabled())
							return visitSpillableSort(sort,state,visitor,chargedReceiver,*provider);

					struct DecoratedRow {
						LogicalPlanReadRow row;
						std::vector<PropertyValue> keys;
					};
					const auto decoratedRowBytes = [&state](const DecoratedRow &decorated) {
						size_t bytes = logicalReadRowBytes(decorated.row, state.shared.context);
						for (const PropertyValue &key: decorated.keys) {
							bytes = checkedSizeAdd(bytes, retainedExpressionValueBytes(key, state),
												   "logical-plan Sort materialization size overflowed");
						}
						return bytes;
					};
					// Destroy buffered key vectors before their slot reservation on
					// cancellation or exception as well as on the normal output path.
					MaterializedByteReservation keySlots(&state.shared.materialized_budget);
					MaterializedByteReservation enrichmentPayload(&state.shared.materialized_budget);
					BoundedBatch<DecoratedRow> rows(state.shared.bounds.maximum_rows, state.shared.bounds.maximum_materialized_bytes,
													decoratedRowBytes, &state.shared.materialized_budget,
                            state.shared.context && state.shared.context->resource_account, sizeof(LogicalPlanReadRow));
					// Start with one bounded batch, not the complete cardinality/byte
					// allowance. Small grouped results must not allocate millions of
					// unused sort slots; the vector grows for actual input rows.
					rows.reserve(std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.shared.bounds.maximum_rows,
										   state.shared.bounds.maximum_node_records}));
					const auto collectChargedRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						if (rows.size() == state.shared.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Sort exceeded its materialization row bound");
						}
						// Delay expression evaluation until we know ordering can affect the
						// result. This also preserves valid single-row aggregate results when
						// an otherwise unavailable input-only ORDER BY expression is present.
						if (charge.bytes() == 0U)
							rows.append({std::move(row), {}});
						else
							rows.appendCharged({std::move(row), {}}, std::move(charge));
						return true;
					};
					const auto collectRow = [&](LogicalPlanReadRow row) {
						return collectChargedRow(std::move(row),
												 MaterializedByteReservation(&state.shared.materialized_budget));
					};
					std::vector<const expressions::Expression *> sortCandidates;
					sortCandidates.reserve(items.size());
					for (const logical::LogicalSortItem &item: items) {
						sortCandidates.push_back(item.expression.get());
					}
					const std::vector<const expressions::Expression *> sortExpressions =
							selectBatchableExpressions(sortCandidates);
					const std::vector<std::string> completeEntityVariables =
							selectBatchableCompleteEntityVariables(sortCandidates);
					const logical::LogicalOperator &child = requireOnlyChild(sort);
					const ChargedRowVisitor chargedCollect = collectChargedRow;
					const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
							child, sortExpressions, state, collectRow, {}, completeEntityVariables, &chargedCollect);
					const bool completed =
							batched.has_value()
									? *batched
									: visitOperator(child, state, collectRow,
													supportsChargedRowHandoff(child.getType()) ? &chargedCollect
																							   : nullptr);
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Sort could not prove that its materialized input was exhausted");
					}
					size_t slotsPerRow = 0U;
					if (rows.size() > 1U) {
						std::optional<detail::BoundExpressionInputs> boundInputs;
						if (state.shared.bound_program) boundInputs.emplace(*state.shared.bound_program,
							state.shared.bound_program->id(op), sortCandidates, state.shared.snapshot, state.shared.materialized_budget,
							[&] { checkGuardNow(state); }, std::pmr::get_default_resource(), state.shared.context);
						slotsPerRow = checkedSizeMul(items.size(), sizeof(PropertyValue),
													 "logical-plan Sort key-slot size overflowed");
						size_t keySlotBytes =
								checkedSizeMul(rows.size(), slotsPerRow, "logical-plan Sort key-slot size overflowed");
						if (keySlotBytes > state.shared.bounds.maximum_materialized_bytes -
												   std::min(rows.bytes(), state.shared.bounds.maximum_materialized_bytes) ||
							keySlotBytes > state.shared.materialized_budget.available()) {
							throw std::runtime_error(
									"logical-plan Sort key slots exceeded its materialized byte bound");
						}
						keySlots.set(keySlotBytes);
						size_t additionalEnrichmentBytes = 0U;
						for (DecoratedRow &row: rows.values()) {
							row.keys.reserve(items.size());
							const size_t actualSlots = checkedSizeMul(row.keys.capacity(), sizeof(PropertyValue),
									"logical-plan Sort key-slot size overflowed");
							const size_t extraSlots = actualSlots - slotsPerRow;
							const size_t retainedBytes = checkedSizeAdd(
								checkedSizeAdd(rows.bytes(), keySlotBytes,
									"logical-plan Sort materialization size overflowed"),
								additionalEnrichmentBytes, "logical-plan Sort materialization size overflowed");
							if (extraSlots > state.shared.bounds.maximum_materialized_bytes -
									std::min(retainedBytes, state.shared.bounds.maximum_materialized_bytes) ||
								extraSlots > state.shared.materialized_budget.available()) {
								throw std::runtime_error("logical-plan Sort key slots exceeded its materialized byte bound");
							}
							keySlotBytes = checkedSizeAdd(keySlotBytes, extraSlots,
								"logical-plan Sort key-slot size overflowed");
							keySlots.set(keySlotBytes);
							size_t accountedRowBytes = logicalReadRowBytes(row.row, state.shared.context);
							for (const logical::LogicalSortItem &item: items) {
								checkGuard(state);
								PropertyValue key;
								{ ScopedBoundInputs scopedInputs(state, boundInputs, row.row);
									key = scalarValue(evaluateExpression(*item.expression, row.row, state)); }
								const size_t keyBytes = retainedExpressionValueBytes(key, state);
								const size_t currentRowBytes = logicalReadRowBytes(row.row, state.shared.context);
								const size_t rowGrowth = currentRowBytes > accountedRowBytes ?
									currentRowBytes - accountedRowBytes : 0U;
								const size_t enrichmentBytes = checkedSizeAdd(keyBytes, rowGrowth,
									"logical-plan Sort cache enrichment size overflowed");
								if (enrichmentBytes >
									state.shared.bounds.maximum_materialized_bytes -
											std::min(
													checkedSizeAdd(checkedSizeAdd(rows.bytes(), keySlotBytes,
																				  "logical-plan Sort materialization "
																				  "size overflowed"),
																   additionalEnrichmentBytes,
																   "logical-plan Sort materialization size overflowed"),
													state.shared.bounds.maximum_materialized_bytes)) {
									throw std::runtime_error(
											"logical-plan Sort exceeded its materialization byte bound");
								}
								if (enrichmentBytes >
									state.shared.materialized_budget.available()) {
									throw std::runtime_error(
											"logical-plan Sort keys exceeded the shared materialized byte bound");
								}
								additionalEnrichmentBytes =
										checkedSizeAdd(additionalEnrichmentBytes, enrichmentBytes,
													   "logical-plan Sort materialization size overflowed");
								enrichmentPayload.set(additionalEnrichmentBytes);
								accountedRowBytes = std::max(accountedRowBytes, currentRowBytes);
								row.keys.push_back(std::move(key));
							}
						}
						rows.recalculateBytes(std::move(enrichmentPayload));
						std::stable_sort(rows.values().begin(), rows.values().end(),
										 [&](const DecoratedRow &left, const DecoratedRow &right) {
											 checkGuard(state);
											 for (size_t index = 0U; index < items.size(); ++index) {
												 if (left.keys[index] == right.keys[index]) {
													 continue;
												 }
												 return detail::orderByPrecedes(left.keys[index], right.keys[index], items[index].ascending);
											 }
											 return false;
										 });
					}
					// Pop in sorted order so ownership leaves this batch before a
					// downstream buffer charges the same row. Free obsolete sort keys.
					std::reverse(rows.values().begin(), rows.values().end());
					while (!rows.empty()) {
						checkGuard(state);
						if (chargedReceiver != nullptr) {
							auto charged = rows.popBackCharged();
							const size_t releasedSlots = checkedSizeMul(charged.value.keys.capacity(), sizeof(PropertyValue),
								"logical-plan Sort key-slot size overflowed");
							std::vector<PropertyValue>().swap(charged.value.keys);
							keySlots.set(keySlots.bytes() - releasedSlots);
							charged.reservation.set(logicalReadRowBytes(charged.value.row, state.shared.context));
							if (!(*chargedReceiver)(std::move(charged.value.row), std::move(charged.reservation))) {
								return false;
							}
						} else {
							DecoratedRow row = rows.popBack();
							const size_t releasedSlots = checkedSizeMul(row.keys.capacity(), sizeof(PropertyValue),
								"logical-plan Sort key-slot size overflowed");
							std::vector<PropertyValue>().swap(row.keys);
							keySlots.set(keySlots.bytes() - releasedSlots);
							if (!visitor(std::move(row.row))) {
								return false;
							}
						}
					}
					return true;
				}
				case logical::LogicalOpType::LOP_SKIP: {
					const auto &skip = dynamic_cast<const logical::LogicalSkip &>(op);
					if (skip.getOffset() < 0) {
						throw std::invalid_argument("logical-plan SKIP cannot be negative");
					}
					const uint64_t offset = static_cast<uint64_t>(skip.getOffset());
					if (state.shared.bounds.maximum_rows == 0U) {
						throw std::runtime_error("logical-plan SKIP has no positive batch capacity");
					}
					size_t skipped = 0U;
					const auto acceptRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						checkGuard(state);
						if (static_cast<uint64_t>(skipped) < offset) {
							incrementBounded(skipped, "logical-plan SKIP counter overflowed");
							debug::PerfTrace::addValue("logical_plan.skip.discarded_rows", 1);
							return true;
						}
						enforceMaterializedRowBound(row, state);
						if (chargedReceiver != nullptr) {
							return (*chargedReceiver)(std::move(row), std::move(charge));
						}
						charge.reset();
						return visitor(std::move(row));
					};
					const logical::LogicalOperator &child = requireOnlyChild(skip);
					const ChargedRowVisitor chargedRow = acceptRow;
					return visitOperator(
							child, state,
							[&](LogicalPlanReadRow row) {
								return acceptRow(std::move(row),
												 MaterializedByteReservation(&state.shared.materialized_budget));
							},
							supportsChargedRowHandoff(child.getType()) ? &chargedRow : nullptr);
				}
				case logical::LogicalOpType::LOP_LIMIT: {
					const auto &limit = dynamic_cast<const logical::LogicalLimit &>(op);
					if (limit.getLimit() < 0) {
						throw std::invalid_argument("logical-plan LIMIT cannot be negative");
					}
					if (limit.getLimit() == 0) {
						return true;
					}
					const uint64_t maximum = static_cast<uint64_t>(limit.getLimit());
					if (state.shared.bounds.maximum_rows == 0U) {
						throw std::runtime_error("logical-plan LIMIT has no positive batch capacity");
					}
					size_t delivered = 0U;
					bool reachedLimit = false;
					bool cancelledByVisitor = false;
					const auto acceptRow = [&](LogicalPlanReadRow row, MaterializedByteReservation charge) {
						checkGuard(state);
						enforceMaterializedRowBound(row, state);
						incrementBounded(delivered, "logical-plan LIMIT counter overflowed");
						debug::PerfTrace::addValue("logical_plan.limit.delivered_rows", 1);
						bool keepGoing = true;
						if (chargedReceiver != nullptr) {
							keepGoing = (*chargedReceiver)(std::move(row), std::move(charge));
						} else {
							charge.reset();
							keepGoing = visitor(std::move(row));
						}
						if (!keepGoing)
							cancelledByVisitor = true;
						reachedLimit = static_cast<uint64_t>(delivered) >= maximum;
						if (!keepGoing || reachedLimit) {
							debug::PerfTrace::addValue("logical_plan.limit.early_stop", 1);
							return false;
						}
						return true;
					};
					const logical::LogicalOperator &child = requireOnlyChild(limit);
					struct RestoreTopK {
						ExecutionTaskState &task; const logical::LogicalSort *sort; size_t rows;
						~RestoreTopK() { task.topk_sort = sort; task.topk_rows = rows; }
					} restoreTopK{state.task, state.task.topk_sort, state.task.topk_rows};
					const logical::LogicalOperator *candidate = &child;
					while (candidate->getType() == logical::LogicalOpType::LOP_PROJECT &&
						!dynamic_cast<const logical::LogicalProject &>(*candidate).isDistinct())
						candidate = &requireOnlyChild(*candidate);
					if (candidate->getType() == logical::LogicalOpType::LOP_SORT &&
						maximum <= state.shared.bounds.maximum_rows) {
						state.task.topk_sort = &dynamic_cast<const logical::LogicalSort &>(*candidate);
						state.task.topk_rows = static_cast<size_t>(maximum);
					}

					const ChargedRowVisitor chargedRow = acceptRow;
					const bool childCompleted = visitOperator(
							child, state,
							[&](LogicalPlanReadRow row) {
								return acceptRow(std::move(row),
												 MaterializedByteReservation(&state.shared.materialized_budget));
							},
							supportsChargedRowHandoff(child.getType()) ? &chargedRow : nullptr);
					// Reaching the window exhausts its logical output even when the
					// physical child stopped; cancellation still propagates as incomplete.
					if (cancelledByVisitor)
						return false;
					return reachedLimit || childCompleted;
				}
				case logical::LogicalOpType::LOP_TRAVERSAL: {
					const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(op);
					const std::string &direction = traversal.getDirection();
					if (direction != "out" && direction != "in" && direction != "both") {
						throw std::invalid_argument("logical-plan Traversal has an unknown direction");
					}
					if (const std::optional<bool> direct = tryVisitDirectTraversal(traversal, state, visitor);
						direct.has_value()) {
						return *direct;
					}
					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						// The child scan has already validated source predicates. Traversal only
						// needs the stable identity, so retain deferred native references.
						const uint64_t sourceNodeId = nodeBindingId(sourceBinding, state);
						size_t sourceEdges = 0U;
						const bool topologyOnly = traversal.getEdgeProperties().empty();
						std::optional<uint64_t> relationshipTypeToken;
						if (!traversal.getEdgeType().empty() &&
							state.shared.snapshot.supportsNativeRelationshipTypeTokens(traversal.getEdgeType())) {
							relationshipTypeToken = state.shared.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
							if (!relationshipTypeToken.has_value()) {
								return true;
							}
						}
						// Reference pages carry authenticated endpoints and type tokens. They
						// can drive selected Edge-property predicates even when the pattern
						// omits an Edge binding; no complete Edge object is required.
						const bool useReferencePages =
								state.shared.snapshot.supportsNativeAdjacentEdgeRefScans() &&
								(traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
						std::vector<std::pair<std::string, PropertyValue>> edgePredicates;
						edgePredicates.reserve(traversal.getEdgeProperties().size());
						for (const auto &[key, expected]: traversal.getEdgeProperties()) {
							if (key.empty()) {
								throw std::invalid_argument("logical-plan Edge Property predicate has an empty key");
							}
							edgePredicates.emplace_back(key, expected);
						}
						std::ranges::sort(edgePredicates, {}, &std::pair<std::string, PropertyValue>::first);
						std::vector<std::string_view> edgePropertyKeys;
						edgePropertyKeys.reserve(edgePredicates.size());
						for (const auto &[key, expected]: edgePredicates) {
							static_cast<void>(expected);
							edgePropertyKeys.push_back(key);
						}
						const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
							std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
							uint64_t previousEdgeId = 0U;
							if (useReferencePages) {
								struct TargetCandidate {
										size_t reference_index = 0U;
										uint64_t target_id = 0U;
										size_t property_ordinal = 0U;
									};
								MaterializedByteReservation pendingReferenceBytes(&state.shared.materialized_budget);
								const size_t requestedBatchCapacity =
										edgePredicates.empty() && traversal.getTargetProperties().empty()
												? size_t{1U}
												: std::max<size_t>(
														  1U, std::min({DEFAULT_PROPERTY_BATCH_ROWS,
																		state.shared.bounds.maximum_adjacency_page_records,
																		state.shared.bounds.maximum_edges_per_node}));
								// Include the live source page, retained references and target scratch.
								size_t bytesPerReference = 2U * sizeof(storage::EntityRef) + sizeof(TargetCandidate) +
										sizeof(uint64_t) + 2U * sizeof(size_t);
								if (!edgePredicates.empty()) {
									bytesPerReference = checkedSizeAdd(
											bytesPerReference,
											checkedSizeAdd(sizeof(uint64_t) + sizeof(size_t),
														   checkedSizeMul(edgePredicates.size(),
																		  2U * (sizeof(PropertyValue) +
																				sizeof(std::vector<PropertyValue>)),
																		  "logical-plan adjacent reference batch "
																		  "estimate overflowed"),
														   "logical-plan adjacent reference batch estimate overflowed"),
											"logical-plan adjacent reference batch estimate overflowed");
								}
								if (!traversal.getTargetProperties().empty()) {
									bytesPerReference = checkedSizeAdd(
											bytesPerReference,
											checkedSizeAdd(
													sizeof(uint64_t),
													checkedSizeMul(
															traversal.getTargetProperties().size(),
															sizeof(PropertyValue) + sizeof(std::vector<PropertyValue>),
															"logical-plan adjacent target batch estimate overflowed"),
													"logical-plan adjacent target batch estimate overflowed"),
											"logical-plan adjacent target batch estimate overflowed");
								}
								const size_t budgetRows = state.shared.materialized_budget.available() / bytesPerReference;
								const size_t batchCapacity =
										std::min(requestedBatchCapacity, std::max<size_t>(1U, budgetRows));
								pendingReferenceBytes.set(
										checkedSizeMul(batchCapacity, sizeof(storage::EntityRef),
													   "logical-plan adjacent reference buffer size overflowed"));
								std::vector<storage::EntityRef> pendingReferences;
								pendingReferences.reserve(batchCapacity);
								pendingReferenceBytes.set(checkedSizeMul(pendingReferences.capacity(), sizeof(storage::EntityRef),
									"logical-plan adjacent reference buffer capacity overflowed"));
								const auto flushReferences = [&]() {
									if (pendingReferences.empty()) {
										return true;
									}
									if (!edgePredicates.empty() && state.shared.snapshot.supportsNodeExistenceProbes() &&
											!std::ranges::any_of(edgePredicates, [](const auto &predicate) { return isNull(predicate.second); })) {
										detail::BudgetMemoryResource endpointMemory(state.shared.materialized_budget);
										std::pmr::vector<uint64_t> endpointIds{&endpointMemory};
										endpointIds.reserve(pendingReferences.size());
										for (const auto &reference: pendingReferences) {
											endpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? reference.target_node_id : reference.source_node_id);
										}
										detail::sortUniqueIdsGuarded(endpointIds, [&] { checkGuardNow(state); });
										MaterializedByteReservation presenceBytes(&state.shared.materialized_budget);
										presenceBytes.set(packedBooleanBytes(endpointIds.size()));
										const auto present = state.shared.snapshot.containsNodesSorted(endpointIds);
										presenceBytes.set(packedBooleanBytes(present.capacity()));
										if (present.size() != endpointIds.size())
											throw std::runtime_error("logical graph backend changed adjacent endpoint cardinality");
										if (std::ranges::any_of(present, [](bool value) { return !value; }))
											throw std::runtime_error("logical graph backend returned an adjacent Edge with a missing endpoint");
									}
									detail::BudgetMemoryResource selectionMemory(state.shared.materialized_budget);
									SelectionVector selection(pendingReferences.size(), &selectionMemory);
									MaterializedByteReservation selectedPropertyBytes(&state.shared.materialized_budget);
									std::vector<std::vector<PropertyValue>> selectedProperties;
									if (!edgePredicates.empty()) {
										MaterializedByteReservation orderingBytes(&state.shared.materialized_budget);
										orderingBytes.set(checkedSizeMul(pendingReferences.size(), sizeof(size_t) + sizeof(uint64_t),
											"logical-plan adjacent Edge ordering size overflowed"));
										std::vector<size_t> sortedOrdinals(pendingReferences.size());
										std::iota(sortedOrdinals.begin(), sortedOrdinals.end(), size_t{0U});
										std::ranges::sort(sortedOrdinals, [&](const size_t left, const size_t right) {
											return pendingReferences[left].entity_id <
												   pendingReferences[right].entity_id;
										});
										std::vector<uint64_t> edgeIds;
										edgeIds.reserve(pendingReferences.size());
										orderingBytes.set(checkedSizeAdd(
											checkedSizeMul(sortedOrdinals.capacity(), sizeof(size_t), "logical-plan adjacent Edge ordinal capacity overflowed"),
											checkedSizeMul(edgeIds.capacity(), sizeof(uint64_t), "logical-plan adjacent Edge ID capacity overflowed"),
											"logical-plan adjacent Edge ordering capacity overflowed"));
										for (const size_t ordinal: sortedOrdinals) {
											const uint64_t edgeId = pendingReferences[ordinal].entity_id;
											if (!edgeIds.empty() && edgeIds.back() == edgeId) {
												throw std::runtime_error("logical graph backend returned duplicate "
																		 "adjacent Edge references");
											}
											edgeIds.push_back(edgeId);
										}
										const bool impossiblePredicate =
												std::ranges::any_of(edgePredicates, [](const auto &predicate) {
													return isNull(predicate.second);
												});
										if (impossiblePredicate) {
											selection.rejectAll();
										} else {
											size_t keyBytes = 0U;
											for (const std::string_view key: edgePropertyKeys) {
												keyBytes = checkedSizeAdd(
														keyBytes, key.size(),
														"logical-plan adjacent Edge Property key size overflowed");
											}
											const size_t columnFrames = checkedSizeAdd(
													checkedSizeMul(edgePropertyKeys.size(),
																   sizeof(std::vector<PropertyValue>),
																   "logical-plan adjacent Edge Property frame size "
																   "overflowed"),
													checkedSizeMul(checkedSizeMul(edgePropertyKeys.size(),
																				  edgeIds.size(),
																				  "logical-plan adjacent Edge Property "
																				  "frame size overflowed"),
																   sizeof(PropertyValue),
																   "logical-plan adjacent Edge Property frame size "
																   "overflowed"),
													"logical-plan adjacent Edge Property frame size overflowed");
											const size_t reorderFrames = checkedSizeAdd(
													checkedSizeMul(
															edgePropertyKeys.size(), sizeof(std::vector<PropertyValue>),
															"logical-plan adjacent Edge reorder frame size overflowed"),
													checkedSizeMul(
															checkedSizeMul(edgePropertyKeys.size(),
																		   pendingReferences.size(),
																		   "logical-plan adjacent Edge reorder frame "
																		   "size overflowed"),
															sizeof(PropertyValue),
															"logical-plan adjacent Edge reorder frame size overflowed"),
													"logical-plan adjacent Edge reorder frame size overflowed");
											const size_t fixedBytes = checkedSizeAdd(columnFrames, reorderFrames,
												"logical-plan adjacent Edge Property batch size overflowed");
											const size_t requestedBytes = checkedSizeAdd(
													fixedBytes, keyBytes,
													"logical-plan adjacent Edge Property batch size overflowed");
											if (selectedPropertyBytes.wouldExceed(requestedBytes)) {
												throw std::runtime_error("logical-plan adjacent Edge Property batch "
																		 "exceeded its byte bound");
											}
											selectedPropertyBytes.set(requestedBytes);
											const std::function<void(size_t)> admission = [&](size_t bytes) {
												checkGuard(state);
												const size_t next = checkedSizeAdd(
														selectedPropertyBytes.bytes(), bytes,
														"logical-plan adjacent Edge Property batch size overflowed");
												if (selectedPropertyBytes.wouldExceed(next)) {
													throw std::runtime_error("logical-plan adjacent Edge Property "
																			 "batch exceeded its byte bound");
												}
												selectedPropertyBytes.set(next);
											};
											auto sortedColumns =
													state.shared.snapshot.supportsPropertyReadAdmission()
															? state.shared.snapshot.readEdgePropertyColumnsSortedWithAdmission(
																	  edgeIds, edgePropertyKeys, admission)
															: state.shared.snapshot.readEdgePropertyColumnsSorted(
																	  edgeIds, edgePropertyKeys);
											const size_t selectedEdgeBytes = propertyColumnBytes(
													sortedColumns, edgePropertyKeys,
													"logical-plan adjacent Edge Property batch size overflowed", state.shared.context);
											const size_t retainedEdgeBytes = checkedSizeAdd(
													fixedBytes - columnFrames, selectedEdgeBytes,
													"logical-plan adjacent Edge Property batch size overflowed");
											if (selectedPropertyBytes.wouldExceed(retainedEdgeBytes)) {
												throw std::runtime_error("logical-plan adjacent Edge Property batch "
																		 "exceeded its byte bound");
											}
											selectedPropertyBytes.set(retainedEdgeBytes);
											if (sortedColumns.size() != edgePredicates.size()) {
												throw std::runtime_error("logical graph backend changed adjacent Edge "
																		 "Property column cardinality");
											}
											selectedProperties.assign(edgePredicates.size(), {});
											for (size_t propertyIndex = 0U; propertyIndex < sortedColumns.size();
												 ++propertyIndex) {
												if (sortedColumns[propertyIndex].size() != pendingReferences.size()) {
													throw std::runtime_error("logical graph backend changed adjacent "
																			 "Edge Property result cardinality");
												}
												std::vector<PropertyValue> values(pendingReferences.size());
												for (size_t sortedIndex = 0U; sortedIndex < sortedOrdinals.size();
													 ++sortedIndex) {
													checkGuard(state);
													values[sortedOrdinals[sortedIndex]] =
															std::move(sortedColumns[propertyIndex][sortedIndex]);
												}
												selection.forEachSelected([&](const size_t index) {
													if (isNull(values[index]) ||
														values[index] != edgePredicates[propertyIndex].second) {
														selection.reject(index);
													}
												});
												selectedProperties[propertyIndex] = std::move(values);
											}
										}
									}
									// Source columns and reorder scratch have left scope; retain only the moved columns.
									selectedPropertyBytes.set(propertyColumnBytes(selectedProperties, {},
										"logical-plan retained adjacent Property size overflowed", state.shared.context));
									// Resolve target predicates in one bounded column batch.  The
									// previous per-reference complete read made a high-degree
									// anchored traversal pay one Entity/Property lookup per Edge.
									detail::BudgetMemoryResource targetScratchMemory(state.shared.materialized_budget);
									std::pmr::vector<TargetCandidate> targets{&targetScratchMemory};
									targets.reserve(pendingReferences.size());
									for (size_t index = 0U; index < pendingReferences.size(); ++index) {
										if (!selection.includes(index)) {
											continue;
										}
										const storage::EntityRef &reference = pendingReferences[index];
										const uint64_t targetId =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? reference.target_node_id
														: reference.source_node_id;
										targets.push_back({index, targetId, 0U});
									}
									if (targets.empty()) {
										pendingReferences.clear();
										return true;
									}
									std::pmr::vector<uint64_t> targetIds{&targetScratchMemory};
									targetIds.reserve(targets.size());
									for (const TargetCandidate &target: targets) {
										// Deduplicate after sorting.  A linear search here turns a
										// high-degree adjacency batch into O(n^2) work before the
										// column reader even starts; the batch is already bounded and
										// sorting gives deterministic I/O order.
										targetIds.push_back(target.target_id);
									}
									detail::sortUniqueIdsGuarded(targetIds, [&] {
										if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
									});
									MaterializedByteReservation targetLabelsBytes(&state.shared.materialized_budget);
									std::vector<bool> targetLabelsMatch;
									if (!traversal.getTargetLabels().empty()) {
										targetLabelsBytes.set(packedBooleanBytes(targetIds.size()));
										targetLabelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(
												targetIds, traversal.getTargetLabels());
										targetLabelsBytes.set(packedBooleanBytes(targetLabelsMatch.capacity()));
										if (targetLabelsMatch.size() != targetIds.size()) {
											throw std::runtime_error(
													"logical graph backend changed target label-match cardinality");
										}
									}
									// The immutable plan owns literal predicates throughout execution.
									// Retain validation without copying their potentially large payloads
									// or property names on every adjacency batch.
									const auto &targetPredicates = traversal.getTargetProperties();
									validateTraversalTargetProperties(traversal);
									if (traversal.getTargetLabels().empty() && targetPredicates.empty()) {
										if (state.shared.snapshot.supportsNodeExistenceProbes() && edgePredicates.empty()) {
											MaterializedByteReservation targetPresenceBytes(&state.shared.materialized_budget);
											targetPresenceBytes.set(packedBooleanBytes(targetIds.size()));
											const std::vector<bool> present =
													state.shared.snapshot.containsNodesSorted(targetIds);
											targetPresenceBytes.set(packedBooleanBytes(present.capacity()));
											if (present.size() != targetIds.size()) {
												throw std::runtime_error(
														"logical graph backend changed adjacent endpoint cardinality");
											}
											if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
												throw std::runtime_error("logical graph backend returned an adjacent "
																		 "Edge with a missing endpoint");
											}
										} else if (!state.shared.snapshot.supportsNodeExistenceProbes()) {
											for (const uint64_t targetId: targetIds) {
												validateNodeIdentity(state, targetId);
											}
										}
									}
									std::pmr::vector<std::string_view> targetKeyViews{&targetScratchMemory};
									targetKeyViews.reserve(targetPredicates.size());
									for (const auto &[key, expected]: targetPredicates) targetKeyViews.push_back(key);
									MaterializedByteReservation targetPropertyBytes(&state.shared.materialized_budget);
									size_t targetKeyBytes = 0U;
									for (const std::string_view key: targetKeyViews) {
										targetKeyBytes = checkedSizeAdd(
												targetKeyBytes, key.size(),
												"logical-plan adjacent target Property key size overflowed");
									}
									const size_t targetFrames = checkedSizeAdd(
											checkedSizeMul(
													targetKeyViews.size(), sizeof(std::vector<PropertyValue>),
													"logical-plan adjacent target Property frame size overflowed"),
											checkedSizeMul(
													checkedSizeMul(targetKeyViews.size(), targetIds.size(),
																   "logical-plan adjacent target Property frame size "
																   "overflowed"),
													sizeof(PropertyValue),
													"logical-plan adjacent target Property frame size overflowed"),
											"logical-plan adjacent target Property frame size overflowed");
									const size_t targetFixedBytes = targetFrames; // IDs are charged by targetScratchMemory.
									targetPropertyBytes.set(checkedSizeAdd(
											targetFixedBytes, targetKeyBytes,
											"logical-plan adjacent target Property batch size overflowed"));
									const std::function<void(size_t)> targetAdmission = [&](size_t bytes) {
										checkGuard(state);
										const size_t next = checkedSizeAdd(
												targetPropertyBytes.bytes(), bytes,
												"logical-plan adjacent target Property batch size overflowed");
										if (targetPropertyBytes.wouldExceed(next)) {
											throw std::runtime_error("logical-plan adjacent target Property batch "
																	 "exceeded its byte bound");
										}
										targetPropertyBytes.set(next);
									};
									auto targetColumns =
											targetPredicates.empty() ? std::vector<std::vector<PropertyValue>>{}
											: state.shared.snapshot.supportsPropertyReadAdmission()
													? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															  targetIds, targetKeyViews, targetAdmission)
													: state.shared.snapshot.readNodePropertyColumnsSorted(targetIds,
																								   targetKeyViews);
									const size_t targetPayloadBytes = propertyColumnBytes(
											targetColumns, targetKeyViews,
											"logical-plan adjacent target Property batch size overflowed", state.shared.context);
									const size_t retainedTargetBytes = checkedSizeAdd(
											targetFixedBytes - targetFrames, targetPayloadBytes,
											"logical-plan adjacent target Property batch size overflowed");
									if (targetPropertyBytes.wouldExceed(retainedTargetBytes)) {
										throw std::runtime_error(
												"logical-plan adjacent target Property batch exceeded its byte bound");
									}
									targetPropertyBytes.set(retainedTargetBytes);
									if (targetColumns.size() != targetPredicates.size()) {
										throw std::runtime_error("logical graph backend changed target Node Property "
																 "column cardinality");
									}
									for (const std::vector<PropertyValue> &column: targetColumns) {
										if (column.size() != targetIds.size()) {
											throw std::runtime_error("logical graph backend changed target Node "
																	 "Property result cardinality");
										}
									}
									for (TargetCandidate &target: targets) {
										target.property_ordinal = static_cast<size_t>(
												std::ranges::lower_bound(targetIds, target.target_id) -
												targetIds.begin());
									}
									for (const TargetCandidate &candidate: targets) {
										checkGuard(state);
										const size_t targetOrdinal = candidate.property_ordinal;
										if (!targetLabelsMatch.empty() && !targetLabelsMatch[targetOrdinal]) {
											continue;
										}
										bool targetMatches = true;
										for (size_t propertyIndex = 0U; propertyIndex < targetPredicates.size();
											 ++propertyIndex) {
											if (isNull(targetColumns[propertyIndex][targetOrdinal]) ||
												targetColumns[propertyIndex][targetOrdinal] !=
														targetPredicates[propertyIndex].second) {
												targetMatches = false;
												break;
											}
										}
										if (!targetMatches) {
											continue;
										}
										const storage::EntityRef &reference =
												pendingReferences[candidate.reference_index];
										MaterializedByteReservation expandedPropertyBytes(&state.shared.materialized_budget);
										expandedPropertyBytes.set(logicalReadRowBytes(row, state.shared.context));
										LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(row, [&] { checkGuardNow(state); });
										expandedPropertyBytes.set(logicalReadRowBytes(expanded, state.shared.context));
										if (!traversal.getEdgeVar().empty() &&
											!bindValue(expanded, traversal.getEdgeVar(), reference)) {
											continue;
										}
										if (!traversal.getEdgeVar().empty()) {
											for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size();
												 ++propertyIndex) {
												expanded.property_cache[traversal.getEdgeVar()].insert_or_assign(
														edgePredicates[propertyIndex].first,
														copyReservedPropertyValue(selectedProperties[propertyIndex][candidate.reference_index],
															state, expandedPropertyBytes));
											}
										}
										if (!traversal.getTargetVar().empty()) {
											const LogicalPlanReadValue targetBinding = LogicalPlanReadValue(
													storage::EntityRef{candidate.target_id, false, 0U, 0U, 0U});
											if (!bindValue(expanded, traversal.getTargetVar(), targetBinding)) {
												continue;
											}
											for (size_t propertyIndex = 0U; propertyIndex < targetPredicates.size();
												 ++propertyIndex) {
												expanded.property_cache[traversal.getTargetVar()].insert_or_assign(
														targetPredicates[propertyIndex].first,
														copyReservedPropertyValue(targetColumns[propertyIndex][targetOrdinal],
															state, expandedPropertyBytes));
											}
										}
										if (!visitor(std::move(expanded))) {
											pendingReferences.clear();
											return false;
										}
									}
									pendingReferences.clear();
									return true;
								};
								for (;;) {
									const size_t remaining = state.shared.bounds.maximum_edges_per_node - sourceEdges;
									if (remaining == 0U) {
										return true;
									}
									const size_t pageRecords =
											std::min(remaining, state.shared.bounds.maximum_adjacency_page_records);
									MaterializedByteReservation referenceBytes(&state.shared.materialized_budget);
									const auto page = readReferencePageBudgeted(state, sourceNodeId, adjacencyDirection, continuation, pageRecords, referenceBytes);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value()) ||
										(continuation.has_value() &&
										 (page.edges.empty() ||
										  page.edges.front().entity_id != continuation->next_edge_id))) {
										throw std::runtime_error(
												"logical graph backend returned an invalid adjacency reference page");
									}
									for (const storage::EntityRef &reference: page.edges) {
										checkGuard(state);
										validateEdgeRef(
												reference,
												"logical graph backend returned an invalid adjacent Edge reference");
										if (reference.entity_id <= previousEdgeId) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "adjacent Edge reference");
										}
										previousEdgeId = reference.entity_id;
										const bool endpointMatches =
												adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? reference.source_node_id == sourceNodeId
														: reference.target_node_id == sourceNodeId;
										if (!endpointMatches) {
											throw std::runtime_error("logical graph backend returned an invalid "
																	 "adjacent Edge reference");
										}
										incrementBounded(sourceEdges,
														 "logical-plan Traversal per-Node Edge counter overflowed");
										incrementBounded(state.task.stats.edges_scanned,
														 "logical-plan read runtime Edge counter overflowed");
										if (direction == "both" &&
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
											reference.source_node_id == sourceNodeId) {
											continue;
										}
										if (relationshipTypeToken.has_value() &&
											reference.type_token != *relationshipTypeToken) {
											continue;
										}
										pendingReferences.push_back(reference);
										if (pendingReferences.size() >= batchCapacity && !flushReferences()) {
											return false;
										}
									}
									if (!flushReferences()) {
										return false;
									}
									if (!page.continuation.has_value()) {
										return true;
									}
									if (page.edges.empty() ||
										page.continuation->previous_edge_id != page.edges.back().entity_id ||
										page.continuation->next_edge_id == 0U ||
										page.continuation->previous_edge_id == page.continuation->next_edge_id) {
										throw std::runtime_error("logical graph backend returned an invalid adjacency "
																 "reference continuation");
									}
									continuation = page.continuation;
								}
							}
							for (;;) {
								const size_t remaining = state.shared.bounds.maximum_edges_per_node - sourceEdges;
								if (remaining == 0U) { // ZYX_COV_EXCL_LINE: a full adjacency bound returns immediately
													   // after the preceding page.
									return true;
								}
								const size_t pageRecords =
										std::min(remaining, state.shared.bounds.maximum_adjacency_page_records);
								MaterializedByteReservation topologyBytes(&state.shared.materialized_budget);
								MaterializedByteReservation fullPageBytes(&state.shared.materialized_budget);
								std::optional<storage::LogicalGraphAdjacencyPage> fullPage;
								std::optional<storage::LogicalGraphAdjacencyTopologyPage> topologyPage;
								// Relationship types and endpoints are topology data. When a
								// backend exposes native topology pages, authenticate that
								// topology before decoding the complete Edge payload. This
								// keeps the fallback traversal on the same read-shape contract
								// as the reference-page and direct traversal paths.
								const bool prefilterWithTopology =
										!topologyOnly &&
										state.shared.snapshot.supportsNativeAdjacentEdgeTopologyPages();
								if (topologyOnly || prefilterWithTopology) {
									topologyPage = readTopologyPageBudgeted(state, sourceNodeId, adjacencyDirection,
										continuation, pageRecords, topologyBytes);
								} else {
									if (state.shared.snapshot.supportsAdjacencyReadAdmission()) {
										size_t propertyBytes = 0U, bufferBytes = 0U;
										const auto updateBudget = [&] {
											checkGuardNow(state);
											fullPageBytes.set(checkedSizeAdd(propertyBytes, bufferBytes, "logical-plan adjacency source size overflowed"));
										};
										fullPage = state.shared.snapshot.scanAdjacentEdgesWithAdmission(sourceNodeId, adjacencyDirection,
											continuation, pageRecords,
											[&](size_t bytes) { propertyBytes = checkedSizeAdd(propertyBytes, bytes, "adjacency Property size overflowed"); updateBudget(); },
											[&](size_t bytes) { bufferBytes = bytes; updateBudget(); });
									} else if (state.shared.snapshot.supportsAdjacencyPropertyReadAdmission()) {
										fullPage = state.shared.snapshot.scanAdjacentEdgesWithPropertyAdmission(sourceNodeId, adjacencyDirection,
											continuation, pageRecords, [&](size_t bytes) {
												checkGuardNow(state);
												fullPageBytes.set(checkedSizeAdd(fullPageBytes.bytes(), bytes, "logical-plan adjacency source size overflowed"));
											});
									} else {
										fullPage = state.shared.snapshot.scanAdjacentEdges(sourceNodeId, adjacencyDirection, continuation, pageRecords);
									}
									for (const auto &edge : fullPage->edges) {
										validateEdge(edge);
										static_cast<void>(inlineResultPropertyBytes(edge.properties, state));
									}
									retainShapedBatchBytes(fullPage->edges, fullPageBytes, state.shared.context);
								}
								const bool topologyPageSelected = topologyOnly || prefilterWithTopology;
								const size_t pageSize =
										topologyPageSelected ? topologyPage->edges.size() : fullPage->edges.size();
								if (pageSize > pageRecords) {
									throw std::runtime_error(
											"logical graph backend exceeded the adjacency page bound (returned " +
											std::to_string(pageSize) + ", requested " + std::to_string(pageRecords) +
											")");
								}
								const auto &pageContinuation =
										topologyPageSelected ? topologyPage->continuation : fullPage->continuation;
								if (pageSize == 0U) {
									if (pageContinuation.has_value()) {
										throw std::runtime_error(
												"logical graph backend returned an empty nonterminal adjacency page");
									}
									return true;
								}
								const bool targetNeedsRecord = !traversal.getTargetLabels().empty() ||
															   !traversal.getTargetProperties().empty();
								// A selected column cannot distinguish a missing Property from an
								// explicitly stored NULL. Keep the complete-read path for nullable
								// target predicates, whose existing semantics distinguish those cases.
								const bool targetPropertiesCanBeSelected =
										!traversal.getTargetProperties().empty() &&
										!std::ranges::any_of(
												traversal.getTargetProperties(),
												[](const auto &predicate) { return isNull(predicate.second); });
								const bool canBatchTargetShape =
										state.shared.snapshot.supportsNativeNodeRefScans() && targetNeedsRecord &&
										(traversal.getTargetLabels().empty() ||
										 state.shared.snapshot.supportsNodeExistenceProbes()) &&
										(traversal.getTargetProperties().empty() || targetPropertiesCanBeSelected);
								detail::BudgetMemoryResource pageTargetMemory(state.shared.materialized_budget);
								std::pmr::unordered_map<uint64_t, bool> pageTargetMatches{&pageTargetMemory};
								// Both topology and complete adjacency pages already carry endpoint
								// IDs. Validate their liveness once per bounded page instead of
								// performing a Directory lookup for every Edge. The map is page-local
								// so memory remains bounded by the adjacency page limit and
								// early-stop semantics stay unchanged at the visitor boundary.
								std::pmr::unordered_map<uint64_t, bool> pageTargetPresence{&pageTargetMemory};
								if (state.shared.snapshot.supportsNodeExistenceProbes() &&
									traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
									// Validate the topology record before building a batch request.  The
									// sorted existence API intentionally rejects zero IDs with
									// invalid_argument, while malformed storage topology must retain the
									// logical runtime's fail-closed runtime_error contract.
									if (topologyPage.has_value()) {
										for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
											validateEdge(topology);
										}
									} else if (fullPage.has_value()) {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											validateEdge(edge);
										}
									}
									std::pmr::vector<uint64_t> pageTargetIds{&pageTargetMemory};
									pageTargetIds.reserve(pageSize);
									if (topologyPage.has_value()) {
										for (const auto &topology: topologyPage->edges) {
											pageTargetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? topology.target_node_id
															: topology.source_node_id);
										}
									} else if (fullPage.has_value()) {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											pageTargetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
															: edge.source_node_id);
										}
									}
									detail::sortUniqueIdsGuarded(pageTargetIds, [&] {
										if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
									});
									MaterializedByteReservation presentBytes(&state.shared.materialized_budget);
									presentBytes.set(packedBooleanBytes(pageTargetIds.size()));
									const std::vector<bool> present = state.shared.snapshot.containsNodesSorted(pageTargetIds);
									presentBytes.set(packedBooleanBytes(present.capacity()));
									if (present.size() != pageTargetIds.size()) {
										throw std::runtime_error(
												"logical graph backend changed adjacent endpoint cardinality");
									}
									for (size_t index = 0U; index < pageTargetIds.size(); ++index) {
										pageTargetPresence.emplace(pageTargetIds[index], present[index]);
									}
								}
								if (canBatchTargetShape) {
									std::pmr::vector<uint64_t> targetIds{&pageTargetMemory};
									targetIds.reserve(pageSize);
									if (topologyPage.has_value()) {
										for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
											validateEdge(topology);
											targetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? topology.target_node_id
															: topology.source_node_id);
										}
									} else {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											validateEdge(edge);
											targetIds.push_back(
													adjacencyDirection ==
																	storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
															: edge.source_node_id);
										}
									}
									detail::sortUniqueIdsGuarded(targetIds, [&] {
										if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
									});
									MaterializedByteReservation labelsBytes(&state.shared.materialized_budget);
									std::vector<bool> labelsMatch;
									if (!traversal.getTargetLabels().empty()) {
										labelsBytes.set(packedBooleanBytes(targetIds.size()));
										labelsMatch = state.shared.snapshot.readNodeLabelMatchesSorted(
												targetIds, traversal.getTargetLabels());
										labelsBytes.set(packedBooleanBytes(labelsMatch.capacity()));
										if (labelsMatch.size() != targetIds.size()) {
											throw std::runtime_error(
													"logical graph backend changed target label-match cardinality");
										}
									}
									std::pmr::vector<std::pair<std::string_view, const PropertyValue *>> orderedTargetPredicates{&pageTargetMemory};
									orderedTargetPredicates.reserve(traversal.getTargetProperties().size());
									for (const auto &[key, expected]: traversal.getTargetProperties()) {
										checkGuardNow(state);
										if (key.empty()) throw std::invalid_argument("logical-plan target Property predicate has an empty key");
										orderedTargetPredicates.emplace_back(key, &expected);
									}
									std::ranges::sort(orderedTargetPredicates, {}, [](const auto &predicate) { return predicate.first; });
									std::pmr::vector<std::string_view> targetKeyViews{&pageTargetMemory};
									targetKeyViews.reserve(orderedTargetPredicates.size());
									for (const auto &[key, expected] : orderedTargetPredicates) targetKeyViews.push_back(key);
									MaterializedByteReservation targetPropertyBytes(&state.shared.materialized_budget);
									size_t targetKeyBytes = 0U;
									for (const std::string_view key: targetKeyViews) {
										targetKeyBytes = checkedSizeAdd(
												targetKeyBytes, key.size(),
												"logical-plan paged target Property key size overflowed");
									}
									const size_t targetFrames = checkedSizeAdd(
											checkedSizeMul(targetKeyViews.size(), sizeof(std::vector<PropertyValue>),
														   "logical-plan paged target Property frame size overflowed"),
											checkedSizeMul(
													checkedSizeMul(
															targetKeyViews.size(), targetIds.size(),
															"logical-plan paged target Property frame size overflowed"),
													sizeof(PropertyValue),
													"logical-plan paged target Property frame size overflowed"),
											"logical-plan paged target Property frame size overflowed");
									const size_t targetFixedBytes = targetFrames;
									targetPropertyBytes.set(
											checkedSizeAdd(targetFixedBytes, targetKeyBytes,
														   "logical-plan paged target Property batch size overflowed"));
									const std::function<void(size_t)> targetAdmission = [&](size_t bytes) {
										checkGuard(state);
										const size_t next = checkedSizeAdd(
												targetPropertyBytes.bytes(), bytes,
												"logical-plan paged target Property batch size overflowed");
										if (targetPropertyBytes.wouldExceed(next)) {
											throw std::runtime_error(
													"logical-plan paged target Property batch exceeded its byte bound");
										}
										targetPropertyBytes.set(next);
									};
									const auto targetColumns =
											targetKeyViews.empty() ? std::vector<std::vector<PropertyValue>>{}
											: state.shared.snapshot.supportsPropertyReadAdmission()
													? state.shared.snapshot.readNodePropertyColumnsSortedWithAdmission(
															  targetIds, targetKeyViews, targetAdmission)
													: state.shared.snapshot.readNodePropertyColumnsSorted(targetIds,
																								   targetKeyViews);
									const size_t targetPayloadBytes = propertyColumnBytes(
											targetColumns, targetKeyViews,
											"logical-plan paged target Property batch size overflowed", state.shared.context);
									const size_t retainedTargetBytes =
											checkedSizeAdd(targetFixedBytes - targetFrames, targetPayloadBytes,
														   "logical-plan paged target Property batch size overflowed");
									if (targetPropertyBytes.wouldExceed(retainedTargetBytes)) {
										throw std::runtime_error(
												"logical-plan paged target Property batch exceeded its byte bound");
									}
									targetPropertyBytes.set(retainedTargetBytes);
									if (targetColumns.size() != targetKeyViews.size() ||
										std::ranges::any_of(targetColumns, [&](const auto &column) {
											return column.size() != targetIds.size();
										})) {
										throw std::runtime_error(
												"logical graph backend changed target Property column cardinality");
									}
									for (size_t index = 0U; index < targetIds.size(); ++index) {
										bool matches = traversal.getTargetLabels().empty() || labelsMatch[index];
										for (size_t column = 0U; matches && column < targetKeyViews.size(); ++column) {
											checkGuardNow(state);
											matches = targetColumns[column][index] == *orderedTargetPredicates[column].second;
										}
										pageTargetMatches.emplace(targetIds[index], matches);
									}
								}
								const auto processEdge = [&](const storage::LogicalGraphEdge &edge) {
									checkGuard(state);
									validateEdge(edge);
									validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "Traversal");
									const bool endpointMatches =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.source_node_id == sourceNodeId
													: edge.target_node_id == sourceNodeId;
									if (!endpointMatches) {
										throw std::runtime_error(
												"logical graph backend returned an invalid adjacent Edge");
									}
									incrementBounded(sourceEdges,
													 "logical-plan Traversal per-Node Edge counter overflowed");
									incrementBounded(state.task.stats.edges_scanned,
													 "logical-plan read runtime Edge counter overflowed");
									if (direction == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										edge.source_node_id == sourceNodeId) {
										return true;
									}
									const uint64_t prefilterTargetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.target_node_id : edge.source_node_id;
									const auto targetMatch = pageTargetMatches.find(prefilterTargetId);
									if (targetMatch != pageTargetMatches.end() && !targetMatch->second) return true;
									if (!edgeMatches(state.shared.snapshot, edge, traversal.getEdgeType(),
													 traversal.getEdgeProperties(), state)) {
										return true;
									}
									const uint64_t targetId =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
									const auto existingTarget = row.values.find(traversal.getTargetVar());
									// If no target variable is requested, endpoint existence is sufficient;
									// avoid reading and decoding a Node that can never reach the result.
									const bool canDeferTarget = !targetNeedsRecord &&
																state.shared.allow_deferred_entity_refs &&
																state.shared.snapshot.supportsNodeExistenceProbes() &&
																(traversal.getTargetVar().empty() ||
																 (state.shared.snapshot.supportsNativeNodeRefScans() &&
																  existingTarget == row.values.end()));
									MaterializedByteReservation uncachedTargetBytes(&state.shared.materialized_budget);
									std::optional<storage::LogicalGraphNode> uncachedTarget;
									const storage::LogicalGraphNode *target = nullptr;
									if (canBatchTargetShape) {
										const auto targetMatch = pageTargetMatches.find(targetId);
										if (targetMatch == pageTargetMatches.end()) {
											throw std::runtime_error(
													"logical graph backend lost a batched target predicate result");
										}
										if (!targetMatch->second) {
											return true;
										}
										// If the target is not part of the output, selected columns are
										// sufficient; do not materialize a Node that cannot be observed.
										if (!traversal.getTargetVar().empty()) {
											target = &readNodeCachedBorrowed(state, targetId, uncachedTarget, uncachedTargetBytes);
											if (!nodeMatches(state.shared.snapshot, *target, traversal.getTargetLabels(),
															 traversal.getTargetProperties(), state)) {
												return true;
											}
										}
									} else if (targetNeedsRecord || !canDeferTarget) {
										target = &readNodeCachedBorrowed(state, targetId, uncachedTarget, uncachedTargetBytes);
										if (!nodeMatches(state.shared.snapshot, *target, traversal.getTargetLabels(),
														 traversal.getTargetProperties(), state)) {
											return true;
										}
									} else if (state.shared.snapshot.supportsNodeExistenceProbes()) {
										const auto pagePresence = pageTargetPresence.find(targetId);
										const bool present = pagePresence != pageTargetPresence.end()
																	 ? pagePresence->second
																	 : state.shared.snapshot.containsNode(targetId);
										if (!present) {
											throw std::runtime_error("logical graph backend returned an adjacent Edge "
																	 "with a missing endpoint");
										}
									}
									MaterializedByteReservation outputReservation(&state.shared.materialized_budget);
									constexpr const char *outputMessage = "logical-plan traversal output size overflowed";
									size_t outputBytes = logicalReadRowBytes(row, state.shared.context);
									const auto addBindingBytes = [&](const std::string &alias, size_t payloadBytes) {
										outputBytes = checkedSizeAdd(outputBytes, alias.size(), outputMessage);
										outputBytes = checkedSizeAdd(outputBytes, sizeof(LogicalPlanReadValue), outputMessage);
										outputBytes = checkedSizeAdd(outputBytes, payloadBytes, outputMessage);
									};
									if (!traversal.getEdgeVar().empty()) {
										addBindingBytes(traversal.getEdgeVar(), topologyOnly ? sizeof(storage::EntityRef) : logicalEdgeCacheBytes(edge, state.shared.context));
									}
									if (!traversal.getTargetVar().empty()) {
										addBindingBytes(traversal.getTargetVar(), canDeferTarget ? sizeof(storage::EntityRef) : logicalNodeCacheBytes(*target, state.shared.context));
									}
									outputReservation.set(outputBytes);
									LogicalPlanReadRow expanded = detail::copyLogicalReadRowGuarded(row, [&] { checkGuardNow(state); });
									if (!traversal.getEdgeVar().empty()) {
										LogicalPlanReadValue edgeBinding =
												topologyOnly
														? LogicalPlanReadValue(storage::EntityRef{edge.edge_id, true,
																								  edge.source_node_id,
																								  edge.target_node_id, 0U})
														: LogicalPlanReadValue(detail::copyLogicalEntityGuarded(edge, [&] { checkGuardNow(state); }));
										if (!bindValue(expanded, traversal.getEdgeVar(), std::move(edgeBinding))) {
											return true;
									}
									}
									if (!traversal.getTargetVar().empty()) {
										LogicalPlanReadValue targetBinding =
												canDeferTarget ? LogicalPlanReadValue(storage::EntityRef{
																		 targetId, false, 0U, 0U, 0U})
															   : LogicalPlanReadValue(detail::copyLogicalEntityGuarded(*target, [&] { checkGuardNow(state); }));
										if (!bindValue(expanded, traversal.getTargetVar(), std::move(targetBinding))) {
											return true;
										}
									}
									outputReservation.set(logicalReadRowBytes(expanded, state.shared.context));
									if (chargedReceiver != nullptr) {
										return (*chargedReceiver)(std::move(expanded), std::move(outputReservation));
									}
									outputReservation.reset();
									if (!visitor(std::move(expanded))) {
										return false;
									}
									return true;
								};
								if (topologyOnly) {
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										const storage::LogicalGraphEdge edge{topology.edge_id, topology.source_node_id,
																			 topology.target_node_id, topology.type};
										if (!processEdge(edge)) {
											return false;
										}
									}
								} else if (prefilterWithTopology) {
									// Keep the original adjacency order while issuing one sorted,
									// complete read only for type-matching Edge IDs. Rejected
									// topology records still pass through processEdge so counters,
									// endpoint validation, duplicate handling, and early-stop
									// semantics remain identical to the complete-page path.
									detail::BudgetMemoryResource matchingIdMemory(state.shared.materialized_budget);
									std::pmr::vector<uint64_t> matchingIds{&matchingIdMemory};
									matchingIds.reserve(topologyPage->edges.size());
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										checkGuardNow(state);
										validateEdge(topology);
										const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? topology.target_node_id : topology.source_node_id;
										const auto presence = pageTargetPresence.find(targetId);
										if ((traversal.getEdgeType().empty() || topology.type == traversal.getEdgeType()) && presence != pageTargetPresence.end() && !presence->second) {
											throw std::runtime_error("logical graph backend returned an adjacent Edge with a missing endpoint");
										}
										const auto target = pageTargetMatches.find(targetId);
										if ((traversal.getEdgeType().empty() || topology.type == traversal.getEdgeType()) &&
												(target == pageTargetMatches.end() || target->second)) {
											matchingIds.push_back(topology.edge_id);
										}
									}
									std::ranges::sort(matchingIds);
									matchingIds.erase(std::unique(matchingIds.begin(), matchingIds.end()),
													  matchingIds.end());
									MaterializedByteReservation matchingEdgeBytes(&state.shared.materialized_budget);
									const std::vector<storage::LogicalGraphEdge> matchingEdges =
											matchingIds.empty() ? std::vector<storage::LogicalGraphEdge>{}
																: readEdgesShapedSortedBudgeted(
																		  state, matchingIds, completeEdgeReadShape(), &matchingEdgeBytes);
									retainShapedBatchBytes(matchingEdges, matchingEdgeBytes, state.shared.context);
									if (matchingEdges.size() != matchingIds.size()) {
										throw std::runtime_error(
												"logical graph backend changed typed Edge batch cardinality");
									}
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										const auto matching = std::ranges::lower_bound(matchingIds, topology.edge_id);
										if (matching == matchingIds.end() || *matching != topology.edge_id) {
											const storage::LogicalGraphEdge rejected{
													topology.edge_id, topology.source_node_id, topology.target_node_id,
													topology.type};
											if (!processEdge(rejected)) {
												return false;
											}
											continue;
										}
										const size_t matchingIndex =
												static_cast<size_t>(matching - matchingIds.begin());
										const storage::LogicalGraphEdge &edge = matchingEdges[matchingIndex];
										if (edge.edge_id != topology.edge_id ||
											edge.source_node_id != topology.source_node_id ||
											edge.target_node_id != topology.target_node_id ||
											edge.type != topology.type) {
											throw std::runtime_error("logical graph backend changed typed Edge "
																	 "topology during materialization");
										}
										if (!processEdge(edge)) {
											return false;
										}
									}
								} else {
									for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
										if (!processEdge(edge)) {
											return false;
										}
									}
								}
								if (!pageContinuation.has_value() ||
									sourceEdges == state.shared.bounds.maximum_edges_per_node) {
									return true;
								}
								if (pageContinuation->previous_edge_id == 0U || pageContinuation->next_edge_id == 0U ||
									pageContinuation->previous_edge_id == pageContinuation->next_edge_id) {
									throw std::runtime_error(
											"logical graph backend returned an invalid adjacency continuation");
								}
								continuation = pageContinuation;
							}
						};
						if (direction == "out" || direction == "both") {
							if (!visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING)) {
								return false;
							}
						}
						if (direction == "in" || direction == "both") {
							return visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
						}
						return true;
					});
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
