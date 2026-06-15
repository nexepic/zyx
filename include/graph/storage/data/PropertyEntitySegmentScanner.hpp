/**
 * @file PropertyEntitySegmentScanner.hpp
 * @brief Reusable coalesced scanner for serialized Property entity segments.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ParallelScanExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Property.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage::detail {

	constexpr size_t kPropertyScannerMaxCoalescedReadSegments = 16;
	constexpr size_t kPropertyScannerMinParallelReadTasks = 2;
	constexpr size_t kPropertyScannerMinParallelReadSegments =
			kPropertyScannerMaxCoalescedReadSegments * 2;

	template<typename State, typename SegmentVisitor, typename Merger>
	bool scanAllPropertyEntitySegments(const DataManager &dm,
									 concurrent::ThreadPool *pool,
									 std::string_view phase,
									 SegmentVisitor &&visitSegment,
									 Merger &&mergeState) {
		if (!dm.hasPreadSupport()) {
			return false;
		}
		const auto segmentIndexManager = dm.getSegmentIndexManager();
		if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
			return false; // ZYX_COV_EXCL_LINE
		}
		const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
		if (segIndex.empty()) {
			return true;
		}

		std::vector<size_t> segmentIndices;
		segmentIndices.reserve(segIndex.size());
		for (size_t index = 0; index < segIndex.size(); ++index) {
			segmentIndices.push_back(index);
		}

		const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
		auto tasks = buildCoalescedReadTasks(groups, kPropertyScannerMaxCoalescedReadSegments);
		const size_t segmentCount = totalCoalescedSegments(groups);
		const auto decision = concurrent::decideParallelExecution(
				pool,
				{.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
				 .partitions = tasks.size(),
				 .estimatedItems = segmentCount,
				 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
				 .minPartitions = kPropertyScannerMinParallelReadTasks,
				 .minItems = kPropertyScannerMinParallelReadSegments});
		const concurrent::ParallelScanOptions parallelOptions{
				.phase = phase,
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
				.estimatedItems = segmentCount,
				.estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
				.minPartitions = kPropertyScannerMinParallelReadTasks,
				.minItems = kPropertyScannerMinParallelReadSegments};

		if (decision.useParallel) {
			return concurrent::runIndexedPartitions<State>(
					tasks.size(),
					pool,
					parallelOptions,
					[&](size_t taskIndex, State &state) {
						const auto &task = tasks[taskIndex];
						const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
						state.readBuffer.resize(totalBytes);
						const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
						if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
							std::vector<char>().swap(state.readBuffer); // ZYX_COV_EXCL_LINE
							return false; // ZYX_COV_EXCL_LINE
						}

						for (size_t member = 0; member < task.memberCount; ++member) {
							const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
							SegmentHeader header;
							std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
							std::invoke(visitSegment,
										header,
										state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
										state);
						}
						std::vector<char>().swap(state.readBuffer);
						return true;
					},
					[&](size_t partition, State &state) {
						std::invoke(mergeState, partition, state);
					});
		}

		State state;
		for (const auto &group: groups) {
			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kPropertyScannerMaxCoalescedReadSegments) {
				const size_t chunkSegments =
						std::min(kPropertyScannerMaxCoalescedReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				state.readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = dm.preadSegments(state.readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue; // ZYX_COV_EXCL_LINE
				}
				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					std::invoke(visitSegment,
								header,
								state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
								state);
				}
			}
		}
		std::invoke(mergeState, size_t{0}, state);
		return true;
	}

	template<typename State, typename Work, typename WorkVisitor, typename Merger>
	bool scanPropertyEntitySegmentWork(const DataManager &dm,
									   concurrent::ThreadPool *pool,
									   std::string_view phase,
									   const std::vector<Work> &work,
									   WorkVisitor &&visitWork,
									   Merger &&mergeState) {
		if (work.empty() || !dm.hasPreadSupport()) {
			return false;
		}
		const auto segmentIndexManager = dm.getSegmentIndexManager();
		const auto segmentTracker = dm.getSegmentTracker();
		if (!segmentIndexManager || !segmentTracker) { // ZYX_COV_EXCL_LINE
			return false; // ZYX_COV_EXCL_LINE
		}

		const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
		std::vector<size_t> workSegmentIndices;
		workSegmentIndices.reserve(work.size());
		for (const auto &entry: work) {
			workSegmentIndices.push_back(entry.segmentIndex);
		}

		const auto groups = buildCoalescedGroups(workSegmentIndices, segIndex);
		if (groups.empty()) {
			return true;
		}
		const auto tasks = buildCoalescedReadTasks(groups, kPropertyScannerMaxCoalescedReadSegments);
		const size_t segmentCount = totalCoalescedSegments(groups);
		const auto decision = concurrent::decideParallelExecution(
				pool,
				{.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
				 .partitions = tasks.size(),
				 .estimatedItems = segmentCount,
				 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
				 .minPartitions = kPropertyScannerMinParallelReadTasks,
				 .minItems = kPropertyScannerMinParallelReadSegments});
		const concurrent::ParallelScanOptions parallelOptions{
				.phase = phase,
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
				.estimatedItems = segmentCount,
				.estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
				.minPartitions = kPropertyScannerMinParallelReadTasks,
				.minItems = kPropertyScannerMinParallelReadSegments};

		if (decision.useParallel) {
			return concurrent::runIndexedPartitions<State>(
					tasks.size(),
					pool,
					parallelOptions,
					[&](size_t taskIndex, State &state) {
						const auto &task = tasks[taskIndex];
						const auto &group = groups[task.groupIndex];
						const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
						state.readBuffer.resize(totalBytes);
						const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
						if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
							std::vector<char>().swap(state.readBuffer); // ZYX_COV_EXCL_LINE
							return true; // ZYX_COV_EXCL_LINE
						}

						for (size_t member = 0; member < task.memberCount; ++member) {
							const size_t workIndex = group.memberIndices[task.memberBegin + member];
							const auto &workItem = work[workIndex];
							const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
							SegmentHeader header;
							std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
							if (header.used == 0) {
								continue;
							}
							std::invoke(visitWork,
										taskIndex,
										workIndex,
										workItem,
										header,
										state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
										state);
						}
						std::vector<char>().swap(state.readBuffer);
						return true;
					},
					[&](size_t partition, State &state) {
						std::invoke(mergeState, partition, state);
					});
		}

		State state;
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t workIndex = group.memberIndices.front();
				const auto &workItem = work[workIndex];
				const auto &seg = segIndex[workItem.segmentIndex];
				SegmentHeader header = segmentTracker->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * Property::getTotalSize();
				state.readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = dm.preadBytes(state.readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE
					continue; // ZYX_COV_EXCL_LINE
				}
				std::invoke(visitWork, size_t{0}, workIndex, workItem, header, state.readBuffer.data(), state);
				continue;
			}

			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kPropertyScannerMaxCoalescedReadSegments) {
				const size_t chunkSegments =
						std::min(kPropertyScannerMaxCoalescedReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				state.readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = dm.preadSegments(state.readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue; // ZYX_COV_EXCL_LINE
				}

				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t workIndex = group.memberIndices[chunkBegin + member];
					const auto &workItem = work[workIndex];
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}
					std::invoke(visitWork,
								size_t{0},
								workIndex,
								workItem,
								header,
								state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
								state);
				}
			}
		}
		std::invoke(mergeState, size_t{0}, state);
		return true;
	}

} // namespace graph::storage::detail
