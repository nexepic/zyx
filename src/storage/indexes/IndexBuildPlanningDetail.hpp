/**
 * @file IndexBuildPlanningDetail.hpp
 * @brief Pure planning helpers shared by index rebuild paths.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace graph::query::indexes::index_build_detail {

	struct ActiveIdRangeTask {
		int64_t startId = 0;
		int64_t endId = 0;
	};

	void appendUnique(std::vector<std::string> &values, const std::string &value);
	void appendMappedPhysicalKeys(
			const std::unordered_map<std::string, std::vector<std::string>> &physicalKeysByProperty,
			const std::string &propertyKey,
			std::vector<std::string> &physicalKeys);
	bool shouldBuildScopedNodePropertyEntries(int64_t scopedLabelId, const std::string &scopedPropertyKey);
	bool hasOwnerScanWork(size_t ownerCount, size_t requestedKeyCount);
	bool needsPropertyMapFallbackScan(bool typedScanCoversAllProperties);
	std::vector<int64_t> sortedUniqueIds(std::vector<int64_t> ids);
	void appendUncheckpointedTailRange(std::vector<std::pair<int64_t, int64_t>> &ranges, int64_t maxAllocatedId);
	bool rangesContainId(const std::vector<std::pair<int64_t, int64_t>> &ranges, int64_t id);
	void appendUniqueId(std::vector<int64_t> &ids, std::unordered_set<int64_t> &seen, int64_t id);
	std::vector<ActiveIdRangeTask> buildActiveIdRangeTasks(
			const std::vector<std::pair<int64_t, int64_t>> &ranges,
			size_t targetIdsPerTask = 4096);

} // namespace graph::query::indexes::index_build_detail
