/**
 * @file RelationshipTraversal.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/traversal/RelationshipTraversal.hpp"
#include <unordered_set>
#include "graph/storage/data/DataManager.hpp"

namespace graph::traversal {

	RelationshipTraversal::RelationshipTraversal(const std::shared_ptr<storage::DataManager> &dataManager) :
		dataManager_(dataManager) {}

	std::vector<Edge> RelationshipTraversal::getOutgoingEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges;
		if (auto dm = dataManager_.lock()) {
			Node node = dm->getNode(nodeId);
			int64_t currentEdgeId = node.getFirstOutEdgeId();

			// Keep track of visited edge IDs to detect cycles in the linked list.
			std::unordered_set<int64_t> visitedEdgeIds;

			while (currentEdgeId != 0) {
				// Cycle detected in the edge linked-list. Abort traversal.
				if (visitedEdgeIds.contains(currentEdgeId)) {
					throw std::runtime_error("Cycle detected in outgoing edges linked-list for node " + std::to_string(nodeId));
				}
				visitedEdgeIds.insert(currentEdgeId);

				Edge edge = dm->getEdge(currentEdgeId);
				if (edge.isActive()) {
					outEdges.push_back(edge);
				}
				currentEdgeId = edge.getNextOutEdgeId();
			}
		}
		return outEdges;
	}

	std::vector<Edge> RelationshipTraversal::getIncomingEdges(int64_t nodeId) const {
		std::vector<Edge> inEdges;
		if (auto dm = dataManager_.lock()) {
			Node node = dm->getNode(nodeId);
			int64_t currentEdgeId = node.getFirstInEdgeId();

			// Keep track of visited edge IDs to detect cycles in the linked list.
			std::unordered_set<int64_t> visitedEdgeIds;

			while (currentEdgeId != 0) {
				// Cycle detected in the edge linked-list. Abort traversal.
				if (visitedEdgeIds.count(currentEdgeId)) {
					throw std::runtime_error("Cycle detected in incoming edges linked-list for node " + std::to_string(nodeId));
				}
				visitedEdgeIds.insert(currentEdgeId);

				Edge edge = dm->getEdge(currentEdgeId);
				if (edge.isActive()) {
					inEdges.push_back(edge);
				}
				currentEdgeId = edge.getNextInEdgeId();
			}
		}
		return inEdges;
	}

	std::vector<Edge> RelationshipTraversal::getAllConnectedEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges = getOutgoingEdges(nodeId);
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		// Combine the two vectors
		outEdges.insert(outEdges.end(), inEdges.begin(), inEdges.end());
		return outEdges;
	}

	std::vector<Node> RelationshipTraversal::getConnectedTargetNodes(int64_t nodeId) const {
		std::vector<Node> targetNodes;
		std::vector<Edge> outEdges = getOutgoingEdges(nodeId);

		targetNodes.reserve(outEdges.size());
		if (auto dm = dataManager_.lock()) {
			for (const auto &edge: outEdges) {
				targetNodes.push_back(dm->getNode(edge.getTargetNodeId()));
			}
		}
		return targetNodes;
	}

	std::vector<Node> RelationshipTraversal::getConnectedSourceNodes(int64_t nodeId) const {
		std::vector<Node> sourceNodes;
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		sourceNodes.reserve(inEdges.size());
		if (auto dm = dataManager_.lock()) {
			for (const auto &edge: inEdges) {
				sourceNodes.push_back(dm->getNode(edge.getSourceNodeId()));
			}
		}
		return sourceNodes;
	}

	std::vector<Node> RelationshipTraversal::getAllConnectedNodes(int64_t nodeId) const {
		std::vector<Node> connectedNodes;

		if (auto dm = dataManager_.lock()) {
			std::unordered_set<int64_t> nodeIds;
			for (const auto &edge: getOutgoingEdges(nodeId)) {
				int64_t targetId = edge.getTargetNodeId();
				if (nodeIds.insert(targetId).second) {
					connectedNodes.push_back(dm->getNode(targetId));
				}
			}

			for (const auto &edge: getIncomingEdges(nodeId)) {
				int64_t sourceId = edge.getSourceNodeId();
				if (nodeIds.insert(sourceId).second) {
					connectedNodes.push_back(dm->getNode(sourceId));
				}
			}
		}
		return connectedNodes;
	}

	void RelationshipTraversal::linkEdge(Edge &edge) const {
		int64_t edgeId = edge.getId();
		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		if (auto dm = dataManager_.lock()) {
			Node sourceNode = dm->getNode(sourceNodeId);
			int64_t firstOutEdgeId = sourceNode.getFirstOutEdgeId();

			if (firstOutEdgeId == 0) {
				sourceNode.setFirstOutEdgeId(edgeId);
				dm->updateNode(sourceNode);
			} else {
				Edge firstOutEdge = dm->getEdge(firstOutEdgeId);
				edge.setNextOutEdgeId(firstOutEdgeId);
				firstOutEdge.setPrevOutEdgeId(edgeId);

				sourceNode.setFirstOutEdgeId(edgeId);

				dm->updateEdge(firstOutEdge);
				dm->updateNode(sourceNode);
			}

			Node targetNode = dm->getNode(targetNodeId);
			int64_t firstInEdgeId = targetNode.getFirstInEdgeId();

			if (firstInEdgeId == 0) {
				targetNode.setFirstInEdgeId(edgeId);
				dm->updateNode(targetNode);
			} else {
				Edge firstInEdge = dm->getEdge(firstInEdgeId);
				edge.setNextInEdgeId(firstInEdgeId);
				firstInEdge.setPrevInEdgeId(edgeId);

				targetNode.setFirstInEdgeId(edgeId);

				dm->updateEdge(firstInEdge);
				dm->updateNode(targetNode);
			}

			dm->updateEdge(edge);
		}
	}

	void RelationshipTraversal::unlinkEdge(Edge &edge) const {
		if (auto dm = dataManager_.lock()) {
			int64_t sourceNodeId = edge.getSourceNodeId();
			int64_t targetNodeId = edge.getTargetNodeId();

			int64_t prevOutEdgeId = edge.getPrevOutEdgeId();
			int64_t nextOutEdgeId = edge.getNextOutEdgeId();

			if (prevOutEdgeId == 0) {
				Node sourceNode = dm->getNode(sourceNodeId);
				sourceNode.setFirstOutEdgeId(nextOutEdgeId);
				dm->updateNode(sourceNode);
			} else {
				Edge prevOutEdge = dm->getEdge(prevOutEdgeId);
				prevOutEdge.setNextOutEdgeId(nextOutEdgeId);
				dm->updateEdge(prevOutEdge);
			}

			if (nextOutEdgeId != 0) {
				Edge nextOutEdge = dm->getEdge(nextOutEdgeId);
				nextOutEdge.setPrevOutEdgeId(prevOutEdgeId);
				dm->updateEdge(nextOutEdge);
			}

			int64_t prevInEdgeId = edge.getPrevInEdgeId();
			int64_t nextInEdgeId = edge.getNextInEdgeId();

			if (prevInEdgeId == 0) {
				Node targetNode = dm->getNode(targetNodeId);
				targetNode.setFirstInEdgeId(nextInEdgeId);
				dm->updateNode(targetNode);
			} else {
				Edge prevInEdge = dm->getEdge(prevInEdgeId);
				prevInEdge.setNextInEdgeId(nextInEdgeId);
				dm->updateEdge(prevInEdge);
			}

			if (nextInEdgeId != 0) {
				Edge nextInEdge = dm->getEdge(nextInEdgeId);
				nextInEdge.setPrevInEdgeId(prevInEdgeId);
				dm->updateEdge(nextInEdge);
			}

			edge.setNextOutEdgeId(0);
			edge.setPrevOutEdgeId(0);
			edge.setNextInEdgeId(0);
			edge.setPrevInEdgeId(0);
		}
	}

} // namespace graph::traversal
