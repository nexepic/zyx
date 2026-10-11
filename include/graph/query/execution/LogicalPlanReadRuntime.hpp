/**
 * @file LogicalPlanReadRuntime.hpp
 * @brief Streaming logical-plan runtime over the format-independent graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::logical {
	class LogicalOperator;
}

namespace graph::query::expressions {
	class Expression;
}

namespace graph::query::execution {

	// Values intentionally retain logical labels, relationship types, and IDs.
	// They never expose legacy token IDs or physical storage locations.
	using LogicalPlanReadValue = std::variant<PropertyValue, storage::LogicalGraphNode, storage::LogicalGraphEdge>;

	// Variable-length traversal retains its selected path separately from regular
	// scalar bindings so a following NamedPath can materialize the exact route.
	struct LogicalPlanReadPath {
		std::vector<storage::LogicalGraphNode> nodes;
		std::vector<storage::LogicalGraphEdge> edges;

		bool operator==(const LogicalPlanReadPath &) const = default;
	};

	struct LogicalPlanReadRow {
		std::unordered_map<std::string, LogicalPlanReadValue> values;
		// A row owns only the Property collections it has already resolved from its
		// pinned snapshot. Operators use this cache to avoid duplicate reads while
		// keeping the cache lifetime bounded by the row.
		std::unordered_map<std::string, std::unordered_map<std::string, PropertyValue>> property_cache;
		std::unordered_map<std::string, LogicalPlanReadPath> path_cache;

		bool operator==(const LogicalPlanReadRow &) const = default;
	};

	struct LogicalPlanReadRuntimeBounds {
		size_t maximum_node_records = 0U;
		size_t maximum_node_page_records = 0U;
		size_t maximum_metadata_pages_per_node_page = 0U;
		size_t maximum_edges_per_node = 0U;
		size_t maximum_adjacency_page_records = 0U;
		size_t maximum_property_entries = 0U;
		size_t maximum_rows = 0U;
		size_t maximum_distinct_rows = 0U;

		constexpr bool operator==(const LogicalPlanReadRuntimeBounds &) const = default;
	};

	struct LogicalPlanReadRuntimeStats {
		size_t nodes_scanned = 0U;
		size_t edges_scanned = 0U;
		// Number of owner-keyed Property entry sets converted into a lookup map.
		size_t property_sets_materialized = 0U;
		size_t rows_emitted = 0U;

		constexpr bool operator==(const LogicalPlanReadRuntimeStats &) const = default;
	};

	// This runtime deliberately supports only the streaming read subset required
	// for the first production relocatable query path. Unsupported logical operators
	// fail closed instead of silently falling back to the legacy physical engine.
	class LogicalPlanReadRuntime final {
	public:
		using RowVisitor = std::function<bool(const LogicalPlanReadRow &)>;

		explicit LogicalPlanReadRuntime(const storage::LogicalGraphReadBackend &backend) : backend_(backend) {}

		[[nodiscard]] LogicalPlanReadRuntimeStats execute(const logical::LogicalOperator &root,
														  const LogicalPlanReadRuntimeBounds &bounds,
														  const RowVisitor &visitor) const;
		[[nodiscard]] LogicalPlanReadRuntimeStats execute(const logical::LogicalOperator &root,
														  const LogicalPlanReadRuntimeBounds &bounds,
														  const RowVisitor &visitor, const QueryContext &context) const;
		// Executes against a caller-owned logical snapshot. Mutation planners use
		// this form to retain the exact optimistic fence that selected their rows.
		[[nodiscard]] LogicalPlanReadRuntimeStats executeWithSnapshot(storage::LogicalGraphReadSnapshot &snapshot,
												  const logical::LogicalOperator &root,
												  const LogicalPlanReadRuntimeBounds &bounds,
												  const RowVisitor &visitor,
												  const QueryContext &context) const;
		// Evaluates a subtree from a supplied logical input row. This is used by
		// batched write subqueries to re-resolve imports under each fresh snapshot.
		[[nodiscard]] LogicalPlanReadRuntimeStats executeWithSnapshotAndInputRow(
				storage::LogicalGraphReadSnapshot &snapshot, const logical::LogicalOperator &root,
				const LogicalPlanReadRuntimeBounds &bounds, const LogicalPlanReadRow &inputRow,
				const RowVisitor &visitor, const QueryContext &context) const;

		// Materializes result entities only at the output boundary. Logical Node
		// and Edge values retain resolved labels/types and complete Property maps,
		// never legacy token encodings or physical storage locations.
		[[nodiscard]] query::QueryResult executeToQueryResult(const logical::LogicalOperator &root,
															  const LogicalPlanReadRuntimeBounds &bounds) const;
		[[nodiscard]] query::QueryResult executeToQueryResult(const logical::LogicalOperator &root,
															  const LogicalPlanReadRuntimeBounds &bounds,
															  const QueryContext &context) const;
		// Evaluates one expression against an already-pinned logical row. Write
		// planning uses this narrow entry point so read and write paths share
		// Property access, parameter, arithmetic, and null semantics.
		[[nodiscard]] static LogicalPlanReadValue
		evaluateExpressionWithSnapshot(storage::LogicalGraphReadSnapshot &snapshot,
									   const expressions::Expression &expression, LogicalPlanReadRow &row,
									   const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context);
		// A type-preserving, length-delimited key for bounded DISTINCT consumers
		// outside the streaming operator tree. Logical entities use stable IDs,
		// never an ambiguous display representation.
		[[nodiscard]] static std::string makeDistinctRowKey(const LogicalPlanReadRow &row,
														 const std::vector<std::string> &columns);

	private:
		const storage::LogicalGraphReadBackend &backend_;
	};

} // namespace graph::query::execution
