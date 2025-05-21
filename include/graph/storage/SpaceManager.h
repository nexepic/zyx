/**
 * @file SpaceManager.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "FileHeaderManager.h"
#include "IDAllocator.h"


#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>
#include "SegmentTracker.h"
#include "StorageHeaders.h"

namespace graph::storage {

	// Callback function type for updating references
	using ReferenceUpdateCallback = std::function<void(uint64_t, uint64_t, uint32_t)>;

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

		// Update references when a segment is moved
		void updateSegmentReferences(uint64_t oldOffset, uint64_t newOffset, uint32_t type);
		void updateNodeSegmentReferences(uint64_t oldOffset, uint64_t newOffset);
		void updateEdgeSegmentReferences(uint64_t oldOffset, uint64_t newOffset);
		void updatePropertySegmentReferences(uint64_t oldOffset, uint64_t newOffset);

		// Helper methods for reference updates
		void updateEdgeReferencesToNode(uint64_t nodeId, uint64_t oldNodeSegment, uint64_t newNodeSegment);
		void updatePropertyReferencesToEntity(int64_t entityId, uint32_t entityType, uint64_t oldSegment,
											  uint64_t newSegment);

		// void setReferenceUpdateCallback(ReferenceUpdateCallback callback);

		bool shouldCompact() const;

		void compactSegments();

		double getTotalFragmentationRatio() const;

		// Get the tracker
		std::shared_ptr<SegmentTracker> getTracker() const { return tracker_; }

		// methods for segment merging
		std::vector<uint64_t> findCandidatesForMerge(uint32_t type, double usageThreshold);
		bool mergeSegments(uint32_t type, double usageThreshold);
		bool mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type);

		// Remove empty segments
		bool removeEmptySegments(uint32_t type);
		void removeAllEmptySegments();

		// Property reference updates
		void updatePropertyReference(uint64_t entityId, uint32_t entityType, uint64_t oldSegmentOffset,
									 uint64_t newSegmentOffset, uint64_t oldEntryOffset, uint64_t newEntryOffset);

		// Truncates the file by removing unused trailing segments
		bool truncateFile();

		std::mutex &getMutex() { return mutex_; }

		bool isSegmentAtEndOfFile(uint64_t offset) const;
		uint64_t findFreeSegmentNotAtEnd() const;
		void initializeSegmentHeader(uint64_t offset, uint32_t type, uint32_t capacity);

	private:
		std::shared_ptr<std::fstream> file_;
		std::string fileName_;
		std::shared_ptr<SegmentTracker> tracker_;
		std::mutex mutex_;
		// ReferenceUpdateCallback referenceUpdateCallback_;

		std::shared_ptr<FileHeaderManager> fileHeaderManager_;

		std::shared_ptr<IDAllocator> idAllocator_;

		// Update file header with current max IDs
		void updateMaxIds();

		// Compaction thresholds and counters
		static constexpr double COMPACTION_THRESHOLD = 0.3; // 30% inactive triggers compaction

		bool compactNodeSegment(uint64_t offset);
		bool compactEdgeSegment(uint64_t offset);
		bool compactPropertySegment(uint64_t offset);
		bool compactBlobSegment(uint64_t offset);
		bool copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset, const SegmentHeader &info);
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
	};

} // namespace graph::storage
