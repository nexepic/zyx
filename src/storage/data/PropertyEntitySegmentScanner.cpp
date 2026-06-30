/**
 * @file PropertyEntitySegmentScanner.cpp
 * @brief Non-template scan scheduler for serialized Property entity segments.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/storage/data/PropertyEntitySegmentScanner.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ParallelScanExecutor.hpp"
#include "graph/core/Property.hpp"
#include "graph/storage/SegmentReadUtils.hpp"

namespace graph::storage::detail {
namespace {
	struct PropertySegmentReadState {
		std::vector<char> readBuffer;
	};

	concurrent::ParallelScanOptions makeScanOptions(std::string_view phase, size_t segmentCount) {
		return {.phase = phase,
				.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
				.estimatedItems = segmentCount,
				.estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
				.minPartitions = kPropertyScannerMinParallelReadTasks,
				.minItems = kPropertyScannerMinParallelReadSegments};
	}

	concurrent::ParallelWorkEstimate makeScanEstimate(size_t partitionCount, size_t segmentCount) {
		return {.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
				.partitions = partitionCount,
				.estimatedItems = segmentCount,
				.estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
				.minPartitions = kPropertyScannerMinParallelReadTasks,
				.minItems = kPropertyScannerMinParallelReadSegments};
	}

	concurrent::ParallelExecutionDecision decideSegmentScan(
			concurrent::ThreadPool *pool,
			size_t partitionCount,
			size_t segmentCount) {
		return concurrent::decideParallelExecution(pool, makeScanEstimate(partitionCount, segmentCount));
	}

	void visitAllTaskMembers(const CoalescedReadTask &task,
					   const char *buffer,
					   size_t partition,
					   const PropertySegmentVisitor &visitSegment) {
		for (size_t member = 0; member < task.memberCount; ++member) {
			const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
			SegmentHeader header;
			std::memcpy(&header, buffer + bufferOffset, sizeof(SegmentHeader));
			if (header.data_type != Property::typeId) {
				continue;
			}
			visitSegment(partition, header, buffer + bufferOffset + sizeof(SegmentHeader));
		}
	}

	void visitTargetedTaskMembers(const std::vector<CoalescedGroup> &groups,
							   const std::vector<size_t> &sourceWorkIndices,
							   const CoalescedReadTask &task,
							   const char *buffer,
							   size_t partition,
							   const PropertySegmentWorkVisitor &visitWork) {
		const auto &group = groups[task.groupIndex];
		for (size_t member = 0; member < task.memberCount; ++member) {
			const size_t filteredIndex = group.memberIndices[task.memberBegin + member];
			const size_t workIndex = sourceWorkIndices[filteredIndex];
			const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
			SegmentHeader header;
			std::memcpy(&header, buffer + bufferOffset, sizeof(SegmentHeader));
			if (header.data_type != Property::typeId || header.used == 0) {
				continue;
			}
			visitWork(partition, workIndex, header, buffer + bufferOffset + sizeof(SegmentHeader));
		}
	}
} // namespace

bool scanAllPropertyEntitySegmentsCore(const DataManager &dm,
								   concurrent::ThreadPool *pool,
								   std::string_view phase,
								   const PropertySegmentPartitionInitializer &initializePartitions,
								   const PropertySegmentVisitor &visitSegment,
								   const PropertySegmentPartitionMerger &mergePartition) {
	if (!dm.hasPreadSupport()) {
		return false;
	}

	const auto &segIndex = dm.getSegmentIndexManager()->getPropertySegmentIndex();
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
	const auto decision = decideSegmentScan(pool, tasks.size(), segmentCount);
	const auto parallelOptions = makeScanOptions(phase, segmentCount);

	if (decision.useParallel) {
		initializePartitions(tasks.size());
		return concurrent::runOperatorIndexedPartitionsWithDecision<PropertySegmentReadState>(
				tasks.size(),
				pool,
				parallelOptions,
				makeScanEstimate(tasks.size(), segmentCount),
				decision,
				[&](size_t taskIndex, PropertySegmentReadState &state) {
					const auto &task = tasks[taskIndex];
					const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
					state.readBuffer.resize(totalBytes);
					const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
					if (n < static_cast<ssize_t>(totalBytes)) {
						std::vector<char>().swap(state.readBuffer);
						return false;
					}
					visitAllTaskMembers(task, state.readBuffer.data(), taskIndex, visitSegment);
					std::vector<char>().swap(state.readBuffer);
					return true;
				},
				[&](size_t partition, PropertySegmentReadState &) {
					mergePartition(partition);
				});
	}

	initializePartitions(1);
	std::vector<char> readBuffer;
	for (const auto &group: groups) {
		for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
			 chunkBegin += kPropertyScannerMaxCoalescedReadSegments) {
			const size_t chunkSegments =
					std::min(kPropertyScannerMaxCoalescedReadSegments, group.memberIndices.size() - chunkBegin);
			const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
			readBuffer.resize(totalBytes);
			const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
			const ssize_t n = dm.preadSegments(readBuffer.data(), chunkSegments, groupOffset);
			if (n < static_cast<ssize_t>(totalBytes)) {
				continue;
			}
			for (size_t member = 0; member < chunkSegments; ++member) {
				const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
				SegmentHeader header;
				std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
				if (header.data_type != Property::typeId) {
					continue;
				}
				visitSegment(0, header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader));
			}
		}
	}
	mergePartition(0);
	return true;
}

bool scanPropertyEntitySegmentWorkCore(const DataManager &dm,
								   concurrent::ThreadPool *pool,
								   std::string_view phase,
								   const std::vector<size_t> &workSegmentIndices,
								   const PropertySegmentPartitionInitializer &initializePartitions,
								   const PropertySegmentWorkVisitor &visitWork,
								   const PropertySegmentPartitionMerger &mergePartition) {
	if (workSegmentIndices.empty() || !dm.hasPreadSupport()) {
		return false;
	}

	const auto &segIndex = dm.getSegmentIndexManager()->getPropertySegmentIndex();
	std::vector<size_t> validSegmentIndices;
	std::vector<size_t> sourceWorkIndices;
	validSegmentIndices.reserve(workSegmentIndices.size());
	sourceWorkIndices.reserve(workSegmentIndices.size());
	for (size_t workIndex = 0; workIndex < workSegmentIndices.size(); ++workIndex) {
		const size_t segmentIndex = workSegmentIndices[workIndex];
		if (segmentIndex >= segIndex.size()) {
			continue;
		}
		validSegmentIndices.push_back(segmentIndex);
		sourceWorkIndices.push_back(workIndex);
	}

	const auto groups = buildCoalescedGroups(validSegmentIndices, segIndex);
	if (groups.empty()) {
		return true;
	}

	const auto tasks = buildCoalescedReadTasks(groups, kPropertyScannerMaxCoalescedReadSegments);
	const size_t segmentCount = totalCoalescedSegments(groups);
	const auto decision = decideSegmentScan(pool, tasks.size(), segmentCount);
	const auto parallelOptions = makeScanOptions(phase, segmentCount);

	if (decision.useParallel) {
		initializePartitions(tasks.size());
		return concurrent::runOperatorIndexedPartitionsWithDecision<PropertySegmentReadState>(
				tasks.size(),
				pool,
				parallelOptions,
				makeScanEstimate(tasks.size(), segmentCount),
				decision,
				[&](size_t taskIndex, PropertySegmentReadState &state) {
					const auto &task = tasks[taskIndex];
					const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
					state.readBuffer.resize(totalBytes);
					const ssize_t n = dm.preadSegments(state.readBuffer.data(), task.segCount, task.startOffset);
					if (n < static_cast<ssize_t>(totalBytes)) {
						std::vector<char>().swap(state.readBuffer);
						return true;
					}
					visitTargetedTaskMembers(
							groups, sourceWorkIndices, task, state.readBuffer.data(), taskIndex, visitWork);
					std::vector<char>().swap(state.readBuffer);
					return true;
				},
				[&](size_t partition, PropertySegmentReadState &) {
					mergePartition(partition);
				});
	}

	initializePartitions(1);
	std::vector<char> readBuffer;
	const auto segmentTracker = dm.getSegmentTracker();
	for (const auto &group: groups) {
		if (group.segCount == 1) {
			const size_t filteredIndex = group.memberIndices.front();
			const size_t workIndex = sourceWorkIndices[filteredIndex];
			const auto &seg = segIndex[validSegmentIndices[filteredIndex]];
			SegmentHeader header = segmentTracker->getSegmentHeaderCopy(seg.segmentOffset);
			if (header.data_type != Property::typeId || header.used == 0) {
				continue;
			}

			const size_t dataBytes = static_cast<size_t>(header.used) * Property::getTotalSize();
			readBuffer.resize(dataBytes);
			const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
			const ssize_t n = dm.preadBytes(readBuffer.data(), dataBytes, dataOffset);
			if (n < static_cast<ssize_t>(dataBytes)) {
				continue;
			}
			visitWork(0, workIndex, header, readBuffer.data());
			continue;
		}

		for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
			 chunkBegin += kPropertyScannerMaxCoalescedReadSegments) {
			const size_t chunkSegments =
					std::min(kPropertyScannerMaxCoalescedReadSegments, group.memberIndices.size() - chunkBegin);
			const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
			readBuffer.resize(totalBytes);
			const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
			const ssize_t n = dm.preadSegments(readBuffer.data(), chunkSegments, groupOffset);
			if (n < static_cast<ssize_t>(totalBytes)) {
				continue;
			}

			for (size_t member = 0; member < chunkSegments; ++member) {
				const size_t filteredIndex = group.memberIndices[chunkBegin + member];
				const size_t workIndex = sourceWorkIndices[filteredIndex];
				const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
				SegmentHeader header;
				std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
				if (header.data_type != Property::typeId || header.used == 0) {
					continue;
				}
				visitWork(0, workIndex, header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader));
			}
		}
	}
	mergePartition(0);
	return true;
}

} // namespace graph::storage::detail
