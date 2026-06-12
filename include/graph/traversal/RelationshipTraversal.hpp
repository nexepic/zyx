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
#include <string>
#include <vector>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::traversal {

	using EdgeVisitor = std::function<bool(const Edge &)>;

	enum class RelationshipDirectionKind {
		RDK_OUT,
		RDK_IN,
		RDK_BOTH,
	};

	enum class RelationshipTraversalIntegrity {
		RTI_DETECT_CYCLES,
		RTI_BOUND_BY_EDGE_COUNT,
	};

	struct RelationshipTraversalOptions {
		RelationshipDirectionKind direction = RelationshipDirectionKind::RDK_BOTH;
		RelationshipTraversalIntegrity integrity = RelationshipTraversalIntegrity::RTI_DETECT_CYCLES;
		bool activeOnly = true;
		int64_t typeId = 0;
	};

	struct RelationshipEdgeRef {
		int64_t edgeId = 0;
		int64_t sourceNodeId = 0;
		int64_t targetNodeId = 0;
		int64_t nextOutEdgeId = 0;
		int64_t nextInEdgeId = 0;
		int64_t typeId = 0;
		bool active = false;
	};

	using RelationshipEdgeRefVisitor = std::function<bool(const RelationshipEdgeRef &)>;

	struct RelationshipBatchLinkUpdates {
		std::vector<Node> nodes;
		std::vector<Node> oldNodes;
		std::vector<Edge> oldHeadEdges;
		std::vector<Edge> oldHeadEdgesBefore;

		[[nodiscard]] bool empty() const {
			return nodes.empty() && oldHeadEdges.empty();
		}
	};

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
		size_t visitOutgoingEdges(int64_t nodeId, const EdgeVisitor &visitor) const;

		/**
		 * Gets all incoming edges to a node
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all incoming edges
		 */
		std::vector<Edge> getIncomingEdges(int64_t nodeId) const;
		size_t visitIncomingEdges(int64_t nodeId, const EdgeVisitor &visitor) const;

		/**
		 * Gets all edges connected to a node (both incoming and outgoing)
		 *
		 * @param nodeId The ID of the node
		 * @return A vector of all connected edges
		 */
		std::vector<Edge> getAllConnectedEdges(int64_t nodeId) const;
		size_t visitAllConnectedEdges(int64_t nodeId, const EdgeVisitor &visitor) const;

		/**
		 * Streams lightweight relationship metadata for adjacency-driven query execution.
		 *
		 * This path is intentionally generic: callers describe direction, type, and
		 * integrity policy, while storage traversal owns linked-list navigation.
		 */
		size_t visitAdjacentEdgeRefs(int64_t nodeId,
		                             const RelationshipTraversalOptions &options,
		                             const RelationshipEdgeRefVisitor &visitor) const;
		size_t countAdjacentEdgeRefs(int64_t nodeId, const RelationshipTraversalOptions &options) const;

		static RelationshipDirectionKind directionFromString(const std::string &direction);

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
		 * Mutates newly-created edges with batch prev/next pointers and returns
		 * the existing nodes / head edges that must be persisted afterward.
		 */
		[[nodiscard]] RelationshipBatchLinkUpdates buildBatchLinks(std::vector<Edge> &edges) const;

		/**
		 * Persists node head-pointer and existing-head-edge updates produced by
		 * buildBatchLinks().
		 */
		void applyBatchLinkUpdates(const RelationshipBatchLinkUpdates &updates) const;

		/**
		 * Links a batch of newly-created edges into the relationship lists.
		 *
		 * The batch form preserves the same prepend order as repeated linkEdge()
		 * calls, but loads and updates each affected node / existing head edge at
		 * most once. The input edges must already have stable IDs.
		 */
		void linkEdgesBatch(std::vector<Edge> &edges) const;

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
