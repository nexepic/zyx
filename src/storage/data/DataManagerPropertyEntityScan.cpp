#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <vector>

#include "DataManagerPropertyEntityScanDetail.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentReadUtils.hpp"

namespace graph::storage {
namespace {

	constexpr size_t kSerializedBlobIdOffset = 0;
	constexpr size_t kSerializedBlobEntityTypeOffset = sizeof(int64_t) * 4 + sizeof(uint32_t);
	constexpr size_t kSerializedBlobActiveOffset =
			sizeof(int64_t) * 4 + sizeof(uint32_t) * 3 + sizeof(int32_t) + sizeof(bool);

	bool hasActiveBlobPropertiesForOwnerType(const DataManager &dm, EntityType ownerType) {
		const auto segmentIndexManager = dm.getSegmentIndexManager();
		if (!segmentIndexManager || !dm.hasPreadSupport()) { // ZYX_COV_EXCL_LINE: production DataManager instances expose both for persisted scans.
			return true;
		}

		const auto &segIndex = segmentIndexManager->getBlobSegmentIndex();
		if (segIndex.empty()) {
			return false;
		}

		std::vector<size_t> segmentIndices;
		segmentIndices.reserve(segIndex.size());
		for (size_t index = 0; index < segIndex.size(); ++index) {
			segmentIndices.push_back(index);
		}

		const auto groups = buildCoalescedGroups(segmentIndices, segIndex);
		const auto ownerTypeId = toUnderlying(ownerType);
		constexpr size_t entitySize = Blob::getTotalSize();
		std::vector<char> readBuffer;

		for (const auto &group: groups) {
			const size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
			readBuffer.resize(totalBytes);
			const ssize_t n = dm.preadSegments(readBuffer.data(), group.segCount, group.startOffset);
			if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE: OS/file-corruption defensive path.
				return true;
			}

			for (size_t member = 0; member < group.memberIndices.size(); ++member) {
				const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
				SegmentHeader header{};
				std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
				if (header.data_type != Blob::typeId || header.used == 0) { // ZYX_COV_EXCL_LINE: blob index entries point at blob segments.
					continue;
				}

				const char *data = readBuffer.data() + bufferOffset + sizeof(SegmentHeader);
				for (uint32_t slot = 0; slot < header.used; ++slot) {
					const char *serializedBlob = data + static_cast<size_t>(slot) * entitySize;
					int64_t blobId = 0;
					uint32_t entityType = 0;
					bool active = false;
					std::memcpy(&blobId, serializedBlob + kSerializedBlobIdOffset, sizeof(blobId));
					std::memcpy(&entityType, serializedBlob + kSerializedBlobEntityTypeOffset, sizeof(entityType));
					std::memcpy(&active, serializedBlob + kSerializedBlobActiveOffset, sizeof(active));
					const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
					if (active && blobId == expectedId && entityType == ownerTypeId) { // ZYX_COV_EXCL_LINE: non-matching blob owners are covered by fallback paths.
						return true;
					}
				}
			}
		}
		return false;
	}

} // namespace

	std::unordered_map<int64_t, Property> DataManager::bulkLoadPropertyEntities(const std::vector<int64_t> &ids,
																				concurrent::ThreadPool *pool) const {
		std::unordered_map<int64_t, Property> result;
		if (ids.empty() || !hasPreadSupport())
			return result;

		// Sort IDs and group by segment for sequential I/O
		std::vector<int64_t> sortedIds(ids);
		if (!std::is_sorted(sortedIds.begin(), sortedIds.end())) {
			std::sort(sortedIds.begin(), sortedIds.end());
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		// Find relevant segments and their ID ranges
		struct SegWork {
			size_t segIdx;
			size_t idBegin, idEnd; // indices into sortedIds
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty())
			return result;

		std::vector<CoalescedGroup> groups;
		std::vector<CoalescedReadTask> tasks;
		if (concurrent::hasParallelWorkers(pool) && work.size() > 1) {
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work)
				workSegIndices.push_back(w.segIdx);

			groups = buildCoalescedGroups(workSegIndices, segIndex);
			tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
		}

		// Parallel path: split coalesced segment groups into bounded read tasks.
		const auto decision = decidePropertyEntityScan(pool, tasks.size(), totalCoalescedSegments(groups));
		if (decision.useParallel) {
			std::vector<std::vector<std::pair<int64_t, Property>>> perSeg(work.size());

			pool->parallelFor(0, tasks.size(), decision.workerCount, [&](size_t taskIndex) {
				const auto &task = tasks[taskIndex];
				const auto &group = groups[task.groupIndex];
				size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), task.segCount, task.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					return;

				for (size_t mi = 0; mi < task.memberCount; ++mi) {
					size_t wi = group.memberIndices[task.memberBegin + mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0)
						continue;

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &local = perSeg[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
							continue;
						Property prop = Property::deserializeFromBuffer(dataBuf + slot * entitySize);
						if (prop.isActive())
							local.emplace_back(id, std::move(prop));
					}
				}
			});

			// Merge thread-local results
			size_t totalCount = 0;
			for (const auto &v: perSeg)
				totalCount += v.size();
			result.reserve(totalCount);
			for (auto &v: perSeg) {
				for (auto &[id, prop]: v)
					result[id] = std::move(prop);
			}
		} else {
			// Sequential path
			result.reserve(ids.size());
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0)
					continue;

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				auto &buf = propertyEntityScanScratchBuffer(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					continue;

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
						continue;
					Property prop = Property::deserializeFromBuffer(buf.data() + slot * entitySize);
					if (prop.isActive())
						result[id] = std::move(prop);
				}
			}
		}

		return result;
	}

	std::unordered_map<int64_t, std::unordered_map<std::string, PropertyValue>>
	DataManager::bulkLoadPropertyEntityValues(const std::vector<int64_t> &ids, const std::vector<std::string> &keys,
											  concurrent::ThreadPool *pool) const {
		std::unordered_map<int64_t, std::unordered_map<std::string, PropertyValue>> result;
		if (ids.empty() || keys.empty() || !hasPreadSupport()) {
			return result;
		}

		std::vector<int64_t> sortedIds(ids);
		std::sort(sortedIds.begin(), sortedIds.end());
		sortedIds.erase(std::unique(sortedIds.begin(), sortedIds.end()), sortedIds.end());

		std::unordered_set<std::string> requestedKeys;
		requestedKeys.reserve(keys.size());
		for (const auto &key: keys) {
			requestedKeys.insert(key);
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		struct SegWork {
			size_t segIdx;
			size_t idBegin;
			size_t idEnd;
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty()) {
			return result;
		}

		auto readPropertyValues =
				[&](const char *entityBuffer) -> std::optional<std::unordered_map<std::string, PropertyValue>> {
			return readSelectedPropertyValues(entityBuffer, requestedKeys);
		};

		std::vector<CoalescedGroup> groups;
		std::vector<CoalescedReadTask> tasks;
		if (concurrent::hasParallelWorkers(pool) && work.size() > 1) {
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			groups = buildCoalescedGroups(workSegIndices, segIndex);
			tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
		}

		const auto decision = decidePropertyEntityScan(pool, tasks.size(), totalCoalescedSegments(groups));
		if (decision.useParallel) { // ZYX_COV_EXCL_LINE
			std::vector<std::vector<std::pair<int64_t, std::unordered_map<std::string, PropertyValue>>>> perWork(
					work.size());

			pool->parallelFor(0, tasks.size(), decision.workerCount, [&](size_t taskIndex) {
				const auto &task = tasks[taskIndex];
				const auto &group = groups[task.groupIndex];
				size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), task.segCount, task.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					return;
				}

				for (size_t mi = 0; mi < task.memberCount; ++mi) {
					size_t wi = group.memberIndices[task.memberBegin + mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
						if (header.used == 0) { // ZYX_COV_EXCL_LINE: coalesced group members are allocated non-empty property segments.
							continue;
						}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &local = perWork[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) { // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
							continue;
						}
						auto values = readPropertyValues(dataBuf + slot * entitySize);
						if (values.has_value()) {
							local.emplace_back(id, std::move(*values));
						}
					}
				}
			});

			size_t totalCount = 0;
			for (const auto &values: perWork) {
				totalCount += values.size();
			}
			result.reserve(totalCount);
			for (auto &values: perWork) {
				for (auto &[id, propertyValues]: values) {
					result.emplace(id, std::move(propertyValues));
				}
			}
		} else {
			result.reserve(sortedIds.size());
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				auto &buf = propertyEntityScanScratchBuffer(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) { // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
						continue;
					}
					auto values = readPropertyValues(buf.data() + slot * entitySize);
					if (values.has_value()) {
						result.emplace(id, std::move(*values));
					}
				}
			}
		}

		return result;
	}

	std::vector<size_t> DataManager::bulkLoadPropertyEntityColumns(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::vector<std::string> &keys,
			std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> &columns,
			concurrent::ThreadPool *pool) const {
		std::vector<size_t> loadedRows;
		if (ids.empty() || rows.size() != ids.size() || keys.empty() || rowCount == 0 || !hasPreadSupport()) {
			return loadedRows;
		}

		std::unordered_map<std::string, size_t> requestedKeyIndices;
		std::vector<std::vector<std::optional<PropertyValue>> *> columnTargets;
		requestedKeyIndices.reserve(keys.size());
		columnTargets.reserve(keys.size());
		for (const auto &key: keys) {
			if (requestedKeyIndices.contains(key)) {
				continue;
			}
			auto columnIt = columns.find(key);
			if (columnIt == columns.end() || columnIt->second.size() < rowCount) {
				continue;
			}
			const size_t index = columnTargets.size();
			requestedKeyIndices.emplace(key, index);
			columnTargets.push_back(&columnIt->second);
		}
		if (columnTargets.empty()) {
			return loadedRows;
		}
		if (canUseDirectOrderedRows(ids, rows, rowCount)) {
			loadedRows.reserve(ids.size());
			(void) visitPropertyEntityRowsDirect(*this, ids, rows, [&](size_t row, const char *entityBuffer) {
				if (readSelectedPropertyColumnsOne(entityBuffer, requestedKeyIndices, columnTargets, row)) {
					loadedRows.push_back(row);
					return size_t{1};
				}
				return size_t{0};
			});
			return loadedRows;
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return loadedRows;
		}

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		struct SegWork {
			size_t segIdx;
			size_t idBegin;
			size_t idEnd;
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back(
						{s, static_cast<size_t>(lo - sortedIds.begin()), static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty()) {
			return loadedRows;
		}

		std::vector<CoalescedGroup> groups;
		std::vector<CoalescedReadTask> tasks;
		if (concurrent::hasParallelWorkers(pool) && work.size() > 1) {
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			groups = buildCoalescedGroups(workSegIndices, segIndex);
			tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
		}

		const auto decision = decidePropertyEntityScan(pool, tasks.size(), totalCoalescedSegments(groups));
		if (decision.useParallel) { // ZYX_COV_EXCL_LINE
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());

			pool->parallelFor(0, tasks.size(), decision.workerCount, [&](size_t taskIndex) {
				const auto &task = tasks[taskIndex];
				const auto &group = groups[task.groupIndex];
				size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), task.segCount, task.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < task.memberCount; ++mi) {
					size_t wi = group.memberIndices[task.memberBegin + mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &localLoadedRows = perWorkLoadedRows[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) { // ZYX_COV_EXCL_LINE
							continue;
						}
						const char *entityBuffer = dataBuf + slot * entitySize;
							if (readSerializedPropertyId(entityBuffer) != id) { // ZYX_COV_EXCL_LINE: mismatched ids indicate corrupt persisted segments.
								continue;
							}
						const auto [refBegin, refEnd] = refRanges[i];
						if (readSelectedPropertyColumns(entityBuffer, requestedKeyIndices, columnTargets, refs,
														refBegin, refEnd)) {
							for (size_t ref = refBegin; ref < refEnd; ++ref) {
								localLoadedRows.push_back(refs[ref].row);
							}
						}
					}
				}
			});

			size_t loadedCount = 0;
			for (const auto &rowsForWork: perWorkLoadedRows) {
				loadedCount += rowsForWork.size();
			}
			loadedRows.reserve(loadedCount);
			for (auto &rowsForWork: perWorkLoadedRows) {
				loadedRows.insert(loadedRows.end(), rowsForWork.begin(), rowsForWork.end());
			}
		} else {
			loadedRows.reserve(refs.size());
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) { // ZYX_COV_EXCL_LINE: work is derived from allocated non-empty property segments.
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				auto &buf = propertyEntityScanScratchBuffer(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) { // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) { // ZYX_COV_EXCL_LINE: mismatched ids indicate corrupt persisted segments.
						continue;
					}
					const auto [refBegin, refEnd] = refRanges[i];
					if (readSelectedPropertyColumns(entityBuffer, requestedKeyIndices, columnTargets, refs, refBegin,
													refEnd)) {
						for (size_t ref = refBegin; ref < refEnd; ++ref) {
							loadedRows.push_back(refs[ref].row);
						}
					}
				}
			}
		}

		return loadedRows;
	}

	size_t DataManager::bulkVisitPropertyEntityValues(const std::vector<int64_t> &ids, const std::vector<size_t> &rows,
													  size_t rowCount, const std::string &key,
													  const PropertyEntityValueVisitor &visitor,
													  concurrent::ThreadPool *pool) const {
		(void) pool;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || key.empty() || !visitor ||
			!hasPreadSupport()) {
			return 0;
		}
		if (canUseDirectOrderedRows(ids, rows, rowCount)) {
			return visitPropertyEntityRowsDirect(*this, ids, rows, [&](size_t row, const char *entityBuffer) {
				auto visitCount = visitSelectedPropertyValueOne(entityBuffer, key, row, visitor);
				return visitCount.value_or(size_t{0});
			});
		}

		std::vector<PropertyEntityRowRef> refs;
		refs.reserve(ids.size());
		std::vector<uint8_t> rowSeen(rowCount, 0);
		for (size_t i = 0; i < ids.size(); ++i) {
			if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
				rowSeen[rows[i]] = 1;
				refs.push_back({ids[i], rows[i]});
			}
		}
		if (refs.empty()) {
			return 0;
		}

		auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
			if (lhs.id != rhs.id) {
				return lhs.id < rhs.id;
			}
			return lhs.row < rhs.row;
		};
		if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
			std::sort(refs.begin(), refs.end(), refLess);
		}

		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		sortedIds.reserve(refs.size());
		refRanges.reserve(refs.size());
		for (size_t begin = 0; begin < refs.size();) {
			size_t end = begin + 1;
			while (end < refs.size() && refs[end].id == refs[begin].id) {
				++end;
			}
			sortedIds.push_back(refs[begin].id);
			refRanges.emplace_back(begin, end);
			begin = end;
		}

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();
		const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
		if (work.empty()) {
			return 0;
		}

		size_t visited = 0;
		auto scanPropertyWork = [&](const PropertyEntitySegmentWork &w, const SegmentHeader &header,
									const char *dataBuf) {
			for (size_t i = w.idBegin; i < w.idEnd; ++i) {
				const int64_t id = sortedIds[i];
				const auto slot = static_cast<uint32_t>(id - header.start_id);
				if (slot >= header.used) { // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
					continue;
				}
				const char *entityBuffer = dataBuf + slot * entitySize;
				if (readSerializedPropertyId(entityBuffer) != id) { // ZYX_COV_EXCL_LINE: mismatched ids indicate corrupt persisted segments.
					continue;
				}
				const auto [refBegin, refEnd] = refRanges[i];
				auto visitCount = visitSelectedPropertyValue(entityBuffer, key, refs, refBegin, refEnd, visitor);
				if (visitCount.has_value()) {
					visited += *visitCount;
				}
			}
		};

		const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
		auto groups = buildCoalescedGroups(workSegIndices, segIndex);
		auto &readBuffer = propertyEntityScanScratchBuffer(0);
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t wi = group.memberIndices.front();
				const auto &w = work[wi];
				const auto &seg = segIndex[w.segmentIndex];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) { // ZYX_COV_EXCL_LINE: work is derived from allocated non-empty property segments.
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = preadBytes(readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					continue;
				}
				scanPropertyWork(w, header, readBuffer.data());
				continue;
			}

			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kMaxCoalescedPropertyReadSegments) {
				const size_t chunkSegments =
						std::min(kMaxCoalescedPropertyReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = preadSegments(readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue;
				}

				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t wi = group.memberIndices[chunkBegin + member];
					const auto &w = work[wi];
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					if (header.used == 0) { // ZYX_COV_EXCL_LINE: coalesced group members are allocated non-empty property segments.
						continue;
					}
					const char *dataBuf = readBuffer.data() + bufferOffset + sizeof(SegmentHeader);
					scanPropertyWork(w, header, dataBuf);
				}
			}
		}
		return visited;
	}

	size_t DataManager::bulkVisitPropertyEntityScalarValues(const std::vector<int64_t> &ids,
															const std::vector<size_t> &rows, size_t rowCount,
															const std::string &key,
															const PropertyEntityScalarValueVisitor &visitor,
															concurrent::ThreadPool *pool) const {
		(void) pool;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || key.empty() || !visitor ||
			!hasPreadSupport()) {
			return 0;
		}
		return bulkVisitPropertyEntityScalarValuesPartitioned(
				ids, rows, rowCount, key, {}, [&](size_t, size_t row, const PropertyEntityScalarValue &value) {
					visitor(row, value);
				}, nullptr);
	}

	size_t DataManager::bulkVisitPropertyEntityScalarValuesPartitioned(
			const std::vector<int64_t> &ids,
			const std::vector<size_t> &rows, size_t rowCount,
			const std::string &key,
			const PropertyEntityScalarPartitionInitializer &initializer,
			const PropertyEntityScalarPartitionVisitor &visitor,
			concurrent::ThreadPool *pool) const {
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || key.empty() || !visitor ||
			!hasPreadSupport()) {
			return 0;
		}

		const bool directRows = canUseDirectOrderedRows(ids, rows, rowCount);
		if (directRows && !concurrent::hasParallelWorkers(pool)) {
			if (initializer) {
				initializer(1);
			}
			return visitPropertyEntityRowsDirect(*this, ids, rows, [&](size_t row, const char *entityBuffer) {
				auto visitCount = visitSelectedPropertyScalarValueOne(
						entityBuffer, key, row, [&](size_t visitedRow, const PropertyEntityScalarValue &value) {
							visitor(0, visitedRow, value);
						});
				return visitCount.value_or(size_t{0});
			});
		}

		std::vector<PropertyEntityRowRef> refs;
		std::vector<int64_t> sortedIds;
		std::vector<std::pair<size_t, size_t>> refRanges;
		if (directRows) {
			refs.reserve(ids.size());
			sortedIds.reserve(ids.size());
			refRanges.reserve(ids.size());
			for (size_t i = 0; i < ids.size(); ++i) {
				refs.push_back({ids[i], rows[i]});
				sortedIds.push_back(ids[i]);
				refRanges.emplace_back(i, i + 1);
			}
		} else {
			refs.reserve(ids.size());
			std::vector<uint8_t> rowSeen(rowCount, 0);
			for (size_t i = 0; i < ids.size(); ++i) {
				if (ids[i] != 0 && rows[i] < rowCount && rowSeen[rows[i]] == 0) {
					rowSeen[rows[i]] = 1;
					refs.push_back({ids[i], rows[i]});
				}
			}
			if (refs.empty()) {
				return 0;
			}

			auto refLess = [](const PropertyEntityRowRef &lhs, const PropertyEntityRowRef &rhs) {
				if (lhs.id != rhs.id) {
					return lhs.id < rhs.id;
				}
				return lhs.row < rhs.row;
			};
			if (!std::is_sorted(refs.begin(), refs.end(), refLess)) {
				std::sort(refs.begin(), refs.end(), refLess);
			}

			sortedIds.reserve(refs.size());
			refRanges.reserve(refs.size());
			for (size_t begin = 0; begin < refs.size();) {
				size_t end = begin + 1;
				while (end < refs.size() && refs[end].id == refs[begin].id) {
					++end;
				}
				sortedIds.push_back(refs[begin].id);
				refRanges.emplace_back(begin, end);
				begin = end;
			}
		}
		if (refs.empty()) {
			return 0;
		}

		const auto segmentIndexManager = getSegmentIndexManager();
		const auto segmentTracker = getSegmentTracker();
		if (!segmentIndexManager || !segmentTracker) { // ZYX_COV_EXCL_LINE
			return 0; // ZYX_COV_EXCL_LINE
		}
		const auto &segIndex = segmentIndexManager->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();
		const auto work = collectPropertyEntitySegmentWork(sortedIds, segIndex);
		if (work.empty()) {
			return 0;
		}

		auto scanPropertyWork = [&](size_t partition, const PropertyEntitySegmentWork &w,
									const SegmentHeader &header, const char *dataBuf, size_t &visited) {
			for (size_t i = w.idBegin; i < w.idEnd; ++i) {
				const int64_t id = sortedIds[i];
				const auto slot = static_cast<uint32_t>(id - header.start_id);
				if (slot >= header.used) { // ZYX_COV_EXCL_LINE: segment index bounds persisted ids to used slots.
					continue;
				}
				const char *entityBuffer = dataBuf + slot * entitySize;
				if (readSerializedPropertyId(entityBuffer) != id) { // ZYX_COV_EXCL_LINE: mismatched ids indicate corrupt persisted segments.
					continue;
				}
				const auto [refBegin, refEnd] = refRanges[i];
				auto visitCount = visitSelectedPropertyScalarValue(
						entityBuffer, key, refs, refBegin, refEnd,
						[&](size_t row, const PropertyEntityScalarValue &value) {
							visitor(partition, row, value);
						});
				if (visitCount.has_value()) {
					visited += *visitCount;
				}
			}
		};

		const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
		auto groups = buildCoalescedGroups(workSegIndices, segIndex);
		auto tasks = buildCoalescedReadTasks(groups, kMaxCoalescedPropertyReadSegments);
		const auto decision = decidePropertyEntityScan(pool, tasks.size(), totalCoalescedSegments(groups));
		if (decision.useParallel) { // ZYX_COV_EXCL_LINE
			if (initializer) {
				initializer(tasks.size());
			}
			std::vector<size_t> perTaskVisited(tasks.size(), 0);
			pool->parallelFor(0, tasks.size(), decision.workerCount, [&](size_t taskIndex) {
				const auto &task = tasks[taskIndex];
				const auto &group = groups[task.groupIndex];
				const size_t totalBytes = task.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				const ssize_t n = preadSegments(groupBuf.data(), task.segCount, task.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < task.memberCount; ++mi) {
					const size_t wi = group.memberIndices[task.memberBegin + mi];
					const auto &w = work[wi];
					const size_t bufferOffset = mi * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufferOffset, sizeof(SegmentHeader));
					if (header.used == 0) { // ZYX_COV_EXCL_LINE: coalesced group members are allocated non-empty property segments.
						continue;
					}
					const char *dataBuf = groupBuf.data() + bufferOffset + sizeof(SegmentHeader);
					scanPropertyWork(taskIndex, w, header, dataBuf, perTaskVisited[taskIndex]);
				}
			});

			size_t visited = 0;
			for (const auto count: perTaskVisited) {
				visited += count;
			}
			return visited;
		}

		if (initializer) {
			initializer(1);
		}
		size_t visited = 0;
		auto &readBuffer = propertyEntityScanScratchBuffer(0);
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t wi = group.memberIndices.front();
				const auto &w = work[wi];
				const auto &seg = segIndex[w.segmentIndex];
				SegmentHeader header = segmentTracker->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) { // ZYX_COV_EXCL_LINE: work is derived from allocated non-empty property segments.
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = preadBytes(readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread is an OS/file-corruption defensive path.
					continue;
				}
				scanPropertyWork(0, w, header, readBuffer.data(), visited);
				continue;
			}

			for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
				 chunkBegin += kMaxCoalescedPropertyReadSegments) {
				const size_t chunkSegments =
						std::min(kMaxCoalescedPropertyReadSegments, group.memberIndices.size() - chunkBegin);
				const size_t totalBytes = chunkSegments * TOTAL_SEGMENT_SIZE;
				readBuffer.resize(totalBytes);
				const uint64_t groupOffset = group.startOffset + chunkBegin * TOTAL_SEGMENT_SIZE;
				const ssize_t n = preadSegments(readBuffer.data(), chunkSegments, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					continue;
				}

				for (size_t member = 0; member < chunkSegments; ++member) {
					const size_t wi = group.memberIndices[chunkBegin + member];
					const auto &w = work[wi];
					const size_t bufferOffset = member * TOTAL_SEGMENT_SIZE;
					SegmentHeader header;
					std::memcpy(&header, readBuffer.data() + bufferOffset, sizeof(SegmentHeader));
					if (header.used == 0) { // ZYX_COV_EXCL_LINE: coalesced group members are allocated non-empty property segments.
						continue;
					}
					const char *dataBuf = readBuffer.data() + bufferOffset + sizeof(SegmentHeader);
					scanPropertyWork(0, w, header, dataBuf, visited);
				}
			}
		}
		return visited;
	}

	PropertyEntityPredicateMatchResult DataManager::bulkMatchPropertyEntityPredicates(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::unordered_map<std::string, PropertyValue> &expected, concurrent::ThreadPool *pool,
			PropertyEntityPredicateMatchOptions options) const {
		if (expected.empty()) {
			return {};
		}

		std::vector<PredicateExpectation> predicateExpectations;
		predicateExpectations.reserve(expected.size());
		for (const auto &[key, value]: expected) {
			predicateExpectations.push_back({&key, compilePropertyValue(value)});
		}
		const bool useSinglePredicate = predicateExpectations.size() == 1;
		const SinglePredicateExpectation singlePredicate =
				useSinglePredicate ? SinglePredicateExpectation{predicateExpectations.front().key,
													predicateExpectations.front().value}
							   : SinglePredicateExpectation{};

		return matchPropertyEntityRows(*this, ids, rows, rowCount, pool, options, [&](const char *entityBuffer) {
			return useSinglePredicate ? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
								  : readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations);
		});
	}

	size_t
	DataManager::bulkCountPropertyEntityPredicates(const std::vector<int64_t> &ids,
												   const std::unordered_map<std::string, PropertyValue> &expected,
												   concurrent::ThreadPool *pool) const {
		return bulkCountPropertyEntityPredicateMatches(ids, expected, pool).matchedCount;
	}

	PropertyEntityPredicateCountResult DataManager::bulkCountPropertyEntityPredicateMatches(
			const std::vector<int64_t> &ids,
			const std::unordered_map<std::string, PropertyValue> &expected,
			concurrent::ThreadPool *pool) const {
		if (expected.empty()) {
			return {};
		}

		std::vector<PredicateExpectation> predicateExpectations;
		predicateExpectations.reserve(expected.size());
		for (const auto &[key, value]: expected) {
			predicateExpectations.push_back({&key, compilePropertyValue(value)});
		}
		const bool useSinglePredicate = predicateExpectations.size() == 1;
		const SinglePredicateExpectation singlePredicate =
				useSinglePredicate ? SinglePredicateExpectation{predicateExpectations.front().key,
																predicateExpectations.front().value}
								   : SinglePredicateExpectation{};
		return countPropertyEntityMatches(*this, ids, pool, [&](const char *entityBuffer) {
			return useSinglePredicate ? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
									  : readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations);
		});
	}

	size_t DataManager::bulkCountPropertyEntityPredicateSpecs(const std::vector<int64_t> &ids,
															  const std::vector<PropertyEntityPredicate> &predicates,
															  concurrent::ThreadPool *pool) const {
		return bulkCountPropertyEntityPredicateSpecMatches(ids, predicates, pool).matchedCount;
	}

	PropertyEntityPredicateCountResult DataManager::bulkCountPropertyEntityPredicateSpecMatches(
			const std::vector<int64_t> &ids,
			const std::vector<PropertyEntityPredicate> &predicates,
			concurrent::ThreadPool *pool) const {
		if (predicates.empty()) {
			return {};
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		if (predicateExpectations.size() == 1) {
			return countPropertyEntityMatches(*this, ids, pool, [&](const char *entityBuffer) {
				return readPropertyEntitySinglePredicateSpecMatch(entityBuffer, predicateExpectations.front());
			});
		}

		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);
		return countPropertyEntityMatches(*this, ids, pool, [&](const char *entityBuffer) {
			return readPropertyEntityPredicateMatch(entityBuffer, predicateGroups, predicateExpectations.size());
		});
	}

	bool DataManager::canCountPropertyEntityPredicatesByOwnerType(EntityType ownerType) const {
		if ((ownerType != EntityType::Node && ownerType != EntityType::Edge) ||
			!hasPreadSupport() || hasUnsavedChanges() || !segmentIndexManager_ || !segmentTracker_) {
			return false;
		}

		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr &&
			(!snapshot->properties.empty() || !snapshot->blobs.empty() ||
			 (ownerType == EntityType::Node ? !snapshot->nodes.empty() : !snapshot->edges.empty()))) {
			return false;
		}

		const auto &ownerSegments = ownerType == EntityType::Node
										   ? segmentIndexManager_->getNodeSegmentIndex()
										   : segmentIndexManager_->getEdgeSegmentIndex();
		const auto ownerTypeId = toUnderlying(ownerType);
		for (const auto &entry: ownerSegments) {
			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) { // ZYX_COV_EXCL_LINE: segment offsets originate from a validated segment index.
				return false;
			}
			if (header.data_type != ownerTypeId || header.inactive_count != 0) { // ZYX_COV_EXCL_LINE: type mismatch indicates corrupt segment metadata.
				return false;
			}
		}
		return true;
	}

	PropertyEntityPredicateCountResult DataManager::bulkCountPropertyEntityPredicateSpecsByOwnerType(
			EntityType ownerType,
			const std::vector<PropertyEntityPredicate> &predicates,
			concurrent::ThreadPool *pool) const {
		if (predicates.empty() || !canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return {};
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		if (predicateExpectations.size() == 1) {
			return countPropertyEntityMatchesByOwnerType(*this, ownerType, pool, [&](const char *entityBuffer) {
				return readPropertyEntitySinglePredicateSpecMatch(entityBuffer, predicateExpectations.front());
			});
		}

		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);
		return countPropertyEntityMatchesByOwnerType(*this, ownerType, pool, [&](const char *entityBuffer) {
			return readPropertyEntityPredicateMatch(entityBuffer, predicateGroups, predicateExpectations.size());
		});
	}

	bool DataManager::canCountAllPropertyPredicatesByOwnerType(EntityType ownerType) const {
		return canCountPropertyEntityPredicatesByOwnerType(ownerType) &&
			   !hasActiveBlobPropertiesForOwnerType(*this, ownerType);
	}

	PropertyEntityPredicateCountResult DataManager::bulkCountAllPropertyPredicateSpecsByOwnerType(
			EntityType ownerType,
			const std::vector<PropertyEntityPredicate> &predicates,
			concurrent::ThreadPool *pool) const {
		if (predicates.empty() || !canCountAllPropertyPredicatesByOwnerType(ownerType)) {
			return {};
		}
		return bulkCountPropertyEntityPredicateSpecsByOwnerType(ownerType, predicates, pool);
	}

	std::vector<PropertyEntityOwnerValue> DataManager::bulkCollectPropertyValuesByOwnerType(
			EntityType ownerType,
			const std::string &key,
			concurrent::ThreadPool *pool) const {
		if ((ownerType != EntityType::Node && ownerType != EntityType::Edge) ||
			!canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return {};
		}
		return collectPropertyValuesByOwnerType(*this, ownerType, key, {}, pool);
	}

	std::vector<PropertyEntityOwnerKeyValue> DataManager::bulkCollectPropertyValuesByOwnerType(
			EntityType ownerType,
			const std::vector<std::string> &keys,
			concurrent::ThreadPool *pool) const {
		if ((ownerType != EntityType::Node && ownerType != EntityType::Edge) ||
			keys.empty() ||
			!canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return {};
		}
		return collectPropertyValuesByOwnerType(*this, ownerType, keys, {}, pool);
	}

	std::vector<PropertyEntityOwnerValue> DataManager::bulkCollectPropertyValuesByOwnerType(
			EntityType ownerType,
			const std::string &key,
			const std::vector<int64_t> &ownerIds,
			concurrent::ThreadPool *pool) const {
		if ((ownerType != EntityType::Node && ownerType != EntityType::Edge) ||
			ownerIds.empty() ||
			!canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return {};
		}

		std::vector<int64_t> sortedOwnerIds(ownerIds);
		if (!std::is_sorted(sortedOwnerIds.begin(), sortedOwnerIds.end())) {
			std::sort(sortedOwnerIds.begin(), sortedOwnerIds.end());
		}
		sortedOwnerIds.erase(std::unique(sortedOwnerIds.begin(), sortedOwnerIds.end()), sortedOwnerIds.end());
		return collectPropertyValuesByOwnerType(
				*this,
				ownerType,
				key,
				std::span<const int64_t>(sortedOwnerIds.data(), sortedOwnerIds.size()),
				pool);
	}

	std::vector<int64_t> DataManager::bulkCollectPropertyPredicateOwnerIdsByOwnerType(
			EntityType ownerType,
			const std::vector<PropertyEntityPredicate> &predicates,
			concurrent::ThreadPool *pool) const {
		return bulkCollectPropertyPredicateOwnerIdsByOwnerType(ownerType, predicates, {}, pool);
	}

	std::vector<int64_t> DataManager::bulkCollectPropertyPredicateOwnerIdsByOwnerType(
			EntityType ownerType,
			const std::vector<PropertyEntityPredicate> &predicates,
			const PropertyEntityOwnerPredicateScanOptions &options,
			concurrent::ThreadPool *pool) const {
		if ((ownerType != EntityType::Node && ownerType != EntityType::Edge) ||
			predicates.empty() ||
			!canCountPropertyEntityPredicatesByOwnerType(ownerType)) {
			return {};
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		if (predicateExpectations.size() == 1) {
			return collectPropertyPredicateOwnerIdsByOwnerType(
					*this, ownerType, options, pool, [&](const char *entityBuffer) {
						return readPropertyEntitySinglePredicateSpecMatch(entityBuffer, predicateExpectations.front());
					});
		}

		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);
		return collectPropertyPredicateOwnerIdsByOwnerType(
				*this, ownerType, options, pool, [&](const char *entityBuffer) {
					return readPropertyEntityPredicateMatch(
							entityBuffer, predicateGroups, predicateExpectations.size());
				});
	}

	std::vector<int64_t> DataManager::bulkCollectAllPropertyPredicateOwnerIdsByOwnerType(
			EntityType ownerType,
			const std::vector<PropertyEntityPredicate> &predicates,
			const PropertyEntityOwnerPredicateScanOptions &options,
			concurrent::ThreadPool *pool) const {
		if (predicates.empty() || !canCountAllPropertyPredicatesByOwnerType(ownerType)) {
			return {};
		}
		return bulkCollectPropertyPredicateOwnerIdsByOwnerType(ownerType, predicates, options, pool);
	}


	PropertyEntityPredicateMatchResult DataManager::bulkMatchPropertyEntityPredicateSpecs(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::vector<PropertyEntityPredicate> &predicates, concurrent::ThreadPool *pool,
			PropertyEntityPredicateMatchOptions options) const {
		if (predicates.empty()) {
			return {};
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		const bool useSinglePredicate = predicateExpectations.size() == 1;
		const auto predicateGroups =
				useSinglePredicate ? std::vector<PredicateSpecGroup>{}
							   : groupPredicateSpecExpectations(predicateExpectations);

		return matchPropertyEntityRows(*this, ids, rows, rowCount, pool, options, [&](const char *entityBuffer) {
			return useSinglePredicate
					   ? readPropertyEntitySinglePredicateSpecMatch(entityBuffer, predicateExpectations.front())
					   : readPropertyEntityPredicateMatch(entityBuffer, predicateGroups, predicateExpectations.size());
		});
	}


	// --- Edge Operations (delegate to EdgeManager) ---

} // namespace graph::storage
