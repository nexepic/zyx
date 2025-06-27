/**
 * @file SpaceManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>
#include "EntityReferenceUpdater.hpp"
#include "FileHeaderManager.hpp"
#include "IDAllocator.hpp"
#include "SegmentTracker.hpp"
#include "SegmentType.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	class SpaceManager {
	public:
		SpaceManager(std::shared_ptr<std::fstream> file, std::string fileName, std::shared_ptr<SegmentTracker> tracker,
					 std::shared_ptr<FileHeaderManager> fileHeaderManager, std::shared_ptr<IDAllocator> idAllocator);
		~SpaceManager();

		void initialize(FileHeader &header);
		uint64_t findMaxId(uint32_t type, std::shared_ptr<SegmentTracker> &tracker);
		uint64_t allocateSegment(uint32_t type, uint32_t capacity);
		void deallocateSegment(uint64_t offset);
		void compactSegments(uint32_t type, double threshold);
		bool moveSegment(uint64_t sourceOffset, uint64_t destinationOffset);

		bool shouldCompact() const;

		bool safeCompactSegments();
		void compactSegments();

		double getTotalFragmentationRatio() const;

		// Get the tracker
		std::shared_ptr<SegmentTracker> getTracker() const { return segmentTracker_; }

		// methods for segment merging
		std::vector<uint64_t> findCandidatesForMerge(uint32_t type, double usageThreshold);
		bool mergeSegments(uint32_t type, double usageThreshold);

		template<typename EntityType>
		void processEntity(uint64_t sourceOffset, uint64_t targetOffset, uint32_t i, uint32_t &targetNextIndex,
						   uint8_t *newBitmap, size_t itemSize, const SegmentHeader &sourceHeader,
						   const SegmentHeader &targetHeader, uint32_t type);

		bool mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type);

		// Remove empty segments
		bool processEmptySegments(uint32_t type);
		void processAllEmptySegments();

		// Truncates the file by removing unused trailing segments
		bool truncateFile();

		std::mutex &getMutex() { return mutex_; }

		bool isSegmentAtEndOfFile(uint64_t offset) const;
		uint64_t findFreeSegmentNotAtEnd() const;
		void initializeSegmentHeader(uint64_t offset, uint32_t type, uint32_t capacity);

		void setEntityReferenceUpdater(std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater) {
			entityReferenceUpdater_ = std::move(entityReferenceUpdater);
		}

		// Update FileHeader chain heads based on current SegmentTracker state
		void updateFileHeaderChainHeads();

		// Validate segment chains to ensure integrity
		void validateSegmentChains() {
			segmentTracker_->validateSegmentChains();
			updateFileHeaderChainHeads();
		}

		void recalculateMaxIds();

	private:
		std::mutex compactionMutex_;
		std::atomic<bool> compactionInProgress_{false};

		std::shared_ptr<std::fstream> file_;
		std::string fileName_;
		std::shared_ptr<SegmentTracker> segmentTracker_;
		std::mutex mutex_;

		std::shared_ptr<FileHeaderManager> fileHeaderManager_;

		std::shared_ptr<IDAllocator> idAllocator_;

		std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater_;

		// Update file header with current max IDs
		void updateMaxIds();

		// Compaction thresholds and counters
		static constexpr double COMPACTION_THRESHOLD = 0.3; // 30% inactive triggers compaction

		template<typename EntityType>
		bool compactSegment(uint64_t offset, SegmentType segmentType, size_t entitySize);

		bool compactNodeSegment(uint64_t offset);
		bool compactEdgeSegment(uint64_t offset);
		bool compactPropertySegment(uint64_t offset);
		bool compactBlobSegment(uint64_t offset);
		bool compactIndexSegment(uint64_t offset);
		bool compactStateSegment(uint64_t offset);
		bool copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset);
		void updateSegmentChain(uint64_t newOffset, const SegmentHeader &info);
		uint64_t findLastSegment() const;

		uint64_t findSegmentForNodeId(int64_t id) const;
		uint64_t findSegmentForEdgeId(int64_t id) const;
		SegmentHeader readSegmentHeader(uint64_t offset) const;

		void writeSegmentHeader(uint64_t offset, const SegmentHeader &header);

		// Identifies segments at the end of file that can be truncated
		std::vector<uint64_t> findTruncatableSegments() const;

		// Relocate segments from the end of the file to fill gaps
		bool relocateSegmentsFromEnd();

		// Find segments near the end of the file that could be relocated
		std::vector<uint64_t> findRelocatableSegments() const;

		// Find the file size by examining segment positions
		uint64_t calculateCurrentFileSize() const;

		int64_t calculateLastUsedIdInSegment(const SegmentHeader &header);
	};

} // namespace graph::storage
