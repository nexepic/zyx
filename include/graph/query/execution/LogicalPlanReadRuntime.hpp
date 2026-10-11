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
#include <unordered_set>
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
	// EntityRef keeps scan rows compact until an operator actually needs labels,
	// type, or Properties. It is resolved only through the row's pinned snapshot.
	using LogicalPlanReadValue =
			std::variant<PropertyValue, storage::LogicalGraphNode, storage::LogicalGraphEdge, storage::EntityRef>;

	// Variable-length traversal retains its selected path separately from regular
	// scalar bindings so a following NamedPath can materialize the exact route.
	struct LogicalPlanReadPath {
		std::vector<storage::LogicalGraphNode> nodes;
		std::vector<storage::LogicalGraphEdge> edges;
		// Native variable-length traversal may retain topology references instead
		// of materializing each Edge. NamedPath resolves these references only when
		// it needs the complete public path representation.
		std::vector<storage::EntityRef> edge_refs;
		// The same delayed representation is used for endpoint Nodes when a
		// variable-length traversal has no target label/Property predicate. A zero
		// reference means that the corresponding entry in `nodes` is materialized;
		// otherwise the reference carries the stable logical Node identity.
		std::vector<storage::EntityRef> node_refs;

		bool operator==(const LogicalPlanReadPath &) const = default;
	};

	struct LogicalPlanReadRow {
		std::unordered_map<std::string, LogicalPlanReadValue> values;
		// A row owns only the Property collections it has already resolved from its
		// pinned snapshot. Operators use this cache to avoid duplicate reads while
		// keeping the cache lifetime bounded by the row.
		std::unordered_map<std::string, std::unordered_map<std::string, PropertyValue>> property_cache;
		// Snapshot-local results for metadata-only entity functions such as
		// labels() and type().  These values are separate from Property data so a
		// repeated introspection expression does not rematerialize the entity.
		std::unordered_map<std::string, std::unordered_map<std::string, PropertyValue>> entity_metadata_cache;
		// EntityRef property reads normally cache only selected fields. This set
		// records the aliases whose cache has been expanded to the complete native
		// Property collection, so `properties()` and map projections never mistake
		// a selected-field cache for an authoritative complete map.
		std::unordered_set<std::string> complete_entity_property_cache;
		std::unordered_map<std::string, LogicalPlanReadPath> path_cache;

		bool operator==(const LogicalPlanReadRow &) const = default;
	};

	struct LogicalPlanReadRuntimeBounds {
		size_t maximum_node_records = 0U;
		size_t maximum_node_page_records = 0U;
		size_t maximum_metadata_pages_per_node_page = 0U;
		size_t maximum_edges_per_node = 0U;
		// Bounds storage-wide Edge scans independently from per-Node adjacency.
		// Global relationship filters may inspect more records than they emit.
		size_t maximum_edge_records = 0U;
		size_t maximum_adjacency_page_records = 0U;
		size_t maximum_property_entries = 0U;
		size_t maximum_rows = 0U;
		size_t maximum_distinct_rows = 0U;
		// Bounds temporary row, Property, and sort materialization independently
		// from cardinality so a single LargeValue cannot exhaust query memory.
		size_t maximum_materialized_bytes = 64U * 1024U * 1024U;
		// Candidate batches may carry verified Property payloads before they become
		// rows. Keep this budget independent so an index result cannot bypass the
		// query's memory policy through a columnar staging path.
		size_t maximum_candidate_bytes = 16U * 1024U * 1024U;

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

	struct LogicalPlanBatchedPropertyRequest {
		std::string variable;
		std::string key;

		constexpr bool operator==(const LogicalPlanBatchedPropertyRequest &) const = default;
	};

	struct LogicalPlanBatchedMetadataRequest {
		std::string variable;
		std::string function;

		constexpr bool operator==(const LogicalPlanBatchedMetadataRequest &) const = default;
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
		// Collects the selected Property reads in expressions that can be safely
		// prefetched across rows. Unsupported expressions retain their evaluator
		// path; safe sibling expressions still batch. Returns false if none qualify.
		[[nodiscard]] static bool collectBatchedPropertyRequests(
				const std::vector<const expressions::Expression *> &expressions,
				std::vector<LogicalPlanBatchedPropertyRequest> &requests);
		[[nodiscard]] static std::vector<LogicalPlanBatchedMetadataRequest> collectBatchedMetadataRequests(
				const std::vector<const expressions::Expression *> &expressions);
		// Shares the scoped complete-Entity collector with buffered procedures.
		// Only properties(), keys() and all-Property map projections request a
		// complete binding; unsupported nested graph scopes keep their evaluator.
		[[nodiscard]] static std::vector<std::string> collectBatchedCompleteEntityVariables(
				const std::vector<const expressions::Expression *> &expressions);
		// A type-preserving, length-delimited key for bounded DISTINCT consumers
		// outside the streaming operator tree. Logical entities use stable IDs,
		// never an ambiguous display representation.
		[[nodiscard]] static std::string makeDistinctRowKey(const LogicalPlanReadRow &row,
														 const std::vector<std::string> &columns);

	private:
		const storage::LogicalGraphReadBackend &backend_;
	};

} // namespace graph::query::execution
