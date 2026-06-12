/**
 * @file RelationshipTraversal.cpp
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

#include "graph/traversal/RelationshipTraversal.hpp"
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"

namespace graph::traversal {
	namespace {
		constexpr size_t kNoBatchEdgeIndex = std::numeric_limits<size_t>::max();

		struct LinkListBuildState {
			int64_t currentHeadId = 0;
			size_t currentHeadIndex = kNoBatchEdgeIndex;
			bool nodeLoaded = false;
		};

		struct NodeLinkUpdate {
			Node before;
			Node after;
		};

		struct EdgeLinkUpdate {
			Edge before;
			Edge after;
		};
	} // namespace

	RelationshipTraversal::RelationshipTraversal(const std::shared_ptr<storage::DataManager> &dataManager) :
		dataManager_(dataManager) {}

	std::vector<Edge> RelationshipTraversal::getOutgoingEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges;
		visitOutgoingEdges(nodeId, [&](const Edge &edge) {
			outEdges.push_back(edge);
			return true;
		});
		return outEdges;
	}

	size_t RelationshipTraversal::visitOutgoingEdges(int64_t nodeId, const EdgeVisitor &visitor) const {
		size_t visitedCount = 0;
		const auto dataManager = dataManager_.lock();
		if (!dataManager || !visitor) {
			return visitedCount;
		}

		const Node node = dataManager->getNode(nodeId);
		int64_t currentEdgeId = node.getFirstOutEdgeId();

		// Keep track of visited edge IDs to detect cycles in the linked list.
		std::unordered_set<int64_t> visitedEdgeIds;

		while (currentEdgeId != 0) {
			// Cycle detected in the edge linked-list. Abort traversal.
			if (visitedEdgeIds.contains(currentEdgeId)) {
				throw std::runtime_error("Cycle detected in outgoing edges linked-list for node " +
										 std::to_string(nodeId));
			}
			visitedEdgeIds.insert(currentEdgeId);

			Edge edge = dataManager->getEdge(currentEdgeId);
			const int64_t nextEdgeId = edge.getNextOutEdgeId();
			if (edge.isActive()) {
				++visitedCount;
				if (!visitor(edge)) {
					break;
				}
			}
			currentEdgeId = nextEdgeId;
		}
		return visitedCount;
	}

	std::vector<Edge> RelationshipTraversal::getIncomingEdges(int64_t nodeId) const {
		std::vector<Edge> inEdges;
		visitIncomingEdges(nodeId, [&](const Edge &edge) {
			inEdges.push_back(edge);
			return true;
		});
		return inEdges;
	}

	size_t RelationshipTraversal::visitIncomingEdges(int64_t nodeId, const EdgeVisitor &visitor) const {
		size_t visitedCount = 0;
		const auto dataManager = dataManager_.lock();
		if (!dataManager || !visitor) {
			return visitedCount;
		}

		const Node node = dataManager->getNode(nodeId);
		int64_t currentEdgeId = node.getFirstInEdgeId();

		// Keep track of visited edge IDs to detect cycles in the linked list.
		std::unordered_set<int64_t> visitedEdgeIds;

		while (currentEdgeId != 0) {
			// Cycle detected in the edge linked-list. Abort traversal.
			if (visitedEdgeIds.contains(currentEdgeId)) {
				throw std::runtime_error("Cycle detected in incoming edges linked-list for node " +
										 std::to_string(nodeId));
			}
			visitedEdgeIds.insert(currentEdgeId);

			Edge edge = dataManager->getEdge(currentEdgeId);
			const int64_t nextEdgeId = edge.getNextInEdgeId();
			if (edge.isActive()) {
				++visitedCount;
				if (!visitor(edge)) {
					break;
				}
			}
			currentEdgeId = nextEdgeId;
		}
		return visitedCount;
	}

	std::vector<Edge> RelationshipTraversal::getAllConnectedEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges = getOutgoingEdges(nodeId);
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		// Combine the two vectors
		outEdges.insert(outEdges.end(), inEdges.begin(), inEdges.end());
		return outEdges;
	}

	size_t RelationshipTraversal::visitAllConnectedEdges(int64_t nodeId, const EdgeVisitor &visitor) const {
		if (!visitor) {
			return 0;
		}
		bool keepGoing = true;
		size_t visitedCount = visitOutgoingEdges(nodeId, [&](const Edge &edge) {
			keepGoing = visitor(edge);
			return keepGoing;
		});
		if (keepGoing) {
			visitedCount += visitIncomingEdges(nodeId, visitor);
		}
		return visitedCount;
	}

	std::vector<Node> RelationshipTraversal::getConnectedTargetNodes(int64_t nodeId) const {
		std::vector<Node> targetNodes;
		std::vector<Edge> outEdges = getOutgoingEdges(nodeId);

		targetNodes.reserve(outEdges.size());
		const auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return targetNodes;
		}

		for (const auto &edge: outEdges) {
			targetNodes.push_back(dataManager->getNode(edge.getTargetNodeId()));
		}
		return targetNodes;
	}

	std::vector<Node> RelationshipTraversal::getConnectedSourceNodes(int64_t nodeId) const {
		std::vector<Node> sourceNodes;
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		sourceNodes.reserve(inEdges.size());
		const auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return sourceNodes;
		}

		for (const auto &edge: inEdges) {
			sourceNodes.push_back(dataManager->getNode(edge.getSourceNodeId()));
		}
		return sourceNodes;
	}

	std::vector<Node> RelationshipTraversal::getAllConnectedNodes(int64_t nodeId) const {
		std::vector<Node> connectedNodes;

		const auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return connectedNodes;
		}

		std::unordered_set<int64_t> nodeIds;
		for (const auto &edge: getOutgoingEdges(nodeId)) {
			int64_t targetId = edge.getTargetNodeId();
			if (nodeIds.insert(targetId).second) {
				connectedNodes.push_back(dataManager->getNode(targetId));
			}
		}

		for (const auto &edge: getIncomingEdges(nodeId)) {
			int64_t sourceId = edge.getSourceNodeId();
			if (nodeIds.insert(sourceId).second) {
				connectedNodes.push_back(dataManager->getNode(sourceId));
			}
		}
		return connectedNodes;
	}

	void RelationshipTraversal::linkEdge(Edge &edge) const {
		int64_t edgeId = edge.getId();
		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		Node sourceNode = dataManager->getNode(sourceNodeId);
		int64_t firstOutEdgeId = sourceNode.getFirstOutEdgeId();

		if (firstOutEdgeId == 0) {
			sourceNode.setFirstOutEdgeId(edgeId);
			dataManager->updateNode(sourceNode);
		} else {
			Edge firstOutEdge = dataManager->getEdge(firstOutEdgeId);
			edge.setNextOutEdgeId(firstOutEdgeId);
			firstOutEdge.setPrevOutEdgeId(edgeId);

			sourceNode.setFirstOutEdgeId(edgeId);

			dataManager->updateEdge(firstOutEdge);
			dataManager->updateNode(sourceNode);
		}

		Node targetNode = dataManager->getNode(targetNodeId);
		int64_t firstInEdgeId = targetNode.getFirstInEdgeId();

		if (firstInEdgeId == 0) {
			targetNode.setFirstInEdgeId(edgeId);
			dataManager->updateNode(targetNode);
		} else {
			Edge firstInEdge = dataManager->getEdge(firstInEdgeId);
			edge.setNextInEdgeId(firstInEdgeId);
			firstInEdge.setPrevInEdgeId(edgeId);

			targetNode.setFirstInEdgeId(edgeId);

			dataManager->updateEdge(firstInEdge);
			dataManager->updateNode(targetNode);
		}

		dataManager->updateEdge(edge);
	}

	RelationshipBatchLinkUpdates RelationshipTraversal::buildBatchLinks(std::vector<Edge> &edges) const {
		RelationshipBatchLinkUpdates updates;
		if (edges.empty()) {
			return updates;
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return updates;
		}

		std::unordered_map<int64_t, LinkListBuildState> outStates;
		std::unordered_map<int64_t, LinkListBuildState> inStates;
		std::unordered_map<int64_t, NodeLinkUpdate> nodeUpdates;
		std::unordered_map<int64_t, EdgeLinkUpdate> oldHeadUpdates;
		outStates.reserve(edges.size());
		inStates.reserve(edges.size());

		auto getNodeForUpdate = [&](int64_t nodeId) -> std::optional<std::reference_wrapper<Node>> {
			auto [it, inserted] = nodeUpdates.try_emplace(nodeId);
			if (inserted) {
				it->second.before = dataManager->getNode(nodeId);
				if (it->second.before.getId() == 0) {
					nodeUpdates.erase(it);
					return std::nullopt;
				}
				it->second.after = it->second.before;
			}
			return it->second.after;
		};

		auto getOldHeadForUpdate = [&](int64_t edgeId) -> std::optional<std::reference_wrapper<Edge>> {
			if (edgeId == 0) {
				return std::nullopt;
			}
			auto [it, inserted] = oldHeadUpdates.try_emplace(edgeId);
			if (inserted) {
				it->second.before = dataManager->getEdge(edgeId);
				if (it->second.before.getId() == 0) {
					oldHeadUpdates.erase(it);
					return std::nullopt;
				}
				it->second.after = it->second.before;
			}
			return it->second.after;
		};

		auto appendToOutgoingList = [&](size_t index) {
			Edge &edge = edges[index];
			auto nodeRef = getNodeForUpdate(edge.getSourceNodeId());
			LinkListBuildState &state = outStates[edge.getSourceNodeId()];
			if (!state.nodeLoaded) {
				state.currentHeadId = nodeRef.has_value() ? nodeRef->get().getFirstOutEdgeId() : int64_t{0};
				state.currentHeadIndex = kNoBatchEdgeIndex;
				state.nodeLoaded = true;
			}

			edge.setPrevOutEdgeId(0);
			edge.setNextOutEdgeId(state.currentHeadId);
			if (state.currentHeadIndex != kNoBatchEdgeIndex) {
				edges[state.currentHeadIndex].setPrevOutEdgeId(edge.getId());
			} else if (auto oldHead = getOldHeadForUpdate(state.currentHeadId)) {
				oldHead->get().setPrevOutEdgeId(edge.getId());
			}

			state.currentHeadId = edge.getId();
			state.currentHeadIndex = index;
		};

		auto appendToIncomingList = [&](size_t index) {
			Edge &edge = edges[index];
			auto nodeRef = getNodeForUpdate(edge.getTargetNodeId());
			LinkListBuildState &state = inStates[edge.getTargetNodeId()];
			if (!state.nodeLoaded) {
				state.currentHeadId = nodeRef.has_value() ? nodeRef->get().getFirstInEdgeId() : int64_t{0};
				state.currentHeadIndex = kNoBatchEdgeIndex;
				state.nodeLoaded = true;
			}

			edge.setPrevInEdgeId(0);
			edge.setNextInEdgeId(state.currentHeadId);
			if (state.currentHeadIndex != kNoBatchEdgeIndex) {
				edges[state.currentHeadIndex].setPrevInEdgeId(edge.getId());
			} else if (auto oldHead = getOldHeadForUpdate(state.currentHeadId)) {
				oldHead->get().setPrevInEdgeId(edge.getId());
			}

			state.currentHeadId = edge.getId();
			state.currentHeadIndex = index;
		};

		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.prepare");
			for (size_t index = 0; index < edges.size(); ++index) {
				if (edges[index].getId() == 0) {
					continue;
				}
				appendToOutgoingList(index);
				appendToIncomingList(index);
			}

			for (const auto &[nodeId, state]: outStates) {
				if (auto node = getNodeForUpdate(nodeId)) {
					node->get().setFirstOutEdgeId(state.currentHeadId);
				}
			}
			for (const auto &[nodeId, state]: inStates) {
				if (auto node = getNodeForUpdate(nodeId)) {
					node->get().setFirstInEdgeId(state.currentHeadId);
				}
			}

			updates.nodes.reserve(nodeUpdates.size());
			updates.oldNodes.reserve(nodeUpdates.size());
			for (auto &[nodeId, update]: nodeUpdates) {
				(void) nodeId;
				updates.oldNodes.push_back(std::move(update.before));
				updates.nodes.push_back(std::move(update.after));
			}
			updates.oldHeadEdges.reserve(oldHeadUpdates.size());
			updates.oldHeadEdgesBefore.reserve(oldHeadUpdates.size());
			for (auto &[edgeId, update]: oldHeadUpdates) {
				(void) edgeId;
				updates.oldHeadEdgesBefore.push_back(std::move(update.before));
				updates.oldHeadEdges.push_back(std::move(update.after));
			}
		}

		return updates;
	}

	void RelationshipTraversal::applyBatchLinkUpdates(const RelationshipBatchLinkUpdates &updates) const {
		if (updates.empty()) {
			return;
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.update_nodes");
			dataManager->updateNodesWithBeforeImages(updates.nodes, updates.oldNodes);
		}

		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.update_old_heads");
			dataManager->updateEdgesWithBeforeImages(updates.oldHeadEdges, updates.oldHeadEdgesBefore);
		}
	}

	void RelationshipTraversal::linkEdgesBatch(std::vector<Edge> &edges) const {
		if (edges.empty()) {
			return;
		}

		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		debug::ScopedPerfTimer totalTimer("relationship_traversal.link_edges_batch");
		const auto updates = buildBatchLinks(edges);
		applyBatchLinkUpdates(updates);
		{
			debug::ScopedPerfTimer timer("relationship_traversal.link_edges_batch.update_new_edges");
			dataManager->getEdgeManager()->updateBatch(edges);
		}
	}

	void RelationshipTraversal::unlinkEdge(Edge &edge) const {
		auto dataManager = dataManager_.lock();
		if (!dataManager) {
			return;
		}

		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		int64_t prevOutEdgeId = edge.getPrevOutEdgeId();
		int64_t nextOutEdgeId = edge.getNextOutEdgeId();

		if (prevOutEdgeId == 0) {
			Node sourceNode = dataManager->getNode(sourceNodeId);
			sourceNode.setFirstOutEdgeId(nextOutEdgeId);
			dataManager->updateNode(sourceNode);
		} else {
			Edge prevOutEdge = dataManager->getEdge(prevOutEdgeId);
			prevOutEdge.setNextOutEdgeId(nextOutEdgeId);
			dataManager->updateEdge(prevOutEdge);
		}

		if (nextOutEdgeId != 0) {
			Edge nextOutEdge = dataManager->getEdge(nextOutEdgeId);
			nextOutEdge.setPrevOutEdgeId(prevOutEdgeId);
			dataManager->updateEdge(nextOutEdge);
		}

		int64_t prevInEdgeId = edge.getPrevInEdgeId();
		int64_t nextInEdgeId = edge.getNextInEdgeId();

		if (prevInEdgeId == 0) {
			Node targetNode = dataManager->getNode(targetNodeId);
			targetNode.setFirstInEdgeId(nextInEdgeId);
			dataManager->updateNode(targetNode);
		} else {
			Edge prevInEdge = dataManager->getEdge(prevInEdgeId);
			prevInEdge.setNextInEdgeId(nextInEdgeId);
			dataManager->updateEdge(prevInEdge);
		}

		if (nextInEdgeId != 0) {
			Edge nextInEdge = dataManager->getEdge(nextInEdgeId);
			nextInEdge.setPrevInEdgeId(prevInEdgeId);
			dataManager->updateEdge(nextInEdge);
		}

		edge.setNextOutEdgeId(0);
		edge.setPrevOutEdgeId(0);
		edge.setNextInEdgeId(0);
		edge.setPrevInEdgeId(0);
	}

} // namespace graph::traversal
