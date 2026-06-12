/**
 * @file PropertyIndexBuildScanner.cpp
 * @brief Typed property-owner scanner for index build pipelines.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/storage/data/PropertyIndexBuildScanner.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

#include "DataManagerPropertyEntityScanDetail.hpp"
#include "graph/concurrent/ParallelScanExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/SegmentReadUtils.hpp"

namespace graph::storage {
namespace {

	struct PropertyIndexBuildScanState {
		std::vector<char> readBuffer;
		std::vector<PropertyEntityOwnerScalarKeyValue> values;
	};

	std::optional<PropertyEntityOwnerScalarKeyValue> readIndexableScalarOwnerValue(
			const char *&cursor,
			const char *end,
			int64_t ownerId,
			const std::string &key) {
		const char *valueStart = cursor;
		PropertyType type = PropertyType::UNKNOWN;
		if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
			return std::nullopt; // ZYX_COV_EXCL_LINE
		}

		PropertyEntityOwnerScalarKeyValue value;
		value.ownerId = ownerId;
		value.key = key;
		value.type = type;

		switch (type) {
			case PropertyType::BOOLEAN:
				if (!readPod(cursor, end, value.boolValue)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return value;
			case PropertyType::INTEGER:
				if (!readPod(cursor, end, value.intValue)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return value;
			case PropertyType::DOUBLE:
				if (!readPod(cursor, end, value.doubleValue)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return value;
			case PropertyType::STRING: {
				SerializedStringView stringValue;
				if (!readStringView(cursor, end, stringValue)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				value.stringValue.assign(stringValue.data, stringValue.size);
				return value;
			}
			default:
				cursor = valueStart;
				if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return std::nullopt;
		}
	}

	void readIndexableOwnerValues(
			const char *buf,
			EntityType ownerType,
			std::span<const std::string> requestedKeys,
			std::span<const int64_t> sortedOwnerIds,
			std::vector<PropertyEntityOwnerScalarKeyValue> &out) {
		if (requestedKeys.empty()) {
			return;
		}

		const char *cursor = buf;
		const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

		auto header = readActivePropertyRecordHeader(cursor, end);
		if (!header.has_value()) {
			return; // ZYX_COV_EXCL_LINE
		}
		if (header->entityType != toUnderlying(ownerType) ||
			!ownerFilterContains(sortedOwnerIds, header->entityId)) {
			return;
		}

		for (uint32_t i = 0; i < header->propertyCount; ++i) {
			SerializedStringView keyView;
			if (!readStringView(cursor, end, keyView)) { // ZYX_COV_EXCL_LINE
				return; // ZYX_COV_EXCL_LINE
			}

			const std::string *matchedKey = nullptr;
			for (const auto &requestedKey: requestedKeys) {
				if (stringViewEquals(keyView, requestedKey)) {
					matchedKey = &requestedKey;
					break;
				}
			}

			if (matchedKey == nullptr) {
				if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return; // ZYX_COV_EXCL_LINE
				}
				continue;
			}

			auto value = readIndexableScalarOwnerValue(cursor, end, header->entityId, *matchedKey);
			if (value.has_value()) {
				out.push_back(std::move(*value));
			}
		}
	}

} // namespace

	PropertyIndexBuildScanner::PropertyIndexBuildScanner(const DataManager &dataManager) :
		dataManager_(dataManager) {}

	std::vector<PropertyEntityOwnerScalarKeyValue> PropertyIndexBuildScanner::collect(
			EntityType ownerType,
			const std::vector<std::string> &keys,
			std::span<const int64_t> sortedOwnerIds,
			concurrent::ThreadPool *pool) const {
		std::vector<PropertyEntityOwnerScalarKeyValue> values;
		if ((ownerType != EntityType::Node && ownerType != EntityType::Edge) ||
			keys.empty() ||
			!dataManager_.canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return values;
		}

		std::vector<std::string> requestedKeys;
		requestedKeys.reserve(keys.size());
		for (const auto &key: keys) {
			if (key.empty() || std::find(requestedKeys.begin(), requestedKeys.end(), key) != requestedKeys.end()) {
				continue;
			}
			requestedKeys.push_back(key);
		}
		if (requestedKeys.empty()) {
			return values;
		}

		const auto segmentIndexManager = dataManager_.getSegmentIndexManager();
		if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE
			return values; // ZYX_COV_EXCL_LINE
		}
		const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
		if (segIndex.empty()) {
			return values;
		}

		std::vector<size_t> segmentIndices;
		segmentIndices.reserve(segIndex.size());
		for (size_t index = 0; index < segIndex.size(); ++index) {
			segmentIndices.push_back(index);
		}

		constexpr size_t entitySize = Property::getTotalSize();
		auto scanSegmentInto = [&](const SegmentHeader &header,
								   const char *dataBuf,
								   std::vector<PropertyEntityOwnerScalarKeyValue> &target) {
			if (header.data_type != Property::typeId || header.used == 0) {
				return;
			}
			for (uint32_t slot = 0; slot < header.used; ++slot) {
				const char *entityBuffer = dataBuf + static_cast<size_t>(slot) * entitySize;
				const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
				if (readSerializedPropertyId(entityBuffer) != expectedId) {
					continue; // ZYX_COV_EXCL_LINE
				}
				readIndexableOwnerValues(
						entityBuffer,
						ownerType,
						std::span<const std::string>(requestedKeys.data(), requestedKeys.size()),
						sortedOwnerIds,
						target);
			}
		};

		const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
		auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
		const size_t segmentCount = totalCoalescedSegments(groups);
		const auto decision = decidePropertyEntityScan(pool, tasks.size(), segmentCount);
		if (decision.useParallel) {
			(void) concurrent::runIndexedPartitions<PropertyIndexBuildScanState>(
					tasks.size(),
					pool,
					{.phase = "property_index_build_scan.parallel",
					 .workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					 .estimatedItems = segmentCount,
					 .estimatedBytes = segmentCount * TOTAL_SEGMENT_SIZE,
					 .minPartitions = kMinParallelPropertyReadTasks,
					 .minItems = kMinParallelPropertyReadSegments},
					[&](size_t taskIndex, PropertyIndexBuildScanState &state) {
						const auto &task = tasks[taskIndex];
						const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
						state.readBuffer.resize(totalBytes);
						const ssize_t n = dataManager_.preadSegments(
								state.readBuffer.data(), task.segCount, task.startOffset);
						if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
							std::vector<char>().swap(state.readBuffer); // ZYX_COV_EXCL_LINE
							return; // ZYX_COV_EXCL_LINE
						}

						for (size_t member = 0; member < task.memberCount; ++member) {
							const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
							SegmentHeader header;
							std::memcpy(&header, state.readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
							scanSegmentInto(
									header,
									state.readBuffer.data() + bufferOffset + sizeof(SegmentHeader),
									state.values);
						}
						std::vector<char>().swap(state.readBuffer);
					},
					[&](size_t, PropertyIndexBuildScanState &state) {
						values.insert(values.end(),
									  std::make_move_iterator(state.values.begin()),
									  std::make_move_iterator(state.values.end()));
					});
			return values;
		}

		auto &readBuffer = propertyEntityScanScratchBuffer(0);
		for (const auto &group: groups) {
			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kMaxCoalescedPropertyReadSegments) {
				const size_t chunkSegments =
						std::min(kMaxCoalescedPropertyReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = dataManager_.preadSegments(readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue; // ZYX_COV_EXCL_LINE
				}
				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					scanSegmentInto(header, readBuffer.data() + bufferOffset + sizeof(SegmentHeader), values);
				}
			}
		}
		return values;
	}

} // namespace graph::storage
