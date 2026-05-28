/**
 * @file NodeCandidateSource.cpp
 * @date 2026/05/26
 *
 * Licensed under the Apache License, Version 2.0.
 **/

#include "graph/query/execution/NodeCandidateSource.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph::query::execution {
namespace {

	void intersectSorted(std::vector<int64_t> &left, const std::vector<int64_t> &right) {
		std::vector<int64_t> intersection;
		intersection.reserve(std::min(left.size(), right.size()));
		std::set_intersection(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(intersection));
		left = std::move(intersection);
	}

	bool applyLabelIndexFilter(const std::shared_ptr<indexes::IndexManager> &im,
	                           const NodeScanConfig &config,
	                           NodeCandidateSet &candidateSet) {
		if (!im || config.labels.empty() || !im->hasLabelIndex("node")) {
			return config.labels.empty();
		}

		for (const auto &label : config.labels) {
			auto labelIds = im->findNodeIdsByLabel(label);
			if (labelIds.size() > 1) {
				std::sort(labelIds.begin(), labelIds.end());
				labelIds.erase(std::unique(labelIds.begin(), labelIds.end()), labelIds.end());
			}
			intersectSorted(candidateSet.ids, labelIds);
			if (candidateSet.ids.empty()) {
				break;
			}
		}
		return true;
	}

} // namespace

	NodeCandidateSource::NodeCandidateSource(std::shared_ptr<storage::DataManager> dm,
										   std::shared_ptr<indexes::IndexManager> im)
		: dm_(std::move(dm)), im_(std::move(im)) {}

	std::vector<int64_t> NodeCandidateSource::collect(const NodeScanConfig &config) const {
		return collectWithMetadata(config).ids;
	}

	NodeCandidateSet NodeCandidateSource::collectWithMetadata(const NodeScanConfig &config) const {
		using Clock = std::chrono::steady_clock;
		const bool profileEnabled = debug::PerfTrace::isEnabled();
		const auto candidatesStart = profileEnabled ? Clock::now() : Clock::time_point{};

		NodeCandidateSet candidateSet;
		switch (config.type) {
			case ScanType::PROPERTY_SCAN:
				if (config.labels.size() == 1 && im_->hasNodePropertyIndexForLabel(config.label(), config.indexKey)) {
					candidateSet.ids = im_->findNodeIdsByLabelAndProperty(config.label(), config.indexKey, config.indexValue);
					candidateSet.labelsSatisfied = true;
				} else {
					candidateSet.ids = im_->findNodeIdsByProperty(config.indexKey, config.indexValue);
				}
				candidateSet.activeOnly = true;
				break;

			case ScanType::RANGE_SCAN:
				if (config.labels.size() == 1 && im_->hasNodePropertyIndexForLabel(config.label(), config.indexKey)) {
					candidateSet.ids = im_->findNodeIdsByLabelAndPropertyRange(
							config.label(),
							config.indexKey,
							config.rangeMin,
							config.rangeMax,
							config.minInclusive,
							config.maxInclusive);
					candidateSet.labelsSatisfied = true;
				} else {
					candidateSet.ids = im_->findNodeIdsByPropertyRange(
							config.indexKey,
							config.rangeMin,
							config.rangeMax,
							config.minInclusive,
							config.maxInclusive);
				}
				candidateSet.activeOnly = true;
				break;

			case ScanType::COMPOSITE_SCAN:
				candidateSet.ids = im_->findNodeIdsByCompositeIndex(config.compositeKeys, config.compositeValues);
				candidateSet.activeOnly = true;
				break;

			case ScanType::LABEL_SCAN:
				candidateSet.ids = im_->findNodeIdsByLabel(config.label());
				candidateSet.activeOnly = true;
				candidateSet.labelsSatisfied = config.labels.size() <= 1;
				break;

			case ScanType::FULL_SCAN:
			default:
				int64_t maxId = dm_->getIdAllocator(EntityType::Node)->getCurrentMaxId();
				candidateSet.ids.reserve(static_cast<size_t>(maxId));
				for (int64_t i = 1; i <= maxId; ++i) {
					candidateSet.ids.push_back(i);
				}
				break;
		}

		if (candidateSet.ids.size() > 1) {
			std::sort(candidateSet.ids.begin(), candidateSet.ids.end());
			candidateSet.ids.erase(std::unique(candidateSet.ids.begin(), candidateSet.ids.end()), candidateSet.ids.end());
		}
		if (config.type == ScanType::LABEL_SCAN && !candidateSet.labelsSatisfied) {
			candidateSet.labelsSatisfied = applyLabelIndexFilter(im_, config, candidateSet);
		} else if (config.labels.empty()) {
			candidateSet.labelsSatisfied = true;
		}

		if (profileEnabled) {
			debug::PerfTrace::addDuration(
				"node_scan.candidates",
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - candidatesStart)
								 .count()));
		}

		return candidateSet;
	}

	NodeCandidateCount NodeCandidateSource::countWithMetadata(const NodeScanConfig &config) const {
		using Clock = std::chrono::steady_clock;
		const bool profileEnabled = debug::PerfTrace::isEnabled();
		const auto candidatesStart = profileEnabled ? Clock::now() : Clock::time_point{};

		NodeCandidateCount candidateCount;
		switch (config.type) {
			case ScanType::PROPERTY_SCAN:
				if (config.labels.size() == 1 && im_->hasNodePropertyIndexForLabel(config.label(), config.indexKey)) {
					candidateCount.count = static_cast<int64_t>(
							im_->countNodeIdsByLabelAndProperty(config.label(), config.indexKey, config.indexValue));
					candidateCount.labelsSatisfied = true;
				} else {
					candidateCount.count = static_cast<int64_t>(
							im_->countNodeIdsByProperty(config.indexKey, config.indexValue));
					candidateCount.labelsSatisfied = config.labels.empty();
				}
				candidateCount.activeOnly = true;
				candidateCount.available = true;
				break;

			case ScanType::RANGE_SCAN:
				if (config.labels.size() == 1 && im_->hasNodePropertyIndexForLabel(config.label(), config.indexKey)) {
					candidateCount.count = static_cast<int64_t>(
							im_->countNodeIdsByLabelAndPropertyRange(
									config.label(),
									config.indexKey,
									config.rangeMin,
									config.rangeMax,
									config.minInclusive,
									config.maxInclusive));
					candidateCount.labelsSatisfied = true;
				} else {
					candidateCount.count = static_cast<int64_t>(
							im_->countNodeIdsByPropertyRange(
									config.indexKey,
									config.rangeMin,
									config.rangeMax,
									config.minInclusive,
									config.maxInclusive));
					candidateCount.labelsSatisfied = config.labels.empty();
				}
				candidateCount.activeOnly = true;
				candidateCount.available = true;
				break;

			case ScanType::COMPOSITE_SCAN: {
				auto ids = im_->findNodeIdsByCompositeIndex(config.compositeKeys, config.compositeValues);
				if (ids.size() > 1) {
					std::sort(ids.begin(), ids.end());
					ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
				}
				candidateCount.count = static_cast<int64_t>(ids.size());
				candidateCount.activeOnly = true;
				candidateCount.labelsSatisfied = config.labels.empty();
				candidateCount.available = true;
				break;
			}

			case ScanType::LABEL_SCAN:
				if (config.labels.size() <= 1) {
					auto ids = im_->findNodeIdsByLabel(config.label());
					if (ids.size() > 1) {
						std::sort(ids.begin(), ids.end());
						ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
					}
					candidateCount.count = static_cast<int64_t>(ids.size());
					candidateCount.activeOnly = true;
					candidateCount.labelsSatisfied = true;
					candidateCount.available = true;
				}
				break;

			case ScanType::FULL_SCAN:
			default:
				break;
		}

		if (profileEnabled) {
			debug::PerfTrace::addDuration(
				"node_scan.candidates",
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - candidatesStart)
								 .count()));
		}

		return candidateCount;
	}

} // namespace graph::query::execution
