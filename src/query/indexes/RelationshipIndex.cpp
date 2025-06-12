/**
 * @file RelationshipIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/indexes/RelationshipIndex.h"
#include <algorithm>

namespace graph::query::indexes {

	RelationshipIndex::RelationshipIndex() = default;

	void RelationshipIndex::addEdge(int64_t edgeId, int64_t fromNodeId, int64_t toNodeId, const std::string &label) {
		std::unique_lock lock(mutex_);

		// Store edge information
		EdgeInfo info{fromNodeId, toNodeId, label};
		edgeInfo_[edgeId] = info;

		// Add to label index
		labelToEdges_[label].push_back(edgeId);

		// Add to node-to-node index
		nodeToNodeEdges_[fromNodeId][toNodeId].push_back(edgeId);

		// Add to outgoing edges index
		outgoingEdges_[fromNodeId][label].push_back(edgeId);

		// Add to incoming edges index
		incomingEdges_[toNodeId][label].push_back(edgeId);
	}

	void RelationshipIndex::removeEdge(int64_t edgeId) {
		std::unique_lock lock(mutex_);

		auto edgeIt = edgeInfo_.find(edgeId);
		if (edgeIt == edgeInfo_.end()) {
			return;
		}

		const EdgeInfo &info = edgeIt->second;

		// Remove from label index
		auto &labelEdges = labelToEdges_[info.label];
		labelEdges.erase(std::remove(labelEdges.begin(), labelEdges.end(), edgeId), labelEdges.end());
		if (labelEdges.empty()) {
			labelToEdges_.erase(info.label);
		}

		// Remove from node-to-node index
		auto &nodeToNodeEdgesList = nodeToNodeEdges_[info.fromNodeId][info.toNodeId];
		nodeToNodeEdgesList.erase(std::remove(nodeToNodeEdgesList.begin(), nodeToNodeEdgesList.end(), edgeId),
								  nodeToNodeEdgesList.end());
		if (nodeToNodeEdgesList.empty()) {
			nodeToNodeEdges_[info.fromNodeId].erase(info.toNodeId);
			if (nodeToNodeEdges_[info.fromNodeId].empty()) {
				nodeToNodeEdges_.erase(info.fromNodeId);
			}
		}

		// Remove from outgoing edges index
		auto &outEdges = outgoingEdges_[info.fromNodeId][info.label];
		outEdges.erase(std::remove(outEdges.begin(), outEdges.end(), edgeId), outEdges.end());
		if (outEdges.empty()) {
			outgoingEdges_[info.fromNodeId].erase(info.label);
			if (outgoingEdges_[info.fromNodeId].empty()) {
				outgoingEdges_.erase(info.fromNodeId);
			}
		}

		// Remove from incoming edges index
		auto &inEdges = incomingEdges_[info.toNodeId][info.label];
		inEdges.erase(std::remove(inEdges.begin(), inEdges.end(), edgeId), inEdges.end());
		if (inEdges.empty()) {
			incomingEdges_[info.toNodeId].erase(info.label);
			if (incomingEdges_[info.toNodeId].empty()) {
				incomingEdges_.erase(info.toNodeId);
			}
		}

		// Remove edge info
		edgeInfo_.erase(edgeId);
	}

	std::vector<int64_t> RelationshipIndex::findEdgesByLabel(const std::string &label) const {
		std::shared_lock lock(mutex_);

		auto it = labelToEdges_.find(label);
		if (it == labelToEdges_.end()) {
			return {};
		}

		return it->second;
	}

	std::vector<int64_t> RelationshipIndex::findEdgesBetweenNodes(int64_t fromNodeId, int64_t toNodeId) const {
		std::shared_lock lock(mutex_);

		auto fromIt = nodeToNodeEdges_.find(fromNodeId);
		if (fromIt == nodeToNodeEdges_.end()) {
			return {};
		}

		auto toIt = fromIt->second.find(toNodeId);
		if (toIt == fromIt->second.end()) {
			return {};
		}

		return toIt->second;
	}

	std::vector<int64_t> RelationshipIndex::findOutgoingEdges(int64_t nodeId, const std::string &label) const {
		std::shared_lock lock(mutex_);

		auto nodeIt = outgoingEdges_.find(nodeId);
		if (nodeIt == outgoingEdges_.end()) {
			return {};
		}

		if (label.empty()) {
			// Return all outgoing edges regardless of label
			std::vector<int64_t> result;
			for (const auto &[edgeLabel, edges]: nodeIt->second) {
				result.insert(result.end(), edges.begin(), edges.end());
			}
			return result;
		} else {
			// Return edges with specific label
			auto labelIt = nodeIt->second.find(label);
			if (labelIt == nodeIt->second.end()) {
				return {};
			}
			return labelIt->second;
		}
	}

	std::vector<int64_t> RelationshipIndex::findIncomingEdges(int64_t nodeId, const std::string &label) const {
		std::shared_lock lock(mutex_);

		auto nodeIt = incomingEdges_.find(nodeId);
		if (nodeIt == incomingEdges_.end()) {
			return {};
		}

		if (label.empty()) {
			// Return all incoming edges regardless of label
			std::vector<int64_t> result;
			for (const auto &[edgeLabel, edges]: nodeIt->second) {
				result.insert(result.end(), edges.begin(), edges.end());
			}
			return result;
		} else {
			// Return edges with specific label
			auto labelIt = nodeIt->second.find(label);
			if (labelIt == nodeIt->second.end()) {
				return {};
			}
			return labelIt->second;
		}
	}

	void RelationshipIndex::clear() {
		std::unique_lock lock(mutex_);

		labelToEdges_.clear();
		nodeToNodeEdges_.clear();
		outgoingEdges_.clear();
		incomingEdges_.clear();
		edgeInfo_.clear();
	}

} // namespace graph::query::indexes
