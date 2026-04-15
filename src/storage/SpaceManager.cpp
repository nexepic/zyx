/**
 * @file SpaceManager.cpp
 * @author Nexepic
 * @date 2025/4/11
 *
 * @brief Coordination layer for segment space management.
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/SegmentType.hpp"

namespace graph::storage {

	SpaceManager::SpaceManager(std::shared_ptr<SegmentAllocator> allocator, std::shared_ptr<SegmentCompactor> compactor,
							   std::shared_ptr<FileTruncator> truncator, std::shared_ptr<SegmentTracker> tracker) :
		allocator_(std::move(allocator)), compactor_(std::move(compactor)),
		truncator_(std::move(truncator)), tracker_(std::move(tracker)) {}

	SpaceManager::~SpaceManager() = default;

	double SpaceManager::getTotalFragmentationRatio() const {
		double totalWeightedRatio = 0.0;
		double totalWeight = 0.0;

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			double ratio = tracker_->calculateFragmentationRatio(type);
			auto segments = tracker_->getSegmentsByType(type);
			double weight = segments.size();
			totalWeightedRatio += ratio * weight;
			totalWeight += weight;
		}

		if (totalWeight == 0) {
			return 0.0;
		}

		return totalWeightedRatio / totalWeight;
	}

	bool SpaceManager::shouldCompact() const {
		return getTotalFragmentationRatio() > COMPACTION_THRESHOLD;
	}

	bool SpaceManager::safeCompactSegments() {
		if (!compactionMutex_.try_lock()) {
			return false;
		}

		compactSegments();

		compactionMutex_.unlock();
		return true;
	}

	void SpaceManager::compactSegments() {
		compactor_->processAllEmptySegments();

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			compactor_->compactSegments(type, COMPACTION_THRESHOLD);
		}

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			compactor_->mergeSegments(type, COMPACTION_THRESHOLD);
		}

		static_cast<void>(compactor_->relocateSegmentsFromEnd());

		updateMaxIds();
	}

	void SpaceManager::updateMaxIds() const {
		compactor_->recalculateMaxIds();
	}

} // namespace graph::storage
