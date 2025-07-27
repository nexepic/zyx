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

	RelationshipTraversal::RelationshipTraversal(std::shared_ptr<storage::DataManager> dataManager) :
		dataManager_(std::move(dataManager)) {}

	std::vector<Edge> RelationshipTraversal::getOutgoingEdges(int64_t nodeId) const {
		std::vector<Edge> outEdges;
		Node node = dataManager_->getNode(nodeId);

		int64_t currentEdgeId = node.getFirstOutEdgeId();
		while (currentEdgeId != 0) {
			Edge edge = dataManager_->getEdge(currentEdgeId);
			if (edge.isActive()) {
				outEdges.push_back(edge);
			}
			currentEdgeId = edge.getNextOutEdgeId();
		}
		return outEdges;
	}

	std::vector<Edge> RelationshipTraversal::getIncomingEdges(int64_t nodeId) const {
		std::vector<Edge> inEdges;
		Node node = dataManager_->getNode(nodeId);

		int64_t currentEdgeId = node.getFirstInEdgeId();
		while (currentEdgeId != 0) {
			Edge edge = dataManager_->getEdge(currentEdgeId);
			if (edge.isActive()) {
				inEdges.push_back(edge);
			}
			currentEdgeId = edge.getNextInEdgeId();
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
		for (const auto &edge: outEdges) {
			targetNodes.push_back(dataManager_->getNode(edge.getTargetNodeId()));
		}
		return targetNodes;
	}

	std::vector<Node> RelationshipTraversal::getConnectedSourceNodes(int64_t nodeId) const {
		std::vector<Node> sourceNodes;
		std::vector<Edge> inEdges = getIncomingEdges(nodeId);

		sourceNodes.reserve(inEdges.size());
		for (const auto &edge: inEdges) {
			sourceNodes.push_back(dataManager_->getNode(edge.getSourceNodeId()));
		}
		return sourceNodes;
	}

	std::vector<Node> RelationshipTraversal::getAllConnectedNodes(int64_t nodeId) const {
		std::unordered_set<int64_t> nodeIds;
		std::vector<Node> connectedNodes;

		for (const auto &edge: getOutgoingEdges(nodeId)) {
			int64_t targetId = edge.getTargetNodeId();
			if (nodeIds.insert(targetId).second) {
				connectedNodes.push_back(dataManager_->getNode(targetId));
			}
		}

		for (const auto &edge: getIncomingEdges(nodeId)) {
			int64_t sourceId = edge.getSourceNodeId();
			if (nodeIds.insert(sourceId).second) {
				connectedNodes.push_back(dataManager_->getNode(sourceId));
			}
		}
		return connectedNodes;
	}

	void RelationshipTraversal::linkEdge(Edge &edge) const {
		int64_t edgeId = edge.getId();
		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		Node sourceNode = dataManager_->getNode(sourceNodeId);
		int64_t firstOutEdgeId = sourceNode.getFirstOutEdgeId();

		if (firstOutEdgeId == 0) {
			sourceNode.setFirstOutEdgeId(edgeId);
			dataManager_->updateNode(sourceNode);
		} else {
			Edge firstOutEdge = dataManager_->getEdge(firstOutEdgeId);
			edge.setNextOutEdgeId(firstOutEdgeId);
			firstOutEdge.setPrevOutEdgeId(edgeId);

			sourceNode.setFirstOutEdgeId(edgeId);

			dataManager_->updateEdge(firstOutEdge);
			dataManager_->updateNode(sourceNode);
		}

		Node targetNode = dataManager_->getNode(targetNodeId);
		int64_t firstInEdgeId = targetNode.getFirstInEdgeId();

		if (firstInEdgeId == 0) {
			targetNode.setFirstInEdgeId(edgeId);
			dataManager_->updateNode(targetNode);
		} else {
			Edge firstInEdge = dataManager_->getEdge(firstInEdgeId);
			edge.setNextInEdgeId(firstInEdgeId);
			firstInEdge.setPrevInEdgeId(edgeId);

			targetNode.setFirstInEdgeId(edgeId);

			dataManager_->updateEdge(firstInEdge);
			dataManager_->updateNode(targetNode);
		}

		dataManager_->updateEdge(edge);
	}

	void RelationshipTraversal::unlinkEdge(Edge &edge) const {
		int64_t sourceNodeId = edge.getSourceNodeId();
		int64_t targetNodeId = edge.getTargetNodeId();

		int64_t prevOutEdgeId = edge.getPrevOutEdgeId();
		int64_t nextOutEdgeId = edge.getNextOutEdgeId();

		if (prevOutEdgeId == 0) {
			Node sourceNode = dataManager_->getNode(sourceNodeId);
			sourceNode.setFirstOutEdgeId(nextOutEdgeId);
			dataManager_->updateNode(sourceNode);
		} else {
			Edge prevOutEdge = dataManager_->getEdge(prevOutEdgeId);
			prevOutEdge.setNextOutEdgeId(nextOutEdgeId);
			dataManager_->updateEdge(prevOutEdge);
		}

		if (nextOutEdgeId != 0) {
			Edge nextOutEdge = dataManager_->getEdge(nextOutEdgeId);
			nextOutEdge.setPrevOutEdgeId(prevOutEdgeId);
			dataManager_->updateEdge(nextOutEdge);
		}

		int64_t prevInEdgeId = edge.getPrevInEdgeId();
		int64_t nextInEdgeId = edge.getNextInEdgeId();

		if (prevInEdgeId == 0) {
			Node targetNode = dataManager_->getNode(targetNodeId);
			targetNode.setFirstInEdgeId(nextInEdgeId);
			dataManager_->updateNode(targetNode);
		} else {
			Edge prevInEdge = dataManager_->getEdge(prevInEdgeId);
			prevInEdge.setNextInEdgeId(nextInEdgeId);
			dataManager_->updateEdge(prevInEdge);
		}

		if (nextInEdgeId != 0) {
			Edge nextInEdge = dataManager_->getEdge(nextInEdgeId);
			nextInEdge.setPrevInEdgeId(prevInEdgeId);
			dataManager_->updateEdge(nextInEdge);
		}

		edge.setNextOutEdgeId(0);
		edge.setPrevOutEdgeId(0);
		edge.setNextInEdgeId(0);
		edge.setPrevInEdgeId(0);
	}

} // namespace graph::traversal
