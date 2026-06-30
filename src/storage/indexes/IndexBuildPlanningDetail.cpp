/**
 * @file IndexBuildPlanningDetail.cpp
 * @brief Pure planning helpers shared by index rebuild paths.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "src/storage/indexes/IndexBuildPlanningDetail.hpp"

#include <algorithm>
#include <limits>

namespace graph::query::indexes::index_build_detail {

void appendUnique(std::vector<std::string> &values, const std::string &value) {
	if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(value);
	}
}

void appendMappedPhysicalKeys(
		const std::unordered_map<std::string, std::vector<std::string>> &physicalKeysByProperty,
		const std::string &propertyKey,
		std::vector<std::string> &physicalKeys) {
	auto keyIt = physicalKeysByProperty.find(propertyKey);
	if (keyIt == physicalKeysByProperty.end()) {
		return;
	}
	physicalKeys.insert(physicalKeys.end(), keyIt->second.begin(), keyIt->second.end());
}

bool shouldBuildScopedNodePropertyEntries(int64_t scopedLabelId, const std::string &scopedPropertyKey) {
	return scopedLabelId != 0 && !scopedPropertyKey.empty();
}

bool hasOwnerScanWork(size_t ownerCount, size_t requestedKeyCount) {
	return ownerCount != 0 && requestedKeyCount != 0;
}

bool needsPropertyMapFallbackScan(bool typedScanCoversAllProperties) {
	return !typedScanCoversAllProperties;
}

std::vector<int64_t> sortedUniqueIds(std::vector<int64_t> ids) {
	std::ranges::sort(ids);
	ids.erase(std::ranges::unique(ids).begin(), ids.end());
	return ids;
}

void appendUncheckpointedTailRange(std::vector<std::pair<int64_t, int64_t>> &ranges, int64_t maxAllocatedId) {
	if (maxAllocatedId <= 0) {
		return;
	}

	int64_t maxCoveredId = 0;
	for (const auto &[startId, endId]: ranges) {
		if (endId >= startId) {
			maxCoveredId = (std::max)(maxCoveredId, endId);
		}
	}
	if (maxAllocatedId > maxCoveredId) {
		ranges.emplace_back(maxCoveredId + 1, maxAllocatedId);
	}
}

bool rangesContainId(const std::vector<std::pair<int64_t, int64_t>> &ranges, int64_t id) {
	return std::any_of(ranges.begin(), ranges.end(), [id](const auto &range) {
		return range.first <= id && id <= range.second;
	});
}

void appendUniqueId(std::vector<int64_t> &ids, std::unordered_set<int64_t> &seen, int64_t id) {
	if (id > 0 && seen.insert(id).second) {
		ids.push_back(id);
	}
}

std::vector<ActiveIdRangeTask> buildActiveIdRangeTasks(
		const std::vector<std::pair<int64_t, int64_t>> &ranges,
		size_t targetIdsPerTask) {
	std::vector<ActiveIdRangeTask> tasks;
	targetIdsPerTask = std::max<size_t>(1, targetIdsPerTask);
	for (const auto &[startId, endId]: ranges) {
		if (endId < startId) {
			continue;
		}
		for (int64_t taskStart = startId; taskStart <= endId;) {
			const auto remaining = static_cast<uint64_t>(endId) - static_cast<uint64_t>(taskStart) + 1U;
			const auto taskSize = static_cast<int64_t>(std::min<uint64_t>(remaining, targetIdsPerTask));
			const int64_t taskEnd = taskStart + taskSize - 1;
			tasks.push_back({taskStart, taskEnd});
			if (taskEnd == std::numeric_limits<int64_t>::max()) {
				break;
			}
			taskStart = taskEnd + 1;
		}
	}
	return tasks;
}

} // namespace graph::query::indexes::index_build_detail
