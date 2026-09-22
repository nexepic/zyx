/**
 * @file LogicalPlanReadRuntime.cpp
 * @brief Streaming logical-plan runtime over the format-independent graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/LogicalPlanReadRuntime.hpp"

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
#include <unordered_set>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include "graph/query/execution/BoundedBatch.hpp"
#include "graph/query/execution/CsvReader.hpp"
#include "graph/query/execution/SelectionVector.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
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
#include "graph/query/execution/Record.hpp"
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
#include "graph/query/QueryPlan.hpp"

namespace graph::query::execution {

	namespace {

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;
		using InternalVisitor = std::function<bool(LogicalPlanReadRow)>;
		// Property batches are bounded independently of the result bound.  The
		// value is a memory-safety policy, not a semantic limit; callers can still
		// lower it through the existing maximum_rows bound.
		constexpr size_t DEFAULT_PROPERTY_BATCH_ROWS = 512U;

		struct ExecutionState {
			storage::LogicalGraphReadSnapshot &snapshot;
			const LogicalPlanReadRuntimeBounds &bounds;
			LogicalPlanReadRuntimeStats stats;
			const QueryContext *context = nullptr;
			const LogicalPlanReadRow *injected_row = nullptr;
			// Public row callbacks and mutation planning retain fully materialized
			// entities. Only the result pipeline owns the deferred-row contract.
			bool allow_deferred_entity_refs = false;
			// Logical snapshots are immutable. A small per-query cache avoids repeated
			// point reads when a high-degree adjacency list shares targets. The byte
			// budget is independent of the entry bound: a single LargeValue-bearing
			// entity must not turn this convenience cache into an unbounded copy of
			// the Property store.
			std::unordered_map<uint64_t, storage::LogicalGraphNode> node_cache;
			// Keep the same bounded reuse for Edge references that cross a result
			// boundary more than once (for example, a repeated path or projection).
			std::unordered_map<uint64_t, storage::LogicalGraphEdge> edge_cache{};
			size_t materialized_cache_bytes = 0U;
		};

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

		[[nodiscard]] size_t distinctKeyStorageBytes(std::string_view key, const char *message) {
			// Charge hash-node metadata as well as the encoded key payload so many
			// tiny distinct keys cannot bypass the materialization budget.
			return checkedSizeAdd(key.size(), sizeof(std::string) + sizeof(void *), message);
		}

		[[nodiscard]] size_t logicalNodeCacheBytes(const storage::LogicalGraphNode &node) {
			size_t bytes = sizeof(node);
			bytes = checkedSizeAdd(bytes, checkedSizeMul(node.labels.size(), sizeof(std::string),
					"logical-plan Node cache size overflowed"),
					"logical-plan Node cache size overflowed");
			for (const std::string &label: node.labels) {
				bytes = checkedSizeAdd(bytes, label.size(), "logical-plan Node cache size overflowed");
			}
			bytes = checkedSizeAdd(bytes, checkedSizeMul(node.properties.size(), sizeof(storage::LogicalGraphPropertyEntry),
					"logical-plan Node cache size overflowed"),
					"logical-plan Node cache size overflowed");
			for (const storage::LogicalGraphPropertyEntry &entry: node.properties) {
				bytes = checkedSizeAdd(bytes, entry.key.size(), "logical-plan Node cache size overflowed");
				bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value),
						"logical-plan Node cache size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalEdgeCacheBytes(const storage::LogicalGraphEdge &edge) {
			size_t bytes = sizeof(edge);
			bytes = checkedSizeAdd(bytes, edge.type.size(), "logical-plan Edge cache size overflowed");
			bytes = checkedSizeAdd(bytes, checkedSizeMul(edge.properties.size(), sizeof(storage::LogicalGraphPropertyEntry),
					"logical-plan Edge cache size overflowed"),
					"logical-plan Edge cache size overflowed");
			for (const storage::LogicalGraphPropertyEntry &entry: edge.properties) {
				bytes = checkedSizeAdd(bytes, entry.key.size(), "logical-plan Edge cache size overflowed");
				bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value),
						"logical-plan Edge cache size overflowed");
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalReadValueBytes(const LogicalPlanReadValue &value) {
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return property_utils::getPropertyValueSize(*property);
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return logicalNodeCacheBytes(*node);
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return logicalEdgeCacheBytes(*edge);
			}
			return sizeof(storage::EntityRef);
		}

		[[nodiscard]] size_t logicalReadRowBytes(const LogicalPlanReadRow &row) {
			size_t bytes = sizeof(row);
			for (const auto &[name, value]: row.values) {
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row materialization size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(value), "logical-plan row materialization size overflowed");
				bytes = checkedSizeAdd(bytes, logicalReadValueBytes(value),
						"logical-plan row materialization size overflowed");
			}
			for (const auto &[name, properties]: row.property_cache) {
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row materialization size overflowed");
				for (const auto &[key, value]: properties) {
					bytes = checkedSizeAdd(bytes, key.size(), "logical-plan row materialization size overflowed");
					bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value),
							"logical-plan row materialization size overflowed");
				}
			}
			for (const auto &[name, metadata]: row.entity_metadata_cache) {
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan row metadata size overflowed");
				for (const auto &[key, value]: metadata) {
					bytes = checkedSizeAdd(bytes, key.size(), "logical-plan row metadata size overflowed");
					bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value),
							"logical-plan row metadata size overflowed");
				}
			}
			return bytes;
		}

		void enforceMaterializedRowBound(const LogicalPlanReadRow &row, const ExecutionState &state) {
			if (logicalReadRowBytes(row) > state.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan row exceeded its materialized byte bound");
			}
		}

		[[nodiscard]] size_t propertyMapBytes(const PropertyMap &properties, const char *message) {
			size_t bytes = sizeof(properties);
			for (const auto &[key, value]: properties) {
				bytes = checkedSizeAdd(bytes, key.size(), message);
				bytes = checkedSizeAdd(bytes, sizeof(PropertyValue), message);
				bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value), message);
			}
			return bytes;
		}

		[[nodiscard]] size_t resultValueBytes(const ResultValue &value) {
			constexpr const char *message = "logical-plan result materialization size overflowed";
			if (value.isPrimitive()) {
				return property_utils::getPropertyValueSize(value.asPrimitive());
			}
			if (value.isLogicalNode()) {
				const LogicalNodeResult &node = value.asLogicalNode();
				size_t bytes = sizeof(node);
				for (const std::string &label: node.labels) {
					bytes = checkedSizeAdd(bytes, label.size(), message);
				}
				return checkedSizeAdd(bytes, propertyMapBytes(node.properties, message), message);
			}
			if (value.isLogicalEdge()) {
				const LogicalEdgeResult &edge = value.asLogicalEdge();
				const size_t bytes = checkedSizeAdd(sizeof(edge), edge.type.size(), message);
				return checkedSizeAdd(bytes, propertyMapBytes(edge.properties, message), message);
			}
			throw std::logic_error("logical-plan runtime produced a physical Entity result");
		}

		void enforceMaterializedResultRowBound(const QueryResult::Row &row, const ExecutionState &state) {
			size_t bytes = sizeof(row);
			for (const auto &[name, value]: row) {
				bytes = checkedSizeAdd(bytes, name.size(), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, sizeof(value), "logical-plan result row size overflowed");
				bytes = checkedSizeAdd(bytes, resultValueBytes(value), "logical-plan result row size overflowed");
			}
			if (bytes > state.bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical-plan result row exceeded its materialized byte bound");
			}
		}

		[[nodiscard]] size_t propertyColumnBytes(
				const std::vector<std::vector<PropertyValue>> &columns,
				const std::vector<std::string_view> &keys, const char *message) {
			size_t bytes = 0U;
			for (size_t column = 0U; column < columns.size(); ++column) {
				if (column < keys.size()) {
					bytes = checkedSizeAdd(bytes, keys[column].size(), message);
				}
				for (const PropertyValue &value: columns[column]) {
					bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value), message);
				}
			}
			return bytes;
		}

		[[nodiscard]] size_t logicalIndexCandidateBatchBytes(const storage::IndexCandidateBatch &batch) {
			constexpr const char *message = "logical-plan index candidate batch size overflowed";
			size_t bytes = checkedSizeMul(batch.candidates.capacity(), sizeof(storage::EntityRef), message);
			for (const storage::IndexCandidateBatch::VerifiedPropertyColumn &column:
					batch.verified_property_columns) {
				bytes = checkedSizeAdd(bytes, column.key.capacity(), message);
				bytes = checkedSizeAdd(bytes,
						checkedSizeMul(column.values.capacity(), sizeof(PropertyValue), message), message);
				for (const PropertyValue &value: column.values) {
					bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(value), message);
				}
			}
			return bytes;
		}

		void checkGuard(const ExecutionState &state) {
			if (state.context != nullptr) {
				state.context->checkGuard();
			}
		}

		void validateNode(const storage::LogicalGraphNode &node);
		void validateEdge(const storage::LogicalGraphEdge &edge);

		void cacheNodeRecord(ExecutionState &state, const storage::LogicalGraphNode &node) {
			if (state.node_cache.contains(node.node_id)) {
				return;
			}
			const size_t bytes = logicalNodeCacheBytes(node);
			if (state.node_cache.size() < state.bounds.maximum_node_records &&
				bytes <= MAXIMUM_MATERIALIZED_CACHE_BYTES -
													std::min(state.materialized_cache_bytes, MAXIMUM_MATERIALIZED_CACHE_BYTES) &&
				state.node_cache.emplace(node.node_id, node).second) {
				state.materialized_cache_bytes += bytes;
			}
		}

		void cacheEdgeRecord(ExecutionState &state, const storage::LogicalGraphEdge &edge) {
			if (state.edge_cache.contains(edge.edge_id)) {
				return;
			}
			const size_t bytes = logicalEdgeCacheBytes(edge);
			if (state.edge_cache.size() < state.bounds.maximum_edge_records &&
				bytes <= MAXIMUM_MATERIALIZED_CACHE_BYTES -
													std::min(state.materialized_cache_bytes, MAXIMUM_MATERIALIZED_CACHE_BYTES) &&
				state.edge_cache.emplace(edge.edge_id, edge).second) {
				state.materialized_cache_bytes += bytes;
			}
		}

		[[nodiscard]] storage::LogicalGraphNode readNodeCached(ExecutionState &state, uint64_t nodeId) {
			if (const auto cached = state.node_cache.find(nodeId); cached != state.node_cache.end()) {
				return cached->second;
			}
			storage::LogicalGraphNode node = state.snapshot.readNodeShaped(nodeId, completeNodeReadShape());
			validateNode(node);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph shaped Node read changed logical identity");
			}
			cacheNodeRecord(state, node);
			return node;
		}

		[[nodiscard]] storage::LogicalGraphEdge readEdgeCached(ExecutionState &state, uint64_t edgeId) {
			if (const auto cached = state.edge_cache.find(edgeId); cached != state.edge_cache.end()) {
				return cached->second;
			}
			storage::LogicalGraphEdge edge = state.snapshot.readEdgeShaped(edgeId, completeEdgeReadShape());
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
				bounds.maximum_adjacency_page_records == 0U ||
				bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U || bounds.maximum_distinct_rows == 0U ||
				bounds.maximum_materialized_bytes == 0U ||
				bounds.maximum_candidate_bytes == 0U ||
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
			const std::filesystem::path root = std::filesystem::weakly_canonical(*context.csv_import_policy.import_root,
																											 error);
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
			const int64_t epochDays = checkedAdd(static_cast<int64_t>(monthAdjusted.epochDays), durationWholeDays(duration));
			if (epochDays < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
				epochDays > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
				throw std::overflow_error("logical-plan temporal date overflowed");
			}
			return PropertyValue(TemporalDate{static_cast<int32_t>(epochDays)});
		}

		[[nodiscard]] PropertyValue addDurationToDateTime(const TemporalDateTime &dateTime,
																	 const TemporalDuration &duration) {
			const auto [year, month] = addMonthsToYearMonth(dateTime.year(), dateTime.month(), duration.months);
			TemporalDateTime result = TemporalDateTime::fromComponents(
				year, month, dateTime.day(), dateTime.hour(), dateTime.minute(), dateTime.second());
			constexpr int64_t MILLIS_PER_DAY = 86'400LL * 1'000LL;
			const int64_t dayMillis = checkedMultiply(duration.days, MILLIS_PER_DAY);
			const int64_t nanosMillis = duration.nanos / 1'000'000LL;
			result.epochMillis = checkedAdd(result.epochMillis, checkedAdd(dayMillis, nanosMillis));
			return PropertyValue(result);
		}

		[[nodiscard]] PropertyValue temporalComponent(const PropertyValue &value, std::string_view component) {
			if (value.getType() == PropertyType::DATE) {
				const TemporalDate &date = std::get<TemporalDate>(value.getVariant());
				if (component == "year") return PropertyValue(static_cast<int64_t>(date.year()));
				if (component == "month") return PropertyValue(static_cast<int64_t>(date.month()));
				if (component == "day") return PropertyValue(static_cast<int64_t>(date.day()));
				if (component == "epochDays") return PropertyValue(static_cast<int64_t>(date.epochDays));
			}
			if (value.getType() == PropertyType::DATETIME) {
				const TemporalDateTime &dateTime = std::get<TemporalDateTime>(value.getVariant());
				if (component == "year") return PropertyValue(static_cast<int64_t>(dateTime.year()));
				if (component == "month") return PropertyValue(static_cast<int64_t>(dateTime.month()));
				if (component == "day") return PropertyValue(static_cast<int64_t>(dateTime.day()));
				if (component == "hour") return PropertyValue(static_cast<int64_t>(dateTime.hour()));
				if (component == "minute") return PropertyValue(static_cast<int64_t>(dateTime.minute()));
				if (component == "second") return PropertyValue(static_cast<int64_t>(dateTime.second()));
				if (component == "epochMillis") return PropertyValue(dateTime.epochMillis);
			}
			if (value.getType() == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(value.getVariant());
				if (component == "months") return PropertyValue(duration.months);
				if (component == "days") return PropertyValue(duration.days);
				if (component == "seconds") return PropertyValue(duration.nanos / 1'000'000'000LL);
				if (component == "nanoseconds") return PropertyValue(duration.nanos);
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

		void validateNode(const storage::LogicalGraphNode &node) {
			if (node.node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Node");
			}
			std::unordered_set<std::string_view> labels;
			labels.reserve(node.labels.size());
			for (const std::string &label: node.labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical graph backend returned an invalid Node label set");
				}
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

		void validateEdgeReferenceRecord(const storage::EntityRef &reference,
				const storage::LogicalGraphEdge &edge, ExecutionState &state, const char *context) {
			validateEdge(edge);
			if (edge.edge_id != reference.entity_id || edge.source_node_id != reference.source_node_id ||
				edge.target_node_id != reference.target_node_id) {
				throw std::runtime_error(std::string(context) + " topology disagrees with its materialized relationship");
			}
			if (reference.type_token != 0U && state.snapshot.supportsNativeRelationshipTypeTokens()) {
				const std::optional<uint64_t> resolved = state.snapshot.findRelationshipTypeToken(edge.type);
				if (!resolved.has_value() || *resolved != reference.type_token) {
					throw std::runtime_error(std::string(context) +
							" relationship type disagrees with its materialized relationship");
				}
			}
		}

		[[nodiscard]] std::vector<storage::LogicalGraphNode> materializeNodeReferences(
				std::span<const storage::EntityRef> references, ExecutionState &state) {
			std::vector<uint64_t> missingIds;
			missingIds.reserve(references.size());
			for (const storage::EntityRef &reference: references) {
				validateNodeRef(reference,
						"logical-plan materialization received an invalid or Edge reference where a Node was required");
				if (!state.node_cache.contains(reference.entity_id)) {
					missingIds.push_back(reference.entity_id);
				}
			}
			std::ranges::sort(missingIds);
			missingIds.erase(std::unique(missingIds.begin(), missingIds.end()), missingIds.end());
			if (missingIds.size() > state.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan Node materialization exceeded its record bound");
			}

			std::vector<storage::LogicalGraphNode> loaded;
			loaded.reserve(missingIds.size());
			for (size_t offset = 0U; offset < missingIds.size();) {
				checkGuard(state);
				const size_t end = std::min(missingIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
				const std::vector<storage::LogicalGraphNode> batch = state.snapshot.readNodesShapedSorted(
						std::span<const uint64_t>(missingIds).subspan(offset, end - offset), completeNodeReadShape());
				if (batch.size() != end - offset) {
					throw std::runtime_error("logical-plan shaped Node batch changed result cardinality");
				}
				for (size_t index = 0U; index < batch.size(); ++index) {
					validateNode(batch[index]);
					if (batch[index].node_id != missingIds[offset + index]) {
						throw std::runtime_error("logical-plan shaped Node batch changed logical ID order");
					}
					cacheNodeRecord(state, batch[index]);
					loaded.push_back(batch[index]);
				}
				offset = end;
			}

			std::vector<storage::LogicalGraphNode> result;
			result.reserve(references.size());
			for (const storage::EntityRef &reference: references) {
				if (const auto cached = state.node_cache.find(reference.entity_id); cached != state.node_cache.end()) {
					validateNode(cached->second);
					result.push_back(cached->second);
					continue;
				}
				const auto position = std::ranges::lower_bound(
						loaded, reference.entity_id, {}, &storage::LogicalGraphNode::node_id);
				if (position == loaded.end() || position->node_id != reference.entity_id) {
					throw std::runtime_error("logical-plan shaped Node batch lost a requested Node");
				}
				result.push_back(*position);
			}
			return result;
		}

		[[nodiscard]] std::vector<storage::LogicalGraphEdge> materializeEdgeReferences(
				std::span<const storage::EntityRef> references, ExecutionState &state) {
			std::vector<uint64_t> missingIds;
			missingIds.reserve(references.size());
			for (const storage::EntityRef &reference: references) {
				validateEdgeRef(reference,
						"logical-plan materialization received an invalid or Node reference where an Edge was required");
				if (!state.edge_cache.contains(reference.entity_id)) {
					missingIds.push_back(reference.entity_id);
				}
			}
			std::ranges::sort(missingIds);
			missingIds.erase(std::unique(missingIds.begin(), missingIds.end()), missingIds.end());
			if (missingIds.size() > state.bounds.maximum_edge_records) {
				throw std::runtime_error("logical-plan Edge materialization exceeded its record bound");
			}

			std::vector<storage::LogicalGraphEdge> loaded;
			loaded.reserve(missingIds.size());
			for (size_t offset = 0U; offset < missingIds.size();) {
				checkGuard(state);
				const size_t end = std::min(missingIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
				const std::vector<storage::LogicalGraphEdge> batch = state.snapshot.readEdgesShapedSorted(
						std::span<const uint64_t>(missingIds).subspan(offset, end - offset), completeEdgeReadShape());
				if (batch.size() != end - offset) {
					throw std::runtime_error("logical-plan shaped Edge batch changed result cardinality");
				}
				for (size_t index = 0U; index < batch.size(); ++index) {
					validateEdge(batch[index]);
					if (batch[index].edge_id != missingIds[offset + index]) {
						throw std::runtime_error("logical-plan shaped Edge batch changed logical ID order");
					}
					cacheEdgeRecord(state, batch[index]);
					loaded.push_back(batch[index]);
				}
				offset = end;
			}

			std::vector<storage::LogicalGraphEdge> result;
			result.reserve(references.size());
			for (const storage::EntityRef &reference: references) {
				const storage::LogicalGraphEdge *edge = nullptr;
				if (const auto cached = state.edge_cache.find(reference.entity_id); cached != state.edge_cache.end()) {
					edge = &cached->second;
				} else {
					const auto position = std::ranges::lower_bound(
							loaded, reference.entity_id, {}, &storage::LogicalGraphEdge::edge_id);
					if (position == loaded.end() || position->edge_id != reference.entity_id) {
						throw std::runtime_error("logical-plan shaped Edge batch lost a requested relationship");
					}
					edge = &*position;
				}
				validateEdgeReferenceRecord(reference, *edge, state, "logical graph Entity reference");
				result.push_back(*edge);
			}
			return result;
		}

		[[nodiscard]] storage::LogicalGraphNode materializeNodeValue(ExecutionState &state,
				const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node);
				return *node;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateNodeRef(*reference,
						"logical-plan expected a Node reference but received an invalid or relationship reference");
				return readNodeCached(state, reference->entity_id);
			}
			throw std::runtime_error("logical-plan expected a Node variable");
		}

		[[nodiscard]] uint64_t nodeBindingId(const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node);
				return node->node_id;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateNodeRef(*reference,
						"logical-plan expected a Node reference but received an invalid or relationship reference");
				return reference->entity_id;
			}
			throw std::runtime_error("logical-plan expected a Node variable");
		}

		[[nodiscard]] storage::LogicalGraphEdge materializeEdgeValue(ExecutionState &state,
				const LogicalPlanReadValue &value) {
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				validateEdge(*edge);
				return *edge;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateEdgeRef(*reference,
						"logical-plan expected a relationship reference but received an invalid or Node reference");
				const storage::LogicalGraphEdge edge = readEdgeCached(state, reference->entity_id);
				validateEdgeReferenceRecord(*reference, edge, state, "logical graph Entity reference");
				return edge;
			}
			throw std::runtime_error("logical-plan expected a relationship variable");
		}

		[[nodiscard]] PropertyMap readInlineProperties(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
																						  ExecutionState &state) {
			if (entries.size() > state.bounds.maximum_property_entries) {
				throw std::runtime_error("logical graph backend returned too many direct Property entries");
			}
			incrementBounded(state.stats.property_sets_materialized,
							 "logical-plan Property materialization counter overflowed");
			PropertyMap properties;
			properties.reserve(entries.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				checkGuard(state);
				if (entry.key.empty() || !properties.emplace(entry.key, entry.value).second) {
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
			validateNode(node);
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

		[[nodiscard]] bool nodeScanMatches(storage::LogicalGraphReadSnapshot &snapshot,
										   const storage::LogicalGraphNode &node, const logical::LogicalNodeScan &scan,
										   const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
										   ExecutionState &state) {
			validateNode(node);
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
					throw std::invalid_argument("logical-plan Node scan has an empty or duplicate Property predicate key");
				}
			}
			std::vector<std::pair<std::string, PropertyValue>> result;
			result.reserve(scan.getParameterPropertyPredicates().size());
			for (const logical::ParameterPropertyPredicate &predicate: scan.getParameterPropertyPredicates()) {
				checkGuard(state);
				if (predicate.key.empty() || predicate.parameter_name.empty() || !keys.insert(predicate.key).second) {
					throw std::invalid_argument(
							"logical-plan Node scan has an empty, duplicate, or conflicting parameter Property predicate key");
				}
				if (state.context == nullptr) {
					throw std::runtime_error("logical-plan parameter equality predicate requires a QueryContext");
				}
				const auto value = state.context->parameters.find(predicate.parameter_name);
				if (value == state.context->parameters.end()) {
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

		[[nodiscard]] PropertyValue readProperty(storage::LogicalGraphReadSnapshot &snapshot, LogicalPlanReadRow &row,
												 const std::string &variable, const std::string &key,
												 ExecutionState &state) {
			if (key.empty()) {
				throw std::invalid_argument("logical-plan Property access has an empty key");
			}
			if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end()) {
				const auto found = cached->second.find(key);
				if (found != cached->second.end()) {
					return found->second;
				}
				if (row.complete_entity_property_cache.contains(variable)) {
					return PropertyValue();
				}
				// An EntityRef cache is a selected-field cache, not proof that the
				// requested field is absent. Continue to the token-aware native read
				// for a field not seen on this row yet.
				const auto binding = row.values.find(variable);
				if (binding == row.values.end() || !std::holds_alternative<storage::EntityRef>(binding->second)) {
					return PropertyValue();
				}
			}
			const LogicalPlanReadValue &binding = requireBinding(row, variable);
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() == PropertyType::DATE || value->getType() == PropertyType::DATETIME ||
					value->getType() == PropertyType::DURATION) {
					return temporalComponent(*value, key);
				}
				if (value->getType() != PropertyType::MAP) {
					return PropertyValue();
				}
				const auto [cached, inserted] = row.property_cache.emplace(variable, value->getMap());
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : found->second;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				auto [cached, inserted] =
						row.property_cache.emplace(variable, readNodeProperties(snapshot, *node, state));
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : found->second;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
				auto [cached, inserted] = row.property_cache.emplace(variable, readEdgeProperties(snapshot, *edge, state));
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : found->second;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(binding);
			validateEntityRef(reference);
			// Native snapshots resolve the property name to a token and decode only
			// the requested value. The virtual default retains complete-read
			// compatibility for non-native logical backends.
			const PropertyValue value = reference.is_edge ? snapshot.readEdgeProperty(reference.entity_id, key)
																	 : snapshot.readNodeProperty(reference.entity_id, key);
			row.property_cache[variable].insert_or_assign(key, value);
			return value;
		}

		[[nodiscard]] bool nodeRefScanMatches(storage::LogicalGraphReadSnapshot &snapshot, LogicalPlanReadRow &row,
																									 const std::string &variable,
																									 const logical::LogicalNodeScan &scan,
																									 const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
																									 ExecutionState &state,
																									 const bool labelsAlreadyMatched = false) {
			const storage::EntityRef &reference = std::get<storage::EntityRef>(requireBinding(row, variable));
			validateNodeRef(reference, "logical-plan Node predicate received an invalid or Edge reference");
			if (!labelsAlreadyMatched && !snapshot.nodeHasLabels(reference.entity_id, scan.getLabels())) {
				return false;
			}
			const auto equals = [&](const std::pair<std::string, PropertyValue> &predicate) {
				checkGuard(state);
				if (predicate.first.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				if (isNull(predicate.second)) {
					return false;
				}
				const PropertyValue actual = readProperty(snapshot, row, variable, predicate.first, state);
				return !isNull(actual) && actual == predicate.second;
			};
			for (const auto &predicate: scan.getPropertyPredicates()) {
				if (!equals(predicate)) {
					return false;
				}
			}
			for (const auto &predicate: parameterPredicates) {
				if (!equals(predicate)) {
					return false;
				}
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				checkGuard(state);
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan Node range predicate has an empty key");
				}
				const PropertyValue actual = readProperty(snapshot, row, variable, range.key, state);
				if (isNull(actual)) {
					return false;
				}
				if (!isNull(range.minValue) &&
					(range.minInclusive ? actual < range.minValue : actual <= range.minValue)) {
					return false;
				}
				if (!isNull(range.maxValue) &&
					(range.maxInclusive ? actual > range.maxValue : actual >= range.maxValue)) {
					return false;
				}
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					if (!equals({composite->keys[index], composite->values[index]})) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] const PropertyMap &readBindingProperties(storage::LogicalGraphReadSnapshot &snapshot,
																	 LogicalPlanReadRow &row,
																	 const std::string &variable,
																	 ExecutionState &state) {
			const LogicalPlanReadValue &binding = requireBinding(row, variable);
			const bool isEntityReference = std::holds_alternative<storage::EntityRef>(binding);
			if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end() &&
				(!isEntityReference || row.complete_entity_property_cache.contains(variable))) {
				return cached->second;
			}
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() != PropertyType::MAP) {
					throw std::runtime_error("logical-plan entity Property function requires a node, relationship, or Map");
				}
				return row.property_cache.emplace(variable, value->getMap()).first->second;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				return row.property_cache.emplace(variable, readNodeProperties(snapshot, *node, state)).first->second;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
				return row.property_cache.emplace(variable, readEdgeProperties(snapshot, *edge, state)).first->second;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(binding);
			validateEntityRef(reference);
			PropertyMap complete = reference.is_edge
											? readEdgeProperties(snapshot, materializeEdgeValue(state, binding), state)
											: readNodeProperties(snapshot, materializeNodeValue(state, binding), state);
			auto [cached, inserted] = row.property_cache.insert_or_assign(variable, std::move(complete));
			static_cast<void>(inserted);
			row.complete_entity_property_cache.insert(variable);
			return cached->second;
		}

		// List comprehensions, reducers, and quantifiers bind short-lived scalar
		// values. Restoring both the value and its Property cache prevents a nested
		// expression from corrupting an outer row binding with the same name.
		class ScopedRowBinding final {
		public:
			ScopedRowBinding(LogicalPlanReadRow &row, std::string variable, LogicalPlanReadValue value) :
					row_(row), variable_(std::move(variable)) {
				if (variable_.empty()) {
					throw std::invalid_argument("logical-plan temporary variable name cannot be empty");
				}
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
				row_.values.insert_or_assign(variable_, std::move(value));
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
			LogicalPlanReadRow &row_;
			std::string variable_;
			std::optional<LogicalPlanReadValue> previous_value_;
			std::optional<PropertyMap> previous_properties_;
			std::optional<bool> previous_properties_complete_;
			std::optional<std::unordered_map<std::string, PropertyValue>> previous_metadata_;
		};

		[[nodiscard]] const std::vector<PropertyValue> &requireBoundedList(const PropertyValue &value,
																				const ExecutionState &state,
																				const char *operation) {
			if (value.getType() != PropertyType::LIST) {
				throw std::runtime_error(std::string("logical-plan ") + operation + " requires a List value");
			}
			const std::vector<PropertyValue> &list = value.getList();
			if (list.size() > state.bounds.maximum_rows) {
				throw std::runtime_error(std::string("logical-plan ") + operation + " exceeded the configured List bound");
			}
			return list;
		}

		[[nodiscard]] PropertyValue evaluateScalarExpression(const expressions::Expression &expression,
													 LogicalPlanReadRow &row, ExecutionState &state);

		using PatternMatchVisitor =
				std::function<bool(const storage::LogicalGraphEdge &, const storage::LogicalGraphNode &)>;

		// Evaluate the common topology-only EXISTS shape without materializing an
		// Edge or target Node.  The native reference page is already authenticated
		// against the pinned snapshot; endpoint probes retain the fail-closed
		// invariant while keeping the hot path independent of Property pages.
		[[nodiscard]] std::optional<bool> findPatternReferenceMatch(
				const uint64_t sourceNodeId, const std::string &edgeType,
				expressions::PatternDirection direction, ExecutionState &state) {
			if (!state.snapshot.supportsNativeAdjacentEdgeRefScans() ||
					!state.snapshot.supportsNodeExistenceProbes() ||
					(!edgeType.empty() && !state.snapshot.supportsNativeRelationshipTypeTokens()) ||
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
				edgeTypeToken = state.snapshot.findRelationshipTypeToken(edgeType);
				if (!edgeTypeToken.has_value()) {
					return false;
				}
			}
			size_t sourceEdges = 0U;
			const auto scanDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				for (;;) {
					if (sourceEdges == state.bounds.maximum_edges_per_node) {
						throw std::runtime_error(
								"logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords = std::min(
							state.bounds.maximum_edges_per_node - sourceEdges,
							state.bounds.maximum_adjacency_page_records);
					const storage::LogicalGraphAdjacencyReferencePage page = state.snapshot.scanAdjacentEdgeRefs(
							sourceNodeId, adjacencyDirection, continuation, pageRecords);
					if (page.edges.size() > pageRecords ||
							(page.edges.empty() && page.continuation.has_value()) ||
							(continuation.has_value() &&
									(page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
						throw std::runtime_error(
								"logical graph backend returned an invalid pattern-expression reference page");
					}
					// Validate and filter the complete page before probing endpoints.  The
					// endpoint requests are then sorted/deduplicated for one bounded
					// Directory lookup, while candidates retain their original order.
						std::vector<const storage::EntityRef *> candidates;
						candidates.reserve(page.edges.size());
					for (const storage::EntityRef &reference: page.edges) {
						checkGuard(state);
						validateEdgeRef(reference,
								"logical graph backend returned an invalid pattern-expression reference");
						if (reference.entity_id <= previousEdgeId) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression reference");
						}
						previousEdgeId = reference.entity_id;
						const bool endpointMatches = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? reference.source_node_id == sourceNodeId
								: reference.target_node_id == sourceNodeId;
						if (!endpointMatches) {
							throw std::runtime_error(
									"logical graph backend returned an invalid pattern-expression endpoint");
						}
						incrementBounded(sourceEdges,
								"logical-plan pattern-expression per-Node Edge counter overflowed");
						incrementBounded(state.stats.edges_scanned,
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
					} else if (state.snapshot.supportsNodeExistenceProbes()) {
						std::vector<uint64_t> targetIds;
						targetIds.reserve(candidates.size());
						for (const storage::EntityRef *reference: candidates) {
							targetIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									? reference->target_node_id : reference->source_node_id);
						}
						std::ranges::sort(targetIds);
						targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
						const std::vector<bool> present = state.snapshot.containsNodesSorted(targetIds);
						if (present.size() != targetIds.size()) {
							throw std::runtime_error("logical graph backend changed pattern-expression endpoint cardinality");
						}
						for (const storage::EntityRef *reference: candidates) {
							const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									? reference->target_node_id : reference->source_node_id;
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
						const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? candidates.front()->target_node_id : candidates.front()->source_node_id;
						if (!state.snapshot.containsNode(targetId)) {
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
												 std::string_view targetLabel,
												 expressions::PatternDirection direction, ExecutionState &state,
												 const PatternMatchVisitor &visitor) {
			if (!visitor) {
				throw std::invalid_argument("logical-plan pattern expression requires a target visitor");
			}
			validateNode(source);
			if (direction != expressions::PatternDirection::PAT_OUTGOING &&
				direction != expressions::PatternDirection::PAT_INCOMING &&
				direction != expressions::PatternDirection::PAT_BOTH) {
				throw std::invalid_argument("logical-plan pattern expression has an unknown direction");
			}
			std::optional<uint64_t> edgeTypeToken;
			const bool canUseReferencePages = state.snapshot.supportsNativeAdjacentEdgeRefScans() &&
					(edgeType.empty() || state.snapshot.supportsNativeRelationshipTypeTokens());
			if (canUseReferencePages && !edgeType.empty()) {
				edgeTypeToken = state.snapshot.findRelationshipTypeToken(edgeType);
				if (!edgeTypeToken.has_value()) {
					return true;
				}
			}
			size_t sourceEdges = 0U;
			// The target label predicate is fixed for this pattern invocation. Keep a
			// bounded per-invocation cache so repeated targets in a supernode do not
			// reopen the same Entity Map entry for the same logical predicate.
			std::unordered_map<uint64_t, bool> targetLabelMatches;
			if (!targetLabel.empty()) {
				targetLabelMatches.reserve(std::min(state.bounds.maximum_rows, size_t{64U}));
			}
			const std::vector<std::string> targetLabelFilter =
					targetLabel.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::string(targetLabel)};
			const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				if (canUseReferencePages) {
					for (;;) {
						if (sourceEdges == state.bounds.maximum_edges_per_node) {
							throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
						}
						const size_t pageRecords = std::min(state.bounds.maximum_edges_per_node - sourceEdges,
																			 state.bounds.maximum_adjacency_page_records);
						const storage::LogicalGraphAdjacencyReferencePage page = state.snapshot.scanAdjacentEdgeRefs(
								 source.node_id, adjacencyDirection, continuation, pageRecords);
						if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
							throw std::runtime_error("logical graph backend returned an invalid pattern-expression adjacency reference page");
						}
						if (continuation.has_value() &&
								(page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id)) {
							throw std::runtime_error("logical graph backend returned a discontinuous pattern-expression adjacency reference page");
						}
						// Validate and filter the whole bounded reference page first.  A
						// single sorted label-column lookup then serves all distinct target
						// Nodes on the page instead of reopening the Entity Map per Edge.
						std::vector<const storage::EntityRef *> candidates;
						candidates.reserve(page.edges.size());
						for (const storage::EntityRef &reference: page.edges) {
							checkGuard(state);
							validateEdgeRef(reference,
									"logical graph backend returned an invalid pattern-expression adjacency reference");
							if (reference.entity_id <= previousEdgeId) {
								throw std::runtime_error("logical graph backend returned an invalid pattern-expression adjacency reference");
							}
							previousEdgeId = reference.entity_id;
							const bool endpointMatches = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																			 ? reference.source_node_id == source.node_id
																			 : reference.target_node_id == source.node_id;
							if (!endpointMatches) {
								throw std::runtime_error("logical graph backend returned an invalid pattern-expression Edge reference");
							}
							incrementBounded(sourceEdges,
												 "logical-plan pattern-expression per-Node Edge counter overflowed");
							incrementBounded(state.stats.edges_scanned,
											 "logical-plan pattern-expression Edge counter overflowed");
							if (direction == expressions::PatternDirection::PAT_BOTH &&
									adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
									reference.source_node_id == source.node_id) {
								continue;
							}
							if (edgeTypeToken.has_value() && reference.type_token != *edgeTypeToken) {
								continue;
							}
							const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																				? reference.target_node_id
																				: reference.source_node_id;
							static_cast<void>(targetId);
							candidates.push_back(&reference);
						}
						std::unordered_map<uint64_t, bool> pageLabelMatches;
						if (!targetLabel.empty() && !candidates.empty()) {
							std::vector<uint64_t> targetIds;
							targetIds.reserve(candidates.size());
							for (const storage::EntityRef *candidate: candidates) {
								const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? candidate->target_node_id : candidate->source_node_id;
								if (targetLabelMatches.find(targetId) == targetLabelMatches.end()) {
									targetIds.push_back(targetId);
								}
							}
							std::ranges::sort(targetIds);
							targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
							if (!targetIds.empty()) {
								const std::vector<bool> matches = state.snapshot.readNodeLabelMatchesSorted(targetIds, targetLabelFilter);
								if (matches.size() != targetIds.size()) {
									throw std::runtime_error("logical graph backend changed pattern-expression target label cardinality");
								}
								for (size_t index = 0U; index < targetIds.size(); ++index) {
									pageLabelMatches.emplace(targetIds[index], matches[index]);
									if (targetLabelMatches.size() < std::min(state.bounds.maximum_rows, size_t{64U})) {
										targetLabelMatches.emplace(targetIds[index], matches[index]);
									}
								}
							}
						}
						for (const storage::EntityRef *referencePointer: candidates) {
							const storage::EntityRef &reference = *referencePointer;
							const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																				? reference.target_node_id
																				: reference.source_node_id;
							if (!targetLabel.empty()) {
								const auto cached = targetLabelMatches.find(targetId);
								const auto pageCached = pageLabelMatches.find(targetId);
								const bool labelsMatch = cached != targetLabelMatches.end() ? cached->second
																				: (pageCached != pageLabelMatches.end() && pageCached->second);
								if (!labelsMatch) {
									continue;
								}
							}
							const storage::LogicalGraphEdge edge = readEdgeCached(state, reference.entity_id);
							if (edge.source_node_id != reference.source_node_id ||
									edge.target_node_id != reference.target_node_id ||
									(!edgeType.empty() && edge.type != edgeType)) {
								throw std::runtime_error("logical graph backend returned an adjacency reference with mismatched Edge data");
							}
							const storage::LogicalGraphNode target = readNodeCached(state, targetId);
							if (!visitor(edge, target)) {
								return false;
							}
						}
						if (!page.continuation.has_value()) {
							return true;
						}
						if (sourceEdges == state.bounds.maximum_edges_per_node || page.edges.empty() ||
								page.continuation->previous_edge_id != page.edges.back().entity_id ||
								page.continuation->next_edge_id == 0U ||
								page.continuation->previous_edge_id == page.continuation->next_edge_id) {
							throw std::runtime_error("logical graph backend returned an incomplete pattern-expression adjacency reference scan");
						}
						continuation = page.continuation;
					}
				}
				for (;;) {
					if (sourceEdges == state.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords = std::min(state.bounds.maximum_edges_per_node - sourceEdges,
														 state.bounds.maximum_adjacency_page_records);
					// The pattern visitor has no Edge Property predicate on this path.
					// Scan topology first, then materialize the selected full Edge batch
					// only for candidates that survive endpoint/type validation.
					const storage::LogicalGraphAdjacencyTopologyPage page = state.snapshot.scanAdjacentEdgeTopologies(
							source.node_id, adjacencyDirection, continuation, pageRecords);
					if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
						throw std::runtime_error("logical graph backend returned an invalid pattern-expression adjacency page");
					}
					if (continuation.has_value() &&
						(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
						throw std::runtime_error("logical graph backend returned a discontinuous pattern-expression adjacency page");
					}
					// A complete adjacency page still exposes endpoint IDs independently of
					// the target Node payload. Authenticate the distinct endpoint set once
					// before the per-match materialization loop so parallel edges do not
					// repeat an Entity Directory descent. Readers without the optional batch
					// probe retain the complete-read fallback below.
					std::vector<uint64_t> pageTargetIds;
					if (state.snapshot.supportsNodeExistenceProbes()) {
						pageTargetIds.reserve(page.edges.size());
						for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
							checkGuard(state);
							validateEdge(edge);
							const bool selfLoopDuplicate = direction == expressions::PatternDirection::PAT_BOTH &&
								adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								edge.source_node_id == source.node_id;
							if (!selfLoopDuplicate && (edgeType.empty() || edge.type == edgeType)) {
								pageTargetIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										? edge.target_node_id : edge.source_node_id);
							}
						}
						std::ranges::sort(pageTargetIds);
						pageTargetIds.erase(std::unique(pageTargetIds.begin(), pageTargetIds.end()), pageTargetIds.end());
					}
					const std::vector<bool> pageTargetPresent = state.snapshot.supportsNodeExistenceProbes()
							? state.snapshot.containsNodesSorted(pageTargetIds) : std::vector<bool>{};
					if (pageTargetPresent.size() != pageTargetIds.size()) {
						throw std::runtime_error("logical graph backend changed pattern-expression endpoint cardinality");
					}
					std::vector<uint64_t> pageEdgeIds;
					pageEdgeIds.reserve(page.edges.size());
					for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
						// Do not decode complete Edge payloads for candidates that are
						// guaranteed to be discarded by direction/type semantics.  The
						// topology page remains the validation source; only surviving IDs
						// enter the bounded shaped batch below.
						const bool selfLoopDuplicate = direction == expressions::PatternDirection::PAT_BOTH &&
							adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
							candidate.source_node_id == source.node_id;
						if (!selfLoopDuplicate && (edgeType.empty() || candidate.type == edgeType)) {
							pageEdgeIds.push_back(candidate.edge_id);
						}
					}
					std::ranges::sort(pageEdgeIds);
					pageEdgeIds.erase(std::unique(pageEdgeIds.begin(), pageEdgeIds.end()), pageEdgeIds.end());
					const std::vector<storage::LogicalGraphEdge> pageEdgeRecords =
							state.snapshot.readEdgesShapedSorted(pageEdgeIds, completeEdgeReadShape());
					if (pageEdgeRecords.size() != pageEdgeIds.size()) {
						throw std::runtime_error("logical graph backend changed pattern-expression Edge batch cardinality");
					}
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						checkGuard(state);
						validateEdge(edge);
						validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "pattern-expression");
						const bool endpointMatches = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														 ? edge.source_node_id == source.node_id
														 : edge.target_node_id == source.node_id;
						if (!endpointMatches) {
							throw std::runtime_error("logical graph backend returned an invalid pattern-expression Edge");
						}
						incrementBounded(sourceEdges,
										 "logical-plan pattern-expression per-Node Edge counter overflowed");
						incrementBounded(state.stats.edges_scanned,
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
						const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																	  ? edge.target_node_id
																	  : edge.source_node_id;
						if (!pageTargetIds.empty() && state.snapshot.supportsNodeExistenceProbes()) {
							const auto endpoint = std::ranges::lower_bound(pageTargetIds, targetId);
							if (endpoint == pageTargetIds.end() || *endpoint != targetId ||
								!pageTargetPresent[static_cast<size_t>(endpoint - pageTargetIds.begin())]) {
								throw std::runtime_error(
										"logical graph backend returned a pattern-expression Edge with a missing endpoint");
							}
						}
						const auto edgePosition = std::ranges::lower_bound(pageEdgeIds, edge.edge_id);
						if (edgePosition == pageEdgeIds.end() || *edgePosition != edge.edge_id) {
							throw std::runtime_error("logical graph backend lost a pattern-expression Edge batch entry");
						}
						const storage::LogicalGraphEdge &materializedEdge =
								pageEdgeRecords[static_cast<size_t>(edgePosition - pageEdgeIds.begin())];
						if (materializedEdge.edge_id != edge.edge_id || materializedEdge.source_node_id != edge.source_node_id ||
								materializedEdge.target_node_id != edge.target_node_id ||
								(!edgeType.empty() && materializedEdge.type != edgeType)) {
							throw std::runtime_error("logical graph backend returned an adjacency Edge with mismatched batch data");
						}
						const storage::LogicalGraphNode target = readNodeCached(state, targetId);
						if (!targetLabel.empty() &&
							std::ranges::find(target.labels, targetLabel) == target.labels.end()) {
							continue;
						}
						if (!visitor(materializedEdge, target)) {
							return false;
						}
					}
					if (!page.continuation.has_value()) {
						return true;
					}
					if (sourceEdges == state.bounds.maximum_edges_per_node ||
						page.continuation->previous_edge_id != page.edges.back().edge_id ||
						page.continuation->next_edge_id == 0U ||
						page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error("logical graph backend returned an incomplete pattern-expression adjacency scan");
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
						return literal.getPropertyValue();
					}
					return PropertyValue(literal.getStringValue());
				}
				case ExpressionType::VARIABLE_REFERENCE:
				case ExpressionType::PROPERTY_ACCESS: {
					const auto &reference = dynamic_cast<const expressions::VariableReferenceExpression &>(expression);
					if (reference.hasProperty()) {
						// A WITH/RETURN projection without AS keeps the expression text as
						// its column name (for example `WITH n.value`). Resolve that
						// projected column before interpreting the same text as a property
						// access on a variable that is no longer in scope.
						if (const auto projected = row.values.find(reference.toString()); projected != row.values.end()) {
							return projected->second;
						}
						return readProperty(state.snapshot, row, reference.getVariableName(),
											reference.getPropertyName(), state);
					}
					return requireBinding(row, reference.getVariableName());
				}
				case ExpressionType::PARAMETER: {
					if (state.context == nullptr) {
						throw std::runtime_error("logical-plan parameter evaluation requires a QueryContext");
					}
					const auto &parameter = dynamic_cast<const expressions::ParameterExpression &>(expression);
					const auto found = state.context->parameters.find(parameter.getParameterName());
					if (found == state.context->parameters.end()) {
						throw std::runtime_error("logical-plan read runtime could not resolve a query parameter");
					}
					return found->second;
				}
				case ExpressionType::EXPR_LIST_LITERAL: {
					const auto &list = dynamic_cast<const expressions::ListLiteralExpression &>(expression);
					if (list.isStatic()) {
						if (list.getValue().getList().size() > state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan List literal exceeded the configured List bound");
						}
						return list.getValue();
					}
					if (list.getElements().size() > state.bounds.maximum_rows) {
						throw std::runtime_error("logical-plan List literal exceeded the configured List bound");
					}
					std::vector<PropertyValue> values;
					values.reserve(list.getElements().size());
					for (const auto &element: list.getElements()) {
						values.push_back(scalarValue(evaluateExpression(*element, row, state)));
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
							checkedAdd(leftDuration.days, rightDuration.days), checkedAdd(leftDuration.nanos, rightDuration.nanos)});
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
				return PropertyValue(TemporalDuration{0LL, static_cast<int64_t>(leftDate.epochDays) - rightDate.epochDays, 0LL});
			}
			if (operation == expressions::BinaryOperatorType::BOP_SUBTRACT && leftType == PropertyType::DATETIME &&
				rightType == PropertyType::DATETIME) {
				constexpr int64_t MILLIS_PER_DAY = 86'400LL * 1'000LL;
				const int64_t milliseconds = checkedSubtract(std::get<TemporalDateTime>(left.getVariant()).epochMillis,
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
						return PropertyValue(std::regex_match(leftString,
																			   std::regex(rightString, std::regex_constants::ECMAScript)));
					} catch (const std::regex_error &) {
						throw std::runtime_error("logical-plan received an invalid regular expression");
					}
				default: // ZYX_COV_EXCL_LINE: callers dispatch only the three supported String operator enum values.
					throw std::logic_error("logical-plan runtime received an unsupported string operation");
			}
		}

		[[nodiscard]] std::string lowerCase(std::string_view value) {
			std::string result(value);
			std::ranges::transform(result, result.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
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
				requireScalarFunctionArgumentOneOf(
						arguments.front(), PropertyType::INTEGER, PropertyType::DOUBLE, functionName, 0U);
				return;
			}
			if (functionName == "pow" || functionName == "atan2") {
				for (size_t index = 0U; index < arguments.size(); ++index) {
					requireScalarFunctionArgumentOneOf(
							arguments[index], PropertyType::INTEGER, PropertyType::DOUBLE, functionName, index);
				}
				return;
			}
			const bool unaryString = functionName == "upper" || functionName == "lower" ||
									 functionName == "toupper" || functionName == "tolower" ||
									 functionName == "trim" || functionName == "ltrim" || functionName == "rtrim";
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
				requireScalarFunctionArgumentOneOf(
						arguments.front(), PropertyType::STRING, PropertyType::LIST, functionName, 0U);
				return;
			}
			if (functionName == "head" || functionName == "tail" || functionName == "last" ||
					functionName == "nodes" || functionName == "relationships") {
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
																					 LogicalPlanReadRow &row,
																					 ExecutionState &state) {
			const std::vector<std::unique_ptr<expressions::Expression>> &arguments = function.getArguments();
			if (arguments.size() != 1U || arguments.front() == nullptr) {
				throw std::runtime_error("logical-plan entity function requires exactly one variable argument");
			}
			const auto *reference = dynamic_cast<const expressions::VariableReferenceExpression *>(arguments.front().get());
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
						return cached->second;
					}
				}
				// A deferred Node reference needs only its labels for labels(). Keep the
				// read shape narrow so the Property root is not opened merely to answer
				// an introspection function. Complete Node values already carry labels.
				const storage::LogicalGraphNode node = [&]() {
					if (const auto *referenceValue = std::get_if<storage::EntityRef>(&binding)) {
						validateNodeRef(*referenceValue,
								"logical-plan labels() requires a valid node variable");
						return state.snapshot.readNodeShaped(referenceValue->entity_id, {.include_labels = true});
					}
					return materializeNodeValue(state, binding);
				}();
				if (node.labels.size() > state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan labels() exceeded the configured List bound");
				}
				std::vector<PropertyValue> labels;
				labels.reserve(node.labels.size());
				for (const std::string &label: node.labels) {
					labels.emplace_back(label);
				}
				PropertyValue result(std::move(labels));
				row.entity_metadata_cache[reference->getVariableName()].insert_or_assign(name, result);
				return result;
			}
			if (name == "type") {
				if (const auto metadata = row.entity_metadata_cache.find(reference->getVariableName());
					metadata != row.entity_metadata_cache.end()) {
					if (const auto cached = metadata->second.find(name); cached != metadata->second.end()) {
						return cached->second;
					}
				}
				// type() is topology metadata. Resolve just the relationship type for a
				// deferred Edge reference; full Edge properties are unnecessary here.
				if (const auto *referenceValue = std::get_if<storage::EntityRef>(&binding)) {
					validateEdgeRef(*referenceValue,
							"logical-plan type() requires a valid relationship variable");
					PropertyValue result(
							state.snapshot.readEdgeShaped(referenceValue->entity_id, {.include_type = true}).type);
					row.entity_metadata_cache[reference->getVariableName()].insert_or_assign(name, result);
					return result;
				}
				PropertyValue result(materializeEdgeValue(state, binding).type);
				row.entity_metadata_cache[reference->getVariableName()].insert_or_assign(name, result);
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
			const PropertyMap &properties = readBindingProperties(state.snapshot, row, reference->getVariableName(), state);
			if (name == "properties") {
				return PropertyValue(properties);
			}
			if (properties.size() > state.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan keys() exceeded the configured List bound");
			}
			std::vector<std::string> orderedKeys;
			orderedKeys.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				static_cast<void>(value);
				orderedKeys.push_back(key);
			}
			std::ranges::sort(orderedKeys);
			std::vector<PropertyValue> keys;
			keys.reserve(orderedKeys.size());
			for (const std::string &key: orderedKeys) {
				keys.emplace_back(key);
			}
			return PropertyValue(std::move(keys));
		}

		[[nodiscard]] PropertyValue evaluateRangeFunction(const std::vector<PropertyValue> &arguments,
																				  const ExecutionState &state) {
			if (arguments.size() < 2U || arguments.size() > 3U ||
				std::ranges::any_of(arguments, [](const PropertyValue &argument) { return isNull(argument); })) {
				return PropertyValue();
			}
			const auto toInteger = [](const PropertyValue &value) -> int64_t {
				if (const auto *integer = std::get_if<int64_t>(&value.getVariant())) {
					return *integer;
				}
				if (const auto *number = std::get_if<double>(&value.getVariant()); number != nullptr &&
					std::isfinite(*number) && *number >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
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
			std::vector<PropertyValue> result;
			result.reserve(std::min<size_t>(state.bounds.maximum_rows, 16U));
			int64_t current = start;
			while ((step > 0 && current <= end) || (step < 0 && current >= end)) {
				if (result.size() == state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan range() exceeded the configured List bound");
				}
				result.emplace_back(current);
				if ((step > 0 && current > std::numeric_limits<int64_t>::max() - step) ||
					(step < 0 && current < std::numeric_limits<int64_t>::min() - step)) {
					break;
				}
				current += step;
			}
			return PropertyValue(std::move(result));
		}

		[[nodiscard]] PropertyValue evaluateFunctionExpression(const expressions::FunctionCallExpression &function,
																	LogicalPlanReadRow &row,
																	ExecutionState &state) {
			const expressions::ScalarFunction *registered =
					expressions::FunctionRegistry::getInstance().lookupScalarFunction(function.getFunctionName());
			if (registered == nullptr) {
				throw std::runtime_error("logical-plan runtime could not resolve a scalar function");
			}
			if (!registered->validateArgCount(function.getArgumentCount())) {
				throw std::runtime_error("logical-plan scalar function has an invalid argument count");
			}
			if (function.getArgumentCount() > state.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan scalar function exceeded the configured argument bound");
			}
			if (dynamic_cast<const expressions::EntityIntrospectionFunction *>(registered) != nullptr) {
				return evaluateEntityFunction(function, row, state);
			}
			std::vector<PropertyValue> arguments;
			arguments.reserve(function.getArgumentCount());
			for (const std::unique_ptr<expressions::Expression> &argument: function.getArguments()) {
				if (argument == nullptr) {
					throw std::logic_error("logical-plan scalar function has a null argument expression");
				}
				PropertyValue value = scalarValue(evaluateExpression(*argument, row, state));
				if (value.getType() == PropertyType::LIST && value.getList().size() > state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan scalar function received a List exceeding the configured List bound");
				}
				arguments.push_back(std::move(value));
			}
			const std::string normalizedName = lowerCase(function.getFunctionName());
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
					state.context == nullptr ? emptyParameters : state.context->parameters;
			const expressions::EvaluationContext evaluationContext(
					emptyRecord, parameters, {.maximum_collection_elements = state.bounds.maximum_rows});
			PropertyValue result = registered->evaluate(arguments, evaluationContext);
			if (result.getType() == PropertyType::LIST && result.getList().size() > state.bounds.maximum_rows) {
				throw std::runtime_error("logical-plan scalar function returned a List exceeding the configured List bound");
			}
			return result;
		}

		[[nodiscard]] PropertyValue evaluateScalarExpression(const expressions::Expression &expression,
																		 LogicalPlanReadRow &row, ExecutionState &state) {
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
					const BinaryOperatorType operation = binary.getOperator();
					if (operation == BinaryOperatorType::BOP_AND || operation == BinaryOperatorType::BOP_OR) {
						const std::optional<bool> leftBoolean = booleanOrNull(left);
						if ((operation == BinaryOperatorType::BOP_AND && leftBoolean == false) ||
							(operation == BinaryOperatorType::BOP_OR && leftBoolean == true)) {
							return PropertyValue(*leftBoolean);
						}
					}
					const PropertyValue right = scalarValue(evaluateExpression(*binary.getRight(), row, state));
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
					if (in.hasDynamicList()) {
						const PropertyValue candidates =
								scalarValue(evaluateExpression(*in.getListExpression(), row, state));
						if (isNull(candidates)) {
							return PropertyValue();
						}
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
					return evaluateFunctionExpression(dynamic_cast<const expressions::FunctionCallExpression &>(expression), row,
																			 state);
				}
				case ExpressionType::CASE_EXPRESSION: {
					const auto &caseExpression = dynamic_cast<const expressions::CaseExpression &>(expression);
					if (caseExpression.isSimpleCase()) {
						const PropertyValue comparisonValue =
								scalarValue(evaluateExpression(*caseExpression.getComparisonExpression(), row, state));
						for (const expressions::CaseBranch &branch: caseExpression.getBranches()) {
							if (!branch.whenExpression || !branch.thenExpression) {
								throw std::logic_error("logical-plan CASE expression has an incomplete branch");
							}
							const PropertyValue whenValue = scalarValue(evaluateExpression(*branch.whenExpression, row, state));
							if (!isNull(whenValue) && comparisonValue == whenValue) {
								return scalarValue(evaluateExpression(*branch.thenExpression, row, state));
							}
						}
					} else {
						for (const expressions::CaseBranch &branch: caseExpression.getBranches()) {
							if (!branch.whenExpression || !branch.thenExpression) {
								throw std::logic_error("logical-plan CASE expression has an incomplete branch");
							}
							const std::optional<bool> predicate = booleanOrNull(
									scalarValue(evaluateExpression(*branch.whenExpression, row, state)));
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
					const PropertyValue listValue = scalarValue(evaluateExpression(*slice.getList(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "List slice");
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
						return index < 0 || index >= length ? PropertyValue() : list[static_cast<size_t>(index)];
					}
					int64_t start = slice.getStart() == nullptr ? 0 : normalize(evaluateIndex(slice.getStart(), "start"));
					int64_t end = slice.getEnd() == nullptr ? length : normalize(evaluateIndex(slice.getEnd(), "end"));
					start = std::clamp(start, int64_t{0}, length);
					end = std::clamp(end, int64_t{0}, length);
					std::vector<PropertyValue> result;
					result.reserve(static_cast<size_t>(std::max(int64_t{0}, end - start)));
					for (int64_t index = start; index < end; ++index) {
						result.push_back(list[static_cast<size_t>(index)]);
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::LIST_COMPREHENSION: {
					const auto &comprehension = dynamic_cast<const expressions::ListComprehensionExpression &>(expression);
					if (comprehension.getListExpr() == nullptr || comprehension.getVariable().empty()) {
						throw std::logic_error("logical-plan List comprehension has no source or variable");
					}
					const PropertyValue listValue =
							scalarValue(evaluateExpression(*comprehension.getListExpr(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "List comprehension");
					if (comprehension.getType() == expressions::ListComprehensionExpression::ComprehensionType::COMP_REDUCE) {
						throw std::logic_error("logical-plan List comprehension cannot use a reduce mode");
					}
					std::vector<PropertyValue> result;
					result.reserve(list.size());
					for (const PropertyValue &element: list) {
						ScopedRowBinding binding(row, comprehension.getVariable(), element);
						if (comprehension.getWhereExpr() != nullptr) {
							const PropertyValue predicate =
									scalarValue(evaluateExpression(*comprehension.getWhereExpr(), row, state));
							if (!isNull(predicate) && predicate.getType() != PropertyType::BOOLEAN) {
								throw std::runtime_error("logical-plan List comprehension predicate must be Boolean or NULL");
							}
							if (isNull(predicate) || !std::get<bool>(predicate.getVariant())) {
								continue;
							}
						}
						if (comprehension.getMapExpr() != nullptr) {
							result.push_back(scalarValue(evaluateExpression(*comprehension.getMapExpr(), row, state)));
						} else {
							result.push_back(element);
						}
					}
					return PropertyValue(std::move(result));
				}
				case ExpressionType::EXPR_REDUCE: {
					const auto &reduce = dynamic_cast<const expressions::ReduceExpression &>(expression);
					if (reduce.getInitialExpr() == nullptr || reduce.getListExpr() == nullptr || reduce.getBodyExpr() == nullptr ||
						reduce.getAccumulator().empty() || reduce.getVariable().empty()) {
						throw std::logic_error("logical-plan REDUCE expression is incomplete");
					}
					PropertyValue accumulator = scalarValue(evaluateExpression(*reduce.getInitialExpr(), row, state));
					const PropertyValue listValue = scalarValue(evaluateExpression(*reduce.getListExpr(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "REDUCE");
					for (const PropertyValue &element: list) {
						ScopedRowBinding accumulatorBinding(row, reduce.getAccumulator(), accumulator);
						ScopedRowBinding elementBinding(row, reduce.getVariable(), element);
						accumulator = scalarValue(evaluateExpression(*reduce.getBodyExpr(), row, state));
					}
					return accumulator;
				}
				case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
					const auto &quantifier = dynamic_cast<const expressions::QuantifierFunctionExpression &>(expression);
					if (quantifier.getListExpression() == nullptr || quantifier.getWhereExpression() == nullptr ||
						quantifier.getVariable().empty()) {
						throw std::logic_error("logical-plan quantifier expression is incomplete");
					}
					const PropertyValue listValue =
							scalarValue(evaluateExpression(*quantifier.getListExpression(), row, state));
					if (isNull(listValue)) {
						return PropertyValue();
					}
					const std::vector<PropertyValue> &list = requireBoundedList(listValue, state, "quantifier");
					const std::string function = lowerCase(quantifier.getFunctionName());
					if (function != "all" && function != "any" && function != "none" && function != "single") {
						throw std::runtime_error("logical-plan quantifier function is not supported");
					}
					size_t matches = 0U;
					bool sawNull = false;
					for (const PropertyValue &element: list) {
						ScopedRowBinding binding(row, quantifier.getVariable(), element);
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
					if (projection.getItems().size() > state.bounds.maximum_property_entries) {
						throw std::runtime_error("logical-plan Map projection exceeded the configured Property bound");
					}
					if (!projection.isMapLiteral()) {
						const LogicalPlanReadValue &binding = requireBinding(row, projection.getVariable());
						if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
							if (value->getType() == PropertyType::NULL_TYPE) {
								return PropertyValue();
							}
							if (value->getType() != PropertyType::MAP) {
								throw std::runtime_error(
											"logical-plan Map projection requires a node, relationship, map, or NULL variable");
							}
						}
					}
					PropertyValue::MapType result;
					for (const expressions::MapProjectionItem &item: projection.getItems()) {
							switch (item.type) {
							case expressions::MapProjectionItemType::MPROP_PROPERTY:
								if (projection.isMapLiteral()) {
									throw std::logic_error("logical-plan Map literal has a Property selector");
								}
								if (item.key.empty()) {
									throw std::logic_error("logical-plan Map projection has an empty Property key");
								}
								result.insert_or_assign(item.key,
															  readProperty(state.snapshot, row, projection.getVariable(), item.key, state));
								break;
							case expressions::MapProjectionItemType::MPROP_LITERAL:
								if (item.key.empty()) {
									throw std::logic_error("logical-plan Map projection has an empty literal key");
								}
								result.insert_or_assign(item.key, item.expression == nullptr
																			 ? PropertyValue()
																			 : scalarValue(evaluateExpression(*item.expression, row, state)));
								break;
							case expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES:
								if (projection.isMapLiteral()) {
									throw std::logic_error("logical-plan Map literal has an all-Properties selector");
								}
								for (const auto &[key, value]:
										 readBindingProperties(state.snapshot, row, projection.getVariable(), state)) {
									result.insert_or_assign(key, value);
								}
								break;
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
								nodeBindingId(source->second), exists.getRelType(), exists.getDirection(), state);
							referenceMatch.has_value()) {
							return PropertyValue(*referenceMatch);
						}
					}
					const storage::LogicalGraphNode sourceNode = materializeNodeValue(state, source->second);
					const auto matchesWhere = [&](const storage::LogicalGraphNode *target) {
						if (target != nullptr && !exists.getTargetVar().empty()) {
							if (const auto bound = row.values.find(exists.getTargetVar()); bound != row.values.end() &&
								bound->second != LogicalPlanReadValue(*target)) {
								return false;
							}
							ScopedRowBinding binding(row, exists.getTargetVar(), *target);
							return !exists.hasWhereClause() || booleanOrNull(
										scalarValue(evaluateExpression(*exists.getWhereExpression(), row, state))) == true;
						}
						return !exists.hasWhereClause() || booleanOrNull(
									scalarValue(evaluateExpression(*exists.getWhereExpression(), row, state))) == true;
					};
					if (!hasRelationship) {
						return PropertyValue(matchesWhere(nullptr));
					}
					bool matched = false;
					static_cast<void>(visitPatternTargets(sourceNode, exists.getRelType(), exists.getTargetLabel(),
							exists.getDirection(), state,
							[&](const storage::LogicalGraphEdge &, const storage::LogicalGraphNode &target) {
								if (!matchesWhere(&target)) {
									return true;
								}
								matched = true;
								return false;
							}));
					return PropertyValue(matched);
				}
				case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
					const auto &comprehension =
							dynamic_cast<const expressions::PatternComprehensionExpression &>(expression);
					if (comprehension.getSourceVar().empty() || comprehension.getMapExpression() == nullptr) {
						throw std::logic_error("logical-plan Pattern comprehension has an incomplete pattern or map expression");
					}
					const auto source = row.values.find(comprehension.getSourceVar());
					if (source == row.values.end()) {
						return PropertyValue(std::vector<PropertyValue>{});
					}
					const storage::LogicalGraphNode sourceNode = materializeNodeValue(state, source->second);
					std::vector<PropertyValue> values;
					size_t valuesBytes = 0U;
					const auto appendValue = [&](const storage::LogicalGraphEdge *edge,
												 const storage::LogicalGraphNode *target) {
						const auto appendBoundValue = [&]() {
							if (comprehension.hasWhereClause() && booleanOrNull(
									scalarValue(evaluateExpression(*comprehension.getWhereExpression(), row, state))) != true) {
								return true;
							}
							if (values.size() == state.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan Pattern comprehension exceeded its result bound");
							}
							PropertyValue mapped =
									scalarValue(evaluateExpression(*comprehension.getMapExpression(), row, state));
							const size_t mappedBytes = property_utils::getPropertyValueSize(mapped);
							if (mappedBytes > state.bounds.maximum_materialized_bytes ||
								valuesBytes > state.bounds.maximum_materialized_bytes - mappedBytes) {
								throw std::runtime_error("logical-plan Pattern comprehension exceeded its materialized byte bound");
							}
							valuesBytes += mappedBytes;
							values.push_back(std::move(mapped));
							return true;
						};
						if (target != nullptr && !comprehension.getTargetVar().empty()) {
							if (const auto bound = row.values.find(comprehension.getTargetVar()); bound != row.values.end() &&
								bound->second != LogicalPlanReadValue(*target)) {
								return true;
							}
							ScopedRowBinding targetBinding(row, comprehension.getTargetVar(), *target);
							if (edge != nullptr && !comprehension.getRelationshipVar().empty()) {
								ScopedRowBinding relationshipBinding(
										row, comprehension.getRelationshipVar(), *edge);
								return appendBoundValue();
							}
							return appendBoundValue();
						}
						if (edge != nullptr && !comprehension.getRelationshipVar().empty()) {
							ScopedRowBinding relationshipBinding(row, comprehension.getRelationshipVar(), *edge);
							return appendBoundValue();
						}
						return appendBoundValue();
					};
					const bool hasRelationship = comprehension.getPattern().find('-') != std::string::npos;
					const auto finishValues = [&]() {
						PropertyValue result(std::move(values));
						// Include the collection envelope and nested container metadata in
						// addition to the element payload charged above.
						if (property_utils::getPropertyValueSize(result) > state.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan Pattern comprehension exceeded its materialized byte bound");
						}
						return result;
					};
					if (!hasRelationship) {
						static_cast<void>(appendValue(nullptr, nullptr));
						return finishValues();
					}
					static_cast<void>(visitPatternTargets(sourceNode, comprehension.getRelType(),
							comprehension.getTargetLabel(), comprehension.getDirection(), state,
							[&](const storage::LogicalGraphEdge &edge, const storage::LogicalGraphNode &target) {
								return appendValue(&edge, &target);
							}));
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
					const uint64_t startNodeId = nodeBindingId(startBinding->second);
					const uint64_t endNodeId = nodeBindingId(endBinding->second);
					if (state.snapshot.supportsNodeExistenceProbes()) {
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
							const std::vector<bool> present = state.snapshot.containsNodesSorted(endpointIds);
							if (present.size() != endpointIds.size()) {
								throw std::runtime_error("logical graph shortestPath endpoint probe cardinality changed");
							}
							for (size_t index = 0U; index < endpointIds.size(); ++index) {
								if (!present[index]) {
									throw std::runtime_error("logical graph shortestPath endpoint Node does not exist");
								}
							}
						}
					} else {
						if (std::holds_alternative<storage::EntityRef>(startBinding->second)) {
							validateNode(readNodeCached(state, startNodeId));
						}
						if (std::holds_alternative<storage::EntityRef>(endBinding->second)) {
							validateNode(readNodeCached(state, endNodeId));
						}
					}
					int maximumHops = shortest.getMaxHops();
					if (state.context != nullptr && state.context->maxVarLengthDepth.has_value()) {
						if (*state.context->maxVarLengthDepth < 0) {
							throw std::invalid_argument("logical-plan shortestPath has a negative query depth bound");
						}
						maximumHops = std::min(maximumHops, *state.context->maxVarLengthDepth);
					}
					if (maximumHops < shortest.getMinHops()) {
						return PropertyValue();
					}
					if (static_cast<uint64_t>(maximumHops) > state.bounds.maximum_node_records) {
						throw std::runtime_error("logical-plan shortestPath exceeds its Node-depth bound");
					}

					struct Parent {
						uint64_t node_id = 0U;
						storage::LogicalGraphEdgeTopology edge;
					};
					std::unordered_map<uint64_t, int> distances;
					std::unordered_map<uint64_t, std::vector<Parent>> parents;
					std::deque<uint64_t> frontier;
					distances.emplace(startNodeId, 0);
					frontier.push_back(startNodeId);
					size_t parentEntries = 0U;
					std::optional<int> targetDistance = startNodeId == endNodeId ? std::optional<int>{0} : std::nullopt;
					size_t exploredEdges = 0U;
					const size_t maximumExploredEdges = [&]() {
						if (state.bounds.maximum_node_records >
							std::numeric_limits<size_t>::max() / state.bounds.maximum_edges_per_node) {
							throw std::overflow_error("logical-plan shortestPath Edge budget overflowed");
						}
						return state.bounds.maximum_node_records * state.bounds.maximum_edges_per_node;
					}();
					size_t shortestPathStateBytes = 0U;
					const auto chargeShortestPathState = [&](size_t bytes) {
						const size_t remaining = state.bounds.maximum_materialized_bytes -
								std::min(shortestPathStateBytes, state.bounds.maximum_materialized_bytes);
						if (bytes > remaining) {
							throw std::runtime_error("logical-plan shortestPath exceeded its materialized state byte bound");
						}
						shortestPathStateBytes = checkedSizeAdd(shortestPathStateBytes, bytes,
								"logical-plan shortestPath state size overflowed");
					};
					const bool supportsExistenceProbes = state.snapshot.supportsNodeExistenceProbes();
					std::optional<uint64_t> relationshipTypeToken;
					if (!shortest.getRelType().empty() && state.snapshot.supportsNativeRelationshipTypeTokens()) {
						relationshipTypeToken = state.snapshot.findRelationshipTypeToken(shortest.getRelType());
						if (!relationshipTypeToken.has_value()) {
							return PropertyValue();
						}
					}
					const bool useNativeAdjacencyRefs = state.snapshot.supportsNativeAdjacentEdgeRefScans() &&
							(shortest.getRelType().empty() || relationshipTypeToken.has_value());
					chargeShortestPathState(sizeof(uint64_t) + sizeof(int) + sizeof(uint64_t));

					const auto visitAdjacent = [&](uint64_t nodeId, const auto &onEdge) {
						size_t sourceEdges = 0U;
						const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
							std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
							uint64_t previousEdgeId = 0U;
							for (;;) {
								if (exploredEdges == maximumExploredEdges || sourceEdges == state.bounds.maximum_edges_per_node) {
									throw std::runtime_error("logical-plan shortestPath exceeded its total Edge bound");
								}
								const size_t perNodeRemaining = state.bounds.maximum_edges_per_node - sourceEdges;
								const size_t pageRecords = std::min(perNodeRemaining,
																 state.bounds.maximum_adjacency_page_records);
								std::vector<storage::LogicalGraphEdgeTopology> pageEdges;
								std::optional<storage::LogicalGraphAdjacencyContinuation> pageContinuation;
								if (useNativeAdjacencyRefs) {
									const storage::LogicalGraphAdjacencyReferencePage page = state.snapshot.scanAdjacentEdgeRefs(
											nodeId, adjacencyDirection, continuation, pageRecords);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error(
												"logical graph backend returned an invalid shortestPath adjacency reference page");
									}
									if (continuation.has_value() &&
										(page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id)) {
										throw std::runtime_error(
												"logical graph backend returned a discontinuous shortestPath adjacency reference page");
									}
									pageEdges.reserve(page.edges.size());
									for (const storage::EntityRef &reference: page.edges) {
										validateEdgeRef(reference,
												"logical graph backend returned an invalid or Node reference from a shortestPath adjacency page");
										const bool typeMatches = !relationshipTypeToken.has_value() ||
												reference.type_token == *relationshipTypeToken;
										pageEdges.push_back({reference.entity_id, reference.source_node_id, reference.target_node_id,
												typeMatches && relationshipTypeToken.has_value() ? shortest.getRelType() : std::string{}});
									}
									pageContinuation = page.continuation;
								} else {
									const storage::LogicalGraphAdjacencyTopologyPage page = state.snapshot.scanAdjacentEdgeTopologies(
											nodeId, adjacencyDirection, continuation, pageRecords);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid shortestPath adjacency page");
									}
									if (continuation.has_value() &&
										(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
										throw std::runtime_error("logical graph backend returned a discontinuous shortestPath adjacency page");
									}
									pageEdges = page.edges;
									pageContinuation = page.continuation;
								}
								if (pageEdges.size() > pageRecords || (pageEdges.empty() && pageContinuation.has_value())) {
									throw std::runtime_error("logical graph backend returned an invalid shortestPath adjacency page");
								}
								if (continuation.has_value() &&
									(pageEdges.empty() || pageEdges.front().edge_id != continuation->next_edge_id)) {
									throw std::runtime_error("logical graph backend returned a discontinuous shortestPath adjacency page");
								}
								// Endpoint validation is independent of edge traversal order. Resolve
								// each distinct endpoint once per page so parallel edges and repeated
								// targets do not repeat an Entity Directory descent.
								std::vector<uint64_t> endpointIds;
								if (supportsExistenceProbes) {
									endpointIds.reserve(pageEdges.size());
									for (const storage::LogicalGraphEdgeTopology &candidate: pageEdges) {
										endpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? candidate.target_node_id : candidate.source_node_id);
									}
									std::ranges::sort(endpointIds);
									endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
								}
								const std::vector<bool> endpointPresent = supportsExistenceProbes
										? state.snapshot.containsNodesSorted(endpointIds) : std::vector<bool>{};
								for (const storage::LogicalGraphEdgeTopology &edge: pageEdges) {
									checkGuard(state);
									validateEdge(edge);
									validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "shortestPath");
									const bool endpointMatches = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
															 ? edge.source_node_id == nodeId
															 : edge.target_node_id == nodeId;
									if (!endpointMatches) {
										throw std::runtime_error("logical graph backend returned an invalid shortestPath Edge");
									}
									incrementBounded(exploredEdges, "logical-plan shortestPath Edge counter overflowed");
									incrementBounded(sourceEdges,
													 "logical-plan shortestPath per-Node Edge counter overflowed");
									incrementBounded(state.stats.edges_scanned,
													 "logical-plan shortestPath runtime Edge counter overflowed");
									if (shortest.getDirection() == expressions::PatternDirection::PAT_BOTH &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										edge.source_node_id == nodeId) {
										continue;
									}
									if (!shortest.getRelType().empty() && edge.type != shortest.getRelType()) {
										continue;
									}
									const uint64_t next = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																	? edge.target_node_id
																	: edge.source_node_id;
									// A native probe authenticates the endpoint in the same pinned
								// snapshot without materializing labels or Properties for every
								// BFS frontier candidate. Readers without that stronger contract
								// retain the complete-record validation fallback.
								if (supportsExistenceProbes) {
									const auto endpoint = std::ranges::lower_bound(endpointIds, next);
									if (endpoint == endpointIds.end() || *endpoint != next ||
											!endpointPresent[static_cast<size_t>(endpoint - endpointIds.begin())]) {
											throw std::runtime_error(
													"logical graph backend returned a shortestPath Edge with a missing endpoint");
										}
									} else {
										static_cast<void>(readNodeCached(state, next));
									}
									if (!onEdge(edge, next)) {
										return false;
									}
								}
								if (!pageContinuation.has_value()) {
									return true;
								}
								if (sourceEdges == state.bounds.maximum_edges_per_node ||
									pageContinuation->previous_edge_id != pageEdges.back().edge_id ||
									pageContinuation->next_edge_id == 0U ||
									pageContinuation->previous_edge_id == pageContinuation->next_edge_id) {
									throw std::runtime_error("logical graph backend returned an invalid shortestPath continuation");
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
						static_cast<void>(visitAdjacent(current, [&](const storage::LogicalGraphEdgeTopology &edge, uint64_t next) {
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
								if (distances.size() == state.bounds.maximum_node_records) {
									throw std::runtime_error("logical-plan shortestPath exceeded its frontier Node bound");
								}
								distances.emplace(next, nextDistance);
								frontier.push_back(next);
								chargeShortestPathState(sizeof(uint64_t) + sizeof(int) + sizeof(void *) + sizeof(uint64_t));
								if (parentEntries == state.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								parents[next].push_back({current, edge});
								chargeShortestPathState(sizeof(Parent));
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							} else if (existing->second == nextDistance) {
								if (parentEntries == state.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								parents[next].push_back({current, edge});
								chargeShortestPathState(sizeof(Parent));
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

					const auto idValue = [](uint64_t id) {
						if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							throw std::overflow_error("logical-plan shortestPath cannot represent an entity ID in a Property value");
						}
						return PropertyValue(static_cast<int64_t>(id));
					};
					const auto materializePath = [&](const std::vector<uint64_t> &nodeIds,
															 const std::vector<storage::LogicalGraphEdgeTopology> &edges) {
						std::vector<PropertyValue> path;
						path.reserve(nodeIds.size() + edges.size());
						std::vector<uint64_t> orderedNodeIds = nodeIds;
						std::ranges::sort(orderedNodeIds);
						orderedNodeIds.erase(std::unique(orderedNodeIds.begin(), orderedNodeIds.end()), orderedNodeIds.end());
						std::unordered_map<uint64_t, storage::LogicalGraphNode> nodeRecords;
						nodeRecords.reserve(orderedNodeIds.size());
						for (size_t offset = 0U; offset < orderedNodeIds.size(); offset += DEFAULT_PROPERTY_BATCH_ROWS) {
							const size_t count = std::min(DEFAULT_PROPERTY_BATCH_ROWS, orderedNodeIds.size() - offset);
							const auto batch = state.snapshot.readNodesShapedSorted(
									std::span<const uint64_t>(orderedNodeIds).subspan(offset, count), completeNodeReadShape());
							if (batch.size() != count) {
								throw std::runtime_error("logical-plan shortestPath Node batch changed its cardinality");
							}
							for (size_t index = 0U; index < count; ++index) {
								validateNode(batch[index]);
								if (batch[index].node_id != orderedNodeIds[offset + index]) {
									throw std::runtime_error("logical-plan shortestPath Node batch changed logical ID order");
								}
								nodeRecords.emplace(batch[index].node_id, batch[index]);
							}
						}
						std::vector<uint64_t> orderedEdgeIds;
						orderedEdgeIds.reserve(edges.size());
						for (const auto &edge: edges) {
							orderedEdgeIds.push_back(edge.edge_id);
						}
						std::ranges::sort(orderedEdgeIds);
						orderedEdgeIds.erase(std::unique(orderedEdgeIds.begin(), orderedEdgeIds.end()), orderedEdgeIds.end());
						std::unordered_map<uint64_t, storage::LogicalGraphEdge> edgeRecords;
						edgeRecords.reserve(orderedEdgeIds.size());
						for (size_t offset = 0U; offset < orderedEdgeIds.size(); offset += DEFAULT_PROPERTY_BATCH_ROWS) {
							const size_t count = std::min(DEFAULT_PROPERTY_BATCH_ROWS, orderedEdgeIds.size() - offset);
							const auto batch = state.snapshot.readEdgesShapedSorted(
									std::span<const uint64_t>(orderedEdgeIds).subspan(offset, count), completeEdgeReadShape());
							if (batch.size() != count) {
								throw std::runtime_error("logical-plan shortestPath Edge batch changed its cardinality");
							}
							for (size_t index = 0U; index < count; ++index) {
								validateEdge(batch[index]);
								if (batch[index].edge_id != orderedEdgeIds[offset + index]) {
									throw std::runtime_error("logical-plan shortestPath Edge batch changed logical ID order");
								}
								edgeRecords.emplace(batch[index].edge_id, batch[index]);
							}
						}
						for (size_t index = 0U; index < nodeIds.size(); ++index) {
							const auto nodeIt = nodeRecords.find(nodeIds[index]);
							if (nodeIt == nodeRecords.end()) {
								throw std::runtime_error("logical-plan shortestPath Node batch lost a path Node");
							}
							const storage::LogicalGraphNode &node = nodeIt->second;
							validateNode(node);
							PropertyValue::MapType nodeMap;
							nodeMap.emplace("_type", PropertyValue("node"));
							nodeMap.emplace("_id", idValue(node.node_id));
							for (const auto &[key, value]: readNodeProperties(state.snapshot, node, state)) {
								nodeMap.insert_or_assign(key, value);
							}
							path.emplace_back(std::move(nodeMap));
							if (index == edges.size()) {
								continue;
							}
							const storage::LogicalGraphEdgeTopology &topology = edges[index];
							const auto edgeIt = edgeRecords.find(topology.edge_id);
							if (edgeIt == edgeRecords.end()) {
								throw std::runtime_error("logical-plan shortestPath Edge batch lost a path Edge");
							}
							const storage::LogicalGraphEdge &edge = edgeIt->second;
							validateEdge(edge);
							// A native untyped adjacency reference carries no relationship name.
							// Its empty topology type is therefore an unknown marker, not a claim
							// that the persisted Edge type is empty.
							if (edge.source_node_id != topology.source_node_id || edge.target_node_id != topology.target_node_id ||
								(!topology.type.empty() && edge.type != topology.type)) {
								throw std::runtime_error(
										"logical graph backend changed an Edge while materializing a pinned shortestPath result");
							}
							PropertyValue::MapType edgeMap;
							edgeMap.emplace("_type", PropertyValue("relationship"));
							edgeMap.emplace("_id", idValue(edge.edge_id));
							for (const auto &[key, value]: readEdgeProperties(state.snapshot, edge, state)) {
								edgeMap.insert_or_assign(key, value);
							}
							path.emplace_back(std::move(edgeMap));
						}
						return PropertyValue(std::move(path));
					};

					std::vector<PropertyValue> allPaths;
					size_t allPathBytes = 0U;
					std::vector<uint64_t> reversedNodes{endNodeId};
					std::vector<storage::LogicalGraphEdgeTopology> reversedEdges;
					std::function<void(uint64_t)> collectPaths = [&](uint64_t nodeId) {
						if (allPaths.size() == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan shortestPath exceeded its result-path bound");
						}
						if (nodeId == startNodeId) {
							std::vector<uint64_t> nodes(reversedNodes.rbegin(), reversedNodes.rend());
							std::vector<storage::LogicalGraphEdgeTopology> edges(reversedEdges.rbegin(), reversedEdges.rend());
							PropertyValue path = materializePath(nodes, edges);
							const size_t pathBytes = property_utils::getPropertyValueSize(path);
							if (pathBytes > state.bounds.maximum_materialized_bytes ||
								allPathBytes > state.bounds.maximum_materialized_bytes - pathBytes) {
								throw std::runtime_error("logical-plan shortestPath exceeded its materialized byte bound");
							}
							allPathBytes += pathBytes;
							allPaths.push_back(std::move(path));
							return;
						}
						const auto previous = parents.find(nodeId);
						if (previous == parents.end() || previous->second.empty()) {
							throw std::runtime_error("logical-plan shortestPath has an incomplete parent DAG");
						}
						for (const Parent &parent: previous->second) {
							reversedNodes.push_back(parent.node_id);
							reversedEdges.push_back(parent.edge);
							collectPaths(parent.node_id);
							reversedEdges.pop_back();
							reversedNodes.pop_back();
							if (!shortest.isAll() && !allPaths.empty()) {
								return;
							}
						}
					};
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

		[[nodiscard]] std::optional<std::pair<bool, uint64_t>> logicalEntityIdentity(
				const LogicalPlanReadValue &candidate) {
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


		// DISTINCT needs a structural key, not a display string: PropertyValue::toString()
		// intentionally permits human-readable ambiguity in nested lists and maps.
		void appendDistinctUint64(std::string &output, uint64_t value) {
			for (size_t byte = 0U; byte < sizeof(value); ++byte) {
				output.push_back(static_cast<char>(value & 0xFFU));
				value >>= 8U;
			}
		}

		void appendDistinctString(std::string &output, std::string_view value) {
			appendDistinctUint64(output, value.size());
			output.append(value);
		}

		void appendDistinctPropertyValue(std::string &output, const PropertyValue &value) {
			const auto appendType = [&output](PropertyType type) {
				output.push_back(static_cast<char>(static_cast<uint8_t>(type)));
			};
			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					appendType(PropertyType::NULL_TYPE);
					return;
				case PropertyType::BOOLEAN:
					appendType(PropertyType::BOOLEAN);
					output.push_back(std::get<bool>(value.getVariant()) ? '\x01' : '\x00');
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
					std::vector<const PropertyValue::MapType::value_type *> entries;
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
					for (const std::byte byte : bytes) {
						output.push_back(static_cast<char>(std::to_integer<uint8_t>(byte)));
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

		void appendDistinctValueKey(std::string &output, const LogicalPlanReadValue &value) {
			if (const PropertyValue *property = std::get_if<PropertyValue>(&value)) {
				output.push_back('P');
				appendDistinctPropertyValue(output, *property);
				return;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				output.push_back('N');
				appendDistinctUint64(output, node->node_id);
				return;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				output.push_back('E');
				appendDistinctUint64(output, edge->edge_id);
				return;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(value);
			validateEntityRef(reference);
			output.push_back(reference.is_edge ? 'E' : 'N');
			appendDistinctUint64(output, reference.entity_id);
		}

		[[nodiscard]] std::string distinctRowKey(const LogicalPlanReadRow &row,
												 const std::vector<logical::LogicalProjectItem> &items) {
			std::string result;
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

		[[nodiscard]] LogicalPlanReadRow normalizeOutputRow(const LogicalPlanReadRow &row,
														const std::vector<std::string> &columns) {
			LogicalPlanReadRow normalized;
			normalized.values.reserve(columns.size());
			normalized.property_cache.reserve(columns.size());
			normalized.path_cache.reserve(columns.size());
			for (const std::string &column: columns) {
				const auto value = row.values.find(column);
				if (value == row.values.end()) {
					throw std::runtime_error("logical-plan runtime child did not bind a declared output variable");
				}
				normalized.values.emplace(column, value->second);
				if (const auto properties = row.property_cache.find(column); properties != row.property_cache.end()) {
					normalized.property_cache.emplace(column, properties->second);
					if (row.complete_entity_property_cache.contains(column)) {
						normalized.complete_entity_property_cache.insert(column);
					}
				}
				if (const auto metadata = row.entity_metadata_cache.find(column); metadata != row.entity_metadata_cache.end()) {
					normalized.entity_metadata_cache.emplace(column, metadata->second);
				}
				if (const auto path = row.path_cache.find(column); path != row.path_cache.end()) {
					normalized.path_cache.emplace(column, path->second);
				}
			}
			return normalized;
		}

		[[nodiscard]] std::vector<std::string> visibleOutputColumns(const logical::LogicalOperator &root) {
			std::vector<std::string> columns = root.getOutputVariables();
			std::erase_if(columns, isAnonymousVariable);
			return columns;
		}

		[[nodiscard]] std::string distinctUnionRowKey(const LogicalPlanReadRow &row,
													  const std::vector<std::string> &columns) {
			std::string result;
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

		[[nodiscard]] std::string bindingKey(const LogicalPlanReadRow &row, const std::vector<std::string> &columns) {
			std::string result;
			for (const std::string &column: columns) {
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

		using MaterializedPropertyCache = std::unordered_map<std::string, PropertyMap>;
		using FinalRowVisitor = std::function<bool(LogicalPlanReadRow &, ExecutionState &)>;
		using BatchSelectionPredicate = std::function<bool(LogicalPlanReadRow &, ExecutionState &)>;

		template<typename Entity>
		[[nodiscard]] const PropertyMap &materializeEntityProperties(const LogicalPlanReadRow &row,
																		 const Entity &entity, ExecutionState &state,
																		 MaterializedPropertyCache &materialized) {
			static_assert(std::is_same_v<Entity, storage::LogicalGraphNode> ||
						  std::is_same_v<Entity, storage::LogicalGraphEdge>);
			const std::string cacheKey = [&entity]() {
				if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
					return std::string("N:") + std::to_string(entity.node_id);
				} else {
					return std::string("E:") + std::to_string(entity.edge_id);
				}
			}();
			if (const auto cached = materialized.find(cacheKey); cached != materialized.end()) {
				return cached->second;
			}
			const LogicalPlanReadValue value = entity;
			for (const auto &[alias, candidate]: row.values) {
				if (candidate != value) {
					continue;
				}
				if (const auto cached = row.property_cache.find(alias); cached != row.property_cache.end()) {
					return materialized.emplace(cacheKey, cached->second).first->second;
				}
			}
			if constexpr (std::is_same_v<Entity, storage::LogicalGraphNode>) {
				validateNode(entity);
				return materialized.emplace(cacheKey, readNodeProperties(state.snapshot, entity, state)).first->second;
			} else {
				validateEdge(entity);
				return materialized.emplace(cacheKey, readEdgeProperties(state.snapshot, entity, state)).first->second;
			}
		}

		[[nodiscard]] ResultValue materializeResultValue(const LogicalPlanReadRow &row,
																								 const LogicalPlanReadValue &value, ExecutionState &state,
																								 MaterializedPropertyCache &materialized) {
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return ResultValue(*property);
			}
			// A deferred reference needs a complete record for the public result's
			// identity/topology and its properties. Read it once, then pass that
			// record through the property materializer; doing the property lookup
			// first and resolving the reference again used to copy/read the same
			// cached entity twice for every result column.
		if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
			validateEntityRef(*reference);
			if (reference->is_edge) {
				const storage::LogicalGraphEdge edge = readEdgeCached(state, reference->entity_id);
				validateEdgeReferenceRecord(*reference, edge, state, "logical graph Entity reference");
				const PropertyMap &properties = materializeEntityProperties(row, edge, state, materialized);
				return ResultValue(
						LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type, properties});
			}
			const storage::LogicalGraphNode node = readNodeCached(state, reference->entity_id);
			const PropertyMap &properties = materializeEntityProperties(row, node, state, materialized);
			return ResultValue(LogicalNodeResult{node.node_id, node.labels, properties});
		}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				const PropertyMap &properties = materializeEntityProperties(row, *node, state, materialized);
				return ResultValue(LogicalNodeResult{node->node_id, node->labels, properties});
			}
			const storage::LogicalGraphEdge &edge = std::get<storage::LogicalGraphEdge>(value);
			const PropertyMap &properties = materializeEntityProperties(row, edge, state, materialized);
			return ResultValue(
					LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type, properties});
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

		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor);

		struct BatchableEntityPropertyPredicate {
			std::string variable;
			std::string key;
		};

		void appendBatchableEntityPropertyPredicate(
				std::vector<BatchableEntityPropertyPredicate> &result,
				BatchableEntityPropertyPredicate candidate) {
			const bool duplicate = std::ranges::any_of(result, [&](const auto &existing) {
				return existing.variable == candidate.variable && existing.key == candidate.key;
			});
			if (!duplicate) {
				result.push_back(std::move(candidate));
			}
		}

		[[nodiscard]] bool collectBatchableEntityPropertyPredicates(
				const expressions::Expression &expression,
				std::vector<BatchableEntityPropertyPredicate> &result) {
			using ExpressionType = expressions::ExpressionType;
			if (expression.getExpressionType() == ExpressionType::PROPERTY_ACCESS) {
				const auto &reference = static_cast<const expressions::VariableReferenceExpression &>(expression);
				if (!reference.hasProperty() || reference.getVariableName().empty() ||
					reference.getPropertyName().empty()) {
					return false;
				}
				appendBatchableEntityPropertyPredicate(
						result, {reference.getVariableName(), reference.getPropertyName()});
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::LITERAL ||
				expression.getExpressionType() == ExpressionType::PARAMETER) {
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::BINARY_OP) {
				const auto &binary = static_cast<const expressions::BinaryOpExpression &>(expression);
				return binary.getLeft() != nullptr && binary.getRight() != nullptr &&
					collectBatchableEntityPropertyPredicates(*binary.getLeft(), result) &&
					collectBatchableEntityPropertyPredicates(*binary.getRight(), result);
			}
			if (expression.getExpressionType() == ExpressionType::UNARY_OP) {
				const auto &unary = static_cast<const expressions::UnaryOpExpression &>(expression);
				return unary.getOperand() != nullptr &&
					collectBatchableEntityPropertyPredicates(*unary.getOperand(), result);
			}
			if (expression.getExpressionType() == ExpressionType::FUNCTION_CALL) {
				const auto &function = static_cast<const expressions::FunctionCallExpression &>(expression);
				for (const auto &argument: function.getArguments()) {
					if (argument == nullptr || !collectBatchableEntityPropertyPredicates(*argument, result)) {
						return false;
					}
				}
				return true;
			}
			if (expression.getExpressionType() == ExpressionType::IN_LIST) {
				const auto &in = static_cast<const expressions::InExpression &>(expression);
				if (in.getValue() == nullptr || !collectBatchableEntityPropertyPredicates(*in.getValue(), result)) {
					return false;
				}
				return !in.hasDynamicList() ||
					(in.getListExpression() != nullptr &&
					 collectBatchableEntityPropertyPredicates(*in.getListExpression(), result));
			}
			if (expression.getExpressionType() == ExpressionType::IS_NULL) {
				const auto &isNullExpression = static_cast<const expressions::IsNullExpression &>(expression);
				return isNullExpression.getExpression() != nullptr &&
					collectBatchableEntityPropertyPredicates(*isNullExpression.getExpression(), result);
			}
			const auto collectOptional = [&result](const expressions::Expression *nested) {
				return nested == nullptr || collectBatchableEntityPropertyPredicates(*nested, result);
			};
			if (expression.getExpressionType() == ExpressionType::CASE_EXPRESSION) {
				const auto &caseExpression = static_cast<const expressions::CaseExpression &>(expression);
				if (!collectOptional(caseExpression.getComparisonExpression())) {
					return false;
				}
				for (const auto &branch: caseExpression.getBranches()) {
					if (!collectOptional(branch.whenExpression.get()) ||
						!collectOptional(branch.thenExpression.get())) {
						return false;
					}
				}
				return collectOptional(caseExpression.getElseExpression());
			}
			if (expression.getExpressionType() == ExpressionType::LIST_SLICE) {
				const auto &slice = static_cast<const expressions::ListSliceExpression &>(expression);
				return collectOptional(slice.getList()) && collectOptional(slice.getStart()) &&
					collectOptional(slice.getEnd());
			}
			return false;
		}

		// Collect direct property accesses from side-effect-free expression shapes;
		// the original evaluator remains authoritative after the batch is filled.
		[[nodiscard]] std::optional<std::vector<BatchableEntityPropertyPredicate>>
		inspectBatchableEntityPropertyPredicates(const expressions::Expression &expression) {
			std::vector<BatchableEntityPropertyPredicate> result;
			if (!collectBatchableEntityPropertyPredicates(expression, result)) {
				return std::nullopt;
			}
			return result;
		}

		// Prefetch selected Properties for any storage-neutral row producer.  Keeping
		// this boundary independent of a particular logical operator lets filters,
		// projections, and aggregates share the same bounded page-grouped read path.
		[[nodiscard]] std::optional<bool> tryVisitBatchedPropertyExpressions(
				const logical::LogicalOperator &child,
				const std::vector<const expressions::Expression *> &expressions,
				ExecutionState &state, const InternalVisitor &visitor,
				const BatchSelectionPredicate &selectionPredicate = {}) {
			if (!state.allow_deferred_entity_refs ||
				(!state.snapshot.supportsNativeNodeRefScans() && !state.snapshot.supportsNativeEdgeRefScans())) {
				return std::nullopt;
			}
		std::vector<BatchableEntityPropertyPredicate> requests;
		for (const expressions::Expression *expression: expressions) {
			if (expression == nullptr) {
				return std::nullopt;
			}
			const auto nested = inspectBatchableEntityPropertyPredicates(*expression);
			if (!nested.has_value()) {
				return std::nullopt;
			}
			for (const auto &candidate: *nested) {
				if (!std::ranges::any_of(requests, [&](const auto &existing) {
						return existing.variable == candidate.variable && existing.key == candidate.key;
					})) {
					requests.push_back(candidate);
				}
			}
		}
		if (requests.empty()) {
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

		const size_t batchCapacity = std::min(DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_rows);
		if (batchCapacity == 0U) {
			return std::nullopt;
		}
		BoundedBatch<LogicalPlanReadRow> rows(
				batchCapacity, state.bounds.maximum_materialized_bytes,
				[](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row); });
		rows.reserve(batchCapacity);
		bool stoppedByVisitor = false;
		const auto flush = [&]() {
			if (rows.empty()) {
				return true;
			}
			for (const PropertyGroup &group: groups) {
				std::vector<uint64_t> nodeIds;
				std::vector<uint64_t> edgeIds;
				for (const LogicalPlanReadRow &row: rows.values()) {
					const auto found = row.values.find(group.variable);
					if (found == row.values.end()) {
						throw std::runtime_error("logical-plan Property batch lost its Entity binding");
					}
					if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
						validateEntityRef(*reference);
						const auto cached = row.property_cache.find(group.variable);
						const bool complete = cached != row.property_cache.end() && std::ranges::all_of(group.keys,
								[&](const std::string &key) { return cached->second.contains(key); });
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
				const auto nodeColumns = nodeIds.empty()
						? std::vector<std::vector<PropertyValue>>{}
						: state.snapshot.readNodePropertyColumnsSorted(nodeIds, keys);
				const auto edgeColumns = edgeIds.empty()
						? std::vector<std::vector<PropertyValue>>{}
						: state.snapshot.readEdgePropertyColumnsSorted(edgeIds, keys);
				const size_t nodeColumnBytes = propertyColumnBytes(
						nodeColumns, keys, "logical-plan Node Property batch size overflowed");
				const size_t edgeColumnBytes = propertyColumnBytes(
						edgeColumns, keys, "logical-plan Edge Property batch size overflowed");
				if (checkedSizeAdd(nodeColumnBytes, edgeColumnBytes,
							"logical-plan Property batch size overflowed") > state.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan Property batch exceeded its byte bound");
				}
				const auto fill = [&](const bool isEdge, const std::vector<uint64_t> &ids,
						const std::vector<std::vector<PropertyValue>> &columns) {
					if (ids.empty()) {
						return;
					}
					if (columns.size() != group.keys.size() || std::ranges::any_of(columns, [&](const auto &column) {
							return column.size() != ids.size();
						})) {
						throw std::runtime_error("logical graph backend changed Property batch cardinality");
					}
					for (LogicalPlanReadRow &row: rows.values()) {
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
						for (size_t column = 0U; column < group.keys.size(); ++column) {
							row.property_cache[group.variable][group.keys[column]] =
									columns[column][static_cast<size_t>(position - ids.begin())];
						}
					}
				};
				fill(false, nodeIds, nodeColumns);
				fill(true, edgeIds, edgeColumns);
			}
			// Selected columns are attached to the buffered rows during the flush;
			// recompute the byte budget after that enrichment so LargeValue payloads
			// are covered by the same bound as the input rows.
			rows.recalculateBytes();
			debug::PerfTrace::addValue("logical_plan.batch.flushes", 1);
			debug::PerfTrace::addValue("logical_plan.batch.rows", static_cast<int64_t>(rows.size()));
			debug::PerfTrace::addValue("logical_plan.batch.materialized_bytes", static_cast<int64_t>(rows.bytes()));
			SelectionVector selection(rows.size());
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
			bool keepGoing = true;
			static_cast<void>(selection.forEachSelectedUntil([&](const size_t index) {
				if (!keepGoing) {
					return false;
				}
				checkGuard(state);
				keepGoing = visitor(std::move(rows.values()[index]));
				return keepGoing;
			}));
			rows.clear();
			return keepGoing;
		};

		const bool childCompleted = visitOperator(child, state, [&](LogicalPlanReadRow row) {
			rows.append(std::move(row));
			if (rows.size() == batchCapacity) {
				const bool keepGoing = flush();
				if (!keepGoing) {
					stoppedByVisitor = true;
				}
				return keepGoing;
			}
			return true;
		});
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
		[[nodiscard]] std::optional<std::vector<std::pair<std::string, PropertyValue>>>
		inspectDirectEdgePropertyFilter(const expressions::Expression &expression, const std::string &edgeVariable,
										 ExecutionState &state) {
			if (edgeVariable.empty()) {
				return std::nullopt;
			}
			std::vector<std::pair<std::string, PropertyValue>> result;
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
					const auto *left = dynamic_cast<const expressions::VariableReferenceExpression *>(binary->getLeft());
					const auto *right = dynamic_cast<const expressions::VariableReferenceExpression *>(binary->getRight());
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
						literal->getExpressionType() != ExpressionType::LITERAL || property->getPropertyName().empty()) {
						return false;
					}
					LogicalPlanReadRow emptyRow;
					const PropertyValue value = scalarValue(evaluateExpression(*literal, emptyRow, state));
					if (isNull(value)) {
						return false;
					}
					const auto existing = std::ranges::find(result, property->getPropertyName(),
							&std::pair<std::string, PropertyValue>::first);
					if (existing != result.end()) {
						return existing->second == value;
					}
					result.emplace_back(property->getPropertyName(), value);
					return true;
				}
				return false;
			};
			if (!visit(visit, expression) || result.empty()) {
				return std::nullopt;
			}
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
				!state.snapshot.supportsNativeNodeRefScans()) {
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
				return true;
			}
			if (!parameterPredicates->empty()) {
				return std::nullopt;
			}
			if (count.argument != nullptr) {
				const auto *variable = dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
				if (variable == nullptr || variable->hasProperty() || variable->getVariableName() != scan.getVariable()) {
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
				incrementBounded(state.stats.nodes_scanned,
								 "logical-plan direct Node count counter overflowed");
				if (!labelsMatch) {
					return true;
				}
				if (inputRows == state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
				return true;
			};
			if (scan.getLabels().empty()) {
				state.snapshot.visitNodeRefs(state.bounds.maximum_node_records,
						[&visitReference](const storage::EntityRef &reference) { return visitReference(reference, true); });
			} else {
				state.snapshot.visitNodeRefsWithLabelMatches(state.bounds.maximum_node_records, scan.getLabels(),
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
		[[nodiscard]] std::optional<bool> tryVisitBatchedVariableLengthCount(
				const logical::LogicalAggregate &aggregate, ExecutionState &state, const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U ||
				!state.snapshot.supportsNativeBatchAdjacentEdgeRefs() ||
				!state.snapshot.supportsNodeExistenceProbes()) {
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
			if (state.context != nullptr && state.context->maxVarLengthDepth.has_value()) {
				if (*state.context->maxVarLengthDepth < 0) {
					throw std::invalid_argument(
							"logical-plan variable-length count has a negative query depth bound");
				}
				maximumHops = std::min(maximumHops, *state.context->maxVarLengthDepth);
			}
			if (static_cast<uint64_t>(maximumHops) > state.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan variable-length count exceeds its configured depth bound");
			}

			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty()) {
				if (!state.snapshot.supportsNativeRelationshipTypeTokens()) {
					return std::nullopt;
				}
				relationshipTypeToken = state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
			}

			storage::EntityReadShape targetShape;
			targetShape.include_labels = !traversal.getTargetLabels().empty();
			for (const auto &[key, expected]: traversal.getTargetProperties()) {
				static_cast<void>(expected);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan variable-length target predicate has an empty key");
				}
				const std::optional<uint32_t> token = state.snapshot.findPropertyToken(key);
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
				std::vector<uint64_t> visited_node_ids;
			};
			struct AdjacentRecord {
				uint64_t source_node_id = 0U;
				uint64_t target_node_id = 0U;
				uint64_t edge_id = 0U;

				bool operator==(const AdjacentRecord &) const = default;
			};
			const auto pathBytes = [](const FrontierPath &path) {
				return checkedSizeAdd(sizeof(FrontierPath),
						checkedSizeMul(path.visited_node_ids.capacity(), sizeof(uint64_t),
								"logical-plan variable-length frontier size overflowed"),
						"logical-plan variable-length frontier size overflowed");
			};
			const auto incrementResult = [&](uint64_t &resultCount) {
				if (resultCount == state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				if (resultCount == static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
					throw std::overflow_error("logical-plan variable-length count exceeds integer result range");
				}
				++resultCount;
			};

			debug::ScopedPerfTimer timer("logical_plan.batched_variable_length_count");
			std::vector<FrontierPath> frontier;
			frontier.reserve(std::min(state.bounds.maximum_node_records, size_t{4'096U}));
			size_t frontierBytes = 0U;
			uint64_t resultCount = 0U;
			static_cast<void>(visitOperator(sourceScan, state, [&](LogicalPlanReadRow row) {
				checkGuard(state);
				const LogicalPlanReadValue &binding = requireBinding(row, traversal.getSourceVar());
				const uint64_t sourceId = nodeBindingId(binding);
				if (sourceId == 0U || frontier.size() == state.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length count exceeded its source Node bound");
				}
				if (traversal.getMinHops() == 0 && maximumHops >= 0) {
					const storage::LogicalGraphNode source = materializeNodeValue(state, binding);
					if (nodeMatches(state.snapshot, source, traversal.getTargetLabels(),
							traversal.getTargetProperties(), state)) {
						incrementResult(resultCount);
					}
				}
				FrontierPath path;
				path.current_node_id = sourceId;
				path.visited_node_ids.reserve(1U);
				path.visited_node_ids.push_back(sourceId);
				frontierBytes = checkedSizeAdd(frontierBytes, pathBytes(path),
						"logical-plan variable-length frontier size overflowed");
				if (frontierBytes > state.bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical-plan variable-length count exceeded its materialized byte bound");
				}
				frontier.push_back(std::move(path));
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
				if (currentNodeIds.size() > state.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length frontier exceeded its Node bound");
				}

				std::vector<size_t> edgeCounts(currentNodeIds.size(), 0U);
				const size_t layerFixedBytes = checkedSizeAdd(
						checkedSizeMul(currentNodeIds.capacity(), sizeof(uint64_t),
								"logical-plan variable-length layer size overflowed"),
						checkedSizeMul(edgeCounts.capacity(), sizeof(size_t),
								"logical-plan variable-length layer size overflowed"),
						"logical-plan variable-length layer size overflowed");
				const size_t frontierAndLayerBytes = checkedSizeAdd(frontierBytes, layerFixedBytes,
						"logical-plan variable-length layer size overflowed");
				if (frontierAndLayerBytes >= state.bounds.maximum_materialized_bytes) {
					throw std::runtime_error(
							"logical-plan variable-length count exceeded its materialized byte bound");
				}
				const size_t maximumAdjacencyRecords =
						(state.bounds.maximum_materialized_bytes - frontierAndLayerBytes) / sizeof(AdjacentRecord);
				std::vector<AdjacentRecord> adjacency;
				const size_t estimatedAdjacency = std::min(state.bounds.maximum_edge_records,
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
										? maximumAdjacencyRecords : adjacency.capacity() * 2U);
						adjacency.reserve(std::min(maximumAdjacencyRecords, doubled));
					}
					adjacency.push_back(record);
				};
				const auto visitDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
					if (scannedEdges == state.bounds.maximum_edge_records) {
						throw std::runtime_error("logical-plan variable-length count exceeded its Edge bound");
					}
					const size_t remaining = state.bounds.maximum_edge_records - scannedEdges;
					const size_t edgeVisitBound = checkedSizeAdd(
							remaining, 1U, "logical-plan variable-length Edge bound overflowed");
					state.snapshot.visitAdjacentEdgeRefsForNodes(
							currentNodeIds, adjacencyDirection, edgeVisitBound,
							[&](const storage::EntityRef &reference) {
								checkGuard(state);
								validateEdgeRef(reference,
										"logical graph batch adjacency returned an invalid or non-Edge reference");
								if (scannedEdges == state.bounds.maximum_edge_records) {
									throw std::runtime_error(
											"logical-plan variable-length count exceeded its Edge bound");
								}
								++scannedEdges;
								incrementBounded(state.stats.edges_scanned,
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
								const bool duplicateSelfLoop = traversal.getDirection() == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										reference.source_node_id == reference.target_node_id;
								if (duplicateSelfLoop) {
									return true;
								}
								const size_t sourceOrdinal = static_cast<size_t>(source - currentNodeIds.begin());
								if (edgeCounts[sourceOrdinal] == state.bounds.maximum_edges_per_node) {
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
				const size_t retainedLayerBytes = checkedSizeAdd(frontierBytes, layerBytes,
						"logical-plan variable-length layer size overflowed");
				const size_t targetVectorBytes = checkedSizeMul(adjacency.size(), sizeof(uint64_t),
						"logical-plan variable-length target vector size overflowed");
				if (targetVectorBytes > state.bounds.maximum_materialized_bytes -
						std::min(retainedLayerBytes, state.bounds.maximum_materialized_bytes)) {
					throw std::runtime_error(
							"logical-plan variable-length count exceeded its materialized byte bound");
				}

				std::vector<uint64_t> targetNodeIds;
				targetNodeIds.reserve(adjacency.size());
				for (const AdjacentRecord &record: adjacency) {
					targetNodeIds.push_back(record.target_node_id);
				}
				std::ranges::sort(targetNodeIds);
				targetNodeIds.erase(std::unique(targetNodeIds.begin(), targetNodeIds.end()), targetNodeIds.end());
				if (targetNodeIds.size() > state.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan variable-length endpoints exceeded the Node bound");
				}
				std::vector<uint8_t> targetMatches(targetNodeIds.size(), uint8_t{0U});
				if (traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()) {
					const std::vector<bool> present = state.snapshot.containsNodesSorted(targetNodeIds);
					if (present.size() != targetNodeIds.size() ||
						std::ranges::any_of(present, [](const bool value) { return !value; })) {
						throw std::runtime_error(
								"logical graph batch adjacency returned an Edge with a missing endpoint");
					}
					std::fill(targetMatches.begin(), targetMatches.end(), uint8_t{1U});
				} else {
					const std::vector<storage::LogicalGraphNode> targets =
							state.snapshot.readNodesShapedSorted(targetNodeIds, targetShape);
					if (targets.size() != targetNodeIds.size()) {
						throw std::runtime_error(
								"logical graph backend changed variable-length target cardinality");
					}
					size_t targetBytes = 0U;
					for (size_t index = 0U; index < targets.size(); ++index) {
						if (targets[index].node_id != targetNodeIds[index]) {
							throw std::runtime_error(
									"logical graph backend changed variable-length target identity");
						}
						targetBytes = checkedSizeAdd(targetBytes, logicalNodeCacheBytes(targets[index]),
								"logical-plan variable-length target batch size overflowed");
						const size_t stagedBytes = checkedSizeAdd(retainedLayerBytes,
								checkedSizeAdd(targetVectorBytes, targetBytes,
										"logical-plan variable-length target batch size overflowed"),
								"logical-plan variable-length target batch size overflowed");
						if (stagedBytes > state.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan variable-length count exceeded its materialized byte bound");
						}
						targetMatches[index] = nodeMatches(state.snapshot, targets[index],
								traversal.getTargetLabels(), traversal.getTargetProperties(), state);
					}
				}

				std::vector<FrontierPath> nextFrontier;
				size_t nextFrontierBytes = 0U;
				const size_t retainedBytes = checkedSizeAdd(retainedLayerBytes,
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
						if (std::ranges::find(path.visited_node_ids, candidate->target_node_id) !=
							path.visited_node_ids.end()) {
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
						next.visited_node_ids.reserve(path.visited_node_ids.size() + 1U);
						next.visited_node_ids.insert(next.visited_node_ids.end(), path.visited_node_ids.begin(),
								path.visited_node_ids.end());
						next.visited_node_ids.push_back(candidate->target_node_id);
						nextFrontierBytes = checkedSizeAdd(nextFrontierBytes, pathBytes(next),
								"logical-plan variable-length frontier size overflowed");
						if (checkedSizeAdd(retainedBytes, nextFrontierBytes,
								"logical-plan variable-length frontier size overflowed") >
							state.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan variable-length count exceeded its materialized byte bound");
						}
						nextFrontier.push_back(std::move(next));
					}
				}
				frontier = std::move(nextFrontier);
				frontierBytes = nextFrontierBytes;
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(resultCount)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool> tryVisitAnchoredTwoHopRelationshipCount(
				const logical::LogicalAggregate &aggregate, ExecutionState &state, const InternalVisitor &visitor) {
			const auto &groupExpressions = aggregate.getGroupByExprs();
			const auto &aggregations = aggregate.getAggregations();
			if (!groupExpressions.empty() || aggregations.size() != 1U || !state.snapshot.supportsNativeEdgeRefScans() ||
				!state.snapshot.supportsNodeExistenceProbes()) {
				return std::nullopt;
			}
			const logical::LogicalAggItem &count = aggregations.front();
			if (count.functionName != "count" || count.distinct || count.alias.empty() || count.extraArg || !count.argument) {
				return std::nullopt;
			}
			const auto *countVariable = dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
		if (countVariable == nullptr || countVariable->hasProperty()) {
				return std::nullopt;
			}
		const std::vector<logical::LogicalOperator *> aggregateChildren = aggregate.getChildren();
		if (debug::PerfTrace::areValuesEnabled() && !aggregateChildren.empty() && aggregateChildren.front() != nullptr) {
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
		if (state.bounds.maximum_rows == std::numeric_limits<size_t>::max() ||
				state.bounds.maximum_edge_records == std::numeric_limits<size_t>::max()) {
			throw std::overflow_error("logical-plan two-hop relationship count bound overflowed");
		}
		std::optional<uint64_t> innerType;
		if (!inner.getEdgeType().empty()) {
			if (!state.snapshot.supportsNativeRelationshipTypeTokens()) {
				return std::nullopt;
			}
			innerType = state.snapshot.findRelationshipTypeToken(inner.getEdgeType());
			if (!innerType.has_value()) {
				LogicalPlanReadRow result;
				result.values.emplace(count.alias, PropertyValue(int64_t{0}));
				return visitor(std::move(result));
			}
		}
		std::optional<uint64_t> outerType;
		if (!outer.getEdgeType().empty()) {
			if (!state.snapshot.supportsNativeRelationshipTypeTokens()) {
				return std::nullopt;
			}
			outerType = state.snapshot.findRelationshipTypeToken(outer.getEdgeType());
			if (!outerType.has_value()) {
				LogicalPlanReadRow result;
				result.values.emplace(count.alias, PropertyValue(int64_t{0}));
				return visitor(std::move(result));
			}
		}

		std::vector<uint64_t> seedIds;
		seedIds.reserve(std::min(state.bounds.maximum_node_records, size_t{4'096U}));
		static_cast<void>(visitOperator(seed, state, [&](LogicalPlanReadRow row) {
			const uint64_t id = nodeBindingId(requireBinding(row, seed.getVariable()));
			if (id == 0U || seedIds.size() == state.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan two-hop relationship count exceeded its source Node bound");
			}
			seedIds.push_back(id);
			return true;
		}));
		std::ranges::sort(seedIds);
		seedIds.erase(std::unique(seedIds.begin(), seedIds.end()), seedIds.end());

		using Multiplicity = uint64_t;
		const auto expand = [&](const std::vector<uint64_t> &sources, const std::string &direction,
								 const std::optional<uint64_t> &type, std::unordered_map<uint64_t, Multiplicity> *next) {
			std::unordered_set<uint64_t> sourceSet;
			sourceSet.reserve(sources.size());
			for (const uint64_t id: sources) {
				sourceSet.insert(id);
			}
			std::unordered_map<uint64_t, size_t> sourceEdgeCounts;
			sourceEdgeCounts.reserve(sources.size());
			std::vector<uint64_t> targetIds;
			size_t scanned = 0U;
			const auto consume = [&](const storage::EntityRef &reference) {
				checkGuard(state);
				validateEdgeRef(reference,
						"logical-plan two-hop relationship count received an invalid or Node reference");
				if (scanned == state.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
				}
				++scanned;
				incrementBounded(state.stats.edges_scanned, "logical-plan two-hop Edge counter overflowed");
				const bool outgoing = sourceSet.contains(reference.source_node_id);
				const bool incoming = sourceSet.contains(reference.target_node_id);
				const auto consumeOne = [&](const uint64_t sourceId, const uint64_t targetId) {
					if (sourceEdgeCounts[sourceId] == state.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan two-hop relationship count exceeded its per-Node Edge bound");
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
						targetIds.push_back(targetId);
					}
				};
				if (direction == "out" || direction == "both") {
					if (outgoing) consumeOne(reference.source_node_id, reference.target_node_id);
				}
				if ((direction == "in" || direction == "both") && incoming &&
						!(outgoing && reference.source_node_id == reference.target_node_id)) {
					consumeOne(reference.target_node_id, reference.source_node_id);
				}
				return true;
			};
			const auto visitDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
				if (scanned == state.bounds.maximum_edge_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
				}
				const size_t remaining = state.bounds.maximum_edge_records - scanned;
				const size_t edgeVisitBound = checkedSizeAdd(remaining, 1U,
												"logical-plan two-hop Edge bound overflowed");
				state.snapshot.visitAdjacentEdgeRefsForNodes(sources, adjacencyDirection, edgeVisitBound,
						consume);
			};
			if (direction == "out" || direction == "both") {
				visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
			}
			if (direction == "in" || direction == "both") {
				visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
			}
			if (next != nullptr) {
				if (next->size() > state.bounds.maximum_node_records) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its intermediate Node bound");
				}
				std::ranges::sort(targetIds);
				targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
				const std::vector<bool> present = state.snapshot.containsNodesSorted(targetIds);
				if (present.size() != targetIds.size() || std::ranges::any_of(present, [](bool value) { return !value; })) {
					throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
				}
				const std::vector<std::string> &labels = inner.getTargetLabels();
				if (!labels.empty()) {
					const std::vector<bool> matches = state.snapshot.readNodeLabelMatchesSorted(targetIds, labels);
					if (matches.size() != targetIds.size()) {
						throw std::runtime_error("logical graph backend changed two-hop intermediate label cardinality");
					}
					for (size_t index = 0U; index < targetIds.size(); ++index) {
						if (!matches[index]) {
							next->erase(targetIds[index]);
						}
					}
				}
			}
		};
		std::unordered_map<uint64_t, Multiplicity> intermediate;
		if (!seedIds.empty()) {
			expand(seedIds, inner.getDirection(), innerType, &intermediate);
		}
		std::vector<uint64_t> intermediateIds;
		intermediateIds.reserve(intermediate.size());
		for (const auto &[id, multiplicity]: intermediate) {
			static_cast<void>(multiplicity);
			intermediateIds.push_back(id);
		}
		std::ranges::sort(intermediateIds);
		intermediateIds.erase(std::unique(intermediateIds.begin(), intermediateIds.end()), intermediateIds.end());
		uint64_t resultCount = 0U;
		// The second pass retains multiplicity from the first hop.  It uses the same
		// endpoint/type checks but does not materialize intermediate rows.
		std::unordered_set<uint64_t> sourceSet;
		sourceSet.reserve(intermediate.size());
		for (const uint64_t id: intermediateIds) sourceSet.insert(id);
		std::unordered_map<uint64_t, size_t> edgeCounts;
		struct FinalCandidate { uint64_t source_id = 0U; uint64_t target_id = 0U; uint64_t multiplicity = 0U; };
		std::vector<FinalCandidate> finalCandidates;
		std::vector<uint64_t> finalTargetIds;
		size_t scanned = 0U;
		const auto consumeOuterReference = [&](const storage::EntityRef &reference) {
			checkGuard(state);
			validateEdgeRef(reference,
					"logical-plan two-hop relationship count received an invalid or Node reference");
			if (scanned == state.bounds.maximum_edge_records) {
				throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
			}
			++scanned;
			incrementBounded(state.stats.edges_scanned, "logical-plan two-hop Edge counter overflowed");
			const bool outgoing = sourceSet.contains(reference.source_node_id);
			const bool incoming = sourceSet.contains(reference.target_node_id);
			const auto consume = [&](uint64_t sourceId, uint64_t targetId) {
				if (edgeCounts[sourceId] == state.bounds.maximum_edges_per_node) {
					throw std::runtime_error("logical-plan two-hop relationship count exceeded its per-Node Edge bound");
				}
				++edgeCounts[sourceId];
				if ((outerType.has_value() && reference.type_token != *outerType)) return;
				const uint64_t multiplicity = intermediate.at(sourceId);
				finalCandidates.push_back({sourceId, targetId, multiplicity});
				finalTargetIds.push_back(targetId);
			};
			if (outer.getDirection() == "out" || outer.getDirection() == "both") {
				if (outgoing) consume(reference.source_node_id, reference.target_node_id);
			}
			if ((outer.getDirection() == "in" || outer.getDirection() == "both") && incoming &&
					!(outgoing && reference.source_node_id == reference.target_node_id)) {
				consume(reference.target_node_id, reference.source_node_id);
			}
			return true;
		};
		const auto visitOuterDirection = [&](const storage::LogicalGraphReadDirection adjacencyDirection) {
			if (scanned == state.bounds.maximum_edge_records) {
				throw std::runtime_error("logical-plan two-hop relationship count exceeded its Edge bound");
			}
			const size_t remaining = state.bounds.maximum_edge_records - scanned;
			const size_t edgeVisitBound = checkedSizeAdd(remaining, 1U,
												"logical-plan two-hop Edge bound overflowed");
			state.snapshot.visitAdjacentEdgeRefsForNodes(intermediateIds, adjacencyDirection, edgeVisitBound,
					consumeOuterReference);
		};
		if (outer.getDirection() == "out" || outer.getDirection() == "both") {
			visitOuterDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
		}
		if (outer.getDirection() == "in" || outer.getDirection() == "both") {
			visitOuterDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
		}
		std::ranges::sort(finalTargetIds);
		finalTargetIds.erase(std::unique(finalTargetIds.begin(), finalTargetIds.end()), finalTargetIds.end());
		const std::vector<bool> finalPresent = state.snapshot.containsNodesSorted(finalTargetIds);
		if (finalPresent.size() != finalTargetIds.size() ||
				std::ranges::any_of(finalPresent, [](bool value) { return !value; })) {
			throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
		}
		std::vector<bool> finalLabels(finalTargetIds.size(), true);
		if (!outer.getTargetLabels().empty()) {
			finalLabels = state.snapshot.readNodeLabelMatchesSorted(finalTargetIds, outer.getTargetLabels());
			if (finalLabels.size() != finalTargetIds.size()) {
				throw std::runtime_error("logical graph backend changed two-hop target label cardinality");
			}
		}
		for (const FinalCandidate &candidate: finalCandidates) {
			const auto position = std::ranges::lower_bound(finalTargetIds, candidate.target_id);
			if (position == finalTargetIds.end() || !finalLabels[static_cast<size_t>(position - finalTargetIds.begin())]) {
				continue;
			}
			if (resultCount > std::numeric_limits<uint64_t>::max() - candidate.multiplicity) {
				throw std::overflow_error("logical-plan two-hop relationship count overflowed");
			}
			resultCount += candidate.multiplicity;
			if (resultCount > state.bounds.maximum_rows) {
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
			if (!traversal.getEdgeType().empty() && state.snapshot.supportsNativeRelationshipTypeTokens()) {
				relationshipTypeToken = state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			const bool useNativeAdjacencyRefs = state.snapshot.supportsNativeAdjacentEdgeRefScans() &&
					(traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
			// Selected nullable columns cannot distinguish a missing Property from a
			// stored NULL. Retain the complete-node path for nullable predicates;
			// non-null predicates can be validated through bounded column reads.
			const bool targetPropertiesCanBeSelected = useNativeAdjacencyRefs &&
				!traversal.getTargetProperties().empty() &&
				!std::ranges::any_of(traversal.getTargetProperties(),
						[](const auto &predicate) { return isNull(predicate.second); });

			debug::ScopedPerfTimer timer("logical_plan.anchored_relationship_count");
			size_t inputRows = 0U;
			const bool supportsExistenceProbes = state.snapshot.supportsNodeExistenceProbes();
			const auto visitSource = [&](LogicalPlanReadRow row) {
				const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
				// The topology cursor needs only the stable logical ID. Keep a native
				// deferred source as a reference instead of reopening its full Node record;
				// source labels and predicates were already validated by the scan.
				const uint64_t sourceNodeId = nodeBindingId(sourceBinding);
				size_t sourceEdges = 0U;
				struct PendingTarget {
					uint64_t edge_id = 0U;
					uint64_t source_node_id = 0U;
					uint64_t target_node_id = 0U;
					bool type_matches = false;
				};
				std::vector<PendingTarget> pendingTargets;
			std::unordered_map<uint64_t, bool> targetLabelCache;
				// Target Property values are stable for the pinned snapshot.  Keep a
				// bounded per-source cache so a super-node whose adjacency spans many
				// pages does not reread the same target on every page flush.
				std::unordered_map<uint64_t, std::vector<PropertyValue>> targetPropertyCache;
			size_t targetPropertyCacheBytes = 0U;
			const size_t targetCacheReserve = std::min(state.bounds.maximum_edges_per_node,
					state.bounds.maximum_adjacency_page_records);
			if (!traversal.getTargetLabels().empty()) {
				targetLabelCache.reserve(targetCacheReserve);
			}
			if (targetPropertiesCanBeSelected) {
				targetPropertyCache.reserve(targetCacheReserve);
				}
				const size_t targetBatchCapacity = std::max<size_t>(1U, std::min(
						state.bounds.maximum_adjacency_page_records, state.bounds.maximum_edges_per_node));
				pendingTargets.reserve(targetBatchCapacity);
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
						std::vector<uint64_t> targetIds;
						if (useNativeAdjacencyRefs ||
							(traversal.getTargetProperties().empty() &&
								(!traversal.getTargetLabels().empty() || supportsExistenceProbes))) {
							targetIds.reserve(pendingTargets.size());
							for (const PendingTarget &candidate: pendingTargets) {
								targetIds.push_back(candidate.target_node_id);
							}
							std::ranges::sort(targetIds);
							targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
						}
						std::vector<bool> labelsMatch(targetIds.size(), traversal.getTargetLabels().empty());
						if (!traversal.getTargetLabels().empty()) {
							std::vector<uint64_t> uncachedIds;
							uncachedIds.reserve(targetIds.size());
							for (const uint64_t targetId: targetIds) {
								if (!targetLabelCache.contains(targetId)) {
									uncachedIds.push_back(targetId);
								}
							}
							if (!uncachedIds.empty()) {
								const std::vector<bool> resolved =
										state.snapshot.readNodeLabelMatchesSorted(uncachedIds, traversal.getTargetLabels());
								if (resolved.size() != uncachedIds.size()) {
									throw std::runtime_error("logical graph backend changed anchored target label-match cardinality");
								}
								for (size_t index = 0U; index < uncachedIds.size(); ++index) {
									if (targetLabelCache.size() < state.bounds.maximum_edges_per_node) {
										targetLabelCache.emplace(uncachedIds[index], resolved[index]);
									}
								}
							}
							for (size_t index = 0U; index < targetIds.size(); ++index) {
								const auto found = targetLabelCache.find(targetIds[index]);
								if (found == targetLabelCache.end()) {
									throw std::runtime_error("logical graph backend lost an anchored target label result");
								}
								labelsMatch[index] = found->second;
							}
						}
						std::vector<std::string> targetPropertyKeys;
						std::vector<std::vector<PropertyValue>> targetPropertyColumns;
						if (targetPropertiesCanBeSelected) {
							// Keep the current flush complete even when the cross-page cache
							// reaches its byte budget. Oversized values are used once and are
							// deliberately not retained for later adjacency pages.
							std::unordered_map<uint64_t, std::vector<PropertyValue>> resolvedPropertyValues;
							resolvedPropertyValues.reserve(targetIds.size());
							for (const auto &[key, expected]: traversal.getTargetProperties()) {
								static_cast<void>(expected);
								if (key.empty()) {
									throw std::invalid_argument("logical-plan target Node Property predicate has an empty key");
								}
								if (std::ranges::find(targetPropertyKeys, key) == targetPropertyKeys.end()) {
									targetPropertyKeys.push_back(key);
								}
							}
							std::vector<std::string_view> keyViews;
							keyViews.reserve(targetPropertyKeys.size());
							for (const std::string &key: targetPropertyKeys) {
								keyViews.push_back(key);
							}
							std::vector<uint64_t> uncachedPropertyIds;
							uncachedPropertyIds.reserve(targetIds.size());
							for (const uint64_t targetId: targetIds) {
								if (const auto cached = targetPropertyCache.find(targetId); cached != targetPropertyCache.end()) {
									resolvedPropertyValues.emplace(targetId, cached->second);
								} else {
									uncachedPropertyIds.push_back(targetId);
								}
							}
							if (!uncachedPropertyIds.empty()) {
								const std::vector<std::vector<PropertyValue>> resolved =
										state.snapshot.readNodePropertyColumnsSorted(uncachedPropertyIds, keyViews);
								if (resolved.size() != targetPropertyKeys.size()) {
									throw std::runtime_error("logical graph backend changed anchored target Property column cardinality");
								}
							for (size_t ordinal = 0U; ordinal < uncachedPropertyIds.size(); ++ordinal) {
									std::vector<PropertyValue> values;
									values.reserve(targetPropertyKeys.size());
									for (const auto &column: resolved) {
										if (column.size() != uncachedPropertyIds.size()) {
											throw std::runtime_error("logical graph backend changed anchored target Property result cardinality");
										}
										values.push_back(column[ordinal]);
									}
								const uint64_t targetId = uncachedPropertyIds[ordinal];
								resolvedPropertyValues.emplace(targetId, values);
								if (targetPropertyCache.size() < state.bounds.maximum_edges_per_node) {
									size_t valueBytes = sizeof(uint64_t) + sizeof(std::vector<PropertyValue>);
									for (const PropertyValue &value: values) {
										valueBytes = checkedSizeAdd(valueBytes, sizeof(PropertyValue),
												"logical-plan anchored target Property cache size overflowed");
										valueBytes = checkedSizeAdd(valueBytes, property_utils::getPropertyValueSize(value),
												"logical-plan anchored target Property cache size overflowed");
									}
									if (valueBytes <= MAXIMUM_MATERIALIZED_CACHE_BYTES -
											std::min(targetPropertyCacheBytes, MAXIMUM_MATERIALIZED_CACHE_BYTES)) {
										targetPropertyCache.emplace(targetId, values);
										targetPropertyCacheBytes = checkedSizeAdd(targetPropertyCacheBytes, valueBytes,
											"logical-plan anchored target Property cache size overflowed");
									}
								}
							}
							}
							targetPropertyColumns.assign(targetPropertyKeys.size(), {});
							for (auto &column: targetPropertyColumns) {
								column.reserve(targetIds.size());
							}
							for (const uint64_t targetId: targetIds) {
								const auto found = resolvedPropertyValues.find(targetId);
								if (found == resolvedPropertyValues.end() || found->second.size() != targetPropertyKeys.size()) {
									throw std::runtime_error("logical graph backend lost anchored target Property values");
								}
								for (size_t column = 0U; column < targetPropertyKeys.size(); ++column) {
									targetPropertyColumns[column].push_back(found->second[column]);
								}
							}
						}
						std::vector<bool> targetExists;
						if (state.snapshot.supportsNodeExistenceProbes()) {
							targetExists = state.snapshot.containsNodesSorted(targetIds);
							if (targetExists.size() != targetIds.size()) {
								throw std::runtime_error("logical graph backend changed anchored target existence cardinality");
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
								throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
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
										throw std::logic_error("logical-plan anchored target Property column is missing");
									}
									const size_t columnIndex = static_cast<size_t>(keyPosition - targetPropertyKeys.begin());
									if (isNull(targetPropertyColumns[columnIndex][ordinal]) ||
										targetPropertyColumns[columnIndex][ordinal] != expected) {
										matches = false;
										break;
									}
								}
							}
							if (!matches) {
								continue;
							}
							if (inputRows == state.bounds.maximum_rows) {
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
						if (sourceEdges == state.bounds.maximum_edges_per_node) {
							throw std::runtime_error("logical-plan anchored relationship count exceeded its per-Node Edge bound");
						}
						validateAdjacentEdgeOrder(edgeId, previousEdgeId, "anchored relationship count");
						const bool endpointMatches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														 ? edgeSourceNodeId == sourceNodeId
														 : edgeTargetNodeId == sourceNodeId;
						if (!endpointMatches) {
							throw std::runtime_error("logical graph backend returned an invalid anchored relationship topology");
						}
						++sourceEdges;
						incrementBounded(state.stats.edges_scanned,
										 "logical-plan anchored relationship Edge counter overflowed");
						if (traversal.getDirection() == "both" &&
							 direction == storage::LogicalGraphReadDirection::LGRD_INCOMING && edgeSourceNodeId == sourceNodeId) {
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
									if (!nodeMatches(state.snapshot, target, traversal.getTargetLabels(),
												 traversal.getTargetProperties(), state)) {
										return true;
									}
								} else if (supportsExistenceProbes) {
																	 if (!state.snapshot.containsNode(targetId)) {
										throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
									}
								} else {
									validateNode(readNodeCached(state, targetId));
								}
							}
							// A native reference batch validates all target endpoints in
							// flushTargets().  Do not probe this endpoint eagerly here: doing
							// so turns a page-bounded batch into one Directory lookup per Edge.
						if (requiresBatchedTargetRead) {
							pendingTargets.push_back({edgeId, edgeSourceNodeId, edgeTargetNodeId, typeMatches});
							if (pendingTargets.size() >= targetBatchCapacity) {
								return flushTargets();
							}
							return true;
						}
						if (inputRows == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
						}
						incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						return true;
					};
					if (sourceEdges == state.bounds.maximum_edges_per_node) {
						// Ask for one bounded probe so an additional Edge is rejected rather
						// than silently truncating the aggregate input.
						if (useNativeAdjacencyRefs) {
							const storage::LogicalGraphAdjacencyReferencePage page = state.snapshot.scanAdjacentEdgeRefs(
															 sourceNodeId, direction, std::nullopt, 1U);
							if (!page.edges.empty()) {
								throw std::runtime_error(
										"logical-plan anchored relationship count exceeded its per-Node Edge bound");
							}
						} else {
							state.snapshot.visitAdjacentEdgeTopologies(
									 sourceNodeId, direction, 1U,
									[](const storage::LogicalGraphEdgeTopology &) -> bool {
										throw std::runtime_error(
													"logical-plan anchored relationship count exceeded its per-Node Edge bound");
									});
						}
						return;
					}
					const size_t directionLimit = state.bounds.maximum_edges_per_node - sourceEdges;
					if (useNativeAdjacencyRefs) {
						std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
						for (;;) {
							if (sourceEdges == state.bounds.maximum_edges_per_node) {
								throw std::runtime_error(
										"logical-plan anchored relationship count exceeded its per-Node Edge bound");
							}
							const size_t pageRecords = std::min(
									state.bounds.maximum_edges_per_node - sourceEdges,
									state.bounds.maximum_adjacency_page_records);
							const storage::LogicalGraphAdjacencyReferencePage page = state.snapshot.scanAdjacentEdgeRefs(
														 sourceNodeId, direction, continuation, pageRecords);
							if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value()) ||
								(continuation.has_value() &&
								 (page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
								throw std::runtime_error(
										"logical graph backend returned an invalid anchored relationship reference page");
							}
							for (const storage::EntityRef &reference: page.edges) {
								validateEdgeRef(reference,
										"logical graph backend returned an invalid or Node reference from an anchored relationship page");
								const bool typeMatches = !relationshipTypeToken.has_value() ||
										reference.type_token == *relationshipTypeToken;
								if (!consumeTopology(reference.entity_id, reference.source_node_id, reference.target_node_id,
																 typeMatches)) {
									return;
								}
								}
								if (!page.continuation.has_value()) {
									if (!flushTargets()) {
										return;
									}
									return;
							}
							if (page.edges.empty() || page.continuation->previous_edge_id != page.edges.back().entity_id ||
								page.continuation->next_edge_id == 0U ||
								page.continuation->previous_edge_id == page.continuation->next_edge_id) {
								throw std::runtime_error(
										"logical graph backend returned an invalid anchored relationship reference continuation");
							}
							continuation = page.continuation;
						}
					}
						state.snapshot.visitAdjacentEdgeTopologies(
													 sourceNodeId, direction, directionLimit,
								[&](const storage::LogicalGraphEdgeTopology &edge) {
									return consumeTopology(edge.edge_id, edge.source_node_id, edge.target_node_id,
												 traversal.getEdgeType().empty() || edge.type == traversal.getEdgeType());
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
				state.snapshot.supportsNativeBatchAdjacentEdgeRefs() &&
				(traversal.getEdgeType().empty() || relationshipTypeToken.has_value());
			if (!canUseGlobalSourceSet) {
				static_cast<void>(visitOperator(seed, state, sourceVisitor));
			} else {
				std::vector<uint64_t> sourceIds;
				sourceIds.reserve(std::min(state.bounds.maximum_node_records,
						state.bounds.maximum_adjacency_page_records * size_t{2U}));
				static_cast<void>(visitOperator(seed, state, [&](LogicalPlanReadRow row) {
					const uint64_t sourceId = nodeBindingId(requireBinding(row, traversal.getSourceVar()));
					if (sourceId == 0U) {
						throw std::runtime_error("logical-plan anchored relationship count returned an invalid source Node");
					}
					if (sourceIds.size() == state.bounds.maximum_node_records) {
						throw std::runtime_error("logical-plan anchored relationship count exceeded its source Node bound");
					}
						 sourceIds.push_back(sourceId);
					return true;
				}));
				std::ranges::sort(sourceIds);
				sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
				const size_t globalSourceThreshold = std::max<size_t>(1U, state.bounds.maximum_adjacency_page_records);
				if (sourceIds.size() < globalSourceThreshold) {
					for (const uint64_t sourceId : sourceIds) {
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
					for (const uint64_t sourceId : sourceIds) {
						static_cast<void>(sourceSet.insert(sourceId));
					}
					std::unordered_map<uint64_t, size_t> sourceEdgeCounts;
					sourceEdgeCounts.reserve(sourceIds.size());
					size_t globalEdgesScanned = 0U;
					std::vector<PendingGlobalEdge> pending;
					const size_t globalBatchCapacity = std::max<size_t>(1U, std::min(
							state.bounds.maximum_adjacency_page_records, state.bounds.maximum_edge_records));
					pending.reserve(globalBatchCapacity);
					const auto flushGlobal = [&]() {
						if (pending.empty()) {
							return;
						}
						std::vector<uint64_t> targetIds;
						targetIds.reserve(pending.size());
						for (const PendingGlobalEdge &candidate : pending) {
							targetIds.push_back(candidate.target_node_id);
						}
						std::ranges::sort(targetIds);
						targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
						const std::vector<bool> present = state.snapshot.containsNodesSorted(targetIds);
						if (present.size() != targetIds.size()) {
							throw std::runtime_error("logical graph backend changed anchored global target existence cardinality");
						}
						const std::vector<bool> labelsMatch = traversal.getTargetLabels().empty()
								? std::vector<bool>(targetIds.size(), true)
								: state.snapshot.readNodeLabelMatchesSorted(targetIds, traversal.getTargetLabels());
						if (labelsMatch.size() != targetIds.size()) {
							throw std::runtime_error("logical graph backend changed anchored global target label cardinality");
						}
						for (const PendingGlobalEdge &candidate : pending) {
							const auto position = std::ranges::lower_bound(targetIds, candidate.target_node_id);
							if (position == targetIds.end() || *position != candidate.target_node_id ||
								!present[static_cast<size_t>(position - targetIds.begin())]) {
								throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
							}
							if (!labelsMatch[static_cast<size_t>(position - targetIds.begin())]) {
								continue;
							}
							if (inputRows == state.bounds.maximum_rows) {
								throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
							}
							incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
						}
						pending.clear();
					};
					const auto visitGlobalDirection = [&](const storage::LogicalGraphReadDirection direction) {
						if (globalEdgesScanned == state.bounds.maximum_edge_records) {
							throw std::runtime_error("logical-plan anchored global relationship count exceeded its Edge bound");
						}
						const size_t remaining = state.bounds.maximum_edge_records - globalEdgesScanned;
						const size_t edgeVisitBound = checkedSizeAdd(remaining, 1U,
								"logical-plan anchored global Edge bound overflowed");
					state.snapshot.visitAdjacentEdgeRefsForNodes(sourceIds, direction, edgeVisitBound,
							[&](const storage::EntityRef &reference) {
						checkGuard(state);
						validateEdgeRef(reference,
								"logical-plan anchored global relationship count received an invalid or Node reference");
						if (globalEdgesScanned == state.bounds.maximum_edge_records) {
							throw std::runtime_error("logical-plan anchored global relationship count exceeded its Edge bound");
						}
						incrementBounded(globalEdgesScanned, "logical-plan anchored global Edge counter overflowed");
						incrementBounded(state.stats.edges_scanned,
								"logical-plan anchored global Edge statistics overflowed");
						const bool outgoing = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING;
						const uint64_t sourceId = outgoing ? reference.source_node_id : reference.target_node_id;
						if (!sourceSet.contains(sourceId)) {
							return true;
						}
						const auto countSourceEdge = [&](const uint64_t sourceId) {
							const size_t count = sourceEdgeCounts[sourceId];
							if (count == state.bounds.maximum_edges_per_node) {
								throw std::runtime_error("logical-plan anchored relationship count exceeded its per-Node Edge bound");
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
						if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
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

		[[nodiscard]] std::optional<bool>
		tryVisitDirectRelationshipCount(const logical::LogicalAggregate &aggregate, ExecutionState &state,
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
			std::vector<std::pair<std::string, PropertyValue>> pushedEdgePredicates;
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
				const auto inspected = inspectDirectEdgePropertyFilter(
						*filter.getPredicate(), filteredTraversal.getEdgeVar(), state);
				if (!inspected.has_value()) {
					return std::nullopt;
				}
				pushedEdgePredicates = *inspected;
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
				const auto *variable = dynamic_cast<const expressions::VariableReferenceExpression *>(count.argument.get());
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

			if (state.bounds.maximum_rows == std::numeric_limits<size_t>::max()) {
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
			const size_t endpointCacheCapacity = state.bounds.maximum_node_records;
			std::unordered_set<uint64_t> endpointsToValidate;
			endpointsToValidate.reserve(std::min(endpointCacheCapacity,
					state.bounds.maximum_adjacency_page_records));
			const bool supportsExistenceProbes = state.snapshot.supportsNodeExistenceProbes();
			const auto validateEndpoints = [&]() {
				std::vector<uint64_t> endpointIds(endpointsToValidate.begin(), endpointsToValidate.end());
				if (endpointIds.empty()) {
					return;
				}
				std::ranges::sort(endpointIds);
				if (supportsExistenceProbes) {
					const std::vector<bool> present = state.snapshot.containsNodesSorted(endpointIds);
					if (present.size() != endpointIds.size()) {
						throw std::runtime_error("logical graph backend changed Node existence batch cardinality");
					}
					for (size_t index = 0U; index < endpointIds.size(); ++index) {
						if (!present[index]) {
							throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
						}
					}
				} else {
					for (const uint64_t nodeId : endpointIds) {
						validateNode(readNodeCached(state, nodeId));
					}
				}
			};

			std::vector<std::pair<std::string, PropertyValue>> edgePredicates;
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
				edgePredicates.emplace_back(key, expected);
			}
			std::ranges::sort(edgePredicates, {}, &std::pair<std::string, PropertyValue>::first);
			for (const auto &[key, expected]: pushedEdgePredicates) {
				const auto existing = std::ranges::find(edgePredicates, key,
						&std::pair<std::string, PropertyValue>::first);
				if (existing == edgePredicates.end()) {
					edgePredicates.emplace_back(key, expected);
				} else if (existing->second != expected) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}
			std::vector<std::string_view> edgePropertyKeys;
			edgePropertyKeys.reserve(edgePredicates.size());
			for (const auto &[key, expected]: edgePredicates) {
				static_cast<void>(expected);
				edgePropertyKeys.push_back(key);
			}
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() && state.snapshot.supportsNativeRelationshipTypeTokens()) {
				relationshipTypeToken = state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
				if (!relationshipTypeToken.has_value()) {
					LogicalPlanReadRow result;
					result.values.emplace(count.alias, PropertyValue(int64_t{0}));
					return visitor(std::move(result));
				}
			}

			size_t inputRows = 0U;
			size_t edgeRecords = 0U;
			// Keep topology-only counts batched as well.  A batch of one was useful
			// for an early prototype, but it turns every topology page into a
			// per-edge callback and prevents the same bounded execution substrate
			// from amortizing validation and selection work when no Property column
			// is requested.
			const size_t batchCapacity = std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_edge_records,
												 state.bounds.maximum_rows});
			std::vector<storage::LogicalGraphEdgeTopology> pending;
			pending.reserve(batchCapacity);
			const auto countTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
				if (inputRows == state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
			};
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return;
				}
				for (const auto &edge : pending) {
					for (const uint64_t endpointId: {edge.source_node_id, edge.target_node_id}) {
						if (!endpointsToValidate.contains(endpointId) &&
							endpointsToValidate.size() == endpointCacheCapacity) {
							throw std::runtime_error(
									"logical-plan direct relationship count exceeded its endpoint Node bound");
						}
						static_cast<void>(endpointsToValidate.insert(endpointId));
					}
				}
				if (edgePredicates.empty()) {
					for (const storage::LogicalGraphEdgeTopology &edge: pending) {
						countTopology(edge);
					}
					pending.clear();
					return;
				}
				std::vector<size_t> sortedOrdinals(pending.size());
				for (size_t index = 0U; index < sortedOrdinals.size(); ++index) {
					sortedOrdinals[index] = index;
				}
				std::ranges::sort(sortedOrdinals, [&pending](const size_t left, const size_t right) {
					return pending[left].edge_id < pending[right].edge_id;
				});
				std::vector<uint64_t> edgeIds;
				edgeIds.reserve(pending.size());
				for (const size_t ordinal: sortedOrdinals) {
					const uint64_t edgeId = pending[ordinal].edge_id;
					if (!edgeIds.empty() && edgeId == edgeIds.back()) {
						throw std::runtime_error("logical graph direct relationship count returned duplicate Edge IDs");
					}
					edgeIds.push_back(edgeId);
				}
				SelectionVector selection(pending.size());
												const std::vector<std::vector<PropertyValue>> sortedColumns =
														state.snapshot.readEdgePropertyColumnsSorted(edgeIds, edgePropertyKeys);
											const size_t selectedEdgeBytes = propertyColumnBytes(
													sortedColumns, edgePropertyKeys, "logical-plan direct Edge Property batch size overflowed");
											if (selectedEdgeBytes > state.bounds.maximum_materialized_bytes) {
												throw std::runtime_error("logical-plan direct Edge Property batch exceeded its byte bound");
											}
				if (sortedColumns.size() != edgePredicates.size()) {
					throw std::runtime_error("logical graph backend changed Edge Property column cardinality");
				}
				for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
					const auto &[key, expected] = edgePredicates[propertyIndex];
					static_cast<void>(key);
					const std::vector<PropertyValue> &sortedValues = sortedColumns[propertyIndex];
					if (sortedValues.size() != pending.size()) {
						throw std::runtime_error(
								"logical graph backend changed sorted Edge Property result cardinality");
					}
					for (size_t index = 0U; index < sortedValues.size(); ++index) {
						const size_t original = sortedOrdinals[index];
						if (selection.includes(original) &&
							(isNull(sortedValues[index]) || sortedValues[index] != expected)) {
							selection.reject(original);
						}
					}
				}
				for (size_t index = 0U; index < pending.size(); ++index) {
					if (!selection.includes(index)) {
						continue;
					}
					countTopology(pending[index]);
				}
				pending.clear();
			};
			const auto consumeTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
						checkGuard(state);
						if (edgeRecords == state.bounds.maximum_edge_records) {
							throw std::runtime_error("logical-plan direct relationship count exceeded its Edge record bound");
						}
						incrementBounded(edgeRecords, "logical-plan direct relationship count Edge counter overflowed");
						incrementBounded(state.stats.edges_scanned,
										 "logical-plan read runtime Edge counter overflowed");
						if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
							throw std::runtime_error(
									"logical graph backend returned an Edge topology with invalid identity or endpoints");
						}
						if (!relationshipTypeToken.has_value() && !traversal.getEdgeType().empty() &&
							edge.type != traversal.getEdgeType()) {
							return true;
						}
						pending.push_back(edge);
						if (pending.size() == batchCapacity) {
							flushPending();
						}
						return true;
				};
			// The default Edge-reference implementation projects complete Edge records.
			// Use it only when the snapshot explicitly provides the native cheap path;
			// generic readers retain their topology-only projection.
			const size_t edgeVisitBound = checkedSizeAdd(state.bounds.maximum_edge_records, 1U,
					"logical-plan direct relationship count Edge bound overflowed");
			if (state.snapshot.supportsNativeEdgeRefScans() &&
				(traversal.getEdgeType().empty() || relationshipTypeToken.has_value())) {
				state.snapshot.visitEdgeRefs(edgeVisitBound,
						[&consumeTopology, &relationshipTypeToken](const storage::EntityRef &reference) {
							validateEdgeRef(reference,
									"logical-plan direct relationship count received an invalid or Node reference");
							if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
								return true;
							}
							return consumeTopology({reference.entity_id, reference.source_node_id, reference.target_node_id, {}});
						});
			} else {
				state.snapshot.visitEdgeTopologies(edgeVisitBound, consumeTopology);
			}
			flushPending();
			validateEndpoints();
			if (debug::PerfTrace::areValuesEnabled()) {
				debug::PerfTrace::addValue("logical_plan.direct_relationship_count_edge_records",
						static_cast<int64_t>(edgeRecords));
			}

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
		}

		[[nodiscard]] std::optional<bool>
		tryVisitDirectTraversal(const logical::LogicalTraversal &traversal, ExecutionState &state,
								 const InternalVisitor &visitor,
								 const std::vector<std::pair<std::string, PropertyValue>> *additionalEdgeProperties = nullptr) {
			if (!state.allow_deferred_entity_refs || !state.snapshot.supportsNativeNodeRefScans() ||
				!state.snapshot.supportsNativeEdgeRefScans() ||
				(traversal.getDirection() != "out" && traversal.getDirection() != "in")) {
				return std::nullopt;
			}
			const bool hasAdditionalEdgeProperties = additionalEdgeProperties != nullptr &&
				!additionalEdgeProperties->empty();
			const std::vector<logical::LogicalOperator *> children = traversal.getChildren();
			if (children.size() != 1U || children.front() == nullptr ||
				children.front()->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
				return std::nullopt;
			}
			const auto &sourceScan = dynamic_cast<const logical::LogicalNodeScan &>(*children.front());
			// An unanchored topology traversal is cheaper as one global Edge scan;
			// constrained sources retain the adjacency cursor for locality.
			const bool sourceConstrained = !sourceScan.getLabels().empty() || !sourceScan.getPropertyPredicates().empty() ||
				!sourceScan.getRangePredicates().empty() || sourceScan.getCompositeEquality().has_value() ||
				!sourceScan.getParameterPropertyPredicates().empty();
			if (sourceConstrained && state.snapshot.supportsNativeAdjacentEdgeRefScans()) {
				return std::nullopt;
			}
			if (sourceScan.getVariable() != traversal.getSourceVar() ||
				((!traversal.getEdgeProperties().empty() || hasAdditionalEdgeProperties) && traversal.getEdgeVar().empty()) ||
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
			sources.reserve(std::min(state.bounds.maximum_node_records, size_t{4'096U}));
			// An unconstrained source has no predicate that can reduce the Edge
			// stream.  Keep source bindings lazy and let the global Edge cursor drive
			// the pipeline; scanning every Node first would add an unrelated O(|V|)
			// pass to an O(|E|) operation.  The map is query-local and bounded by the
			// same source-record guard used by the old scan path.
			const bool globalEdgePath = !sourceConstrained;
			std::unordered_map<uint64_t, size_t> sourceOrdinals;
			if (globalEdgePath) {
				sourceOrdinals.reserve(std::min(state.bounds.maximum_node_records, size_t{4'096U}));
			} else {
				state.snapshot.visitNodeRefsWithLabelMatches(state.bounds.maximum_node_records, sourceScan.getLabels(),
						[&](const storage::EntityRef &reference, const bool labelsMatch) {
							checkGuard(state);
							if (reference.is_edge || reference.entity_id == 0U) {
								throw std::runtime_error(
										"logical graph direct traversal returned an invalid source Node reference");
							}
							incrementBounded(state.stats.nodes_scanned,
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
					throw std::runtime_error("logical graph direct traversal returned duplicate source Node references");
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
				std::vector<std::vector<PropertyValue>> sourceValues =
						state.snapshot.readNodePropertyColumnsSorted(sourceIds, sourcePropertyKeyViews);
				if (sourceValues.size() != sourcePropertyKeys.size()) {
					throw std::runtime_error("logical graph backend changed source Node Property column cardinality");
				}
				for (size_t column = 0U; column < sourcePropertyKeys.size(); ++column) {
					if (sourceValues[column].size() != sources.size()) {
						throw std::runtime_error(
								"logical graph backend changed sorted Node Property result cardinality");
					}
					sourceColumns.push_back({sourcePropertyKeys[column], std::move(sourceValues[column])});
				}
				const auto sourceValue = [&sourceColumns](const size_t sourceIndex, const std::string &key)
						-> const PropertyValue & {
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
					for (const SourcePropertyColumn &column: sourceColumns) {
						row.property_cache[sourceScan.getVariable()].insert_or_assign(column.key,
																					column.values[sourceIndex]);
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
					const auto existing = std::ranges::find(edgePredicates, key,
							&std::pair<std::string, PropertyValue>::first);
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
			std::optional<uint64_t> relationshipTypeToken;
			if (!traversal.getEdgeType().empty() && state.snapshot.supportsNativeRelationshipTypeTokens()) {
				relationshipTypeToken = state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
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
			const size_t edgeBatchCapacity = edgePredicates.empty() && targetPredicates.empty()
													 ? size_t{1U}
													 : std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_edge_records,
																	 state.bounds.maximum_rows});
			std::vector<PendingEdge> pending;
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
					if (globalEdgePath && state.snapshot.supportsNodeExistenceProbes()) {
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
						const std::vector<bool> present = state.snapshot.containsNodesSorted(sourceIds);
						if (present.size() != sourceIds.size()) {
							throw std::runtime_error("logical graph backend changed direct traversal source cardinality");
						}
						if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
							throw std::runtime_error(
									"logical graph backend returned an Edge topology with a missing source Node");
						}
					}
					SelectionVector selection(pending.size());
				std::vector<std::vector<PropertyValue>> selectedProperties;
				if (!edgePredicates.empty()) {
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
					for (const size_t ordinal: sortedOrdinals) {
						const uint64_t edgeId = pending[ordinal].topology.edge_id;
						if (edgeIds.empty() || edgeId != edgeIds.back()) {
							edgeIds.push_back(edgeId);
						}
						uniqueOrdinals[ordinal] = edgeIds.size() - 1U;
					}
					if (std::ranges::any_of(edgePredicates, [](const auto &predicate) {
						return isNull(predicate.second);
					})) {
						selection.rejectAll();
					} else {
						const std::vector<std::vector<PropertyValue>> sortedColumns =
								state.snapshot.readEdgePropertyColumnsSorted(edgeIds, edgePropertyKeys);
						const size_t selectedEdgeBytes = propertyColumnBytes(
								sortedColumns, edgePropertyKeys,
								"logical-plan direct traversal Edge Property batch size overflowed");
						if (selectedEdgeBytes > state.bounds.maximum_materialized_bytes) {
							throw std::runtime_error(
									"logical-plan direct traversal Edge Property batch exceeded its byte bound");
						}
						if (sortedColumns.size() != edgePredicates.size()) {
							throw std::runtime_error("logical graph backend changed Edge Property column cardinality");
						}
						selectedProperties.resize(edgePredicates.size());
						for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
							const std::vector<PropertyValue> &sortedValues = sortedColumns[propertyIndex];
							if (sortedValues.size() != edgeIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed sorted Edge Property result cardinality");
							}
							std::vector<PropertyValue> values(pending.size());
							for (const size_t ordinal: sortedOrdinals) {
								values[ordinal] = sortedValues[uniqueOrdinals[ordinal]];
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

				struct TargetCandidate {
					LogicalPlanReadRow row;
					uint64_t target_id = 0U;
					size_t property_ordinal = 0U;
				};
				// Resolve labels before Property columns so a label-rejected target does
				// not consume a selected-Property read. Binding is checked first because
				// a repeated variable can reject a non-self traversal before any target
				// access is observable.
				std::vector<TargetCandidate> targets;
				targets.reserve(pending.size());
				selection.forEachSelected([&](const size_t index) {
					checkGuard(state);
					const PendingEdge &candidate = pending[index];
					SourceRow &source = sources[candidate.source_ordinal];
					const uint64_t targetId = traversal.getDirection() == "out"
															 ? candidate.topology.target_node_id
															 : candidate.topology.source_node_id;
					LogicalPlanReadRow expanded = source.cached_row.has_value() ? *source.cached_row : LogicalPlanReadRow{};
					if (!source.cached_row.has_value()) {
						expanded.values.emplace(sourceScan.getVariable(),
													storage::EntityRef{source.node_id, false, 0U, 0U, 0U});
					}
					const storage::EntityRef edgeRef{candidate.topology.edge_id, true, candidate.topology.source_node_id,
																		 candidate.topology.target_node_id, candidate.type_token};
					const storage::EntityRef targetRef{targetId, false, 0U, 0U, 0U};
						if (!bindValue(expanded, traversal.getEdgeVar(), edgeRef) ||
							!bindValue(expanded, traversal.getTargetVar(), targetRef)) {
							return;
					}
					for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size(); ++propertyIndex) {
						expanded.property_cache[traversal.getEdgeVar()].insert_or_assign(
								edgePredicates[propertyIndex].first, selectedProperties[propertyIndex][index]);
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
					std::ranges::sort(targetIds);
					targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
					const std::vector<bool> labelsMatch =
							state.snapshot.readNodeLabelMatchesSorted(targetIds, traversal.getTargetLabels());
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
										const std::vector<std::vector<PropertyValue>> targetValues =
													state.snapshot.readNodePropertyColumnsSorted(targetIds, targetPropertyKeyViews);
										const size_t selectedTargetBytes = propertyColumnBytes(
												targetValues, targetPropertyKeyViews, "logical-plan direct target Property batch size overflowed");
										if (selectedTargetBytes > state.bounds.maximum_materialized_bytes) {
											throw std::runtime_error("logical-plan direct target Property batch exceeded its byte bound");
										}
						if (targetValues.size() != targetPropertyKeys.size()) {
							throw std::runtime_error("logical graph backend changed target Node Property column cardinality");
						}
						std::vector<TargetPropertyColumn> targetColumns;
						targetColumns.reserve(targetPropertyKeys.size());
						for (size_t column = 0U; column < targetPropertyKeys.size(); ++column) {
							if (targetValues[column].size() != targetIds.size()) {
								throw std::runtime_error(
										"logical graph backend changed sorted target Node Property result cardinality");
							}
							targetColumns.push_back({targetPropertyKeys[column], targetValues[column]});
						}
						for (TargetCandidate &target: targets) {
							checkGuard(state);
							bool matchesTarget = true;
							for (const auto &[key, expected]: targetPredicates) {
								const auto column = std::ranges::find(targetColumns, key, &TargetPropertyColumn::key);
								if (column == targetColumns.end()) { // ZYX_COV_EXCL_LINE: keys are collected before column reads.
									throw std::logic_error("logical-plan target Property column is missing its requested key");
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
						if (edgeRecords == state.bounds.maximum_edge_records) {
							throw std::runtime_error("logical-plan direct traversal exceeded its global Edge bound");
						}
						incrementBounded(edgeRecords, "logical-plan direct traversal Edge counter overflowed");
						incrementBounded(state.stats.edges_scanned,
										 "logical-plan read runtime Edge counter overflowed");

						const uint64_t sourceId = traversal.getDirection() == "out" ? edge.source_node_id
																		 : edge.target_node_id;
						size_t sourceOrdinal = 0U;
						if (globalEdgePath) {
							if (sourceId == 0U) {
								throw std::runtime_error("logical graph direct traversal returned a zero source Node ID");
							}
							const auto existing = sourceOrdinals.find(sourceId);
							if (existing == sourceOrdinals.end()) {
				if (sources.size() == state.bounds.maximum_node_records) {
					throw std::runtime_error(
							"logical-plan direct traversal exceeded its source Node record bound");
				}
				// Property-bearing batches validate source liveness in flushPending().
				// Keep the scalar check only for topology-only batches, whose capacity is
				// intentionally one to preserve strict early termination.
				if (state.snapshot.supportsNodeExistenceProbes() && edgeBatchCapacity == 1U &&
						!state.snapshot.containsNode(sourceId)) {
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
								source->edges_seen == state.bounds.maximum_edges_per_node) {
								return true;
							}
							incrementBounded(source->edges_seen,
											 "logical-plan direct traversal per-Node Edge counter overflowed");
							sourceOrdinal = static_cast<size_t>(source - sources.begin());
						}
						if (globalEdgePath) {
							SourceRow &source = sources[sourceOrdinal];
							if (source.edges_seen == state.bounds.maximum_edges_per_node) {
								return true;
							}
							incrementBounded(source.edges_seen,
										 "logical-plan direct traversal per-Node Edge counter overflowed");
						}
						if (!relationshipTypeToken.has_value() && !traversal.getEdgeType().empty() &&
							edge.type != traversal.getEdgeType()) {
							return true;
						}
						pending.push_back({edge, typeToken, sourceOrdinal});
						return pending.size() != edgeBatchCapacity || flushPending();
				};
			const size_t edgeVisitBound = checkedSizeAdd(state.bounds.maximum_edge_records, 1U,
					"logical-plan direct traversal Edge bound overflowed");
			if (traversal.getEdgeType().empty() || relationshipTypeToken.has_value()) {
				state.snapshot.visitEdgeRefs(edgeVisitBound,
						[&consumeTopology, &relationshipTypeToken](const storage::EntityRef &reference) {
							validateEdgeRef(reference,
									"logical-plan direct traversal received an invalid or Node reference");
							if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
								return true;
							}
							return consumeTopology({reference.entity_id, reference.source_node_id, reference.target_node_id, {}},
												   reference.type_token);
						});
			} else {
				state.snapshot.visitEdgeTopologies(edgeVisitBound, consumeTopology);
			}
			if (continueVisitor) {
				static_cast<void>(flushPending());
			}
			const int64_t tracedEdgeRecords =
					edgeRecords > static_cast<size_t>(std::numeric_limits<int64_t>::max())
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
									 ExecutionState &state, const InternalVisitor &visitor) {
			if (!state.allow_deferred_entity_refs || !state.snapshot.supportsNativeNodeRefScans()) {
				return std::nullopt;
			}

			std::vector<std::pair<std::string, PropertyValue>> equalityPredicates = scan.getPropertyPredicates();
			equalityPredicates.insert(equalityPredicates.end(), parameterPredicates.begin(), parameterPredicates.end());
			std::vector<std::string> propertyKeys;
			propertyKeys.reserve(equalityPredicates.size() + scan.getRangePredicates().size());
			const auto appendKey = [&propertyKeys](const std::string &key) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Node Property predicate has an empty key");
				}
				if (std::ranges::find(propertyKeys, key) == propertyKeys.end()) {
					propertyKeys.push_back(key);
				}
			};
			for (const auto &[key, expected]: equalityPredicates) {
				if (isNull(expected)) {
					// The established row-at-a-time path preserves its existing bounded
					// accounting for an impossible equality predicate.
					return std::nullopt;
				}
				appendKey(key);
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
			if (propertyKeys.empty()) {
				return std::nullopt;
			}

			struct PendingNodeRef {
				storage::EntityRef reference;
			};
			const size_t batchCapacity = std::min({DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_node_records,
														 state.bounds.maximum_rows});
			std::vector<PendingNodeRef> pending;
			pending.reserve(batchCapacity);
			bool continueVisitor = true;
			const auto flushPending = [&]() {
				if (pending.empty()) {
					return continueVisitor;
				}
				std::vector<size_t> sortedOrdinals(pending.size());
				std::iota(sortedOrdinals.begin(), sortedOrdinals.end(), size_t{0U});
				std::ranges::sort(sortedOrdinals, [&pending](const size_t left, const size_t right) {
					return pending[left].reference.entity_id < pending[right].reference.entity_id;
				});
				std::vector<uint64_t> nodeIds;
				nodeIds.reserve(pending.size());
				for (const size_t ordinal: sortedOrdinals) {
					const uint64_t nodeId = pending[ordinal].reference.entity_id;
					if (!nodeIds.empty() && nodeId == nodeIds.back()) {
						throw std::runtime_error("logical graph native Node reference scan returned duplicate Node IDs");
					}
					nodeIds.push_back(nodeId);
				}
				std::vector<std::string_view> keyViews;
				keyViews.reserve(propertyKeys.size());
				for (const std::string &key: propertyKeys) {
					keyViews.push_back(key);
				}
				const std::vector<std::vector<PropertyValue>> sortedColumns =
						state.snapshot.readNodePropertyColumnsSorted(nodeIds, keyViews);
				if (sortedColumns.size() != propertyKeys.size()) {
					throw std::runtime_error("logical graph backend changed Node Property column cardinality");
				}
				std::vector<std::vector<PropertyValue>> columns(
						propertyKeys.size(), std::vector<PropertyValue>(pending.size()));
				for (size_t column = 0U; column < sortedColumns.size(); ++column) {
					if (sortedColumns[column].size() != pending.size()) {
						throw std::runtime_error(
								"logical graph backend changed sorted Node Property result cardinality");
					}
					for (size_t sorted = 0U; sorted < sortedOrdinals.size(); ++sorted) {
						columns[column][sortedOrdinals[sorted]] = sortedColumns[column][sorted];
					}
				}
				for (size_t index = 0U; index < pending.size() && continueVisitor; ++index) {
					checkGuard(state);
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), pending[index].reference);
					for (size_t column = 0U; column < propertyKeys.size(); ++column) {
						row.property_cache[scan.getVariable()].insert_or_assign(propertyKeys[column], columns[column][index]);
					}
					if (nodeRefScanMatches(state.snapshot, row, scan.getVariable(), scan, parameterPredicates, state,
																 true)) {
						continueVisitor = visitor(std::move(row));
					}
				}
				pending.clear();
				return continueVisitor;
			};

			state.snapshot.visitNodeRefsWithLabelMatches(
					state.bounds.maximum_node_records, scan.getLabels(),
					[&](const storage::EntityRef &reference, const bool labelsMatch) {
						checkGuard(state);
						validateNodeRef(reference,
								"logical graph backend returned an invalid or Edge reference from a Node scan");
						incrementBounded(state.stats.nodes_scanned,
										 "logical-plan read runtime Node counter overflowed");
						if (!labelsMatch) {
							return true;
						}
						pending.push_back({reference});
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
		[[nodiscard]] bool
		visitIndexedCandidateBatch(const storage::IndexCandidateBatch &nativeCandidates,
								   const logical::LogicalNodeScan &scan,
								   const std::vector<std::pair<std::string, PropertyValue>> &equalityPredicates,
								   const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
								   size_t &candidateRecords, std::unordered_set<uint64_t> &seenCandidateIds,
								   ExecutionState &state, const InternalVisitor &visitor) {
			if (nativeCandidates.snapshot_generation == 0U) {
				throw std::runtime_error("logical graph native indexed Node candidates have no snapshot generation");
			}
			const size_t candidateBytes = logicalIndexCandidateBatchBytes(nativeCandidates);
			if (candidateBytes > state.bounds.maximum_candidate_bytes) {
				throw std::runtime_error("logical graph indexed Node candidate batch exceeded its byte bound");
			}
			debug::PerfTrace::addValue(
					"logical_plan.index_candidate.materialized_bytes",
					static_cast<int64_t>(
							std::min(candidateBytes, static_cast<size_t>(std::numeric_limits<int64_t>::max()))));
			const std::optional<uint64_t> snapshotGeneration = state.snapshot.nativeSnapshotGeneration();
			if (!snapshotGeneration.has_value() || *snapshotGeneration == 0U ||
				nativeCandidates.snapshot_generation != *snapshotGeneration) {
				throw std::runtime_error(
						"logical graph native indexed Node candidates do not belong to the pinned snapshot");
			}
			if (candidateRecords > state.bounds.maximum_node_records ||
				nativeCandidates.candidates.size() > state.bounds.maximum_node_records - candidateRecords) {
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
			uint64_t previousNodeId = 0U;
			std::vector<uint64_t> candidateNodeIds;
			candidateNodeIds.reserve(nativeCandidates.candidates.size());
			for (const storage::EntityRef &candidate: nativeCandidates.candidates) {
				validateNodeRef(candidate,
								"logical graph indexed Node lookup returned invalid or non-monotonic Node references");
				if (candidate.entity_id <= previousNodeId) {
					throw std::runtime_error(
							"logical graph indexed Node lookup returned a non-monotonic candidate batch");
				}
				previousNodeId = candidate.entity_id;
				if (!seenCandidateIds.insert(candidate.entity_id).second) {
					throw std::runtime_error("logical graph indexed Node lookup returned duplicate Node references");
				}
				candidateNodeIds.push_back(candidate.entity_id);
			}
			std::vector<bool> candidateLabelsMatch(nativeCandidates.candidates.size(), true);
			if (!nativeCandidates.label_scope_verified && nativeCandidates.predicates_verified &&
				!candidateNodeIds.empty() && !scan.getLabels().empty()) {
				candidateLabelsMatch = state.snapshot.readNodeLabelMatchesSorted(candidateNodeIds, scan.getLabels());
				if (candidateLabelsMatch.size() != candidateNodeIds.size()) {
					throw std::runtime_error("logical graph backend changed indexed Node label-match cardinality");
				}
				if (std::ranges::any_of(candidateLabelsMatch, [](const bool matches) { return !matches; })) {
					throw std::runtime_error(
							"logical graph native indexed Node candidates violate the requested label scope");
				}
			}
			for (size_t candidateIndex = 0U; candidateIndex < nativeCandidates.candidates.size(); ++candidateIndex) {
				checkGuard(state);
				const storage::EntityRef &candidate = nativeCandidates.candidates[candidateIndex];
				incrementBounded(state.stats.nodes_scanned, "logical-plan indexed Node lookup counter overflowed");
				if ((nativeCandidates.exact_match_verified || nativeCandidates.predicates_verified) &&
					state.allow_deferred_entity_refs && state.snapshot.supportsNativeNodeRefScans()) {
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), candidate);
					if (nativeCandidates.predicates_verified) {
						for (const auto &column: nativeCandidates.verified_property_columns) {
							row.property_cache[scan.getVariable()].insert_or_assign(column.key,
																					column.values[candidateIndex]);
						}
						if (!nodeRefScanMatches(state.snapshot, row, scan.getVariable(), scan, parameterPredicates,
												state, candidateLabelsMatch[candidateIndex])) {
							throw std::runtime_error("logical graph native indexed Node candidates violate their "
													 "verified Property predicates");
						}
					} else {
						for (const auto &[key, value]: equalityPredicates) {
							row.property_cache[scan.getVariable()].insert_or_assign(key, value);
						}
					}
					if (!visitor(std::move(row))) {
						return false;
					}
					continue;
				}
				const storage::LogicalGraphNode node = readNodeCached(state, candidate.entity_id);
				if (nodeScanMatches(state.snapshot, node, scan, parameterPredicates, state)) {
					LogicalPlanReadRow row;
					row.values.emplace(scan.getVariable(), node);
					if (!visitor(std::move(row))) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor) {
			checkGuard(state);
			switch (op.getType()) {
				case logical::LogicalOpType::LOP_SINGLE_ROW:
					if (!op.getChildren().empty()) { // ZYX_COV_EXCL_LINE: LogicalSingleRow is a concrete leaf and
													 // cannot own a child.
						throw std::runtime_error("logical SingleRow unexpectedly has a child");
					}
					return state.injected_row == nullptr ? visitor({}) : visitor(*state.injected_row);
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
					std::unordered_set<uint64_t> seenIndexedCandidateIds;
					const auto candidateVisitor = [&](const storage::IndexCandidateBatch &batch) {
						return visitIndexedCandidateBatch(batch, scan, equalityPredicates, *parameterPredicates,
													  indexedCandidateRecords, seenIndexedCandidateIds, state, visitor);
					};
					if (exactPropertyLookup && state.snapshot.supportsNativeExactNodeCandidates()) {
						if (const auto streamed = consumeCandidateStream([&] {
									return state.snapshot.visitNodeCandidatesByExactProperties(
											scan.getLabels(), equalityPredicates, state.bounds.maximum_node_records,
											state.bounds.maximum_property_entries, state.bounds.maximum_candidate_bytes,
											candidateVisitor);
								});
							streamed.has_value()) {
							return *streamed;
						}
					}
					if (rangePropertyLookup && state.snapshot.supportsNativeRangeNodeCandidates()) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (const auto streamed = consumeCandidateStream([&] {
									return state.snapshot.visitNodeCandidatesByPropertyRange(
											scan.getLabels(),
											{range.key, range.minValue, range.maxValue, range.minInclusive,
											 range.maxInclusive},
										state.bounds.maximum_node_records, state.bounds.maximum_property_entries,
										state.bounds.maximum_candidate_bytes, candidateVisitor);
								});
							streamed.has_value()) {
							return *streamed;
						}
					}
					if (compositeRangePropertyLookup && state.snapshot.supportsNativeRangeNodeCandidates()) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (const auto streamed = consumeCandidateStream([&] {
									return state.snapshot.visitNodeCandidatesByCompositePropertyRange(
										scan.getLabels(),
										{equalityPredicates,
										 {range.key, range.minValue, range.maxValue, range.minInclusive,
										  range.maxInclusive}},
										state.bounds.maximum_node_records, state.bounds.maximum_property_entries,
										state.bounds.maximum_candidate_bytes, candidateVisitor);
								});
							streamed.has_value()) {
							return *streamed;
						}
					}
					std::optional<storage::IndexCandidateBatch> exactIndexCandidates;
					std::optional<storage::IndexCandidateBatch> rangeIndexCandidates;
					std::optional<std::vector<storage::LogicalGraphNode>> indexNodes;
					if (exactPropertyLookup && state.snapshot.supportsNativeExactNodeCandidates()) {
						exactIndexCandidates = state.snapshot.findNodeCandidatesByExactProperties(
								scan.getLabels(), equalityPredicates, state.bounds.maximum_node_records,
								state.bounds.maximum_property_entries);
					} else if (exactPropertyLookup) {
						indexNodes = state.snapshot.findNodesByExactProperties(scan.getLabels(), equalityPredicates,
																			   state.bounds.maximum_node_records,
																			   state.bounds.maximum_property_entries);
					} else if (rangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (state.snapshot.supportsNativeRangeNodeCandidates()) {
							rangeIndexCandidates = state.snapshot.findNodeCandidatesByPropertyRange(
									scan.getLabels(),
									{range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
									state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
						} else {
							indexNodes = state.snapshot.findNodesByPropertyRange(
									scan.getLabels(),
									{range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
									state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
						}
					} else if (compositeRangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						if (state.snapshot.supportsNativeRangeNodeCandidates()) {
							rangeIndexCandidates = state.snapshot.findNodeCandidatesByCompositePropertyRange(
									scan.getLabels(),
									{equalityPredicates,
									 {range.key, range.minValue, range.maxValue, range.minInclusive,
									  range.maxInclusive}},
									state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
						} else {
							indexNodes = state.snapshot.findNodesByCompositePropertyRange(
									scan.getLabels(),
									{equalityPredicates,
									 {range.key, range.minValue, range.maxValue, range.minInclusive,
									  range.maxInclusive}},
									state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
						}
					}
					if (exactIndexCandidates.has_value() || rangeIndexCandidates.has_value()) {
						const storage::IndexCandidateBatch &nativeCandidates =
								exactIndexCandidates.has_value() ? *exactIndexCandidates : *rangeIndexCandidates;
						return visitIndexedCandidateBatch(nativeCandidates, scan, equalityPredicates,
														  *parameterPredicates, indexedCandidateRecords,
														  seenIndexedCandidateIds, state,
														  visitor);
					}
					if (indexNodes.has_value()) {
						if (indexNodes->size() > state.bounds.maximum_node_records) {
							throw std::runtime_error("logical graph indexed Node lookup exceeded the Node scan bound");
						}
						uint64_t previousNodeId = 0U;
						for (const storage::LogicalGraphNode &node: *indexNodes) {
							checkGuard(state);
							validateNode(node);
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
							incrementBounded(state.stats.nodes_scanned,
											 "logical-plan indexed Node lookup counter overflowed");
							// The physical index is an acceleration only. Re-evaluate all
							// logical scan predicates before exposing an indexed candidate.
							if (nodeScanMatches(state.snapshot, node, scan, *parameterPredicates, state)) {
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
								tryVisitBatchedNativeNodeRefScan(scan, *parameterPredicates, state, visitor);
						batched.has_value()) {
						return *batched;
					}
					if (state.allow_deferred_entity_refs && state.snapshot.supportsNativeNodeRefScans()) {
						bool continueVisitor = true;
						// Keep label evaluation on the page-local reference scan. The native
						// adapter resolves label tokens once and reports a match fact, avoiding
						// a second point Entity-Map read for every scanned Node.
						state.snapshot.visitNodeRefsWithLabelMatches(state.bounds.maximum_node_records, scan.getLabels(),
															 [&](const storage::EntityRef &reference, const bool labelsMatch) {
							checkGuard(state);
			validateNodeRef(reference,
					"logical graph backend returned an invalid or Edge reference from a Node scan");
								incrementBounded(state.stats.nodes_scanned,
														 "logical-plan read runtime Node counter overflowed");
							if (!labelsMatch) {
								return true;
							}
							LogicalPlanReadRow row;
							row.values.emplace(scan.getVariable(), reference);
							if (nodeRefScanMatches(state.snapshot, row, scan.getVariable(), scan,
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
								state.snapshot.scanNodesAfter(afterNodeId, state.bounds.maximum_node_page_records,
															  state.bounds.maximum_metadata_pages_per_node_page);
						if (page.nodes.size() > state.bounds.maximum_node_page_records) {
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
							incrementBounded(state.stats.nodes_scanned,
											 "logical-plan read runtime Node counter overflowed");
							if (nodeScanMatches(state.snapshot, node, scan, *parameterPredicates, state)) {
								LogicalPlanReadRow row;
								row.values.emplace(scan.getVariable(), node);
								if (!visitor(std::move(row))) {
									return false;
								}
							}
							if (state.stats.nodes_scanned == state.bounds.maximum_node_records) {
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
					// A WHERE clause directly above a single-hop traversal is a common
					// shape for Edge-property predicates.  Feed its safe equality
					// constraints into the same global topology path used by inline
					// relationship properties, while retaining the Filter evaluator as
					// the semantic authority.
					if (filterChild.getType() == logical::LogicalOpType::LOP_TRAVERSAL) {
						const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(filterChild);
						if (const auto edgeProperties = inspectDirectEdgePropertyFilter(
								*filter.getPredicate(), traversal.getEdgeVar(), state);
							edgeProperties.has_value()) {
							if (const std::optional<bool> direct = tryVisitDirectTraversal(
									traversal, state,
									[&](LogicalPlanReadRow row) {
										const PropertyValue result = evaluateScalarExpression(*filter.getPredicate(), row, state);
										return booleanOrNull(result) == true ? visitor(std::move(row)) : true;
									}, &*edgeProperties);
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
						const bool hasIndexableScanShape = filterScan != nullptr &&
							(!filterScan->getPropertyPredicates().empty() || !filterScan->getRangePredicates().empty() ||
							 filterScan->getCompositeEquality().has_value());
						if (!hasIndexableScanShape) {
							const std::vector<const expressions::Expression *> propertyExpressions{filter.getPredicate().get()};
							if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
									filterChild, propertyExpressions, state,
									[&](LogicalPlanReadRow row) {
										return visitor(std::move(row));
									},
								[&](LogicalPlanReadRow &row, ExecutionState &predicateState) {
										const PropertyValue result = evaluateScalarExpression(*filter.getPredicate(), row, predicateState);
										return booleanOrNull(result) == true;
									});
								batched.has_value()) {
								return *batched;
							}
						}
					return visitOperator(filterChild, state, [&](LogicalPlanReadRow row) {
						const PropertyValue result = evaluateScalarExpression(*filter.getPredicate(), row, state);
						const std::optional<bool> predicate = booleanOrNull(result);
						return predicate == true ? visitor(std::move(row)) : true;
					});
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
					std::unordered_set<std::string> distinctRows;
				size_t distinctBytes = 0U;
					const auto emitProjectedRow = [&](LogicalPlanReadRow row) {
							LogicalPlanReadRow projected;
							projected.values.reserve(items.size());
							for (const logical::LogicalProjectItem &item: items) {
							checkGuard(state);
							projected.values.emplace(item.alias, evaluateExpression(*item.expression, row, state));
						}
						if (project.isDistinct()) {
							const std::string key = distinctRowKey(projected, items);
							if (distinctRows.contains(key)) {
								return true;
							}
							if (distinctRows.size() == state.bounds.maximum_distinct_rows) {
								throw std::runtime_error("logical-plan DISTINCT exceeded its row bound");
							}
							const size_t keyBytes = distinctKeyStorageBytes(
									key, "logical-plan DISTINCT materialization size overflowed");
							if (keyBytes > state.bounds.maximum_materialized_bytes -
									std::min(distinctBytes, state.bounds.maximum_materialized_bytes)) {
								throw std::runtime_error("logical-plan DISTINCT exceeded its materialized byte bound");
							}
								static_cast<void>(distinctRows.insert(key));
							distinctBytes = checkedSizeAdd(distinctBytes, keyBytes,
									"logical-plan DISTINCT materialization size overflowed");
							}
							return visitor(std::move(projected));
						};

						const logical::LogicalOperator &projectChild = requireOnlyChild(project);
						std::vector<const expressions::Expression *> projectionExpressions;
						projectionExpressions.reserve(items.size());
						for (const logical::LogicalProjectItem &item: items) {
							// A projection may mix batchable Property expressions with entity,
							// path, or function expressions that intentionally remain on the
							// semantic evaluator.  Prefetch the supported subset instead of
							// discarding batching for the whole projection.
							const auto properties = inspectBatchableEntityPropertyPredicates(*item.expression);
							if (properties.has_value() && !properties->empty()) {
								projectionExpressions.push_back(item.expression.get());
							}
						}
						if (!projectionExpressions.empty()) {
							if (const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
								projectChild, projectionExpressions, state, emitProjectedRow);
								batched.has_value()) {
								return *batched;
							}
						}
						return visitOperator(projectChild, state, emitProjectedRow);
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
					if (const std::optional<bool> twoHop = tryVisitAnchoredTwoHopRelationshipCount(aggregate, state, visitor);
						twoHop.has_value()) {
						return *twoHop;
					}
					if (const std::optional<bool> anchored = tryVisitAnchoredRelationshipCount(aggregate, state, visitor);
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
							(function == AggregateFunctionType::AGG_COUNT && !item.argument &&
							 item.distinct)) {
							throw std::invalid_argument("logical-plan Aggregate has an invalid aggregate item");
						}
						outputAliases.push_back(item.alias);
						functions.push_back(function);
						percentiles.push_back(aggregatePercentile(item, function, state));
					}
					bool countOnlyAggregate = groupExpressions.empty();
					for (size_t index = 0U; countOnlyAggregate && index < aggregations.size(); ++index) {
						countOnlyAggregate = functions[index] == AggregateFunctionType::AGG_COUNT && !aggregations[index].distinct;
					}

					struct AggregateGroup {
						std::vector<PropertyValue> values;
						std::vector<std::unique_ptr<AggregateAccumulator>> accumulators;
						std::vector<std::unordered_set<std::string>> distinct_values;
						size_t accounted_bytes = 0U;
					};
					std::vector<AggregateGroup> groups;
					std::unordered_map<std::string, size_t> groupIndexes;
					size_t aggregateMemoryBytes = 0U;
					const auto chargeAggregateBytes = [&](const size_t bytes) {
						if (bytes > state.bounds.maximum_materialized_bytes -
								std::min(aggregateMemoryBytes, state.bounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical-plan Aggregate exceeded its materialization byte bound");
						}
						aggregateMemoryBytes = checkedSizeAdd(aggregateMemoryBytes, bytes,
							"logical-plan Aggregate materialization size overflowed");
					};
					const auto createGroup = [&](std::vector<PropertyValue> values) {
						if (groups.size() == state.bounds.maximum_distinct_rows ||
							groups.size() == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its group bound");
						}
						size_t groupBytes = sizeof(AggregateGroup);
						for (const PropertyValue &value: values) {
							groupBytes = checkedSizeAdd(groupBytes, property_utils::getPropertyValueSize(value),
									"logical-plan Aggregate group size overflowed");
						}
						AggregateGroup group;
						group.values = std::move(values);
						groupBytes = checkedSizeAdd(groupBytes, group.values.capacity() * sizeof(PropertyValue),
								"logical-plan Aggregate group size overflowed");
						group.accumulators.reserve(aggregations.size());
						group.distinct_values.resize(aggregations.size());
						groupBytes = checkedSizeAdd(groupBytes, aggregations.size() *
								(sizeof(std::unique_ptr<AggregateAccumulator>) + sizeof(std::unordered_set<std::string>)),
								"logical-plan Aggregate group size overflowed");
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							std::unique_ptr<AggregateAccumulator> accumulator =
									createAccumulator(functions[index], false, percentiles[index]);
							if (!accumulator) { // aggregateFunctionType accepts exactly the factory's supported enum
												// values.
								throw std::logic_error("logical-plan Aggregate could not create an accumulator");
							}
							group.accumulators.push_back(std::move(accumulator));
						}
						group.accounted_bytes = groupBytes;
						chargeAggregateBytes(groupBytes);
						groups.push_back(std::move(group));
						return groups.size() - 1U;
					};
					if (groupExpressions.empty()) {
						static_cast<void>(createGroup({}));
					}

					size_t inputRows = 0U;
			const auto consumeAggregateRow = [&](LogicalPlanReadRow row) {
				checkGuard(state);
				if (inputRows == state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				++inputRows;
				std::vector<PropertyValue> groupValues;
				groupValues.reserve(groupExpressions.size());
				std::string groupKey;
				for (const std::shared_ptr<expressions::Expression> &expression: groupExpressions) {
					const PropertyValue value = scalarValue(evaluateExpression(*expression, row, state));
					appendDistinctPropertyValue(groupKey, value);
					groupValues.push_back(value);
				}
				size_t groupIndex = 0U;
				if (!groupExpressions.empty()) {
					const auto found = groupIndexes.find(groupKey);
					if (found == groupIndexes.end()) {
						groupIndex = createGroup(std::move(groupValues));
						const size_t keyBytes = groupKey.size();
						chargeAggregateBytes(keyBytes);
						groups[groupIndex].accounted_bytes = checkedSizeAdd(groups[groupIndex].accounted_bytes,
								keyBytes, "logical-plan Aggregate group size overflowed");
						groupIndexes.emplace(std::move(groupKey), groupIndex);
					} else {
						groupIndex = found->second;
					}
				}
				AggregateGroup &group = groups[groupIndex];
				for (size_t index = 0U; index < aggregations.size(); ++index) {
					const logical::LogicalAggItem &item = aggregations[index];
					const size_t beforeBytes = group.accumulators[index]->retainedBytes();
					if (!item.argument) {
						group.accumulators[index]->updateRow();
					} else {
						// A non-distinct COUNT over a direct Entity Property only needs
						// presence.  Native snapshots can answer this from the compact
						// token directory without decoding the value payload, which keeps
						// large properties out of the transient row and session caches.
						std::optional<bool> presence;
						if (countOnlyAggregate && functions[index] == AggregateFunctionType::AGG_COUNT &&
								!item.distinct && item.argument->getExpressionType() == expressions::ExpressionType::PROPERTY_ACCESS) {
							const auto *property = dynamic_cast<const expressions::VariableReferenceExpression *>(item.argument.get());
							if (property != nullptr && property->hasProperty()) {
								const auto binding = row.values.find(property->getVariableName());
								if (binding != row.values.end()) {
									if (const auto *node = std::get_if<storage::LogicalGraphNode>(&binding->second)) {
										presence = state.snapshot.nodePropertyExists(node->node_id, property->getPropertyName());
									} else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&binding->second)) {
										presence = state.snapshot.edgePropertyExists(edge->edge_id, property->getPropertyName());
									} else if (const auto *reference = std::get_if<storage::EntityRef>(&binding->second)) {
										validateEntityRef(*reference);
										presence = reference->is_edge
											? state.snapshot.edgePropertyExists(reference->entity_id, property->getPropertyName())
											: state.snapshot.nodePropertyExists(reference->entity_id, property->getPropertyName());
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
							std::string key;
							appendDistinctPropertyValue(key, value);
							const auto [inserted, isNew] = group.distinct_values[index].insert(key);
							if (!isNew) {
								continue;
							}
							chargeAggregateBytes(inserted->size());
							group.accounted_bytes = checkedSizeAdd(group.accounted_bytes, inserted->size(),
									"logical-plan Aggregate group size overflowed");
						}
						group.accumulators[index]->update(value);
					}
					const size_t afterBytes = group.accumulators[index]->retainedBytes();
					if (afterBytes >= beforeBytes) {
						chargeAggregateBytes(afterBytes - beforeBytes);
						group.accounted_bytes = checkedSizeAdd(group.accounted_bytes, afterBytes - beforeBytes,
								"logical-plan Aggregate group size overflowed");
					} else {
						aggregateMemoryBytes -= beforeBytes - afterBytes;
						group.accounted_bytes -= beforeBytes - afterBytes;
					}
				}
				return true;
			};
			const size_t aggregateBatchCapacity = std::min(DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_rows);
			BoundedBatch<LogicalPlanReadRow> aggregateRows(
					aggregateBatchCapacity, state.bounds.maximum_materialized_bytes,
					[](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row); });
			aggregateRows.reserve(aggregateBatchCapacity);
			const auto flushAggregateRows = [&]() {
				if (aggregateRows.empty()) {
					return true;
				}
				SelectionVector selected(aggregateRows.size());
				bool keepGoing = true;
				static_cast<void>(selected.forEachSelectedUntil([&](const size_t index) {
					if (!keepGoing) {
						return false;
					}
					keepGoing = consumeAggregateRow(std::move(aggregateRows.values()[index]));
					return keepGoing;
				}));
				aggregateRows.clear();
				debug::PerfTrace::addValue("logical_plan.aggregate.batch_flushes", 1);
				return keepGoing;
			};
			const auto consumeAggregateBatch = [&](LogicalPlanReadRow row) {
				// Count-like aggregates do not retain the input payload. Preserve their
				// streaming semantics when one transient row is larger than the batch
				// budget instead of rejecting a value that the accumulator can discard.
				const size_t rowBytes = logicalReadRowBytes(row);
				if (rowBytes > state.bounds.maximum_materialized_bytes ||
						(rowBytes > state.bounds.maximum_materialized_bytes -
								std::min(aggregateRows.bytes(), state.bounds.maximum_materialized_bytes))) {
					if (!flushAggregateRows()) {
						return false;
					}
					return consumeAggregateRow(std::move(row));
				}
				aggregateRows.append(std::move(row));
				return aggregateRows.size() != aggregateBatchCapacity || flushAggregateRows();
			};
			std::vector<const expressions::Expression *> aggregatePropertyExpressions;
			aggregatePropertyExpressions.reserve(groupExpressions.size() + aggregations.size());
			for (const auto &expression: groupExpressions) {
				aggregatePropertyExpressions.push_back(expression.get());
			}
			for (const logical::LogicalAggItem &item: aggregations) {
				if (item.argument) {
					aggregatePropertyExpressions.push_back(item.argument.get());
				}
			}
			std::optional<bool> batchedProperties;
			// COUNT is cardinality-only: it must not force a potentially very large
			// Property payload into the bounded prefetch batch.  Keeping this path
			// streaming lets COUNT(n.large_value) succeed under a small materialized
			// byte budget, while DISTINCT COUNT and grouped aggregates still use the
			// selected-column batch because they retain values for identity/grouping.
			if (!aggregatePropertyExpressions.empty() && !countOnlyAggregate) {
				batchedProperties = tryVisitBatchedPropertyExpressions(requireOnlyChild(aggregate),
						aggregatePropertyExpressions, state, consumeAggregateBatch);
			}
			const bool completed = batchedProperties.has_value()
						? *batchedProperties
						: visitOperator(requireOnlyChild(aggregate), state, consumeAggregateBatch);
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Aggregate could not prove that its complete input was read");
					}
					if (!flushAggregateRows()) {
						throw std::runtime_error("logical-plan Aggregate stopped before flushing its input batch");
					}
					for (AggregateGroup &group: groups) {
						LogicalPlanReadRow row;
						for (size_t index = 0U; index < group.values.size(); ++index) {
							row.values.emplace(outputAliases[index], std::move(group.values[index]));
						}
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							row.values.emplace(aggregations[index].alias, group.accumulators[index]->getResult());
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
					const std::vector<std::string> leftColumns = left->getOutputVariables();
					const std::vector<std::string> rightColumns = right->getOutputVariables();
					const auto validateColumns = [](const std::vector<std::string> &columns) {
						std::unordered_set<std::string> declaredColumns;
						declaredColumns.reserve(columns.size());
						for (const std::string &column: columns) {
							if (column.empty() || !declaredColumns.insert(column).second) {
								throw std::invalid_argument(
										"logical-plan Join has invalid output columns");
							}
						}
					};
					validateColumns(leftColumns);
					validateColumns(rightColumns);

					BoundedBatch<LogicalPlanReadRow> rightRows(
						state.bounds.maximum_rows, state.bounds.maximum_materialized_bytes,
						[](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row); });
					rightRows.reserve(state.bounds.maximum_rows);
					std::vector<std::string> sharedColumns;
					sharedColumns.reserve(std::min(leftColumns.size(), rightColumns.size()));
					std::unordered_set<std::string> rightColumnSet;
					rightColumnSet.reserve(rightColumns.size());
					for (const std::string &column: rightColumns) {
						rightColumnSet.insert(column);
					}
					for (const std::string &column: leftColumns) {
						if (rightColumnSet.contains(column)) {
							sharedColumns.push_back(column);
						}
					}
					// Build a bounded hash index over shared bindings. A join with no
					// shared columns intentionally retains Cartesian semantics by using
					// the same candidate list for every left row.
					std::unordered_map<std::string, std::vector<size_t>> rightRowsByKey;
					size_t rightIndexBytes = 0U;
					const bool rightCompleted = visitOperator(*right, state, [&](LogicalPlanReadRow row) {
						if (rightRows.size() == state.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan Join exceeded its materialized right-side row bound");
						}
						LogicalPlanReadRow normalized = normalizeOutputRow(row, rightColumns);
						const std::string key = bindingKey(normalized, sharedColumns);
						const bool newKey = !rightRowsByKey.contains(key);
						const size_t rowBytes = logicalReadRowBytes(normalized);
						const size_t indexEntryBytes = newKey
								? checkedSizeAdd(key.size(), sizeof(std::string) + sizeof(std::vector<size_t>),
										"logical-plan Join index size overflowed")
								: 0U;
						const size_t additionalBytes = checkedSizeAdd(
								checkedSizeAdd(rowBytes, indexEntryBytes, "logical-plan Join materialization size overflowed"),
								sizeof(size_t),
								"logical-plan Join materialization size overflowed");
						const size_t accountedBytes = checkedSizeAdd(rightRows.bytes(), rightIndexBytes,
								"logical-plan Join materialization size overflowed");
						if (additionalBytes > state.bounds.maximum_materialized_bytes -
								std::min(accountedBytes, state.bounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical-plan Join exceeded its materialized byte bound");
						}
						rightRows.append(std::move(normalized));
						rightRowsByKey[key].push_back(rightRows.size() - 1U);
						rightIndexBytes = checkedSizeAdd(rightIndexBytes, additionalBytes,
								"logical-plan Join index size overflowed");
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
						const LogicalPlanReadRow normalizedLeft = normalizeOutputRow(row, leftColumns);
						const std::string key = bindingKey(normalizedLeft, sharedColumns);
						const auto candidates = rightRowsByKey.find(key);
						if (candidates == rightRowsByKey.end()) {
							return true;
						}
						for (const size_t rightIndex: candidates->second) {
							checkGuard(state);
							const LogicalPlanReadRow &rightRow = rightRows.values()[rightIndex];
							LogicalPlanReadRow combined = normalizedLeft;
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
								combined.values.emplace(column, value);
							}
							if (!compatible) {
								continue;
							}
							for (const auto &[column, properties]: rightRow.property_cache) {
								const auto existing = combined.property_cache.find(column);
								if (existing != combined.property_cache.end() && existing->second != properties) {
									throw std::runtime_error(
											"logical-plan Join received incompatible Property cache entries");
								}
								if (existing == combined.property_cache.end()) {
									combined.property_cache.emplace(column, properties);
									if (rightRow.complete_entity_property_cache.contains(column)) {
										combined.complete_entity_property_cache.insert(column);
									}
								}
							}
							if (!visitor(std::move(combined))) {
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
					const std::vector<std::string> inputColumns = input->getOutputVariables();
					const std::vector<std::string> patternColumns = pattern->getOutputVariables();
					std::unordered_set<std::string> inputColumnSet;
					inputColumnSet.reserve(inputColumns.size());
					for (const std::string &column: inputColumns) {
						if (column.empty() || !inputColumnSet.insert(column).second) {
							throw std::invalid_argument("logical-plan OptionalMatch input has invalid output columns");
						}
					}
					std::unordered_set<std::string> patternColumnSet;
					patternColumnSet.reserve(patternColumns.size());
					std::vector<std::string> sharedColumns;
					sharedColumns.reserve(std::min(inputColumns.size(), patternColumns.size()));
					for (const std::string &column: patternColumns) {
						if (column.empty() || !patternColumnSet.insert(column).second) {
							throw std::invalid_argument(
									"logical-plan OptionalMatch pattern has invalid output columns");
						}
						if (inputColumnSet.contains(column)) {
							sharedColumns.push_back(column);
						}
					}
					for (const std::string &variable: optional.getRequiredVariables()) {
						if (variable.empty() ||
							(!inputColumnSet.contains(variable) && !patternColumnSet.contains(variable))) {
							throw std::invalid_argument("logical-plan OptionalMatch has an invalid required variable");
						}
					}

					std::unordered_map<std::string, std::vector<size_t>> patternRowsByKey;
					size_t patternIndexBytes = 0U;
					BoundedBatch<LogicalPlanReadRow> patternRows(
							state.bounds.maximum_rows, state.bounds.maximum_materialized_bytes,
							[](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row); });
					patternRows.reserve(state.bounds.maximum_rows);
					const bool patternCompleted = visitOperator(*pattern, state, [&](LogicalPlanReadRow row) {
						if (patternRows.size() == state.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan OptionalMatch exceeded its materialized pattern row bound");
						}
						LogicalPlanReadRow normalized = normalizeOutputRow(row, patternColumns);
						const std::string key = bindingKey(normalized, sharedColumns);
						const bool newPatternKey = !patternRowsByKey.contains(key);
						const size_t rowBytes = logicalReadRowBytes(normalized);
						const size_t accountedBytes = checkedSizeAdd(patternRows.bytes(), patternIndexBytes,
								"logical-plan OptionalMatch materialization size overflowed");
						// Account for the owning hash entry and each match-list position as
						// well as the encoded binding key.  Without this metadata charge a
						// large number of tiny pattern rows could exceed the byte budget while
						// their row payloads alone still appeared small.
						const size_t indexEntryBytes = newPatternKey
								? checkedSizeAdd(key.size(), sizeof(std::string) + sizeof(std::vector<size_t>),
										"logical-plan OptionalMatch index size overflowed")
								: 0U;
						const size_t additionalBytes = checkedSizeAdd(
								checkedSizeAdd(rowBytes, indexEntryBytes,
										"logical-plan OptionalMatch materialization size overflowed"),
								sizeof(size_t), "logical-plan OptionalMatch materialization size overflowed");
						if (additionalBytes > state.bounds.maximum_materialized_bytes -
								std::min(accountedBytes, state.bounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical-plan OptionalMatch exceeded its materialized byte bound");
						}
						patternRows.append(std::move(normalized));
						patternRowsByKey[key].push_back(patternRows.size() - 1U);
						patternIndexBytes = checkedSizeAdd(patternIndexBytes, indexEntryBytes,
								"logical-plan OptionalMatch index size overflowed");
						patternIndexBytes = checkedSizeAdd(patternIndexBytes, sizeof(size_t),
								"logical-plan OptionalMatch index size overflowed");
						return true;
					});
					if (!patternCompleted) {
						throw std::runtime_error(
								"logical-plan OptionalMatch could not prove that its pattern input was exhausted");
					}

					bool stoppedByVisitor = false;
					const bool inputCompleted = visitOperator(*input, state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadRow normalizedInput = normalizeOutputRow(row, inputColumns);
						const auto matches = patternRowsByKey.find(bindingKey(normalizedInput, sharedColumns));
						if (matches == patternRowsByKey.end()) {
							LogicalPlanReadRow nullExtended = normalizedInput;
							for (const std::string &column: patternColumns) {
								if (!inputColumnSet.contains(column)) {
									nullExtended.values.emplace(column, PropertyValue());
								}
							}
							if (!visitor(std::move(nullExtended))) {
								stoppedByVisitor = true;
								return false;
							}
							return true;
						}
						for (const size_t matchIndex: matches->second) {
							checkGuard(state);
							const LogicalPlanReadRow &match = patternRows.values()[matchIndex];
							LogicalPlanReadRow combined = normalizedInput;
							for (const auto &[column, value]: match.values) {
								if (!inputColumnSet.contains(column)) {
									combined.values.emplace(column, value);
								}
							}
							for (const auto &[column, properties]: match.property_cache) {
								if (!inputColumnSet.contains(column)) {
									combined.property_cache.emplace(column, properties);
									if (match.complete_entity_property_cache.contains(column)) {
										combined.complete_entity_property_cache.insert(column);
									}
								}
							}
							if (!visitor(std::move(combined))) {
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
						LogicalPlanReadRow injected;
						injected.values.reserve(subquery.getImportedVars().size());
						injected.property_cache.reserve(subquery.getImportedVars().size());
						for (const std::string &variable: subquery.getImportedVars()) {
							if (variable.empty()) {
								throw std::invalid_argument("logical-plan CallSubquery has an empty imported variable");
							}
							const auto value = inputRow.values.find(variable);
							if (value == inputRow.values.end() ||
								!injected.values.emplace(variable, value->second).second) {
								throw std::runtime_error(
										"logical-plan CallSubquery could not resolve a unique imported variable");
							}
							if (const auto properties = inputRow.property_cache.find(variable);
								properties != inputRow.property_cache.end()) {
								injected.property_cache.emplace(variable, properties->second);
								if (inputRow.complete_entity_property_cache.contains(variable)) {
									injected.complete_entity_property_cache.insert(variable);
								}
							}
						}
						struct InjectionScope final {
							ExecutionState &state;
							const LogicalPlanReadRow *previous;

							InjectionScope(ExecutionState &executionState, const LogicalPlanReadRow &row) :
								state(executionState), previous(executionState.injected_row) {
								state.injected_row = &row;
							}

							~InjectionScope() { state.injected_row = previous; }
						};
						InjectionScope injection(state, injected);
						bool emitted = false;
						const bool completed = visitOperator(*body, state, [&](LogicalPlanReadRow subqueryRow) {
							LogicalPlanReadRow normalized = normalizeOutputRow(subqueryRow, returnedVariables);
							LogicalPlanReadRow combined = inputRow;
							for (const auto &[variable, value]: normalized.values) {
								if (combined.values.contains(variable)) {
									throw std::runtime_error(
											"logical-plan CallSubquery attempted to rebind an outer variable");
								}
								combined.values.emplace(variable, value);
							}
							for (const auto &[variable, properties]: normalized.property_cache) {
								if (!combined.property_cache.emplace(variable, properties).second) {
									throw std::runtime_error("logical-plan CallSubquery attempted to rebind an outer "
															 "Property cache entry");
								}
								if (normalized.complete_entity_property_cache.contains(variable)) {
									combined.complete_entity_property_cache.insert(variable);
								}
							}
							emitted = true;
							return visitor(std::move(combined));
						});
						if (!completed) {
							return false;
						}
						if (!emitted && returnedVariables.empty()) {
							return visitor(std::move(inputRow));
						}
						return true;
					};
					if (input == nullptr) {
						return executeForInput({});
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
					return visitOperator(requireOnlyChild(namedPath), state, [&](LogicalPlanReadRow row) {
						std::vector<storage::LogicalGraphNode> boundNodeRecords;
						size_t boundNodeCapacity = nodeVariables.size();
						if (capturedVariableLengthPath) {
							const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured != row.path_cache.end()) {
								boundNodeCapacity = checkedSizeAdd(boundNodeCapacity, captured->second.nodes.size(),
										"logical-plan NamedPath Node capacity overflowed");
							}
						}
					boundNodeRecords.reserve(boundNodeCapacity);
					std::vector<const storage::LogicalGraphNode *> boundNodes;
					boundNodes.reserve(nodeVariables.size());
					std::vector<storage::EntityRef> deferredBoundNodes;
					deferredBoundNodes.reserve(nodeVariables.size());
					for (const std::string &variable: nodeVariables) {
						const LogicalPlanReadValue &value = requireBinding(row, variable);
						if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
							validateNodeRef(*reference,
									"logical-plan NamedPath received an invalid or Edge reference where a Node was required");
							deferredBoundNodes.push_back(*reference);
						} else {
							boundNodeRecords.push_back(materializeNodeValue(state, value));
						}
					}
					const std::vector<storage::LogicalGraphNode> materializedBoundNodes =
							materializeNodeReferences(deferredBoundNodes, state);
					for (const storage::LogicalGraphNode &node: materializedBoundNodes) {
						boundNodeRecords.push_back(node);
					}
					for (const std::string &variable: nodeVariables) {
						const LogicalPlanReadValue &value = requireBinding(row, variable);
						const uint64_t id = nodeBindingId(value);
						const auto position = std::ranges::find(boundNodeRecords, id, &storage::LogicalGraphNode::node_id);
						if (position == boundNodeRecords.end()) {
							throw std::runtime_error("logical-plan NamedPath lost a Node binding");
						}
						boundNodes.push_back(&*position);
					}
						std::vector<const storage::LogicalGraphNode *> nodes = boundNodes;
						std::vector<const storage::LogicalGraphEdge *> edges;
						std::vector<storage::LogicalGraphEdge> boundEdgeRecords;
						if (capturedVariableLengthPath) {
									const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured == row.path_cache.end() || captured->second.nodes.size() < 2U ||
										captured->second.nodes.size() != captured->second.edges.size() +
																	captured->second.edge_refs.size() + 1U ||
										(!captured->second.node_refs.empty() &&
										 captured->second.node_refs.size() != captured->second.nodes.size()) ||
										(captured->second.edges.size() != 0U && !captured->second.edge_refs.empty()) ||
										captured->second.nodes.front().node_id != boundNodes.front()->node_id ||
																captured->second.nodes.back().node_id != boundNodes.back()->node_id) {
								throw std::runtime_error("logical-plan NamedPath lost its variable-length traversal route");
							}
							nodes.clear();
							nodes.reserve(captured->second.nodes.size());
							std::vector<storage::EntityRef> deferredCapturedNodes;
							deferredCapturedNodes.reserve(captured->second.node_refs.size());
							if (!captured->second.node_refs.empty()) {
								for (const storage::EntityRef &reference: captured->second.node_refs) {
									if (reference.entity_id != 0U) {
										deferredCapturedNodes.push_back(reference);
									}
								}
							}
							const std::vector<storage::LogicalGraphNode> materializedCapturedNodes =
									materializeNodeReferences(deferredCapturedNodes, state);
							size_t materializedCapturedOrdinal = 0U;
							for (size_t nodeIndex = 0U; nodeIndex < captured->second.nodes.size(); ++nodeIndex) {
								const storage::LogicalGraphNode *node = &captured->second.nodes[nodeIndex];
								if (!captured->second.node_refs.empty() &&
									captured->second.node_refs[nodeIndex].entity_id != 0U) {
									const storage::EntityRef &reference = captured->second.node_refs[nodeIndex];
									validateNodeRef(reference,
											"logical-plan NamedPath has an invalid deferred Node reference");
									if (reference.entity_id != node->node_id) {
										throw std::runtime_error("logical-plan NamedPath has an invalid deferred Node reference");
									}
									if (materializedCapturedOrdinal >= materializedCapturedNodes.size()) {
										throw std::runtime_error(
												"logical-plan NamedPath lost a deferred Node during batch materialization");
									}
									if (materializedCapturedNodes[materializedCapturedOrdinal].node_id != reference.entity_id) {
										throw std::runtime_error(
												"logical-plan NamedPath changed deferred Node identity during batch materialization");
									}
									boundNodeRecords.push_back(materializedCapturedNodes[materializedCapturedOrdinal++]);
									node = &boundNodeRecords.back();
								} else {
									validateNode(*node);
								}
								nodes.push_back(node);
							}
							boundEdgeRecords.reserve(captured->second.edges.size() + captured->second.edge_refs.size());
							edges.reserve(captured->second.edges.size() + captured->second.edge_refs.size());
									for (const storage::LogicalGraphEdge &edge: captured->second.edges) {
										validateEdge(edge);
										edges.push_back(&edge);
									}
									const std::vector<storage::LogicalGraphEdge> materializedReferences =
										materializeEdgeReferences(captured->second.edge_refs, state);
									for (const storage::LogicalGraphEdge &edge: materializedReferences) {
										boundEdgeRecords.push_back(edge);
										edges.push_back(&boundEdgeRecords.back());
									}
						} else {
							boundEdgeRecords.reserve(edgeVariables.size());
							edges.reserve(edgeVariables.size());
							struct PendingNamedEdge {
								std::optional<storage::EntityRef> reference;
								std::optional<storage::LogicalGraphEdge> complete;
							};
							std::vector<PendingNamedEdge> pendingEdges;
							pendingEdges.reserve(edgeVariables.size());
							std::vector<storage::EntityRef> deferredReferences;
							deferredReferences.reserve(edgeVariables.size());
							for (const std::string &variable: edgeVariables) {
								const LogicalPlanReadValue &value = requireBinding(row, variable);
								if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
									validateEdgeRef(*reference,
											"logical-plan NamedPath received an invalid or Node reference where an Edge was required");
									pendingEdges.push_back({*reference, std::nullopt});
									deferredReferences.push_back(*reference);
								} else {
									pendingEdges.push_back({std::nullopt, materializeEdgeValue(state, value)});
								}
							}
							const std::vector<storage::LogicalGraphEdge> materializedReferences =
								materializeEdgeReferences(deferredReferences, state);
							size_t deferredOrdinal = 0U;
							for (PendingNamedEdge &pending: pendingEdges) {
								if (pending.reference.has_value()) {
									if (deferredOrdinal >= materializedReferences.size()) {
										throw std::runtime_error(
												"logical-plan NamedPath lost a deferred Edge during batch materialization");
									}
									pending.complete = materializedReferences[deferredOrdinal++];
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

						MaterializedPropertyCache materialized;
						const auto pathIdValue = [](uint64_t id) {
							if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
								throw std::overflow_error(
										"logical-plan NamedPath cannot represent an entity ID in a Property value");
							}
							return PropertyValue(static_cast<int64_t>(id));
						};
						std::vector<PropertyValue> path;
						path.reserve(nodes.size() + edges.size());
						for (size_t index = 0U; index < nodes.size(); ++index) {
							const storage::LogicalGraphNode &node = *nodes[index];
							PropertyValue::MapType nodeMap;
							nodeMap.emplace("_type", PropertyValue("node"));
							nodeMap.emplace("_id", pathIdValue(node.node_id));
							for (const auto &[key, value]:
								 materializeEntityProperties(row, node, state, materialized)) {
								nodeMap.insert_or_assign(key, value);
							}
							path.emplace_back(std::move(nodeMap));
							if (index == edges.size()) {
								continue;
							}
							const storage::LogicalGraphEdge &edge = *edges[index];
							PropertyValue::MapType edgeMap;
							edgeMap.emplace("_type", PropertyValue("relationship"));
							edgeMap.emplace("_id", pathIdValue(edge.edge_id));
							for (const auto &[key, value]:
								 materializeEntityProperties(row, edge, state, materialized)) {
								edgeMap.insert_or_assign(key, value);
							}
							path.emplace_back(std::move(edgeMap));
						}
						if (!bindValue(row, namedPath.getPathVariable(), PropertyValue(std::move(path)))) {
							throw std::runtime_error("logical-plan NamedPath attempted to rebind its path variable");
						}
						row.property_cache.erase(namedPath.getPathVariable());
						row.complete_entity_property_cache.erase(namedPath.getPathVariable());
						return visitor(std::move(row));
					});
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
					if (state.context != nullptr && state.context->maxVarLengthDepth.has_value()) {
						if (*state.context->maxVarLengthDepth < 0) {
							throw std::invalid_argument(
									"logical-plan variable-length Traversal has a negative query depth bound");
						}
						maximumHops = std::min(maximumHops, *state.context->maxVarLengthDepth);
					}
					if (static_cast<uint64_t>(maximumHops) > state.bounds.maximum_node_records) {
						throw std::runtime_error(
								"logical-plan variable-length Traversal exceeds its configured depth bound");
					}
					if (maximumHops != 0 &&
						state.bounds.maximum_edges_per_node >
								std::numeric_limits<size_t>::max() / static_cast<size_t>(maximumHops)) {
						throw std::overflow_error("logical-plan variable-length Traversal edge budget overflowed");
					}
					const size_t maximumEdgeVisits =
							maximumHops == 0 ? 0U
											 : state.bounds.maximum_edges_per_node * static_cast<size_t>(maximumHops);
					// An exact target lookup can build one reverse reachability envelope for
					// the whole operator. The forward DFS still owns path semantics; this
					// index-driven envelope only removes branches that cannot reach a target.
					bool reversePruningEnabled = false;
					std::unordered_map<uint64_t, int> reverseDistance;
					if (state.snapshot.supportsNativeBatchAdjacentEdgeRefs() &&
						!traversal.getTargetProperties().empty()) {
						const auto targetNodes = state.snapshot.findNodesByExactProperties(
								traversal.getTargetLabels(), traversal.getTargetProperties(),
								state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
						if (targetNodes.has_value()) {
							reversePruningEnabled = true;
							if (targetNodes->size() > state.bounds.maximum_node_records) {
								throw std::runtime_error(
										"logical graph exact target lookup exceeded the variable-length Node bound");
							}
							std::vector<uint64_t> frontier;
							frontier.reserve(targetNodes->size());
							for (const storage::LogicalGraphNode &target: *targetNodes) {
								validateNode(target);
								if (!reverseDistance.emplace(target.node_id, 0).second) {
									throw std::runtime_error(
											"logical graph exact target lookup returned a duplicate Node");
								}
								frontier.push_back(target.node_id);
							}
							std::ranges::sort(frontier);
							const std::optional<uint64_t> reverseTypeToken = traversal.getEdgeType().empty()
									? std::nullopt
									: state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
							size_t reverseEdgeVisits = 0U;
							const auto reverseDirection = [&](const std::span<const uint64_t> currentFrontier,
									const storage::LogicalGraphReadDirection adjacencyDirection) {
								if (reverseEdgeVisits == state.bounds.maximum_edge_records) {
									throw std::runtime_error(
											"logical-plan reverse reachability exceeded its Edge bound");
								}
								std::vector<uint64_t> nextFrontier;
								const size_t remaining = state.bounds.maximum_edge_records - reverseEdgeVisits;
								const size_t edgeVisitBound = checkedSizeAdd(
										remaining, 1U, "logical-plan reverse reachability Edge bound overflowed");
								state.snapshot.visitAdjacentEdgeRefsForNodes(
										currentFrontier, adjacencyDirection, edgeVisitBound,
										[&](const storage::EntityRef &reference) {
											checkGuard(state);
											validateEdgeRef(reference,
													"logical graph reverse reachability returned an invalid or non-Edge reference");
											if (reverseEdgeVisits == state.bounds.maximum_edge_records) {
												throw std::runtime_error(
														"logical-plan reverse reachability exceeded its Edge bound");
											}
											++reverseEdgeVisits;
											incrementBounded(state.stats.edges_scanned,
													"logical-plan reverse reachability Edge counter overflowed");
											if (reverseTypeToken.has_value() &&
												reference.type_token != *reverseTypeToken) {
												return true;
											}
											const uint64_t currentId =
													adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING
															? reference.target_node_id
															: reference.source_node_id;
											const uint64_t predecessor =
													adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING
															? reference.source_node_id
															: reference.target_node_id;
											if (!std::ranges::binary_search(currentFrontier, currentId) || predecessor == 0U) {
												throw std::runtime_error(
														"logical graph reverse reachability returned an inconsistent endpoint");
											}
											const auto current = reverseDistance.find(currentId);
											if (current == reverseDistance.end() || current->second >= maximumHops) {
												return true;
											}
											if (reverseDistance.emplace(predecessor, current->second + 1).second) {
												nextFrontier.push_back(predecessor);
											}
											return true;
										});
								return nextFrontier;
							};
							const bool canExpandReverse = traversal.getEdgeType().empty() || reverseTypeToken.has_value();
							for (int depth = 0; canExpandReverse && depth < maximumHops && !frontier.empty(); ++depth) {
								std::vector<uint64_t> nextFrontier;
								if (direction == "out" || direction == "both") {
									nextFrontier = reverseDirection(
											frontier, storage::LogicalGraphReadDirection::LGRD_INCOMING);
								}
								if (direction == "in" || direction == "both") {
									std::vector<uint64_t> incoming = reverseDirection(
											frontier, storage::LogicalGraphReadDirection::LGRD_OUTGOING);
									nextFrontier.insert(nextFrontier.end(), incoming.begin(), incoming.end());
								}
								std::ranges::sort(nextFrontier);
								nextFrontier.erase(std::unique(nextFrontier.begin(), nextFrontier.end()),
										nextFrontier.end());
								frontier = std::move(nextFrontier);
							}
						}
					}
					if (reversePruningEnabled && reverseDistance.empty()) {
						return true;
					}

					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						const uint64_t sourceId = nodeBindingId(sourceBinding);
						if (reversePruningEnabled) {
							const auto reachable = reverseDistance.find(sourceId);
							if (reachable == reverseDistance.end() || reachable->second > maximumHops) {
								return true;
							}
						}
						const storage::LogicalGraphNode source = materializeNodeValue(state, sourceBinding);
						std::unordered_set<uint64_t> pathNodes;
						pathNodes.insert(source.node_id);
						std::vector<storage::LogicalGraphNode> pathNodeRecords{source};
						// A variable-length topology traversal does not need a complete
						// endpoint Node when no target label/Property predicate exists. Keep
						// an aligned reference marker so output and NamedPath can materialize
						// the endpoint only at their respective boundaries.
						std::vector<storage::EntityRef> pathNodeRefs;
						pathNodeRefs.push_back({});
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
									state.snapshot.supportsNativeAdjacentEdgeRefScans() &&
									(traversal.getEdgeType().empty() || state.snapshot.supportsNativeRelationshipTypeTokens());
							if (useNativeAdjacencyReferences && !traversal.getEdgeType().empty()) {
								relationshipTypeToken = state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
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
											throw std::runtime_error("logical-plan variable-length Traversal exceeded its edge exploration bound");
										}
										const size_t pageRecords = std::min(maximumEdgeVisits - edgeVisits,
																				 state.bounds.maximum_adjacency_page_records);
										const storage::LogicalGraphAdjacencyReferencePage page = state.snapshot.scanAdjacentEdgeRefs(
													 nodeId, adjacencyDirection, continuation, pageRecords);
										if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value()) ||
											(continuation.has_value() &&
											 (page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
											throw std::runtime_error("logical graph backend returned an invalid variable-length adjacency reference page");
										}
										const bool deferPageTargets = traversal.getTargetLabels().empty() &&
												traversal.getTargetProperties().empty() && state.snapshot.supportsNodeExistenceProbes();
										std::vector<uint64_t> pageEndpointIds;
										if (deferPageTargets) {
											pageEndpointIds.reserve(page.edges.size());
											for (const storage::EntityRef &candidate: page.edges) {
								validateEdgeRef(candidate,
										"logical graph backend returned an invalid variable-length adjacency reference");
												if (direction == "both" &&
														adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
														candidate.source_node_id == nodeId) {
														continue;
													}
												if (relationshipTypeToken.has_value() && candidate.type_token != *relationshipTypeToken) {
														continue;
													}
												pageEndpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														? candidate.target_node_id : candidate.source_node_id);
											}
											std::ranges::sort(pageEndpointIds);
											pageEndpointIds.erase(std::unique(pageEndpointIds.begin(), pageEndpointIds.end()), pageEndpointIds.end());
										}
										const std::vector<bool> pageEndpointsPresent = deferPageTargets
											? state.snapshot.containsNodesSorted(pageEndpointIds) : std::vector<bool>{};
										for (const storage::EntityRef &reference: page.edges) {
											checkGuard(state);
							validateEdgeRef(reference,
									"logical graph backend returned an invalid variable-length adjacency reference");
							if (reference.entity_id <= previousEdgeId) {
												throw std::runtime_error("logical graph backend returned an invalid variable-length adjacency reference");
											}
											previousEdgeId = reference.entity_id;
											const bool endpointMatches =
													adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? reference.source_node_id == nodeId
															: reference.target_node_id == nodeId;
											if (!endpointMatches) {
												throw std::runtime_error("logical graph backend returned an invalid variable-length Edge reference");
											}
											++edgeVisits;
											incrementBounded(state.stats.edges_scanned,
																		 "logical-plan variable-length Edge counter overflowed");
											if (direction == "both" &&
													adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
													reference.source_node_id == nodeId) {
												continue;
											}
											if (relationshipTypeToken.has_value() && reference.type_token != *relationshipTypeToken) {
												continue;
											}
											const uint64_t targetId = adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																			 ? reference.target_node_id
																			 : reference.source_node_id;
																											 const bool deferTargetNode = traversal.getTargetLabels().empty() &&
																											 traversal.getTargetProperties().empty();
																											 if (deferTargetNode) {
																											 if (state.snapshot.supportsNodeExistenceProbes()) {
																		 const auto endpoint = std::ranges::lower_bound(pageEndpointIds, targetId);
																		 if (endpoint == pageEndpointIds.end() || *endpoint != targetId ||
																			 !pageEndpointsPresent[static_cast<size_t>(endpoint - pageEndpointIds.begin())]) {
																													 throw std::runtime_error(
																														 "logical graph backend returned a variable-length Edge with a missing endpoint");
																												 }
																											 } else {
																												 // A reader without an existence probe must retain the
																												 // complete-read fallback to authenticate the endpoint.
																												 static_cast<void>(readNodeCached(state, targetId));
																												 }
																							 }
																	 const storage::LogicalGraphNode target = deferTargetNode
																							 ? storage::LogicalGraphNode{targetId, {}}
																									 : readNodeCached(state, targetId);
																			const storage::LogicalGraphEdgeTopology topology{
																				reference.entity_id, reference.source_node_id, reference.target_node_id, {}};
																			if (!onEdge({topology, reference}, target)) {
																				return false;
																			}
										}
										if (!page.continuation.has_value()) {
											return true;
										}
										if (page.edges.empty() || page.continuation->previous_edge_id != page.edges.back().entity_id ||
											page.continuation->next_edge_id == 0U ||
											page.continuation->previous_edge_id == page.continuation->next_edge_id) {
											throw std::runtime_error("logical graph backend returned an invalid variable-length adjacency reference continuation");
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
											std::min(remaining, state.bounds.maximum_adjacency_page_records);
					// Variable-length traversal never binds an Edge value or reads Edge
					// Properties. Use the topology cursor even when reference pages are
					// unavailable; target Nodes remain materialized only when predicates
					// require them.
					const storage::LogicalGraphAdjacencyTopologyPage page = state.snapshot.scanAdjacentEdgeTopologies(
							nodeId, adjacencyDirection, continuation, pageRecords);
						if (page.edges.size() > pageRecords ||
							(page.edges.empty() && page.continuation.has_value())) {
							throw std::runtime_error("logical graph backend returned an invalid "
															 "variable-length adjacency page");
						}
						const bool deferPageTargets = traversal.getTargetLabels().empty() &&
								traversal.getTargetProperties().empty() && state.snapshot.supportsNodeExistenceProbes();
						std::vector<uint64_t> pageEndpointIds;
						if (deferPageTargets) {
							pageEndpointIds.reserve(page.edges.size());
							for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
								const bool selfLoopDuplicate = direction == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
										candidate.source_node_id == nodeId;
								if (!selfLoopDuplicate &&
									(traversal.getEdgeType().empty() || candidate.type == traversal.getEdgeType())) {
									pageEndpointIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? candidate.target_node_id : candidate.source_node_id);
								}
							}
							std::ranges::sort(pageEndpointIds);
							pageEndpointIds.erase(std::unique(pageEndpointIds.begin(), pageEndpointIds.end()), pageEndpointIds.end());
						}
						const std::vector<bool> pageEndpointsPresent = deferPageTargets
								? state.snapshot.containsNodesSorted(pageEndpointIds) : std::vector<bool>{};
						if (pageEndpointsPresent.size() != pageEndpointIds.size()) {
							throw std::runtime_error("logical graph backend changed variable-length endpoint cardinality");
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
										incrementBounded(state.stats.edges_scanned,
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
											if (state.snapshot.supportsNodeExistenceProbes()) {
												const auto endpoint = std::ranges::lower_bound(pageEndpointIds, targetId);
												if (endpoint == pageEndpointIds.end() || *endpoint != targetId ||
													!pageEndpointsPresent[static_cast<size_t>(endpoint - pageEndpointIds.begin())]) {
													throw std::runtime_error(
															"logical graph backend returned a variable-length Edge with a missing endpoint");
												}
											} else {
												static_cast<void>(readNodeCached(state, targetId));
											}
										}
										const storage::LogicalGraphNode target = deferTargetNode
												? storage::LogicalGraphNode{targetId, {}}
												: readNodeCached(state, targetId);
																		if (!onEdge({{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type}, std::nullopt}, target)) {
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
								nodeMatches(state.snapshot, node, traversal.getTargetLabels(),
										traversal.getTargetProperties(), state)) {
								LogicalPlanReadRow expanded = row;
								const bool deferredNode = depth > 0 && !pathNodeRefs.empty() &&
									pathNodeRefs.back().entity_id != 0U;
								const LogicalPlanReadValue targetValue = deferredNode
										? LogicalPlanReadValue(pathNodeRefs.back())
										: LogicalPlanReadValue(node);
								if (bindValue(expanded, traversal.getTargetVar(), targetValue)) {
									expanded.path_cache.insert_or_assign(
											traversal.getTargetVar(),
											LogicalPlanReadPath{pathNodeRecords, pathEdgeRecords, pathEdgeRefs, pathNodeRefs});
									if (!visitor(std::move(expanded))) {
										return false;
									}
								}
							}
							if (depth == maximumHops) {
								return true;
							}
							return visitAdjacent(node.node_id, [&](const AdjacentEdge &edge,
																										 const storage::LogicalGraphNode &target) {
								if (reversePruningEnabled) {
									const auto reachable = reverseDistance.find(target.node_id);
									if (reachable == reverseDistance.end() || reachable->second > maximumHops - (depth + 1)) {
										return true;
									}
								}
								if (pathNodes.contains(target.node_id)) {
									return true;
								}
								pathNodes.insert(target.node_id);
								if (edge.reference.has_value()) {
									pathEdgeRefs.push_back(*edge.reference);
								} else {
									pathEdgeRecords.push_back({edge.topology.edge_id, edge.topology.source_node_id,
																	 edge.topology.target_node_id, edge.topology.type});
								}
								pathNodeRecords.push_back(target);
								pathNodeRefs.push_back((edge.reference.has_value() || state.snapshot.supportsNodeExistenceProbes()) &&
										traversal.getTargetLabels().empty() && traversal.getTargetProperties().empty()
										? storage::EntityRef{target.node_id, false, 0U, 0U, 0U}
										: storage::EntityRef{});
								const bool completed = visitPath(target, depth + 1);
								pathNodeRecords.pop_back();
								pathNodeRefs.pop_back();
								if (edge.reference.has_value()) {
									pathEdgeRefs.pop_back();
								} else {
									pathEdgeRecords.pop_back();
								}
								pathNodes.erase(target.node_id);
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
					return visitOperator(requireOnlyChild(unwind), state, [&](LogicalPlanReadRow input) {
						std::vector<PropertyValue> values;
						if (unwind.hasLiteralList()) {
							values = unwind.getListValues();
						} else {
							if (!unwind.getListExpr()) {
								throw std::invalid_argument("logical-plan UNWIND has no list expression");
							}
							const PropertyValue evaluated =
									scalarValue(evaluateExpression(*unwind.getListExpr(), input, state));
							if (evaluated.getType() == PropertyType::LIST) {
								values = evaluated.getList();
							} else if (unwind.scalarAsSingleRow()) {
								// Match the established pipeline UNWIND contract: a scalar
								// expression contributes one row for each input row.
								values.push_back(evaluated);
							}
						}
						if (values.size() > state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan UNWIND exceeded its input list bound");
						}
						for (const PropertyValue &value: values) {
							checkGuard(state);
							LogicalPlanReadRow expanded = input;
							expanded.values.insert_or_assign(unwind.getAlias(), value);
							expanded.property_cache.erase(unwind.getAlias());
							expanded.complete_entity_property_cache.erase(unwind.getAlias());
							expanded.entity_metadata_cache.erase(unwind.getAlias());
							expanded.path_cache.erase(unwind.getAlias());
							const size_t rowBytes = logicalReadRowBytes(expanded);
							if (rowBytes > state.bounds.maximum_materialized_bytes) {
								throw std::runtime_error("logical-plan UNWIND exceeded its materialized byte bound");
							}
							if (!visitor(std::move(expanded))) {
								return false;
							}
						}
						return true;
					});
				}
				case logical::LogicalOpType::LOP_LOAD_CSV: {
					const auto &loadCsv = dynamic_cast<const logical::LogicalLoadCsv &>(op);
					if (state.context == nullptr) {
						throw std::runtime_error("logical LOAD CSV requires an execution context with an import policy");
					}
					if (loadCsv.getRowVariable().empty() || !loadCsv.getUrlExpr()) {
						throw std::invalid_argument("logical LOAD CSV requires a URI expression and row variable");
					}
					const std::string &delimiter = loadCsv.getFieldTerminator();
					if (delimiter.empty() || delimiter.size() > 8U || delimiter.find_first_of("\r\n\"") != std::string::npos) {
						throw std::invalid_argument("logical LOAD CSV has an invalid field terminator");
					}
					const auto streamRows = [&](LogicalPlanReadRow input) {
						checkGuard(state);
						const PropertyValue uri = scalarValue(evaluateExpression(*loadCsv.getUrlExpr(), input, state));
						const std::filesystem::path csvPath = resolveCsvImportFile(uri, *state.context);
						CsvReader reader(csvPath.string(), delimiter,
												 CsvReaderLimits{state.context->csv_import_policy.maximum_record_bytes,
																 state.context->csv_import_policy.maximum_columns,
																 state.context->csv_import_policy.maximum_field_bytes});

						std::vector<std::string> headers;
						if (loadCsv.isWithHeaders() && reader.hasNext()) {
							headers = reader.nextRow();
							std::unordered_set<std::string> uniqueHeaders;
							uniqueHeaders.reserve(headers.size());
							for (const std::string &header: headers) {
								if (header.empty() || !uniqueHeaders.insert(header).second) {
									throw std::runtime_error("logical LOAD CSV has empty or duplicate header names");
								}
							}
						}

						size_t loadedRows = 0U;
						while (reader.hasNext()) {
							checkGuard(state);
							if (loadedRows == state.context->csv_import_policy.maximum_rows) {
								throw std::runtime_error("logical LOAD CSV exceeded its row bound");
							}
							std::vector<std::string> fields = reader.nextRow();
							PropertyValue rowValue;
							if (loadCsv.isWithHeaders()) {
								PropertyValue::MapType values;
								values.reserve(std::min(headers.size(), fields.size()));
								for (size_t index = 0U; index < headers.size() && index < fields.size(); ++index) {
									values.emplace(headers[index], PropertyValue(std::move(fields[index])));
								}
								rowValue = PropertyValue(std::move(values));
							} else {
								std::vector<PropertyValue> values;
								values.reserve(fields.size());
								for (std::string &field: fields) {
									values.emplace_back(std::move(field));
								}
								rowValue = PropertyValue(std::move(values));
							}
							++loadedRows;
							LogicalPlanReadRow expanded = input;
							if (!bindValue(expanded, loadCsv.getRowVariable(), std::move(rowValue))) {
								throw std::runtime_error("logical LOAD CSV attempted to rebind its row variable");
							}
							expanded.property_cache.erase(loadCsv.getRowVariable());
							expanded.complete_entity_property_cache.erase(loadCsv.getRowVariable());
							expanded.entity_metadata_cache.erase(loadCsv.getRowVariable());
							const size_t rowBytes = logicalReadRowBytes(expanded);
							if (rowBytes > state.bounds.maximum_materialized_bytes) {
								throw std::runtime_error("logical LOAD CSV exceeded its materialized byte bound");
							}
							if (!visitor(std::move(expanded))) {
								return false;
							}
						}
						return true;
					};

					const std::vector<logical::LogicalOperator *> children = loadCsv.getChildren();
					if (children.empty()) {
						return streamRows({});
					}
					if (children.size() != 1U || children.front() == nullptr) {
						throw std::invalid_argument("logical LOAD CSV has an invalid input pipeline");
					}
					return visitOperator(*children.front(), state, streamRows);
				}
				case logical::LogicalOpType::LOP_UNION: {
					const auto &unionOperator = dynamic_cast<const logical::LogicalUnion &>(op);
					const logical::LogicalOperator *left = unionOperator.getLeft();
					const logical::LogicalOperator *right = unionOperator.getRight();
					if (left == nullptr || right == nullptr) {
						throw std::invalid_argument("logical-plan UNION requires two valid children");
					}
					const std::vector<std::string> columns = left->getOutputVariables();
					if (columns.empty() || columns != right->getOutputVariables()) {
						throw std::invalid_argument(
								"logical-plan UNION children must declare the same nonempty output columns");
					}
					std::unordered_set<std::string> uniqueColumns;
					uniqueColumns.reserve(columns.size());
					for (const std::string &column: columns) {
						if (column.empty() || !uniqueColumns.insert(column).second) {
							throw std::invalid_argument("logical-plan UNION has invalid output columns");
						}
					}

					std::unordered_set<std::string> distinctRows;
					size_t distinctBytes = 0U;
					const auto visitChild = [&](const logical::LogicalOperator &child) {
						return visitOperator(child, state, [&](LogicalPlanReadRow row) {
							LogicalPlanReadRow output = normalizeOutputRow(row, columns);
							if (!unionOperator.isAll()) {
								const std::string key = distinctUnionRowKey(output, columns);
								if (distinctRows.contains(key)) {
									return true;
								}
								if (distinctRows.size() == state.bounds.maximum_distinct_rows) {
									throw std::runtime_error("logical-plan UNION DISTINCT exceeded its row bound");
								}
								const size_t keyBytes = distinctKeyStorageBytes(
										key, "logical-plan UNION DISTINCT materialization size overflowed");
								if (keyBytes > state.bounds.maximum_materialized_bytes -
										std::min(distinctBytes, state.bounds.maximum_materialized_bytes)) {
									throw std::runtime_error("logical-plan UNION DISTINCT exceeded its materialized byte bound");
								}
								static_cast<void>(distinctRows.insert(key));
								distinctBytes = checkedSizeAdd(distinctBytes, keyBytes,
										"logical-plan UNION DISTINCT materialization size overflowed");
							}
							return visitor(std::move(output));
						});
					};
					return visitChild(*left) && visitChild(*right);
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
					struct DecoratedRow {
						LogicalPlanReadRow row;
						std::vector<PropertyValue> keys;
					};
					const auto decoratedRowBytes = [](const DecoratedRow &decorated) {
						size_t bytes = logicalReadRowBytes(decorated.row);
						for (const PropertyValue &key: decorated.keys) {
							bytes = checkedSizeAdd(bytes, property_utils::getPropertyValueSize(key),
									"logical-plan Sort materialization size overflowed");
						}
						return bytes;
					};
					BoundedBatch<DecoratedRow> rows(
							state.bounds.maximum_rows, state.bounds.maximum_materialized_bytes, decoratedRowBytes);
					rows.reserve(std::min(state.bounds.maximum_rows, state.bounds.maximum_node_records));
					const auto collectRow = [&](LogicalPlanReadRow row) {
						if (rows.size() == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Sort exceeded its materialization row bound");
						}
						// Delay expression evaluation until we know ordering can affect the
						// result. This also preserves valid single-row aggregate results when
						// an otherwise unavailable input-only ORDER BY expression is present.
						rows.append({std::move(row), {}});
						return true;
					};
					std::vector<const expressions::Expression *> sortExpressions;
					sortExpressions.reserve(items.size());
					for (const logical::LogicalSortItem &item: items) {
						sortExpressions.push_back(item.expression.get());
					}
					const std::optional<bool> batched = tryVisitBatchedPropertyExpressions(
							requireOnlyChild(sort), sortExpressions, state, collectRow);
					const bool completed = batched.has_value() ? *batched : visitOperator(
							requireOnlyChild(sort), state, collectRow);
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Sort could not prove that its materialized input was exhausted");
					}
					if (rows.size() > 1U) {
						size_t additionalKeyBytes = 0U;
						for (DecoratedRow &row: rows.values()) {
							row.keys.reserve(items.size());
							for (const logical::LogicalSortItem &item: items) {
								checkGuard(state);
								PropertyValue key = scalarValue(evaluateExpression(*item.expression, row.row, state));
								const size_t keyBytes = property_utils::getPropertyValueSize(key);
								if (keyBytes > state.bounds.maximum_materialized_bytes -
										std::min(checkedSizeAdd(rows.bytes(), additionalKeyBytes,
												"logical-plan Sort materialization size overflowed"),
											state.bounds.maximum_materialized_bytes)) {
									throw std::runtime_error("logical-plan Sort exceeded its materialization byte bound");
								}
								additionalKeyBytes = checkedSizeAdd(additionalKeyBytes, keyBytes,
										"logical-plan Sort materialization size overflowed");
								row.keys.push_back(std::move(key));
							}
						}
						rows.recalculateBytes();
						std::stable_sort(rows.values().begin(), rows.values().end(),
									 [&](const DecoratedRow &left, const DecoratedRow &right) {
										 checkGuard(state);
										 for (size_t index = 0U; index < items.size(); ++index) {
											 if (left.keys[index] == right.keys[index]) {
												 continue;
											 }
											 return items[index].ascending ? left.keys[index] < right.keys[index]
																		   : left.keys[index] > right.keys[index];
										 }
										 return false;
									 });
					}
					for (DecoratedRow &row: rows.values()) {
						checkGuard(state);
						if (!visitor(std::move(row.row))) {
							return false;
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
					const size_t batchCapacity = std::min(DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_rows);
					if (batchCapacity == 0U) {
						throw std::runtime_error("logical-plan SKIP has no positive batch capacity");
					}
					BoundedBatch<LogicalPlanReadRow> rows(
							batchCapacity, state.bounds.maximum_materialized_bytes,
							[](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row); });
					rows.reserve(batchCapacity);
					size_t skipped = 0U;
					bool stopped = false;
					const auto flush = [&]() {
						if (rows.empty()) {
							return true;
						}
						SelectionVector selection(rows.size());
						for (size_t index = 0U; index < rows.size() && static_cast<uint64_t>(skipped) < offset; ++index) {
							incrementBounded(skipped, "logical-plan SKIP counter overflowed");
							debug::PerfTrace::addValue("logical_plan.skip.discarded_rows", 1);
							selection.reject(index);
						}
						bool keepGoing = true;
						static_cast<void>(selection.forEachSelectedUntil([&](const size_t index) {
							if (!keepGoing) {
								return false;
							}
							keepGoing = visitor(std::move(rows.values()[index]));
							return keepGoing;
						}));
						rows.clear();
						return keepGoing;
					};
					const bool childCompleted = visitOperator(requireOnlyChild(skip), state, [&](LogicalPlanReadRow row) {
						rows.append(std::move(row));
						// Keep the first post-offset row in the same bounded batch, but do
						// not read an entire large batch when a downstream LIMIT can stop
						// immediately.  This preserves the existing no-scan-past-limit
						// contract while retaining batching for large offsets.
						const uint64_t rowsNeeded = static_cast<uint64_t>(skipped) < offset
								? offset - static_cast<uint64_t>(skipped) + 1U : 1U;
						const size_t flushThreshold = rowsNeeded >= static_cast<uint64_t>(batchCapacity)
								? batchCapacity : static_cast<size_t>(rowsNeeded);
						if (rows.size() >= flushThreshold) {
							if (!flush()) {
								stopped = true;
							}
							return !stopped;
						}
						return true;
					});
					if (stopped || !flush()) {
						return false;
					}
					return childCompleted;
				}
				case logical::LogicalOpType::LOP_LIMIT: {
					const auto &limit = dynamic_cast<const logical::LogicalLimit &>(op);
					if (limit.getLimit() < 0) {
						throw std::invalid_argument("logical-plan LIMIT cannot be negative");
					}
					if (limit.getLimit() == 0) {
						return false;
					}
					const uint64_t maximum = static_cast<uint64_t>(limit.getLimit());
					const size_t batchCapacity = std::min(DEFAULT_PROPERTY_BATCH_ROWS, state.bounds.maximum_rows);
					if (batchCapacity == 0U) {
						throw std::runtime_error("logical-plan LIMIT has no positive batch capacity");
					}
					BoundedBatch<LogicalPlanReadRow> rows(
							batchCapacity, state.bounds.maximum_materialized_bytes,
							[](const LogicalPlanReadRow &row) { return logicalReadRowBytes(row); });
					rows.reserve(batchCapacity);
					size_t delivered = 0U;
					bool stopped = false;
					const auto flush = [&]() {
						if (rows.empty()) {
							return true;
						}
						SelectionVector selection(rows.size());
						const uint64_t remaining = maximum > static_cast<uint64_t>(delivered)
								? maximum - static_cast<uint64_t>(delivered) : 0U;
						for (size_t index = 0U; index < rows.size(); ++index) {
							if (static_cast<uint64_t>(index) >= remaining) {
								selection.reject(index);
							}
						}
						bool keepGoing = true;
						static_cast<void>(selection.forEachSelectedUntil([&](const size_t index) {
							if (!keepGoing) {
								return false;
							}
							incrementBounded(delivered, "logical-plan LIMIT counter overflowed");
							debug::PerfTrace::addValue("logical_plan.limit.delivered_rows", 1);
							keepGoing = visitor(std::move(rows.values()[index]));
							return keepGoing;
						}));
						const bool reachedLimit = static_cast<uint64_t>(delivered) >= maximum;
						rows.clear();
						if (!keepGoing || reachedLimit) {
							debug::PerfTrace::addValue("logical_plan.limit.early_stop", 1);
							return false;
						}
						return true;
					};
					const bool childCompleted = visitOperator(requireOnlyChild(limit), state, [&](LogicalPlanReadRow row) {
						rows.append(std::move(row));
						const uint64_t remaining = maximum > static_cast<uint64_t>(delivered)
								? maximum - static_cast<uint64_t>(delivered) : 0U;
						const size_t flushThreshold = remaining == 0U || remaining >= static_cast<uint64_t>(batchCapacity)
								? batchCapacity : static_cast<size_t>(remaining);
						if (rows.size() >= flushThreshold) {
							if (!flush()) {
								stopped = true;
							}
							return !stopped;
						}
						return true;
					});
					if (stopped || !flush()) {
						return false;
					}
					return childCompleted;
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
						const uint64_t sourceNodeId = nodeBindingId(sourceBinding);
						size_t sourceEdges = 0U;
						const bool topologyOnly = traversal.getEdgeProperties().empty();
						std::optional<uint64_t> relationshipTypeToken;
						if (!traversal.getEdgeType().empty() && state.snapshot.supportsNativeRelationshipTypeTokens()) {
							relationshipTypeToken = state.snapshot.findRelationshipTypeToken(traversal.getEdgeType());
							if (!relationshipTypeToken.has_value()) {
								return true;
							}
						}
						// Reference pages carry authenticated endpoints and type tokens. They
						// can drive selected Edge-property predicates even when the pattern
						// omits an Edge binding; no complete Edge object is required.
						const bool useReferencePages =
								state.snapshot.supportsNativeAdjacentEdgeRefScans() &&
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
								std::vector<storage::EntityRef> pendingReferences;
								const size_t batchCapacity =
										edgePredicates.empty()
												? size_t{1U}
												: std::max<size_t>(
														  1U, std::min({DEFAULT_PROPERTY_BATCH_ROWS,
																		state.bounds.maximum_adjacency_page_records,
																		state.bounds.maximum_edges_per_node}));
								pendingReferences.reserve(batchCapacity);
								const auto flushReferences = [&]() {
									if (pendingReferences.empty()) {
										return true;
									}
									SelectionVector selection(pendingReferences.size());
									std::vector<std::vector<PropertyValue>> selectedProperties;
									if (!edgePredicates.empty()) {
										std::vector<size_t> sortedOrdinals(pendingReferences.size());
										std::iota(sortedOrdinals.begin(), sortedOrdinals.end(), size_t{0U});
										std::ranges::sort(sortedOrdinals, [&](const size_t left, const size_t right) {
											return pendingReferences[left].entity_id <
												   pendingReferences[right].entity_id;
										});
										std::vector<uint64_t> edgeIds;
										edgeIds.reserve(pendingReferences.size());
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
											const std::vector<std::vector<PropertyValue>> sortedColumns =
													state.snapshot.readEdgePropertyColumnsSorted(edgeIds,
																		 edgePropertyKeys);
											const size_t selectedEdgeBytes = propertyColumnBytes(
													sortedColumns, edgePropertyKeys,
														"logical-plan adjacent Edge Property batch size overflowed");
											if (selectedEdgeBytes > state.bounds.maximum_materialized_bytes) {
												throw std::runtime_error(
														"logical-plan adjacent Edge Property batch exceeded its byte bound");
											}
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
													values[sortedOrdinals[sortedIndex]] =
															sortedColumns[propertyIndex][sortedIndex];
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
									// Resolve target predicates in one bounded column batch.  The
									// previous per-reference complete read made a high-degree
									// anchored traversal pay one Entity/Property lookup per Edge.
									struct TargetCandidate {
										size_t reference_index = 0U;
										uint64_t target_id = 0U;
										size_t property_ordinal = 0U;
									};
									std::vector<TargetCandidate> targets;
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
									std::vector<uint64_t> targetIds;
									targetIds.reserve(targets.size());
									for (const TargetCandidate &target: targets) {
										// Deduplicate after sorting.  A linear search here turns a
										// high-degree adjacency batch into O(n^2) work before the
										// column reader even starts; the batch is already bounded and
										// sorting gives deterministic I/O order.
										targetIds.push_back(target.target_id);
									}
									std::ranges::sort(targetIds);
									targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
									std::vector<bool> targetLabelsMatch(targetIds.size(), true);
									if (!traversal.getTargetLabels().empty()) {
										targetLabelsMatch = state.snapshot.readNodeLabelMatchesSorted(
												targetIds, traversal.getTargetLabels());
										if (targetLabelsMatch.size() != targetIds.size()) {
											throw std::runtime_error(
													"logical graph backend changed target label-match cardinality");
										}
									}
									std::vector<std::pair<std::string, PropertyValue>> targetPredicates;
									targetPredicates.reserve(traversal.getTargetProperties().size());
									for (const auto &[key, expected]: traversal.getTargetProperties()) {
										if (key.empty() ||
											std::ranges::any_of(targetPredicates, [&key](const auto &predicate) {
												return predicate.first == key;
											})) {
											throw std::invalid_argument("logical-plan target Property predicate has an "
																		"empty or duplicate key");
										}
										targetPredicates.emplace_back(key, expected);
									}
									std::vector<std::string> targetKeys;
									targetKeys.reserve(targetPredicates.size());
									for (const auto &[key, expected]: targetPredicates) {
										static_cast<void>(expected);
										targetKeys.push_back(key);
									}
									if (traversal.getTargetLabels().empty() && targetPredicates.empty()) {
										if (state.snapshot.supportsNodeExistenceProbes()) {
											const std::vector<bool> present =
													state.snapshot.containsNodesSorted(targetIds);
											if (present.size() != targetIds.size()) {
												throw std::runtime_error(
														"logical graph backend changed adjacent endpoint cardinality");
											}
											if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
												throw std::runtime_error("logical graph backend returned an adjacent "
																		 "Edge with a missing endpoint");
											}
										} else {
											for (const uint64_t targetId: targetIds) {
												validateNode(readNodeCached(state, targetId));
											}
										}
									}
									std::vector<std::string_view> targetKeyViews;
									targetKeyViews.reserve(targetKeys.size());
									for (const std::string &key: targetKeys) {
										targetKeyViews.push_back(key);
									}
									const auto targetColumns = targetKeys.empty()
																	   ? std::vector<std::vector<PropertyValue>>{}
																	   : state.snapshot.readNodePropertyColumnsSorted(
																				 targetIds, targetKeyViews);
									if (targetColumns.size() != targetKeys.size()) {
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
										if (!targetLabelsMatch[targetOrdinal]) {
											continue;
										}
										bool targetMatches = true;
										for (size_t propertyIndex = 0U; propertyIndex < targetKeys.size();
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
										LogicalPlanReadRow expanded = row;
										if (!traversal.getEdgeVar().empty() &&
											!bindValue(expanded, traversal.getEdgeVar(), reference)) {
											continue;
										}
										if (!traversal.getEdgeVar().empty()) {
											for (size_t propertyIndex = 0U; propertyIndex < edgePredicates.size();
												 ++propertyIndex) {
												expanded.property_cache[traversal.getEdgeVar()].insert_or_assign(
														edgePredicates[propertyIndex].first,
														selectedProperties[propertyIndex][candidate.reference_index]);
											}
										}
										if (!traversal.getTargetVar().empty()) {
											const LogicalPlanReadValue targetBinding = LogicalPlanReadValue(
													storage::EntityRef{candidate.target_id, false, 0U, 0U, 0U});
											if (!bindValue(expanded, traversal.getTargetVar(), targetBinding)) {
												continue;
											}
											for (size_t propertyIndex = 0U; propertyIndex < targetKeys.size();
												 ++propertyIndex) {
												expanded.property_cache[traversal.getTargetVar()].insert_or_assign(
														targetKeys[propertyIndex],
														targetColumns[propertyIndex][targetOrdinal]);
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
									const size_t remaining = state.bounds.maximum_edges_per_node - sourceEdges;
									if (remaining == 0U) {
										return true;
									}
									const size_t pageRecords =
											std::min(remaining, state.bounds.maximum_adjacency_page_records);
									const storage::LogicalGraphAdjacencyReferencePage page =
											state.snapshot.scanAdjacentEdgeRefs(sourceNodeId, adjacencyDirection,
																				continuation, pageRecords);
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
										incrementBounded(state.stats.edges_scanned,
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
								const size_t remaining = state.bounds.maximum_edges_per_node - sourceEdges;
								if (remaining == 0U) { // ZYX_COV_EXCL_LINE: a full adjacency bound returns immediately
													   // after the preceding page.
									return true;
								}
								const size_t pageRecords =
										std::min(remaining, state.bounds.maximum_adjacency_page_records);
								std::optional<storage::LogicalGraphAdjacencyPage> fullPage;
								std::optional<storage::LogicalGraphAdjacencyTopologyPage> topologyPage;
								// A relationship-type predicate is topology data. When a
								// backend exposes native topology pages, authenticate that
								// predicate before decoding the complete Edge payload. This
								// keeps the fallback traversal on the same read-shape contract
								// as the reference-page and direct traversal paths.
								const bool typePrefilterWithTopology = !topologyOnly &&
										!traversal.getEdgeType().empty() &&
										state.snapshot.supportsNativeAdjacentEdgeTopologyPages();
								if (topologyOnly || typePrefilterWithTopology) {
									topologyPage = state.snapshot.scanAdjacentEdgeTopologies(
											sourceNodeId, adjacencyDirection, continuation, pageRecords);
								} else {
									fullPage = state.snapshot.scanAdjacentEdges(sourceNodeId, adjacencyDirection,
																				continuation, pageRecords);
								}
								const bool topologyPageSelected = topologyOnly || typePrefilterWithTopology;
								const size_t pageSize = topologyPageSelected ? topologyPage->edges.size() : fullPage->edges.size();
								if (pageSize > pageRecords) {
									throw std::runtime_error("logical graph backend exceeded the adjacency page bound (returned " +
											std::to_string(pageSize) + ", requested " + std::to_string(pageRecords) + ")");
								}
								const auto &pageContinuation = topologyPageSelected ? topologyPage->continuation : fullPage->continuation;
								if (pageSize == 0U) {
									if (pageContinuation.has_value()) {
										throw std::runtime_error(
												"logical graph backend returned an empty nonterminal adjacency page");
									}
									return true;
								}
								// Both topology and complete adjacency pages already carry endpoint
								// IDs. Validate their liveness once per bounded page instead of
								// performing a Directory lookup for every Edge. The map is page-local
								// so memory remains bounded by the adjacency page limit and
								// early-stop semantics stay unchanged at the visitor boundary.
								std::unordered_map<uint64_t, bool> pageTargetPresence;
								if (state.snapshot.supportsNodeExistenceProbes() &&
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
									std::vector<uint64_t> pageTargetIds;
									pageTargetIds.reserve(pageSize);
									if (topologyPage.has_value()) {
										for (const auto &topology: topologyPage->edges) {
											pageTargetIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
											? topology.target_node_id : topology.source_node_id);
										}
									} else if (fullPage.has_value()) {
										for (const storage::LogicalGraphEdge &edge: fullPage->edges) {
											pageTargetIds.push_back(adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id : edge.source_node_id);
										}
									}
									std::ranges::sort(pageTargetIds);
									pageTargetIds.erase(std::unique(pageTargetIds.begin(), pageTargetIds.end()), pageTargetIds.end());
									const std::vector<bool> present = state.snapshot.containsNodesSorted(pageTargetIds);
									if (present.size() != pageTargetIds.size()) {
										throw std::runtime_error("logical graph backend changed adjacent endpoint cardinality");
									}
									for (size_t index = 0U; index < pageTargetIds.size(); ++index) {
										pageTargetPresence.emplace(pageTargetIds[index], present[index]);
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
									incrementBounded(state.stats.edges_scanned,
													 "logical-plan read runtime Edge counter overflowed");
									if (direction == "both" &&
										adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
											edge.source_node_id == sourceNodeId) {
										return true;
									}
									if (!edgeMatches(state.snapshot, edge, traversal.getEdgeType(),
													 traversal.getEdgeProperties(), state)) {
										return true;
									}
									const uint64_t targetId =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
															? edge.target_node_id
													: edge.source_node_id;
					const bool targetNeedsRecord = !traversal.getTargetLabels().empty() ||
							!traversal.getTargetProperties().empty();
					const auto existingTarget = row.values.find(traversal.getTargetVar());
					// If no target variable is requested, endpoint existence is sufficient;
					// avoid reading and decoding a Node that can never reach the result.
					const bool canDeferTarget = !targetNeedsRecord && state.allow_deferred_entity_refs &&
							state.snapshot.supportsNodeExistenceProbes() &&
							(traversal.getTargetVar().empty() ||
								 (state.snapshot.supportsNativeNodeRefScans() && existingTarget == row.values.end()));
					std::optional<storage::LogicalGraphNode> target;
					if (targetNeedsRecord || !canDeferTarget) {
						target = readNodeCached(state, targetId);
						if (!nodeMatches(state.snapshot, *target, traversal.getTargetLabels(),
											 traversal.getTargetProperties(), state)) {
							return true;
						}
									} else if (state.snapshot.supportsNodeExistenceProbes()) {
										const auto pagePresence = pageTargetPresence.find(targetId);
										const bool present = pagePresence != pageTargetPresence.end()
												? pagePresence->second : state.snapshot.containsNode(targetId);
										if (!present) {
							throw std::runtime_error("logical graph backend returned an adjacent Edge with a missing endpoint");
						}
					}
					LogicalPlanReadRow expanded = row;
					const LogicalPlanReadValue edgeBinding =
											topologyOnly
														? LogicalPlanReadValue(storage::EntityRef{edge.edge_id, true, edge.source_node_id,
																				 edge.target_node_id, 0U})
														: LogicalPlanReadValue(edge);
					if (!bindValue(expanded, traversal.getEdgeVar(), edgeBinding)) {
						return true;
					}
					if (!traversal.getTargetVar().empty()) {
						const LogicalPlanReadValue targetBinding = canDeferTarget
								? LogicalPlanReadValue(storage::EntityRef{targetId, false, 0U, 0U, 0U})
								: LogicalPlanReadValue(*target);
						if (!bindValue(expanded, traversal.getTargetVar(), targetBinding)) {
							return true;
						}
					}
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
								} else if (typePrefilterWithTopology) {
									// Keep the original adjacency order while issuing one sorted,
									// complete read only for type-matching Edge IDs. Rejected
									// topology records still pass through processEdge so counters,
									// endpoint validation, duplicate handling, and early-stop
									// semantics remain identical to the complete-page path.
									std::vector<uint64_t> matchingIds;
									matchingIds.reserve(topologyPage->edges.size());
									for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
										validateEdge(topology);
										if (topology.type == traversal.getEdgeType()) {
											matchingIds.push_back(topology.edge_id);
										}
									}
								std::ranges::sort(matchingIds);
								matchingIds.erase(std::unique(matchingIds.begin(), matchingIds.end()), matchingIds.end());
								const std::vector<storage::LogicalGraphEdge> matchingEdges =
										matchingIds.empty()
												? std::vector<storage::LogicalGraphEdge>{}
												: state.snapshot.readEdgesShapedSorted(matchingIds, completeEdgeReadShape());
								if (matchingEdges.size() != matchingIds.size()) {
									throw std::runtime_error("logical graph backend changed typed Edge batch cardinality");
								}
								for (const storage::LogicalGraphEdgeTopology &topology: topologyPage->edges) {
									const auto matching = std::ranges::lower_bound(
											matchingIds, topology.edge_id);
									if (matching == matchingIds.end() || *matching != topology.edge_id) {
										const storage::LogicalGraphEdge rejected{topology.edge_id, topology.source_node_id,
												topology.target_node_id, topology.type};
										if (!processEdge(rejected)) {
											return false;
										}
										continue;
									}
									const size_t matchingIndex = static_cast<size_t>(matching - matchingIds.begin());
									const storage::LogicalGraphEdge &edge = matchingEdges[matchingIndex];
									if (edge.edge_id != topology.edge_id || edge.source_node_id != topology.source_node_id ||
										edge.target_node_id != topology.target_node_id || edge.type != topology.type) {
										throw std::runtime_error("logical graph backend changed typed Edge topology during materialization");
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
									sourceEdges == state.bounds.maximum_edges_per_node) {
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

		[[nodiscard]] LogicalPlanReadRuntimeStats executeOnSnapshot(storage::LogicalGraphReadSnapshot &snapshot,
																	const logical::LogicalOperator &root,
																	const LogicalPlanReadRuntimeBounds &bounds,
																	const FinalRowVisitor &visitor,
																						  const QueryContext *context,
																						  const bool allowDeferredEntityRefs = false) {
			validateBounds(bounds, static_cast<bool>(visitor));
			ExecutionState state{snapshot, bounds, {}, context, nullptr, allowDeferredEntityRefs, {}};
			static_cast<void>(visitOperator(root, state, [&](LogicalPlanReadRow row) {
				if (state.stats.rows_emitted ==
					bounds.maximum_rows) { // ZYX_COV_EXCL_LINE: this visitor returns false with the final row, so its
										   // producer cannot invoke it again.
					return false;
				}
				const bool continueVisitor = visitor(row, state);
				incrementBounded(state.stats.rows_emitted, "logical-plan read runtime row counter overflowed");
				return continueVisitor && state.stats.rows_emitted < bounds.maximum_rows;
			}));
			return state.stats;
		}

		[[nodiscard]] LogicalPlanReadRuntimeStats executeInternal(const storage::LogicalGraphReadBackend &backend,
																  const logical::LogicalOperator &root,
																  const LogicalPlanReadRuntimeBounds &bounds,
																  const FinalRowVisitor &visitor,
																						  const QueryContext *context,
																						  const bool allowDeferredEntityRefs = false) {
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical graph backend returned no read snapshot");
			}
			return executeOnSnapshot(*snapshot, root, bounds, visitor, context, allowDeferredEntityRefs);
		}

	} // namespace

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::execute(const logical::LogicalOperator &root,
																const LogicalPlanReadRuntimeBounds &bounds,
																const RowVisitor &visitor) const {
		// Keep the public callback contract fail-closed before wrapping it in
		// the state-aware internal visitor used by result materialization.
		validateBounds(bounds, static_cast<bool>(visitor));
		return executeInternal(
				backend_, root, bounds, [&visitor](LogicalPlanReadRow &row, ExecutionState &state) {
					materializeRowEntityRefs(row, state);
					enforceMaterializedRowBound(row, state);
					return visitor(row);
				},
				nullptr, true);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::execute(const logical::LogicalOperator &root,
																const LogicalPlanReadRuntimeBounds &bounds,
																const RowVisitor &visitor,
																const QueryContext &context) const {
		validateBounds(bounds, static_cast<bool>(visitor));
		return executeInternal(
				backend_, root, bounds, [&visitor](LogicalPlanReadRow &row, ExecutionState &state) {
					materializeRowEntityRefs(row, state);
					enforceMaterializedRowBound(row, state);
					return visitor(row);
				},
				&context, true);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::executeWithSnapshot(storage::LogicalGraphReadSnapshot &snapshot,
																			const logical::LogicalOperator &root,
																			const LogicalPlanReadRuntimeBounds &bounds,
																			const RowVisitor &visitor,
																			const QueryContext &context) const {
		return executeOnSnapshot(
				snapshot, root, bounds, [&visitor](LogicalPlanReadRow &row, ExecutionState &state) {
					materializeRowEntityRefs(row, state);
					enforceMaterializedRowBound(row, state);
					return visitor(row);
				},
				&context, true);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::executeWithSnapshotAndInputRow(
			storage::LogicalGraphReadSnapshot &snapshot, const logical::LogicalOperator &root,
			const LogicalPlanReadRuntimeBounds &bounds, const LogicalPlanReadRow &inputRow,
			const RowVisitor &visitor, const QueryContext &context) const {
		validateBounds(bounds, static_cast<bool>(visitor));
		LogicalPlanReadRow refreshed = inputRow;
		for (auto &[variable, value]: refreshed.values) {
			if (auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				*node = snapshot.readNodeShaped(node->node_id, completeNodeReadShape());
				refreshed.property_cache.erase(variable);
				refreshed.complete_entity_property_cache.erase(variable);
			} else if (auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				*edge = snapshot.readEdgeShaped(edge->edge_id, completeEdgeReadShape());
				refreshed.property_cache.erase(variable);
				refreshed.complete_entity_property_cache.erase(variable);
			}
		}
		ExecutionState state{snapshot, bounds, {}, &context, &refreshed, false, {}};
		const bool completed = visitOperator(root, state, [&](LogicalPlanReadRow row) {
			if (state.stats.rows_emitted == bounds.maximum_rows) {
				throw std::runtime_error("logical-plan read runtime exceeded its row bound");
			}
			incrementBounded(state.stats.rows_emitted, "logical-plan read runtime row counter overflowed");
			materializeRowEntityRefs(row, state);
			enforceMaterializedRowBound(row, state);
			return visitor(row);
		});
		if (!completed) {
			throw std::runtime_error("logical-plan read runtime input-row execution was stopped by its visitor");
		}
		return state.stats;
	}

	LogicalPlanReadValue LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			storage::LogicalGraphReadSnapshot &snapshot, const expressions::Expression &expression,
			LogicalPlanReadRow &row, const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
		// Resolving an uncached Property collection touches durable storage, so
		// expression-only evaluation enforces the same finite read limits as a plan.
		validateBounds(bounds, true);
		ExecutionState state{snapshot, bounds, {}, &context, nullptr, false, {}};
		checkGuard(state);
		return evaluateExpression(expression, row, state);
	}

	std::string LogicalPlanReadRuntime::makeDistinctRowKey(const LogicalPlanReadRow &row,
														  const std::vector<std::string> &columns) {
		if (columns.empty()) {
			throw std::invalid_argument("logical-plan DISTINCT key requires at least one output column");
		}
		return distinctUnionRowKey(row, columns);
	}

	QueryResult LogicalPlanReadRuntime::executeToQueryResult(const logical::LogicalOperator &root,
															 const LogicalPlanReadRuntimeBounds &bounds) const {
		QueryResult result;
		const std::vector<std::string> columns = visibleOutputColumns(root);
		result.setColumns(columns);
		static_cast<void>(executeInternal(
				backend_, root, bounds,
				[&result, &columns](LogicalPlanReadRow &row, ExecutionState &state) {
					enforceMaterializedRowBound(row, state);
					QueryResult::Row output;
					output.reserve(columns.size());
					MaterializedPropertyCache materialized;
					for (const std::string &column: columns) {
						const auto found = row.values.find(column);
						if (found == row.values.end()) {
							throw std::runtime_error("logical-plan runtime did not bind a declared output variable");
						}
						output.emplace(column, materializeResultValue(row, found->second, state, materialized));
					}
					enforceMaterializedResultRowBound(output, state);
					result.addRow(std::move(output));
					return true;
				},
				nullptr, true));
		return result;
	}

	QueryResult LogicalPlanReadRuntime::executeToQueryResult(const logical::LogicalOperator &root,
															 const LogicalPlanReadRuntimeBounds &bounds,
															 const QueryContext &context) const {
		QueryResult result;
		const std::vector<std::string> columns = visibleOutputColumns(root);
		result.setColumns(columns);
		static_cast<void>(executeInternal(
				backend_, root, bounds,
				[&result, &columns](LogicalPlanReadRow &row, ExecutionState &state) {
					enforceMaterializedRowBound(row, state);
					QueryResult::Row output;
					output.reserve(columns.size());
					MaterializedPropertyCache materialized;
					for (const std::string &column: columns) {
						const auto found = row.values.find(column);
						if (found == row.values.end()) {
							throw std::runtime_error("logical-plan runtime did not bind a declared output variable");
						}
						output.emplace(column, materializeResultValue(row, found->second, state, materialized));
					}
					enforceMaterializedResultRowBound(output, state);
					result.addRow(std::move(output));
					return true;
				},
				&context, true));
		return result;
	}

} // namespace graph::query::execution
