/**
 * @file RelationshipTraversal.hpp
 * @author Nexepic
 * @date 2025/6/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::traversal {

	/**
	 * Provides methods for traversing relationships between nodes in the graph.
	 * Uses the linked-list approach for efficiently finding connected nodes and edges.
	 */
	class RelationshipTraversal {
	public:
		explicit RelationshipTraversal(const std::shared_ptr<storage::DataManager> &dataManager);

		/**
		 * Gets all outgoing edges from a node
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all outgoing edges
		 */
		std::vector<Edge> getOutgoingEdges(int64_t nodeId) const;

		/**
		 * Gets all incoming edges to a node
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all incoming edges
		 */
		std::vector<Edge> getIncomingEdges(int64_t nodeId) const;

		/**
		 * Gets all edges connected to a node (both incoming and outgoing)
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all connected edges
		 */
		std::vector<Edge> getAllConnectedEdges(int64_t nodeId) const;

		/**
		 * Gets all nodes connected to the specified node via outgoing edges
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all target nodes
		 */
		std::vector<Node> getConnectedTargetNodes(int64_t nodeId) const;

		/**
		 * Gets all nodes connected to the specified node via incoming edges
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all source nodes
		 */
		std::vector<Node> getConnectedSourceNodes(int64_t nodeId) const;

		/**
		 * Gets all nodes connected to the specified node (both source and target)
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all connected nodes
		 */
		std::vector<Node> getAllConnectedNodes(int64_t nodeId) const;

		/**
		 * Links a new edge into the appropriate linked lists
		 *
		 * @param edge The edge to be linked
		 */
		void linkEdge(Edge &edge) const;

		/**
		 * Unlinks an edge from the linked lists
		 *
		 * @param edge The edge to be unlinked
		 */
		void unlinkEdge(Edge &edge) const;

	private:
		std::weak_ptr<storage::DataManager> dataManager_;
	};

} // namespace graph::traversal
