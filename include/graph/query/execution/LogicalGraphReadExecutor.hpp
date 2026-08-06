/**
 * @file LogicalGraphReadExecutor.hpp
 * @brief Bounded logical graph scan and traversal executor independent of DataManager.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::execution {

	// All bounds are mandatory. This executor is deliberately streaming so a
	// logical backend cannot turn a query into an unbounded in-memory graph copy.
	struct LogicalGraphReadExecutionBounds {
		size_t maximum_node_records = 0U;
		size_t maximum_edges_per_node = 0U;
		size_t maximum_property_entries = 0U;
		size_t maximum_rows = 0U;

		constexpr bool operator==(const LogicalGraphReadExecutionBounds &) const = default;
	};

	struct LogicalGraphNodeFilter {
		std::vector<std::string> required_labels;
		std::vector<VectorizedPropertyPredicate> property_predicates;
	};

	struct LogicalGraphEdgeFilter {
		std::optional<std::string> relationship_type;
		std::vector<VectorizedPropertyPredicate> property_predicates;
	};

	struct LogicalGraphExpansion {
		storage::LogicalGraphReadDirection direction = storage::LogicalGraphReadDirection::LGRD_OUTGOING;
		LogicalGraphEdgeFilter edge_filter;
		LogicalGraphNodeFilter target_filter;
	};

	struct LogicalGraphReadRequest {
		LogicalGraphNodeFilter source_filter;
		std::optional<LogicalGraphExpansion> expansion;
		LogicalGraphReadExecutionBounds bounds;
	};

	struct LogicalGraphReadRow {
		storage::LogicalGraphNode source_node;
		std::optional<storage::LogicalGraphEdge> edge;
		std::optional<storage::LogicalGraphNode> target_node;

		bool operator==(const LogicalGraphReadRow &) const = default;
	};

	struct LogicalGraphReadExecutionStats {
		size_t nodes_scanned = 0U;
		size_t nodes_matched = 0U;
		size_t edges_scanned = 0U;
		size_t edges_matched = 0U;
		// Direct owner-keyed Property entries are converted to a lookup map once
		// per filtered entity. Empty sets count too: their absence answers a
		// predicate without triggering a second storage read.
		size_t property_sets_materialized = 0U;
		size_t rows_emitted = 0U;

		constexpr bool operator==(const LogicalGraphReadExecutionStats &) const = default;
	};

	// This is a query execution kernel, not a Cypher parser or a read-write
	// transaction. It forms the format-independent read boundary that a future
	// physical-plan lowering path can target without accessing DataManager.
	class LogicalGraphReadExecutor final {
	public:
		using RowVisitor = std::function<void(const LogicalGraphReadRow &)>;

		explicit LogicalGraphReadExecutor(const storage::LogicalGraphReadBackend &backend) : backend_(backend) {}

		[[nodiscard]] LogicalGraphReadExecutionStats execute(const LogicalGraphReadRequest &request,
															 const RowVisitor &visitor) const;

	private:
		const storage::LogicalGraphReadBackend &backend_;
	};

} // namespace graph::query::execution
