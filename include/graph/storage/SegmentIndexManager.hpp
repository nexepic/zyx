/**
 * @file SegmentIndexManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "SegmentTracker.hpp"
#include "SegmentType.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	/**
	 * Manages segment indexes for efficient entity ID lookups.
	 * Provides incremental updates when segments are allocated or deallocated.
	 */
	class SegmentIndexManager {
	public:
		// Structure representing an indexed segment
		struct SegmentIndex {
			int64_t startId;
			int64_t endId;
			uint64_t segmentOffset;
		};

		/**
		 * Constructor
		 * @param segmentTracker The segment tracker to use for fetching segment information
		 */
		explicit SegmentIndexManager(std::shared_ptr<SegmentTracker> segmentTracker);

		/**
		 * Initialize all segment indexes from chain heads
		 */
		void initialize(uint64_t &nodeHead, uint64_t &edgeHead, uint64_t &propertyHead, uint64_t &blobHead,
						uint64_t &indexHead, uint64_t &stateHead);

		/**
		 * Find the segment containing an entity ID
		 * @param type The entity type
		 * @param id The entity ID to locate
		 * @return The file offset of the segment containing the ID, or 0 if not found
		 */
		uint64_t findSegmentForId(uint32_t type, int64_t id) const;

		// Accessors for compatibility with existing code
		const std::vector<SegmentIndex> &getNodeSegmentIndex() const { return nodeSegmentIndex_; }
		const std::vector<SegmentIndex> &getEdgeSegmentIndex() const { return edgeSegmentIndex_; }
		const std::vector<SegmentIndex> &getPropertySegmentIndex() const { return propertySegmentIndex_; }
		const std::vector<SegmentIndex> &getBlobSegmentIndex() const { return blobSegmentIndex_; }
		const std::vector<SegmentIndex> &getIndexSegmentIndex() const { return indexSegmentIndex_; }
		const std::vector<SegmentIndex> &getStateSegmentIndex() const { return stateSegmentIndex_; }

		void buildSegmentIndexes();

		bool updateSegmentIndexByOffset(uint64_t offset, const SegmentHeader &header);

	private:
		std::shared_ptr<SegmentTracker> segmentTracker_;

		// Index vectors for each entity type
		std::vector<SegmentIndex> nodeSegmentIndex_;
		std::vector<SegmentIndex> edgeSegmentIndex_;
		std::vector<SegmentIndex> propertySegmentIndex_;
		std::vector<SegmentIndex> blobSegmentIndex_;
		std::vector<SegmentIndex> indexSegmentIndex_;
		std::vector<SegmentIndex> stateSegmentIndex_;

		uint64_t *nodeSegmentHead_ = nullptr;
		uint64_t *edgeSegmentHead_ = nullptr;
		uint64_t *propertySegmentHead_ = nullptr;
		uint64_t *blobSegmentHead_ = nullptr;
		uint64_t *indexSegmentHead_ = nullptr;
		uint64_t *stateSegmentHead_ = nullptr;

		mutable std::mutex mutex_;

		/**
		 * Build an entire segment index from a chain head
		 */
		void buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead);

		/**
		 * Get the index vector for a specific type
		 */
		std::vector<SegmentIndex> &getSegmentIndexForType(uint32_t type);
		const std::vector<SegmentIndex> &getSegmentIndexForType(uint32_t type) const;

		/**
		 * Sort a segment index for binary search
		 */
		static void sortSegmentIndex(std::vector<SegmentIndex> &segmentIndex);
	};

} // namespace graph::storage
