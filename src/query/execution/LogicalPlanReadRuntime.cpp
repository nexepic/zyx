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
#include <optional>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalAggregateAccumulator.hpp"
#include "graph/query/execution/CsvReader.hpp"
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

		struct ExecutionState {
			storage::LogicalGraphReadSnapshot &snapshot;
			const LogicalPlanReadRuntimeBounds &bounds;
			LogicalPlanReadRuntimeStats stats;
			const QueryContext *context = nullptr;
			const LogicalPlanReadRow *injected_row = nullptr;
			// Logical snapshots are immutable. A small per-query Node cache avoids
			// repeated point reads when a high-degree adjacency list shares targets.
			std::unordered_map<uint64_t, storage::LogicalGraphNode> node_cache;
		};

		void checkGuard(const ExecutionState &state) {
			if (state.context != nullptr) {
				state.context->checkGuard();
			}
		}

		[[nodiscard]] storage::LogicalGraphNode readNodeCached(ExecutionState &state, uint64_t nodeId) {
			if (const auto cached = state.node_cache.find(nodeId); cached != state.node_cache.end()) {
				return cached->second;
			}
			storage::LogicalGraphNode node = state.snapshot.readNode(nodeId);
			constexpr size_t NODE_READ_CACHE_CAPACITY = 4'096U;
			const size_t capacity = std::min(NODE_READ_CACHE_CAPACITY, state.bounds.maximum_node_records);
			if (state.node_cache.size() < capacity) {
				state.node_cache.emplace(nodeId, node);
			}
			return node;
		}

		void validateBounds(const LogicalPlanReadRuntimeBounds &bounds, bool hasVisitor) {
			if (!hasVisitor || bounds.maximum_node_records == 0U || bounds.maximum_node_page_records == 0U ||
				bounds.maximum_metadata_pages_per_node_page == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_adjacency_page_records == 0U || bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U || bounds.maximum_distinct_rows == 0U ||
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
			return PropertyValue(std::get<storage::LogicalGraphEdge>(value).edge_id);
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
				return found == cached->second.end() ? PropertyValue() : found->second;
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
			const storage::LogicalGraphEdge &edge = std::get<storage::LogicalGraphEdge>(binding);
			auto [cached, inserted] = row.property_cache.emplace(variable, readEdgeProperties(snapshot, edge, state));
			static_cast<void>(inserted);
			const auto found = cached->second.find(key);
			return found == cached->second.end() ? PropertyValue() : found->second;
		}

		[[nodiscard]] const PropertyMap &readBindingProperties(storage::LogicalGraphReadSnapshot &snapshot,
																		 LogicalPlanReadRow &row,
																		 const std::string &variable,
																		 ExecutionState &state) {
			if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end()) {
				return cached->second;
			}
			const LogicalPlanReadValue &binding = requireBinding(row, variable);
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() != PropertyType::MAP) {
					throw std::runtime_error("logical-plan entity Property function requires a node, relationship, or Map");
				}
				return row.property_cache.emplace(variable, value->getMap()).first->second;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				return row.property_cache.emplace(variable, readNodeProperties(snapshot, *node, state)).first->second;
			}
			const storage::LogicalGraphEdge &edge = std::get<storage::LogicalGraphEdge>(binding);
			return row.property_cache.emplace(variable, readEdgeProperties(snapshot, edge, state)).first->second;
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
				}
				row_.values.insert_or_assign(variable_, std::move(value));
				row_.property_cache.erase(variable_);
			}

			~ScopedRowBinding() {
				if (previous_value_.has_value()) {
					row_.values.insert_or_assign(variable_, std::move(*previous_value_));
				} else {
					row_.values.erase(variable_);
				}
				if (previous_properties_.has_value()) {
					row_.property_cache.insert_or_assign(variable_, std::move(*previous_properties_));
				} else {
					row_.property_cache.erase(variable_);
				}
			}

			ScopedRowBinding(const ScopedRowBinding &) = delete;
			ScopedRowBinding &operator=(const ScopedRowBinding &) = delete;

		private:
			LogicalPlanReadRow &row_;
			std::string variable_;
			std::optional<LogicalPlanReadValue> previous_value_;
			std::optional<PropertyMap> previous_properties_;
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
			size_t sourceEdges = 0U;
			const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				for (;;) {
					if (sourceEdges == state.bounds.maximum_edges_per_node) {
						throw std::runtime_error("logical-plan pattern expression exceeded its per-Node Edge bound");
					}
					const size_t pageRecords = std::min(state.bounds.maximum_edges_per_node - sourceEdges,
														 state.bounds.maximum_adjacency_page_records);
					const storage::LogicalGraphAdjacencyPage page = state.snapshot.scanAdjacentEdges(
							source.node_id, adjacencyDirection, continuation, pageRecords);
					if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
						throw std::runtime_error("logical graph backend returned an invalid pattern-expression adjacency page");
					}
					if (continuation.has_value() &&
						(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
						throw std::runtime_error("logical graph backend returned a discontinuous pattern-expression adjacency page");
					}
					for (const storage::LogicalGraphEdge &edge: page.edges) {
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
						const storage::LogicalGraphNode target = state.snapshot.readNode(targetId);
						validateNode(target);
						if (target.node_id != targetId) {
							throw std::runtime_error(
									"logical graph backend returned a pattern-expression Node with the wrong identity");
						}
						if (!targetLabel.empty() &&
							std::ranges::find(target.labels, targetLabel) == target.labels.end()) {
							continue;
						}
						if (!visitor(edge, target)) {
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
				throw std::runtime_error("logical-plan id() requires a node or relationship variable");
			}
			if (name == "labels") {
				const auto *node = std::get_if<storage::LogicalGraphNode>(&binding);
				if (node == nullptr) {
					throw std::runtime_error("logical-plan labels() requires a node variable");
				}
				if (node->labels.size() > state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan labels() exceeded the configured List bound");
				}
				std::vector<PropertyValue> labels;
				labels.reserve(node->labels.size());
				for (const std::string &label: node->labels) {
					labels.emplace_back(label);
				}
				return PropertyValue(std::move(labels));
			}
			if (name == "type") {
				const auto *edge = std::get_if<storage::LogicalGraphEdge>(&binding);
				if (edge == nullptr) {
					throw std::runtime_error("logical-plan type() requires a relationship variable");
				}
				return PropertyValue(edge->type);
			}
			if (name != "keys" && name != "properties") {
				throw std::logic_error("logical-plan received an unknown entity introspection function");
			}
			const auto *scalar = std::get_if<PropertyValue>(&binding);
			if (!std::holds_alternative<storage::LogicalGraphNode>(binding) &&
				!std::holds_alternative<storage::LogicalGraphEdge>(binding) &&
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
					const auto *sourceNode = source == row.values.end()
										 ? nullptr
										 : std::get_if<storage::LogicalGraphNode>(&source->second);
					if (sourceNode == nullptr) {
						return PropertyValue(false);
					}
					validateNode(*sourceNode);
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
					const bool hasRelationship = exists.getPattern().find('-') != std::string::npos;
					if (!hasRelationship) {
						return PropertyValue(matchesWhere(nullptr));
					}
					bool matched = false;
					static_cast<void>(visitPatternTargets(*sourceNode, exists.getRelType(), exists.getTargetLabel(),
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
					const auto *sourceNode = source == row.values.end()
										 ? nullptr
										 : std::get_if<storage::LogicalGraphNode>(&source->second);
					if (sourceNode == nullptr) {
						return PropertyValue(std::vector<PropertyValue>{});
					}
					validateNode(*sourceNode);
					std::vector<PropertyValue> values;
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
							values.push_back(
									scalarValue(evaluateExpression(*comprehension.getMapExpression(), row, state)));
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
					if (!hasRelationship) {
						static_cast<void>(appendValue(nullptr, nullptr));
						return PropertyValue(std::move(values));
					}
					static_cast<void>(visitPatternTargets(*sourceNode, comprehension.getRelType(),
							comprehension.getTargetLabel(), comprehension.getDirection(), state,
							[&](const storage::LogicalGraphEdge &edge, const storage::LogicalGraphNode &target) {
								return appendValue(&edge, &target);
							}));
					return PropertyValue(std::move(values));
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
					const auto *start = startBinding == row.values.end()
										? nullptr
										: std::get_if<storage::LogicalGraphNode>(&startBinding->second);
					const auto *end = endBinding == row.values.end()
									  ? nullptr
									  : std::get_if<storage::LogicalGraphNode>(&endBinding->second);
					if (start == nullptr || end == nullptr) {
						return PropertyValue();
					}
					validateNode(*start);
					validateNode(*end);
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
						storage::LogicalGraphEdge edge;
					};
					std::unordered_map<uint64_t, int> distances;
					std::unordered_map<uint64_t, std::vector<Parent>> parents;
					std::deque<uint64_t> frontier;
					distances.emplace(start->node_id, 0);
					frontier.push_back(start->node_id);
					size_t parentEntries = 0U;
					std::optional<int> targetDistance = start->node_id == end->node_id ? std::optional<int>{0} : std::nullopt;
					size_t exploredEdges = 0U;
					const size_t maximumExploredEdges = [&]() {
						if (state.bounds.maximum_node_records >
							std::numeric_limits<size_t>::max() / state.bounds.maximum_edges_per_node) {
							throw std::overflow_error("logical-plan shortestPath Edge budget overflowed");
						}
						return state.bounds.maximum_node_records * state.bounds.maximum_edges_per_node;
					}();

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
								const storage::LogicalGraphAdjacencyPage page = state.snapshot.scanAdjacentEdges(
										nodeId, adjacencyDirection, continuation, pageRecords);
								if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
									throw std::runtime_error("logical graph backend returned an invalid shortestPath adjacency page");
								}
								if (continuation.has_value() &&
									(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
									throw std::runtime_error("logical graph backend returned a discontinuous shortestPath adjacency page");
								}
								for (const storage::LogicalGraphEdge &edge: page.edges) {
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
									const storage::LogicalGraphNode target = state.snapshot.readNode(next);
									validateNode(target);
									if (target.node_id != next) {
										throw std::runtime_error(
												"logical graph backend returned a shortestPath Node with the wrong identity");
									}
									if (!onEdge(edge, next)) {
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
									throw std::runtime_error("logical graph backend returned an invalid shortestPath continuation");
								}
								continuation = page.continuation;
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
						static_cast<void>(visitAdjacent(current, [&](const storage::LogicalGraphEdge &edge, uint64_t next) {
							const int nextDistance = currentDistance + 1;
							if (nextDistance > maximumHops) {
								return true;
							}
							// A target reached before the requested lower hop bound cannot
							// contribute to a simple path. Do not let that too-short route
							// suppress a later valid route to the same target.
							if (next == end->node_id && nextDistance < shortest.getMinHops()) {
								return true;
							}
							const auto existing = distances.find(next);
							if (existing == distances.end()) {
								if (distances.size() == state.bounds.maximum_node_records) {
									throw std::runtime_error("logical-plan shortestPath exceeded its frontier Node bound");
								}
								distances.emplace(next, nextDistance);
								frontier.push_back(next);
								if (parentEntries == state.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								parents[next].push_back({current, edge});
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							} else if (existing->second == nextDistance) {
								if (parentEntries == state.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan shortestPath exceeded its parent-DAG bound");
								}
								parents[next].push_back({current, edge});
								incrementBounded(parentEntries,
												 "logical-plan shortestPath parent-DAG counter overflowed");
							}
							if (next == end->node_id) {
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
														const std::vector<storage::LogicalGraphEdge> &edges) {
						std::vector<PropertyValue> path;
						path.reserve(nodeIds.size() + edges.size());
						for (size_t index = 0U; index < nodeIds.size(); ++index) {
							const storage::LogicalGraphNode node = state.snapshot.readNode(nodeIds[index]);
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
							const storage::LogicalGraphEdge &edge = edges[index];
							validateEdge(edge);
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
					std::vector<uint64_t> reversedNodes{end->node_id};
					std::vector<storage::LogicalGraphEdge> reversedEdges;
					std::function<void(uint64_t)> collectPaths = [&](uint64_t nodeId) {
						if (allPaths.size() == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan shortestPath exceeded its result-path bound");
						}
						if (nodeId == start->node_id) {
							std::vector<uint64_t> nodes(reversedNodes.rbegin(), reversedNodes.rend());
							std::vector<storage::LogicalGraphEdge> edges(reversedEdges.rbegin(), reversedEdges.rend());
							allPaths.push_back(materializePath(nodes, edges));
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
					collectPaths(end->node_id);
					if (allPaths.empty()) {
						throw std::runtime_error("logical-plan shortestPath failed to materialize a discovered path");
					}
					return shortest.isAll() ? PropertyValue(std::move(allPaths)) : std::move(allPaths.front());
				}
				default:
					throw std::logic_error("logical-plan runtime does not support this expression type");
			}
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
			return found->second == value;
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
			output.push_back('E');
			appendDistinctUint64(output, std::get<storage::LogicalGraphEdge>(value).edge_id);
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

		[[nodiscard]] const PropertyMap &materializeEntityProperties(const LogicalPlanReadRow &row,
																	 const LogicalPlanReadValue &value,
																	 ExecutionState &state,
																	 MaterializedPropertyCache &materialized) {
			const std::string cacheKey = [&value]() {
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
					return std::string("N:") + std::to_string(node->node_id);
				}
				return std::string("E:") + std::to_string(std::get<storage::LogicalGraphEdge>(value).edge_id);
			}();
			if (const auto cached = materialized.find(cacheKey); cached != materialized.end()) {
				return cached->second;
			}
			for (const auto &[alias, candidate]: row.values) {
				if (candidate != value) {
					continue;
				}
				if (const auto cached = row.property_cache.find(alias); cached != row.property_cache.end()) {
					return materialized.emplace(cacheKey, cached->second).first->second;
				}
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node);
				return materialized.emplace(cacheKey, readNodeProperties(state.snapshot, *node, state)).first->second;
			}
			const auto &edge = std::get<storage::LogicalGraphEdge>(value);
			validateEdge(edge);
			return materialized.emplace(cacheKey, readEdgeProperties(state.snapshot, edge, state)).first->second;
		}

		[[nodiscard]] ResultValue materializeResultValue(const LogicalPlanReadRow &row,
																 const LogicalPlanReadValue &value, ExecutionState &state,
																 MaterializedPropertyCache &materialized) {
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return ResultValue(*property);
			}
			const PropertyMap &properties = materializeEntityProperties(row, value, state, materialized);
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return ResultValue(LogicalNodeResult{node->node_id, node->labels, properties});
			}
			const auto &edge = std::get<storage::LogicalGraphEdge>(value);
			return ResultValue(
					LogicalEdgeResult{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type, properties});
		}

		[[nodiscard]] bool visitOperator(const logical::LogicalOperator &op, ExecutionState &state,
										 const InternalVisitor &visitor);

		// An anchored one-hop COUNT(v) can use adjacency topology when the Edge
		// predicate is empty. The source scan and any target predicates still use the
		// ordinary bounded/index-aware runtime path.
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

			debug::ScopedPerfTimer timer("logical_plan.anchored_relationship_count");
			size_t inputRows = 0U;
			const bool supportsExistenceProbes = state.snapshot.supportsNodeExistenceProbes();
			const auto visitSource = [&](LogicalPlanReadRow row) {
				const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
				const auto *source = std::get_if<storage::LogicalGraphNode>(&sourceBinding);
				if (source == nullptr) {
					throw std::runtime_error("logical-plan anchored relationship count source is not a Node");
				}
				validateNode(*source);
				size_t sourceEdges = 0U;
				const auto visitDirection = [&](storage::LogicalGraphReadDirection direction) {
					if (sourceEdges == state.bounds.maximum_edges_per_node) {
						// Ask for one bounded probe so an additional Edge is rejected rather
						// than silently truncating the aggregate input.
						state.snapshot.visitAdjacentEdgeTopologies(
								source->node_id, direction, 1U,
								[](const storage::LogicalGraphEdgeTopology &) -> bool {
									throw std::runtime_error(
											"logical-plan anchored relationship count exceeded its per-Node Edge bound");
								});
						return;
					}
					uint64_t previousEdgeId = 0U;
					const size_t directionLimit = state.bounds.maximum_edges_per_node - sourceEdges;
					state.snapshot.visitAdjacentEdgeTopologies(
							source->node_id, direction, directionLimit,
							[&](const storage::LogicalGraphEdgeTopology &edge) {
								checkGuard(state);
								if (sourceEdges == state.bounds.maximum_edges_per_node) {
									throw std::runtime_error("logical-plan anchored relationship count exceeded its "
															 "per-Node Edge bound");
								}
								validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "anchored relationship count");
								const bool endpointMatches =
										direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												? edge.source_node_id == source->node_id
												: edge.target_node_id == source->node_id;
								if (!endpointMatches) {
									throw std::runtime_error(
											"logical graph backend returned an invalid anchored relationship topology");
								}
								++sourceEdges;
								incrementBounded(state.stats.edges_scanned,
												 "logical-plan anchored relationship Edge counter overflowed");
								if (traversal.getDirection() == "both" &&
									direction == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
									edge.source_node_id == source->node_id) {
									return true;
								}
								if (!traversal.getEdgeType().empty() && edge.type != traversal.getEdgeType()) {
									return true;
								}
								if (!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty()) {
									const storage::LogicalGraphNode target = readNodeCached(
											state, direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
														   ? edge.target_node_id
														   : edge.source_node_id);
									if (!nodeMatches(state.snapshot, target, traversal.getTargetLabels(),
													 traversal.getTargetProperties(), state)) {
										return true;
									}
								} else if (supportsExistenceProbes) {
									const uint64_t targetId =
											direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
									if (!state.snapshot.containsNode(targetId)) {
										throw std::runtime_error("logical graph backend returned an Edge topology with "
																 "a missing endpoint");
									}
								} else {
									const uint64_t targetId =
											direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
									validateNode(state.snapshot.readNode(targetId));
								}
								if (inputRows == state.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
								}
								incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
								return true;
							});
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
			static_cast<void>(visitOperator(seed, state, sourceVisitor));
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
			if (aggregateChildren.size() != 1U || aggregateChildren.front() == nullptr ||
				aggregateChildren.front()->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
				return std::nullopt;
			}
			const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(*aggregateChildren.front());
			if (traversal.getDirection() != "out" || traversal.getEdgeVar().empty() ||
				!traversal.getTargetLabels().empty() || !traversal.getTargetProperties().empty() ||
				!traversal.getEdgeProperties().empty()) {
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
			constexpr size_t DIRECT_COUNT_ENDPOINT_CACHE_CAPACITY = 4'096U;
			std::unordered_set<uint64_t> validatedEndpoints;
			validatedEndpoints.reserve(DIRECT_COUNT_ENDPOINT_CACHE_CAPACITY);
			const bool supportsExistenceProbes = state.snapshot.supportsNodeExistenceProbes();
			const auto validateEndpoint = [&](uint64_t nodeId) {
				if (validatedEndpoints.contains(nodeId)) {
					return;
				}
				if (supportsExistenceProbes) {
					if (nodeId == 0U || !state.snapshot.containsNode(nodeId)) {
						throw std::runtime_error("logical graph backend returned an Edge topology with a missing endpoint");
					}
				} else {
					validateNode(state.snapshot.readNode(nodeId));
				}
				if (validatedEndpoints.size() < DIRECT_COUNT_ENDPOINT_CACHE_CAPACITY) {
					static_cast<void>(validatedEndpoints.insert(nodeId));
				}
			};

			size_t inputRows = 0U;
			state.snapshot.visitEdgeTopologies(
					state.bounds.maximum_rows + 1U, [&](const storage::LogicalGraphEdgeTopology &edge) {
				checkGuard(state);
				if (inputRows == state.bounds.maximum_rows) {
					throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
				}
				incrementBounded(state.stats.edges_scanned,
							 "logical-plan read runtime Edge counter overflowed");
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
					throw std::runtime_error("logical graph backend returned an Edge topology with invalid identity or endpoints");
				}
				if (!traversal.getEdgeType().empty() && edge.type != traversal.getEdgeType()) {
					return true;
				}
				validateEndpoint(edge.source_node_id);
				validateEndpoint(edge.target_node_id);
				incrementBounded(inputRows, "logical-plan Aggregate input counter overflowed");
				return true;
				});

			LogicalPlanReadRow result;
			result.values.emplace(count.alias, PropertyValue(static_cast<int64_t>(inputRows)));
			return visitor(std::move(result));
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
					std::vector<std::pair<std::string, PropertyValue>> equalityPredicates = scan.getPropertyPredicates();
					equalityPredicates.insert(equalityPredicates.end(), parameterPredicates->begin(), parameterPredicates->end());
					const bool exactPropertyLookup = !equalityPredicates.empty() &&
							scan.getRangePredicates().empty() && !scan.getCompositeEquality().has_value();
					const bool rangePropertyLookup = equalityPredicates.empty() && scan.getRangePredicates().size() == 1U &&
							!scan.getCompositeEquality().has_value();
					const bool compositeRangePropertyLookup = !equalityPredicates.empty() &&
							scan.getRangePredicates().size() == 1U && !scan.getCompositeEquality().has_value();
					std::optional<std::vector<storage::LogicalGraphNode>> indexNodes;
					if (exactPropertyLookup) {
						indexNodes = state.snapshot.findNodesByExactProperties(
								scan.getLabels(), equalityPredicates, state.bounds.maximum_node_records,
								state.bounds.maximum_property_entries);
					} else if (rangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						indexNodes = state.snapshot.findNodesByPropertyRange(
								scan.getLabels(), {range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive},
								state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
					} else if (compositeRangePropertyLookup) {
						const logical::RangePredicate &range = scan.getRangePredicates().front();
						indexNodes = state.snapshot.findNodesByCompositePropertyRange(
								scan.getLabels(), {equalityPredicates,
														  {range.key, range.minValue, range.maxValue, range.minInclusive, range.maxInclusive}},
								state.bounds.maximum_node_records, state.bounds.maximum_property_entries);
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
										throw std::runtime_error(
													"logical graph indexed Node lookup returned a Node outside the requested label scope");
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
					return visitOperator(requireOnlyChild(filter), state, [&](LogicalPlanReadRow row) {
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
					return visitOperator(requireOnlyChild(project), state, [&](LogicalPlanReadRow row) {
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
							static_cast<void>(distinctRows.insert(key));
						}
						return visitor(std::move(projected));
					});
				}
				case logical::LogicalOpType::LOP_AGGREGATE: {
					const auto &aggregate = dynamic_cast<const logical::LogicalAggregate &>(op);
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

					struct AggregateGroup {
						std::vector<PropertyValue> values;
						std::vector<std::unique_ptr<AggregateAccumulator>> accumulators;
						std::vector<std::unordered_set<std::string>> distinct_values;
					};
					std::vector<AggregateGroup> groups;
					std::unordered_map<std::string, size_t> groupIndexes;
					const auto createGroup = [&](std::vector<PropertyValue> values) {
						if (groups.size() == state.bounds.maximum_distinct_rows ||
							groups.size() == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Aggregate exceeded its group bound");
						}
						AggregateGroup group;
						group.values = std::move(values);
						group.accumulators.reserve(aggregations.size());
						group.distinct_values.resize(aggregations.size());
						for (size_t index = 0U; index < aggregations.size(); ++index) {
							std::unique_ptr<AggregateAccumulator> accumulator =
									createAccumulator(functions[index], false, percentiles[index]);
							if (!accumulator) { // aggregateFunctionType accepts exactly the factory's supported enum
												// values.
								throw std::logic_error("logical-plan Aggregate could not create an accumulator");
							}
							group.accumulators.push_back(std::move(accumulator));
						}
						groups.push_back(std::move(group));
						return groups.size() - 1U;
					};
					if (groupExpressions.empty()) {
						static_cast<void>(createGroup({}));
					}

					size_t inputRows = 0U;
					const bool completed =
							visitOperator(requireOnlyChild(aggregate), state, [&](LogicalPlanReadRow row) {
								checkGuard(state);
								if (inputRows == state.bounds.maximum_rows) {
									throw std::runtime_error("logical-plan Aggregate exceeded its input row bound");
								}
								++inputRows;
								std::vector<PropertyValue> groupValues;
								groupValues.reserve(groupExpressions.size());
								std::string groupKey;
								for (const std::shared_ptr<expressions::Expression> &expression: groupExpressions) {
									const PropertyValue value =
											scalarValue(evaluateExpression(*expression, row, state));
									appendDistinctPropertyValue(groupKey, value);
									groupValues.push_back(value);
								}
								size_t groupIndex = 0U;
								if (!groupExpressions.empty()) {
									const auto found = groupIndexes.find(groupKey);
									if (found == groupIndexes.end()) {
										groupIndex = createGroup(std::move(groupValues));
										groupIndexes.emplace(std::move(groupKey), groupIndex);
									} else {
										groupIndex = found->second;
									}
								}
								AggregateGroup &group = groups[groupIndex];
								for (size_t index = 0U; index < aggregations.size(); ++index) {
									const logical::LogicalAggItem &item = aggregations[index];
									if (!item.argument) {
										group.accumulators[index]->updateRow();
										continue;
									}
									const PropertyValue value =
											scalarValue(evaluateExpression(*item.argument, row, state));
									if (item.distinct) {
										std::string key;
										appendDistinctPropertyValue(key, value);
										if (!group.distinct_values[index].insert(std::move(key)).second) {
											continue;
										}
									}
									group.accumulators[index]->update(value);
								}
								return true;
							});
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Aggregate could not prove that its complete input was read");
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

					std::vector<LogicalPlanReadRow> rightRows;
					rightRows.reserve(state.bounds.maximum_rows);
					const bool rightCompleted = visitOperator(*right, state, [&](LogicalPlanReadRow row) {
						if (rightRows.size() == state.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan Join exceeded its materialized right-side row bound");
						}
						rightRows.push_back(normalizeOutputRow(row, rightColumns));
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
						for (const LogicalPlanReadRow &rightRow: rightRows) {
							checkGuard(state);
							LogicalPlanReadRow combined = normalizedLeft;
							bool compatible = true;
							for (const auto &[column, value]: rightRow.values) {
								const auto existing = combined.values.find(column);
								if (existing != combined.values.end()) {
									if (existing->second != value) {
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

					std::unordered_map<std::string, std::vector<LogicalPlanReadRow>> patternRowsByKey;
					size_t patternRows = 0U;
					const bool patternCompleted = visitOperator(*pattern, state, [&](LogicalPlanReadRow row) {
						if (patternRows == state.bounds.maximum_rows) {
							throw std::runtime_error(
									"logical-plan OptionalMatch exceeded its materialized pattern row bound");
						}
						++patternRows;
						LogicalPlanReadRow normalized = normalizeOutputRow(row, patternColumns);
						patternRowsByKey[bindingKey(normalized, sharedColumns)].push_back(std::move(normalized));
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
						for (const LogicalPlanReadRow &match: matches->second) {
							checkGuard(state);
							LogicalPlanReadRow combined = normalizedInput;
							for (const auto &[column, value]: match.values) {
								if (!inputColumnSet.contains(column)) {
									combined.values.emplace(column, value);
								}
							}
							for (const auto &[column, properties]: match.property_cache) {
								if (!inputColumnSet.contains(column)) {
									combined.property_cache.emplace(column, properties);
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
						std::vector<const storage::LogicalGraphNode *> boundNodes;
						boundNodes.reserve(nodeVariables.size());
						for (const std::string &variable: nodeVariables) {
							const LogicalPlanReadValue &value = requireBinding(row, variable);
							const auto *node = std::get_if<storage::LogicalGraphNode>(&value);
							if (node == nullptr) {
								throw std::runtime_error("logical-plan NamedPath Node component is not a Node");
							}
							validateNode(*node);
							boundNodes.push_back(node);
						}
						std::vector<const storage::LogicalGraphNode *> nodes = boundNodes;
						std::vector<const storage::LogicalGraphEdge *> edges;
						if (capturedVariableLengthPath) {
							const auto captured = row.path_cache.find(nodeVariables.back());
							if (captured == row.path_cache.end() || captured->second.nodes.size() < 2U ||
								captured->second.nodes.size() != captured->second.edges.size() + 1U ||
								captured->second.nodes.front().node_id != boundNodes.front()->node_id ||
								captured->second.nodes.back().node_id != boundNodes.back()->node_id) {
								throw std::runtime_error("logical-plan NamedPath lost its variable-length traversal route");
							}
							nodes.clear();
							nodes.reserve(captured->second.nodes.size());
							for (const storage::LogicalGraphNode &node: captured->second.nodes) {
								validateNode(node);
								nodes.push_back(&node);
							}
							edges.reserve(captured->second.edges.size());
							for (const storage::LogicalGraphEdge &edge: captured->second.edges) {
								validateEdge(edge);
								edges.push_back(&edge);
							}
						} else {
							edges.reserve(edgeVariables.size());
							for (const std::string &variable: edgeVariables) {
								const LogicalPlanReadValue &value = requireBinding(row, variable);
								const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value);
								if (edge == nullptr) {
									throw std::runtime_error(
											"logical-plan NamedPath relationship component is not a relationship");
								}
								validateEdge(*edge);
								edges.push_back(edge);
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

					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						const storage::LogicalGraphNode *source =
								std::get_if<storage::LogicalGraphNode>(&sourceBinding);
						if (source == nullptr) {
							throw std::runtime_error("logical-plan variable-length Traversal source is not a Node");
						}
						validateNode(*source);
						std::unordered_set<uint64_t> pathNodes;
						pathNodes.insert(source->node_id);
						std::vector<storage::LogicalGraphNode> pathNodeRecords{*source};
						std::vector<storage::LogicalGraphEdge> pathEdgeRecords;
						size_t edgeVisits = 0U;

						const auto visitAdjacent = [&](uint64_t nodeId,
													   const std::function<bool(const storage::LogicalGraphEdge &,
																				const storage::LogicalGraphNode &)>
															   &onEdge) {
							const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
								std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
								uint64_t previousEdgeId = 0U;
								for (;;) {
									if (edgeVisits == maximumEdgeVisits) {
										throw std::runtime_error("logical-plan variable-length Traversal exceeded its "
																 "edge exploration bound");
									}
									const size_t remaining = maximumEdgeVisits - edgeVisits;
									const size_t pageRecords =
											std::min(remaining, state.bounds.maximum_adjacency_page_records);
									const storage::LogicalGraphAdjacencyPage page = state.snapshot.scanAdjacentEdges(
											nodeId, adjacencyDirection, continuation, pageRecords);
									if (page.edges.size() > pageRecords ||
										(page.edges.empty() && page.continuation.has_value())) {
										throw std::runtime_error("logical graph backend returned an invalid "
																 "variable-length adjacency page");
									}
									for (const storage::LogicalGraphEdge &edge: page.edges) {
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
										const storage::LogicalGraphNode target = readNodeCached(state, targetId);
										validateNode(target);
										if (!onEdge(edge, target)) {
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
								if (bindValue(expanded, traversal.getTargetVar(), node)) {
									expanded.path_cache.insert_or_assign(
										traversal.getTargetVar(),
										LogicalPlanReadPath{pathNodeRecords, pathEdgeRecords});
									if (!visitor(std::move(expanded))) {
										return false;
									}
								}
							}
							if (depth == maximumHops) {
								return true;
							}
							return visitAdjacent(node.node_id, [&](const storage::LogicalGraphEdge &edge,
														   const storage::LogicalGraphNode &target) {
								if (pathNodes.contains(target.node_id)) {
									return true;
								}
								pathNodes.insert(target.node_id);
								pathEdgeRecords.push_back(edge);
								pathNodeRecords.push_back(target);
								const bool completed = visitPath(target, depth + 1);
								pathNodeRecords.pop_back();
								pathEdgeRecords.pop_back();
								pathNodes.erase(target.node_id);
								return completed;
							});
						};
						return visitPath(*source, 0);
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
						for (const PropertyValue &value: values) {
							checkGuard(state);
							LogicalPlanReadRow expanded = input;
							expanded.values.insert_or_assign(unwind.getAlias(), value);
							expanded.property_cache.erase(unwind.getAlias());
							expanded.path_cache.erase(unwind.getAlias());
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
								static_cast<void>(distinctRows.insert(key));
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
					std::vector<DecoratedRow> rows;
					rows.reserve(std::min(state.bounds.maximum_rows, state.bounds.maximum_node_records));
					const bool completed = visitOperator(requireOnlyChild(sort), state, [&](LogicalPlanReadRow row) {
						if (rows.size() == state.bounds.maximum_rows) {
							throw std::runtime_error("logical-plan Sort exceeded its materialization row bound");
						}
						// Delay expression evaluation until we know ordering can affect the
						// result. This also preserves valid single-row aggregate results when
						// an otherwise unavailable input-only ORDER BY expression is present.
						rows.push_back({std::move(row), {}});
						return true;
					});
					if (!completed) {
						throw std::runtime_error(
								"logical-plan Sort could not prove that its materialized input was exhausted");
					}
					if (rows.size() > 1U) {
						for (DecoratedRow &row: rows) {
							row.keys.reserve(items.size());
							for (const logical::LogicalSortItem &item: items) {
								checkGuard(state);
								row.keys.push_back(scalarValue(evaluateExpression(*item.expression, row.row, state)));
							}
						}
						std::stable_sort(rows.begin(), rows.end(),
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
					for (DecoratedRow &row: rows) {
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
					size_t skipped = 0U;
					return visitOperator(requireOnlyChild(skip), state, [&](LogicalPlanReadRow row) {
						if (static_cast<uint64_t>(skipped) < offset) {
							incrementBounded(skipped, "logical-plan SKIP counter overflowed");
							return true;
						}
						return visitor(std::move(row));
					});
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
					size_t delivered = 0U;
					return visitOperator(requireOnlyChild(limit), state, [&](LogicalPlanReadRow row) {
						if (static_cast<uint64_t>(delivered) ==
							maximum) { // ZYX_COV_EXCL_LINE: the previous callback returns false as soon as the limit is
									   // reached.
							return false;
						}
						incrementBounded(delivered, "logical-plan LIMIT counter overflowed");
						const bool continueVisitor = visitor(std::move(row));
						return continueVisitor && static_cast<uint64_t>(delivered) < maximum;
					});
				}
				case logical::LogicalOpType::LOP_TRAVERSAL: {
					const auto &traversal = dynamic_cast<const logical::LogicalTraversal &>(op);
					const std::string &direction = traversal.getDirection();
					if (direction != "out" && direction != "in" && direction != "both") {
						throw std::invalid_argument("logical-plan Traversal has an unknown direction");
					}
					return visitOperator(requireOnlyChild(traversal), state, [&](LogicalPlanReadRow row) {
						const LogicalPlanReadValue &sourceBinding = requireBinding(row, traversal.getSourceVar());
						const storage::LogicalGraphNode *source =
								std::get_if<storage::LogicalGraphNode>(&sourceBinding);
						if (source == nullptr) {
							throw std::runtime_error("logical-plan Traversal source is not a Node");
						}
						validateNode(*source);
						size_t sourceEdges = 0U;
						const auto visitDirection = [&](storage::LogicalGraphReadDirection adjacencyDirection) {
							std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
							uint64_t previousEdgeId = 0U;
							for (;;) {
								const size_t remaining = state.bounds.maximum_edges_per_node - sourceEdges;
								if (remaining == 0U) { // ZYX_COV_EXCL_LINE: a full adjacency bound returns immediately
													   // after the preceding page.
									return true;
								}
								const size_t pageRecords =
										std::min(remaining, state.bounds.maximum_adjacency_page_records);
								const storage::LogicalGraphAdjacencyPage page = state.snapshot.scanAdjacentEdges(
										source->node_id, adjacencyDirection, continuation, pageRecords);
								if (page.edges.size() > pageRecords) {
									throw std::runtime_error("logical graph backend exceeded the adjacency page bound");
								}
								if (page.edges.empty()) {
									if (page.continuation.has_value()) {
										throw std::runtime_error(
												"logical graph backend returned an empty nonterminal adjacency page");
									}
									return true;
								}
								for (const storage::LogicalGraphEdge &edge: page.edges) {
									checkGuard(state);
									validateEdge(edge);
									validateAdjacentEdgeOrder(edge.edge_id, previousEdgeId, "Traversal");
									const bool endpointMatches =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.source_node_id == source->node_id
													: edge.target_node_id == source->node_id;
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
										edge.source_node_id == source->node_id) {
										continue;
									}
									if (!edgeMatches(state.snapshot, edge, traversal.getEdgeType(),
													 traversal.getEdgeProperties(), state)) {
										continue;
									}
									const uint64_t targetId =
											adjacencyDirection == storage::LogicalGraphReadDirection::LGRD_OUTGOING
													? edge.target_node_id
													: edge.source_node_id;
										const storage::LogicalGraphNode target = readNodeCached(state, targetId);
									if (!nodeMatches(state.snapshot, target, traversal.getTargetLabels(),
													 traversal.getTargetProperties(), state)) {
										continue;
									}
									LogicalPlanReadRow expanded = row;
									if (!bindValue(expanded, traversal.getEdgeVar(), edge) ||
										!bindValue(expanded, traversal.getTargetVar(), target)) {
										continue;
									}
									if (!visitor(std::move(expanded))) {
										return false;
									}
								}
								if (!page.continuation.has_value() ||
									sourceEdges == state.bounds.maximum_edges_per_node) {
									return true;
								}
								if (page.continuation->previous_edge_id != page.edges.back().edge_id ||
									page.continuation->next_edge_id == 0U ||
									page.continuation->previous_edge_id == page.continuation->next_edge_id) {
									throw std::runtime_error(
											"logical graph backend returned an invalid adjacency continuation");
								}
								continuation = page.continuation;
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
																	const QueryContext *context) {
			validateBounds(bounds, static_cast<bool>(visitor));
			ExecutionState state{snapshot, bounds, {}, context, nullptr, {}};
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
																  const QueryContext *context) {
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical graph backend returned no read snapshot");
			}
			return executeOnSnapshot(*snapshot, root, bounds, visitor, context);
		}

	} // namespace

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::execute(const logical::LogicalOperator &root,
																const LogicalPlanReadRuntimeBounds &bounds,
																const RowVisitor &visitor) const {
		// Keep the public callback contract fail-closed before wrapping it in
		// the state-aware internal visitor used by result materialization.
		validateBounds(bounds, static_cast<bool>(visitor));
		return executeInternal(
				backend_, root, bounds, [&visitor](LogicalPlanReadRow &row, ExecutionState &) { return visitor(row); },
				nullptr);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::execute(const logical::LogicalOperator &root,
																const LogicalPlanReadRuntimeBounds &bounds,
																const RowVisitor &visitor,
																const QueryContext &context) const {
		validateBounds(bounds, static_cast<bool>(visitor));
		return executeInternal(
				backend_, root, bounds, [&visitor](LogicalPlanReadRow &row, ExecutionState &) { return visitor(row); },
				&context);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::executeWithSnapshot(storage::LogicalGraphReadSnapshot &snapshot,
																			const logical::LogicalOperator &root,
																			const LogicalPlanReadRuntimeBounds &bounds,
																			const RowVisitor &visitor,
																			const QueryContext &context) const {
		return executeOnSnapshot(
				snapshot, root, bounds, [&visitor](LogicalPlanReadRow &row, ExecutionState &) { return visitor(row); },
				&context);
	}

	LogicalPlanReadRuntimeStats LogicalPlanReadRuntime::executeWithSnapshotAndInputRow(
			storage::LogicalGraphReadSnapshot &snapshot, const logical::LogicalOperator &root,
			const LogicalPlanReadRuntimeBounds &bounds, const LogicalPlanReadRow &inputRow,
			const RowVisitor &visitor, const QueryContext &context) const {
		validateBounds(bounds, static_cast<bool>(visitor));
		LogicalPlanReadRow refreshed = inputRow;
		for (auto &[variable, value]: refreshed.values) {
			if (auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				*node = snapshot.readNode(node->node_id);
				refreshed.property_cache.erase(variable);
			} else if (auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				*edge = snapshot.readEdge(edge->edge_id);
				refreshed.property_cache.erase(variable);
			}
		}
		ExecutionState state{snapshot, bounds, {}, &context, &refreshed, {}};
		const bool completed = visitOperator(root, state, [&](LogicalPlanReadRow row) {
			if (state.stats.rows_emitted == bounds.maximum_rows) {
				throw std::runtime_error("logical-plan read runtime exceeded its row bound");
			}
			incrementBounded(state.stats.rows_emitted, "logical-plan read runtime row counter overflowed");
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
		ExecutionState state{snapshot, bounds, {}, &context, nullptr, {}};
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
					result.addRow(std::move(output));
					return true;
				},
				nullptr));
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
					result.addRow(std::move(output));
					return true;
				},
				&context));
		return result;
	}

} // namespace graph::query::execution
