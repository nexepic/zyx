/**
 * @file PropertyEntitySegmentScanner.hpp
 * @brief Reusable coalesced scanner for serialized Property entity segments.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage::detail {

	constexpr size_t kPropertyScannerMaxCoalescedReadSegments = 16;
	constexpr size_t kPropertyScannerMinParallelReadTasks = 2;
	constexpr size_t kPropertyScannerMinParallelReadSegments =
			kPropertyScannerMaxCoalescedReadSegments * 2;

	using PropertySegmentPartitionInitializer = std::function<void(size_t partitionCount)>;
	using PropertySegmentPartitionMerger = std::function<void(size_t partition)>;
	using PropertySegmentVisitor = std::function<void(size_t partition, const SegmentHeader &, const char *)>;
	using PropertySegmentWorkVisitor =
			std::function<void(size_t partition, size_t workIndex, const SegmentHeader &, const char *)>;

	bool scanAllPropertyEntitySegmentsCore(const DataManager &dm,
										   concurrent::ThreadPool *pool,
										   std::string_view phase,
										   const PropertySegmentPartitionInitializer &initializePartitions,
										   const PropertySegmentVisitor &visitSegment,
										   const PropertySegmentPartitionMerger &mergePartition);

	bool scanPropertyEntitySegmentWorkCore(const DataManager &dm,
										   concurrent::ThreadPool *pool,
										   std::string_view phase,
										   const std::vector<size_t> &workSegmentIndices,
										   const PropertySegmentPartitionInitializer &initializePartitions,
										   const PropertySegmentWorkVisitor &visitWork,
										   const PropertySegmentPartitionMerger &mergePartition);

	template<typename State, typename SegmentVisitor, typename Merger>
	bool scanAllPropertyEntitySegments(const DataManager &dm,
									 concurrent::ThreadPool *pool,
									 std::string_view phase,
									 SegmentVisitor &&visitSegment,
									 Merger &&mergeState) {
		std::vector<State> states;
		auto initializePartitions = [&](size_t partitionCount) {
			states.clear();
			states.resize(partitionCount);
		};
		auto visit = [&](size_t partition, const SegmentHeader &header, const char *data) {
			std::invoke(visitSegment, header, data, states[partition]);
		};
		auto merge = [&](size_t partition) {
			std::invoke(mergeState, partition, states[partition]);
		};
		return scanAllPropertyEntitySegmentsCore(dm, pool, phase, initializePartitions, visit, merge);
	}

	template<typename State, typename Work, typename WorkVisitor, typename Merger>
	bool scanPropertyEntitySegmentWork(const DataManager &dm,
									   concurrent::ThreadPool *pool,
									   std::string_view phase,
									   const std::vector<Work> &work,
									   WorkVisitor &&visitWork,
									   Merger &&mergeState) {
		std::vector<size_t> workSegmentIndices;
		workSegmentIndices.reserve(work.size());
		for (const auto &entry: work) {
			workSegmentIndices.push_back(entry.segmentIndex);
		}

		std::vector<State> states;
		auto initializePartitions = [&](size_t partitionCount) {
			states.clear();
			states.resize(partitionCount);
		};
		auto visit = [&](size_t partition, size_t workIndex, const SegmentHeader &header, const char *data) {
			std::invoke(visitWork, partition, workIndex, work[workIndex], header, data, states[partition]);
		};
		auto merge = [&](size_t partition) {
			std::invoke(mergeState, partition, states[partition]);
		};
		return scanPropertyEntitySegmentWorkCore(
				dm, pool, phase, workSegmentIndices, initializePartitions, visit, merge);
	}

} // namespace graph::storage::detail
