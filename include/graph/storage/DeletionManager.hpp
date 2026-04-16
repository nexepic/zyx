/**
 * @file DeletionManager.hpp
 * @author Nexepic
 * @date 2025/4/1
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
#include <cstdint>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "IDAllocator.hpp"
#include "SegmentTracker.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Property.hpp"

namespace graph::storage {

	// Forward declarations
	class DataManager;
	class FileStorage;

	/**
	 * Manages entity deletion and space management
	 */
	class DeletionManager {
	public:
		DeletionManager(std::shared_ptr<DataManager> dataManager, std::shared_ptr<SegmentTracker> tracker,
						IDAllocators allocators);
		~DeletionManager();

		/**
		 * Initialize the deletion manager
		 */
		static void initialize();

		/**
		 * Delete a node
		 */
		void deleteNode(Node &node) const;

		/**
		 * Delete an edge
		 */
		void deleteEdge(Edge &edge) const;

		void deleteProperty(Property &property) const;

		void deleteBlob(Blob &blob) const;

		void deleteIndex(Index &index) const;

		void deleteState(State &state) const;

		/**
		 * Delete multiple nodes
		 */
		bool deleteBulkNodes(const std::vector<int64_t> &nodeIds, bool cascadeEdges = false) const;

		/**
		 * Delete multiple edges
		 */
		bool deleteBulkEdges(const std::vector<int64_t> &edgeIds) const;

		/**
		 * Refresh segment state information
		 */
		// void refreshSegmentState();

		/**
		 * Mark a node as inactive
		 */
		void markNodeInactive(Node &node) const;

		void deletePropertyEntity(int64_t propertyId, PropertyStorageType storageType) const;

		void deleteNodeConnectedEdges(int64_t nodeId) const;

		/**
		 * Mark an edge as inactive
		 */
		void markEdgeInactive(Edge &edge) const;

		void markPropertyInactive(Property &property) const;

		void markBlobInactive(Blob &blob) const;

		void markIndexInactive(Index &index) const;

		void markStateInactive(State &state) const;

		/**
		 * Check if a node is inactive
		 */
		bool isNodeActive(int64_t nodeId) const;

		/**
		 * Check if an edge is inactive
		 */
		bool isEdgeActive(int64_t edgeId) const;

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

		uint64_t findSegmentForIndexId(int64_t id) const;

		uint64_t findSegmentForStateId(int64_t id) const;

		/**
		 * Analyze segment fragmentation for the given entity type
		 */
		std::unordered_map<uint64_t, double> analyzeSegmentFragmentation(uint32_t entityType) const;

	private:
		// std::shared_ptr<std::fstream> file_;
		// FileStorage &storage_;
		std::shared_ptr<DataManager> dataManager_;
		std::shared_ptr<SegmentTracker> tracker_;
		IDAllocators allocators_;

		// Compaction thresholds and counters
		static constexpr double COMPACTION_THRESHOLD = 0.3; // 30% inactive triggers compaction

		// Internal helper methods
		bool performNodeDeletion(int64_t nodeId) const;
		bool performEdgeDeletion(int64_t edgeId) const;

		// Callback for reference updates after segment movement
		// void handleReferenceUpdate(uint64_t oldOffset, uint64_t newOffset, uint32_t type);

		// Cache for segment headers to reduce disk access
		mutable std::mutex mutex_;
	};

} // namespace graph::storage
