/**
 * @file QueryResult.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <unordered_set>
#include <vector>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph::query {

	class QueryResult {
	public:
		QueryResult() = default;

		// Deduplicate by ID
		void addNode(const Node &node) {
			if (!uniqueNodeIds_.contains(node.getId())) {
				nodes_.push_back(node);
				uniqueNodeIds_.insert(node.getId());
			}
		}
		void addEdge(const Edge &edge) {
			if (!uniqueEdgeIds_.contains(edge.getId())) {
				edges_.push_back(edge);
				uniqueEdgeIds_.insert(edge.getId());
			}
		}

		[[nodiscard]] const std::vector<Node> &getNodes() const { return nodes_; }
		[[nodiscard]] const std::vector<Edge> &getEdges() const { return edges_; }

		[[nodiscard]] size_t nodeCount() const { return nodes_.size(); }
		[[nodiscard]] size_t edgeCount() const { return edges_.size(); }

		using Row = std::unordered_map<std::string, PropertyValue>;

		void addRow(Row row) { rows_.push_back(std::move(row)); }

		const std::vector<Row> &getRows() const { return rows_; }
		size_t rowCount() const { return rows_.size(); }

		bool isEmpty() const { return nodes_.empty() && edges_.empty() && rows_.empty(); }

	private:
		std::vector<Node> nodes_;
		std::vector<Edge> edges_;

		std::unordered_set<int64_t> uniqueNodeIds_;
		std::unordered_set<int64_t> uniqueEdgeIds_;

		std::vector<Row> rows_;
	};

} // namespace graph::query
