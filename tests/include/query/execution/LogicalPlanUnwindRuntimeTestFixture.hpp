/**
 * @file LogicalPlanUnwindRuntimeTestFixture.hpp
 * @brief Shared storage-free logical UNWIND runtime fixture.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "graph/query/execution/LogicalPlanReadRuntime.hpp"

namespace graph::query::execution::test {

	using namespace graph::storage;

	class NoGraphSnapshot final : public LogicalGraphReadSnapshot {
	public:
		[[nodiscard]] LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected Node read"); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected Edge read"); }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("unexpected Node traversal");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
			throw std::logic_error("unexpected Node page scan");
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
			throw std::logic_error("unexpected adjacency scan");
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected Edge traversal");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected adjacency traversal");
		}
	};

	class NoGraphBackend final : public LogicalGraphReadBackend {
	public:
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<NoGraphSnapshot>();
		}
	};

	[[nodiscard]] inline LogicalPlanReadRuntimeBounds bounds() {
		return {
				.maximum_node_records = 16U,
				.maximum_node_page_records = 4U,
				.maximum_metadata_pages_per_node_page = 4U,
				.maximum_edges_per_node = 16U,
				.maximum_edge_records = 32U,
				.maximum_adjacency_page_records = 4U,
				.maximum_property_entries = 16U,
				.maximum_rows = 16U,
				.maximum_distinct_rows = 16U,
		};
	}

} // namespace graph::query::execution::test
