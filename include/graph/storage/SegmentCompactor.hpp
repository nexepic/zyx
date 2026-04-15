/**
 * @file SegmentCompactor.hpp
 * @brief Segment compaction, merging, relocation, and max-ID recalculation
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "EntityDispatch.hpp"
#include "EntityReferenceUpdater.hpp"
#include "FileHeaderManager.hpp"
#include "SegmentAllocator.hpp"
#include "SegmentTracker.hpp"
#include "StorageHeaders.hpp"
#include "StorageIO.hpp"

namespace graph::storage {

	class SegmentCompactor {
	public:
		SegmentCompactor(std::shared_ptr<StorageIO> io, std::shared_ptr<SegmentTracker> tracker,
						 std::shared_ptr<SegmentAllocator> allocator,
						 std::shared_ptr<FileHeaderManager> fileHeaderManager);
		~SegmentCompactor();

		void compactSegments(uint32_t type, double threshold);
		bool mergeSegments(uint32_t type, double usageThreshold);
		bool mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type);
		std::vector<uint64_t> findCandidatesForMerge(uint32_t type, double usageThreshold) const;

		bool processEmptySegments(uint32_t type) const;
		void processAllEmptySegments() const;

		bool moveSegment(uint64_t sourceOffset, uint64_t destinationOffset) const;

		void recalculateMaxIds() const;

		void setEntityReferenceUpdater(std::shared_ptr<EntityReferenceUpdater> updater) {
			entityReferenceUpdater_ = std::move(updater);
		}

		// Utility methods needed by SpaceManager coordination
		bool isSegmentAtEndOfFile(uint64_t offset) const;
		uint64_t findFreeSegmentNotAtEnd() const;
		bool relocateSegmentsFromEnd() const;

	private:
		template<EntityType ET>
		bool compactSegmentImpl(uint64_t offset);

		template<EntityType ET>
		void processEntityImpl(uint64_t sourceOffset, uint64_t targetOffset, uint32_t i,
							   uint32_t &targetNextIndex, uint8_t *newBitmap,
							   const SegmentHeader &sourceHeader, const SegmentHeader &targetHeader);

		bool copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset) const;
		void updateSegmentChain(uint64_t newOffset, const SegmentHeader &info) const;
		std::vector<uint64_t> findRelocatableSegments() const;
		uint64_t calculateCurrentFileSize() const;
		int64_t calculateLastUsedIdInSegment(const SegmentHeader &header) const;

		std::shared_ptr<StorageIO> io_;
		std::shared_ptr<SegmentTracker> tracker_;
		std::shared_ptr<SegmentAllocator> allocator_;
		std::shared_ptr<FileHeaderManager> fileHeaderManager_;
		std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater_;
	};

} // namespace graph::storage
