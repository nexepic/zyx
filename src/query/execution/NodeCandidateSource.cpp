/**
 * @file NodeCandidateSource.cpp
 * @date 2026/05/26
 *
 * Licensed under the Apache License, Version 2.0.
 **/

#include "graph/query/execution/NodeCandidateSource.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph::query::execution {

	NodeCandidateSource::NodeCandidateSource(std::shared_ptr<storage::DataManager> dm,
										   std::shared_ptr<indexes::IndexManager> im)
		: dm_(std::move(dm)), im_(std::move(im)) {}

	std::vector<int64_t> NodeCandidateSource::collect(const NodeScanConfig &config) const {
		using Clock = std::chrono::steady_clock;
		const bool profileEnabled = debug::PerfTrace::isEnabled();
		const auto candidatesStart = profileEnabled ? Clock::now() : Clock::time_point{};

		std::vector<int64_t> candidateIds;
		switch (config.type) {
			case ScanType::PROPERTY_SCAN:
				candidateIds = im_->findNodeIdsByProperty(config.indexKey, config.indexValue);
				break;

			case ScanType::RANGE_SCAN:
				candidateIds = im_->findNodeIdsByPropertyRange(config.indexKey, config.rangeMin, config.rangeMax);
				break;

			case ScanType::COMPOSITE_SCAN:
				candidateIds = im_->findNodeIdsByCompositeIndex(config.compositeKeys, config.compositeValues);
				break;

			case ScanType::LABEL_SCAN:
				candidateIds = im_->findNodeIdsByLabel(config.label());
				break;

			case ScanType::FULL_SCAN:
			default:
				int64_t maxId = dm_->getIdAllocator(EntityType::Node)->getCurrentMaxId();
				candidateIds.reserve(static_cast<size_t>(maxId));
				for (int64_t i = 1; i <= maxId; ++i) {
					candidateIds.push_back(i);
				}
				break;
		}

		if (candidateIds.size() > 1) {
			std::sort(candidateIds.begin(), candidateIds.end());
			candidateIds.erase(std::unique(candidateIds.begin(), candidateIds.end()), candidateIds.end());
		}

		if (profileEnabled) {
			debug::PerfTrace::addDuration(
				"node_scan.candidates",
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - candidatesStart)
								 .count()));
		}

		return candidateIds;
	}

} // namespace graph::query::execution
