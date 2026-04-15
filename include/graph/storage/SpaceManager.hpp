/**
 * @file SpaceManager.hpp
 * @author Nexepic
 * @date 2025/4/11
 *
 * @brief Coordination layer for segment space management.
 *
 * SpaceManager orchestrates SegmentAllocator, SegmentCompactor, and
 * FileTruncator to provide high-level operations like shouldCompact()
 * and safeCompactSegments().
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include "FileTruncator.hpp"
#include "SegmentAllocator.hpp"
#include "SegmentCompactor.hpp"
#include "SegmentTracker.hpp"

namespace graph::storage {

	class SpaceManager {
	public:
		SpaceManager(std::shared_ptr<SegmentAllocator> allocator, std::shared_ptr<SegmentCompactor> compactor,
					 std::shared_ptr<FileTruncator> truncator, std::shared_ptr<SegmentTracker> tracker);
		~SpaceManager();

		bool shouldCompact() const;
		bool safeCompactSegments();
		void compactSegments();
		double getTotalFragmentationRatio() const;

		std::shared_ptr<SegmentAllocator> getAllocator() const { return allocator_; }
		std::shared_ptr<SegmentCompactor> getCompactor() const { return compactor_; }
		std::shared_ptr<FileTruncator> getTruncator() const { return truncator_; }
		std::shared_ptr<SegmentTracker> getTracker() const { return tracker_; }

		std::mutex &getMutex() { return mutex_; }

	private:
		std::mutex compactionMutex_;
		std::mutex mutex_;

		std::shared_ptr<SegmentAllocator> allocator_;
		std::shared_ptr<SegmentCompactor> compactor_;
		std::shared_ptr<FileTruncator> truncator_;
		std::shared_ptr<SegmentTracker> tracker_;

		static constexpr double COMPACTION_THRESHOLD = 0.3;

		void updateMaxIds() const;
	};

} // namespace graph::storage
