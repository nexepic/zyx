/**
 * @file IEntityObserver.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph {

	/**
	 * @class IEntityObserver
	 * @brief An interface for classes that need to be notified of entity lifecycle events.
	 *
	 * This follows the Observer design pattern, allowing components like the IndexManager
	 * to react to data changes without being tightly coupled to the DataManager.
	 */
	class IEntityObserver {
	public:
		virtual ~IEntityObserver() = default;

		// --- Node Event Handlers ---

		/**
		 * @brief Called after a new node has been successfully added.
		 * @param node The node that was added.
		 */
		virtual void onNodeAdded([[maybe_unused]] const Node &node) {};

		virtual void onNodesAdded(const std::vector<Node>& nodes) {}

		/**
		 * @brief Called after a node has been updated.
		 * @param oldNode The state of the node before the update.
		 * @param newNode The state of the node after the update.
		 */
		virtual void onNodeUpdated([[maybe_unused]] const Node &oldNode, [[maybe_unused]] const Node &newNode) {};

		/**
		 * @brief Called after a node has been deleted.
		 * @param node The node that was deleted.
		 */
		virtual void onNodeDeleted([[maybe_unused]] const Node &node) {};


		// --- Edge Event Handlers ---

		/**
		 * @brief Called after a new edge has been successfully added.
		 * @param edge The edge that was added.
		 */
		virtual void onEdgeAdded([[maybe_unused]] const Edge &edge) {};

		virtual void onEdgesAdded(const std::vector<Edge>& edges) {}

		/**
		 * @brief Called after an edge has been updated.
		 * @param oldEdge The state of the edge before the update.
		 * @param newEdge The state of the edge after the update.
		 */
		virtual void onEdgeUpdated([[maybe_unused]] const Edge &oldEdge, [[maybe_unused]] const Edge &newEdge) {};

		/**
		 * @brief Called after an edge has been deleted.
		 * @param edge The edge that was deleted.
		 */
		virtual void onEdgeDeleted([[maybe_unused]] const Edge &edge) {};

		virtual void onStateUpdated([[maybe_unused]] const State &oldState, [[maybe_unused]] const State &newState) {};
	};

} // namespace graph
