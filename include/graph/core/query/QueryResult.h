/**
 * @file QueryResult.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <vector>
#include <memory>
#include "graph/core/Node.h"
#include "graph/core/Edge.h"

namespace graph::query {

	class QueryResult {
	public:
		QueryResult() = default;

		void addNode(const Node& node) { nodes_.push_back(node); }
		void addEdge(const Edge& edge) { edges_.push_back(edge); }

		[[nodiscard]] const std::vector<Node>& getNodes() const { return nodes_; }
		[[nodiscard]] const std::vector<Edge>& getEdges() const { return edges_; }

		[[nodiscard]] size_t nodeCount() const { return nodes_.size(); }
		[[nodiscard]] size_t edgeCount() const { return edges_.size(); }

		bool isEmpty() const { return nodes_.empty() && edges_.empty(); }

	private:
		std::vector<Node> nodes_;
		std::vector<Edge> edges_;
	};

} // namespace graph::query