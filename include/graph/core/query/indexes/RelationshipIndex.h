/**
 * @file RelationshipIndex.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>

namespace graph::query::indexes {

	class RelationshipIndex {
	public:
		RelationshipIndex();

		// Add an edge to the index
		void addEdge(uint64_t edgeId, uint64_t fromNodeId, uint64_t toNodeId, const std::string& label);

		// Remove an edge from the index
		void removeEdge(uint64_t edgeId);

		// Find edges by label
		[[nodiscard]] std::vector<uint64_t> findEdgesByLabel(const std::string& label) const;

		// Find edges between specific nodes
		[[nodiscard]] std::vector<uint64_t> findEdgesBetweenNodes(uint64_t fromNodeId, uint64_t toNodeId) const;

		// Find outgoing edges from a node
		[[nodiscard]] std::vector<uint64_t> findOutgoingEdges(uint64_t nodeId, const std::string& label = "") const;

		// Find incoming edges to a node
		[[nodiscard]] std::vector<uint64_t> findIncomingEdges(uint64_t nodeId, const std::string& label = "") const;

		// Clear the index
		void clear();

	private:
		// Label -> edge IDs
		std::unordered_map<std::string, std::vector<uint64_t>> labelToEdges_;

		// fromNodeId -> toNodeId -> edge IDs
		std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::vector<uint64_t>>> nodeToNodeEdges_;

		// fromNodeId -> (label -> edge IDs)
		std::unordered_map<uint64_t, std::unordered_map<std::string, std::vector<uint64_t>>> outgoingEdges_;

		// toNodeId -> (label -> edge IDs)
		std::unordered_map<uint64_t, std::unordered_map<std::string, std::vector<uint64_t>>> incomingEdges_;

		// Edge metadata
		struct EdgeInfo {
			uint64_t fromNodeId;
			uint64_t toNodeId;
			std::string label;
		};

		// Edge ID -> edge metadata
		std::unordered_map<uint64_t, EdgeInfo> edgeInfo_;

		// Thread safety
		mutable std::shared_mutex mutex_;
	};

} // namespace graph::query::indexes