/**
 * @file DeletionManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/1
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <cstdint>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "SpaceManager.hpp"
#include <graph/core/Blob.hpp>
#include <graph/core/Property.hpp>

namespace graph::storage {

	// Forward declarations
	class DataManager;
	class FileStorage;

	/**
	 * Manages entity deletion and space management
	 */
	class DeletionManager {
	public:
		DeletionManager(std::shared_ptr<DataManager> dataManager, std::shared_ptr<SpaceManager> spaceManager);
		~DeletionManager();

		/**
		 * Initialize the deletion manager
		 */
		void initialize();

		/**
		 * Delete a node
		 */
		void deleteNode(Node &node);

		/**
		 * Delete an edge
		 */
		void deleteEdge(Edge &edge);

		void deleteProperty(Property &property);

		void deleteBlob(Blob &blob);

		/**
		 * Delete multiple nodes
		 */
		bool deleteBulkNodes(const std::vector<uint64_t> &nodeIds, bool cascadeEdges = false);

		/**
		 * Delete multiple edges
		 */
		bool deleteBulkEdges(const std::vector<uint64_t> &edgeIds);

		/**
		 * Refresh segment state information
		 */
		// void refreshSegmentState();

		/**
		 * Mark a node as inactive
		 */
		void markNodeInactive(Node &node);

		void deletePropertyEntity(int64_t propertyId, PropertyStorageType storageType);

		void deleteNodeConnectedEdges(int64_t nodeId);

		/**
		 * Mark an edge as inactive
		 */
		void markEdgeInactive(Edge &edge);

		void markPropertyInactive(Property &property);

		void markBlobInactive(Blob &blob);

		/**
		 * Check if a node is inactive
		 */
		bool isNodeActive(uint64_t nodeId) const;

		/**
		 * Check if an edge is inactive
		 */
		bool isEdgeActive(uint64_t edgeId) const;

		/**
		 * Get the segment that contains the specified node ID
		 */
		uint64_t findSegmentForNodeId(int64_t id) const;

		/**
		 * Get the segment that contains the specified edge ID
		 */
		uint64_t findSegmentForEdgeId(int64_t id) const;

		uint64_t findSegmentForPropertyId(int64_t id) const;

		uint64_t findSegmentForBlobId(int64_t id) const;

		/**
		 * Analyze segment fragmentation for the given entity type
		 */
		std::unordered_map<uint64_t, double> analyzeSegmentFragmentation(uint32_t entityType) const;

	private:
		// std::shared_ptr<std::fstream> file_;
		// FileStorage &storage_;
		std::shared_ptr<DataManager> dataManager_;
		std::shared_ptr<SpaceManager> spaceManager_;

		// Compaction thresholds and counters
		static constexpr double COMPACTION_THRESHOLD = 0.3; // 30% inactive triggers compaction

		// Internal helper methods
		bool performNodeDeletion(uint64_t nodeId);
		bool performEdgeDeletion(uint64_t edgeId);

		// Callback for reference updates after segment movement
		// void handleReferenceUpdate(uint64_t oldOffset, uint64_t newOffset, uint32_t type);

		// Cache for segment headers to reduce disk access
		mutable std::mutex mutex_;
	};

} // namespace graph::storage
