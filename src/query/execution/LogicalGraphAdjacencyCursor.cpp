/**
 * @file LogicalGraphAdjacencyCursor.cpp
 * @brief Snapshot-pinned pull cursor over resumable logical adjacency pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/LogicalGraphAdjacencyCursor.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace graph::query::execution {

	namespace {

		void validateOptions(uint64_t nodeId, storage::LogicalGraphReadDirection direction,
							 const LogicalGraphAdjacencyCursorOptions &options) {
			if (nodeId == 0U || options.page_records == 0U || options.maximum_total_edges == 0U ||
				options.page_records > options.maximum_total_edges) {
				throw std::invalid_argument("logical graph adjacency cursor requires positive compatible bounds");
			}
			if (direction != storage::LogicalGraphReadDirection::LGRD_OUTGOING &&
				direction != storage::LogicalGraphReadDirection::LGRD_INCOMING) {
				throw std::invalid_argument("logical graph adjacency cursor has an unknown direction");
			}
		}

		void validateEdge(const storage::LogicalGraphEdge &edge, uint64_t nodeId,
						  storage::LogicalGraphReadDirection direction) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical graph adjacency cursor received an invalid Edge");
			}
			const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
										 ? edge.source_node_id == nodeId
										 : edge.target_node_id == nodeId;
			if (!matches) {
				throw std::runtime_error("logical graph adjacency cursor received an Edge from another Node");
			}
			std::unordered_set<std::string_view> propertyKeys;
			propertyKeys.reserve(edge.properties.size());
			for (const storage::LogicalGraphPropertyEntry &property: edge.properties) {
				if (property.key.empty() || !propertyKeys.insert(property.key).second) {
					throw std::runtime_error("logical graph adjacency cursor received an invalid owner-keyed Property set");
				}
			}
		}

	} // namespace

	LogicalGraphAdjacencyCursor::LogicalGraphAdjacencyCursor(const storage::LogicalGraphReadBackend &backend,
															 uint64_t nodeId,
															 storage::LogicalGraphReadDirection direction,
															 LogicalGraphAdjacencyCursorOptions options) :
		backend_(backend), node_id_(nodeId), direction_(direction), options_(options) {
		validateOptions(node_id_, direction_, options_);
	}

	void LogicalGraphAdjacencyCursor::open() {
		if (isOpen()) {
			throw std::logic_error("logical graph adjacency cursor is already open");
		}
		snapshot_ = backend_.openLogicalGraphReadSnapshot();
		if (!snapshot_) {
			throw std::runtime_error("logical graph backend returned no read snapshot");
		}
		continuation_.reset();
		edges_returned_ = 0U;
		exhausted_ = false;
	}

	std::optional<storage::LogicalGraphAdjacencyPage> LogicalGraphAdjacencyCursor::next() {
		if (!isOpen()) {
			throw std::logic_error("logical graph adjacency cursor is not open");
		}
		if (exhausted_) {
			return std::nullopt;
		}
		storage::LogicalGraphAdjacencyPage page =
				snapshot_->scanAdjacentEdges(node_id_, direction_, continuation_, options_.page_records);
		if (page.edges.size() > options_.page_records ||
			page.edges.size() > options_.maximum_total_edges - edges_returned_) {
			throw std::runtime_error("logical graph backend exceeded the adjacency cursor edge bound");
		}
		if (continuation_.has_value() &&
			(page.edges.empty() || page.edges.front().edge_id != continuation_->next_edge_id)) {
			throw std::runtime_error("logical graph backend returned an adjacency page that does not resume at its continuation");
		}
		uint64_t previousEdgeId = 0U;
		for (const storage::LogicalGraphEdge &edge: page.edges) {
			validateEdge(edge, node_id_, direction_);
			if (previousEdgeId != 0U && edge.edge_id <= previousEdgeId) {
				throw std::runtime_error("logical graph backend returned non-monotonic adjacency Edge IDs");
			}
			previousEdgeId = edge.edge_id;
		}
		if (page.edges.empty()) {
			if (page.continuation.has_value()) {
				throw std::runtime_error("logical graph backend returned an empty nonterminal adjacency page");
			}
			exhausted_ = true;
			return std::nullopt;
		}
		const size_t nextReturned = edges_returned_ + page.edges.size();
		if (page.continuation.has_value()) {
			const storage::LogicalGraphAdjacencyContinuation &next = *page.continuation;
			if (next.previous_edge_id != page.edges.back().edge_id || next.next_edge_id <= next.previous_edge_id) {
				throw std::runtime_error("logical graph backend returned an invalid adjacency continuation");
			}
			if (nextReturned == options_.maximum_total_edges) {
				throw std::runtime_error("logical graph adjacency traversal exceeded its total edge bound");
			}
			continuation_ = next;
		} else {
			exhausted_ = true;
		}
		// Commit cursor state only after the complete page/continuation passes.
		edges_returned_ = nextReturned;
		return page;
	}

	void LogicalGraphAdjacencyCursor::close() noexcept {
		snapshot_.reset();
		continuation_.reset();
		edges_returned_ = 0U;
		exhausted_ = false;
	}

} // namespace graph::query::execution
