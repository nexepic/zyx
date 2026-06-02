#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

#include "DataManagerPropertyEntityScanDetail.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/SegmentReadUtils.hpp"

namespace graph::storage {

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

		// Parallel path: coalesce consecutive segments into single large I/O calls
		if (pool && !pool->isSingleThreaded() && work.size() > 1) {
			// Build segment index list from work items
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work)
				workSegIndices.push_back(w.segIdx);

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<std::pair<int64_t, Property>>> perSeg(work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				// Single pread for the entire coalesced group
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes))
					return;

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
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
						if (slot >= header.used)
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
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes))
					continue;

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used)
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

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<std::pair<int64_t, std::unordered_map<std::string, PropertyValue>>>> perWork(
					work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) {
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &local = perWork[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) {
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
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
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

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
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
						if (readSerializedPropertyId(entityBuffer) != id) {
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
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
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
				if (slot >= header.used) {
					continue;
				}
				const char *entityBuffer = dataBuf + slot * entitySize;
				if (readSerializedPropertyId(entityBuffer) != id) {
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
		std::vector<char> readBuffer;
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t wi = group.memberIndices.front();
				const auto &w = work[wi];
				const auto &seg = segIndex[w.segmentIndex];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = preadBytes(readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
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
					if (header.used == 0) {
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
				if (slot >= header.used) {
					continue;
				}
				const char *entityBuffer = dataBuf + slot * entitySize;
				if (readSerializedPropertyId(entityBuffer) != id) {
					continue;
				}
				const auto [refBegin, refEnd] = refRanges[i];
				auto visitCount = visitSelectedPropertyScalarValue(entityBuffer, key, refs, refBegin, refEnd, visitor);
				if (visitCount.has_value()) {
					visited += *visitCount;
				}
			}
		};

		const auto workSegIndices = collectPropertyEntityWorkSegmentIndices(work);
		auto groups = buildCoalescedGroups(workSegIndices, segIndex);
		std::vector<char> readBuffer;
		for (const auto &group: groups) {
			if (group.segCount == 1) {
				const size_t wi = group.memberIndices.front();
				const auto &w = work[wi];
				const auto &seg = segIndex[w.segmentIndex];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				readBuffer.resize(dataBytes);
				const auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				const ssize_t n = preadBytes(readBuffer.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
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
					if (header.used == 0) {
						continue;
					}
					const char *dataBuf = readBuffer.data() + bufferOffset + sizeof(SegmentHeader);
					scanPropertyWork(w, header, dataBuf);
				}
			}
		}
		return visited;
	}

	PropertyEntityPredicateMatchResult DataManager::bulkMatchPropertyEntityPredicates(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::unordered_map<std::string, PropertyValue> &expected, concurrent::ThreadPool *pool,
			PropertyEntityPredicateMatchOptions options) const {
		PropertyEntityPredicateMatchResult result;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || expected.empty() || !hasPreadSupport()) {
			return result;
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
			return result;
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
			return result;
		}

		auto appendPredicateResult = [&](std::vector<size_t> &loadedRows, std::vector<size_t> *matchedRows,
										 size_t &loadedCount, size_t &matchedCount, size_t idIndex,
										 std::optional<bool> matches) {
			if (!matches.has_value()) {
				return;
			}
			const auto [refBegin, refEnd] = refRanges[idIndex];
			for (size_t ref = refBegin; ref < refEnd; ++ref) {
				++loadedCount;
				if (options.collectLoadedRows) {
					loadedRows.push_back(refs[ref].row);
				}
				if (matches.value()) {
					++matchedCount;
					if (matchedRows != nullptr) {
						matchedRows->push_back(refs[ref].row);
					}
				}
			}
		};

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());
			std::vector<std::vector<size_t>> perWorkMatchedRows(options.collectMatchedRows ? work.size() : 0);
			std::vector<size_t> perWorkLoadedCounts(work.size(), 0);
			std::vector<size_t> perWorkMatchedCounts(work.size(), 0);

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) {
							continue;
						}
						const char *entityBuffer = dataBuf + slot * entitySize;
						if (readSerializedPropertyId(entityBuffer) != id) {
							continue;
						}
						appendPredicateResult(
								perWorkLoadedRows[wi], options.collectMatchedRows ? &perWorkMatchedRows[wi] : nullptr,
								perWorkLoadedCounts[wi], perWorkMatchedCounts[wi], i,
								useSinglePredicate
										? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
										: readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations));
					}
				}
			});

			size_t loadedCount = 0;
			for (size_t i = 0; i < work.size(); ++i) {
				loadedCount += perWorkLoadedCounts[i];
				result.matchedCount += perWorkMatchedCounts[i];
			}
			result.loadedCount = loadedCount;
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(loadedCount);
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(result.matchedCount);
			}
			for (size_t i = 0; i < work.size(); ++i) {
				if (options.collectLoadedRows) {
					result.loadedRows.insert(result.loadedRows.end(), perWorkLoadedRows[i].begin(),
											 perWorkLoadedRows[i].end());
				}
				if (options.collectMatchedRows) {
					result.matchedRows.insert(result.matchedRows.end(), perWorkMatchedRows[i].begin(),
											  perWorkMatchedRows[i].end());
				}
			}
		} else {
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(refs.size());
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(refs.size());
			}
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					appendPredicateResult(
							result.loadedRows, options.collectMatchedRows ? &result.matchedRows : nullptr,
							result.loadedCount, result.matchedCount, i,
							useSinglePredicate ? readPropertyEntitySinglePredicateMatch(entityBuffer, singlePredicate)
											   : readPropertyEntityPredicateMatch(entityBuffer, predicateExpectations));
				}
			}
		}

		return result;
	}

	size_t
	DataManager::bulkCountPropertyEntityPredicates(const std::vector<int64_t> &ids,
												   const std::unordered_map<std::string, PropertyValue> &expected,
												   concurrent::ThreadPool *pool) const {
		if (expected.empty()) {
			return 0;
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
		if (predicates.empty()) {
			return 0;
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);
		return countPropertyEntityMatches(*this, ids, pool, [&](const char *entityBuffer) {
			return readPropertyEntityPredicateMatch(entityBuffer, predicateGroups, predicateExpectations.size());
		});
	}


	PropertyEntityPredicateMatchResult DataManager::bulkMatchPropertyEntityPredicateSpecs(
			const std::vector<int64_t> &ids, const std::vector<size_t> &rows, size_t rowCount,
			const std::vector<PropertyEntityPredicate> &predicates, concurrent::ThreadPool *pool,
			PropertyEntityPredicateMatchOptions options) const {
		PropertyEntityPredicateMatchResult result;
		if (ids.empty() || rows.size() != ids.size() || rowCount == 0 || predicates.empty() || !hasPreadSupport()) {
			return result;
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
			return result;
		}

		std::vector<PredicateSpecExpectation> predicateExpectations;
		predicateExpectations.reserve(predicates.size());
		for (const auto &predicate: predicates) {
			predicateExpectations.push_back({&predicate.key, &predicate.value,
											 predicate.upperValue.has_value() ? &*predicate.upperValue : nullptr,
											 predicate.op});
		}
		const auto predicateGroups = groupPredicateSpecExpectations(predicateExpectations);

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
			return result;
		}

		auto appendPredicateResult = [&](std::vector<size_t> &loadedRows, std::vector<size_t> *matchedRows,
										 size_t &loadedCount, size_t &matchedCount, size_t idIndex,
										 std::optional<bool> matches) {
			if (!matches.has_value()) {
				return;
			}
			const auto [refBegin, refEnd] = refRanges[idIndex];
			for (size_t ref = refBegin; ref < refEnd; ++ref) {
				++loadedCount;
				if (options.collectLoadedRows) {
					loadedRows.push_back(refs[ref].row);
				}
				if (matches.value()) {
					++matchedCount;
					if (matchedRows != nullptr) {
						matchedRows->push_back(refs[ref].row);
					}
				}
			}
		};

		if (pool && !pool->isSingleThreaded() && work.size() > 1) { // ZYX_COV_EXCL_LINE
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w: work) {
				workSegIndices.push_back(w.segIdx);
			}

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<size_t>> perWorkLoadedRows(work.size());
			std::vector<std::vector<size_t>> perWorkMatchedRows(options.collectMatchedRows ? work.size() : 0);
			std::vector<size_t> perWorkLoadedCounts(work.size(), 0);
			std::vector<size_t> perWorkMatchedCounts(work.size(), 0);

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				ssize_t n = preadSegments(groupBuf.data(), group.segCount, group.startOffset);
				if (n < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return;
				}

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0) {
						continue;
					}

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used) {
							continue;
						}
						const char *entityBuffer = dataBuf + slot * entitySize;
						if (readSerializedPropertyId(entityBuffer) != id) {
							continue;
						}
						appendPredicateResult(perWorkLoadedRows[wi],
											  options.collectMatchedRows ? &perWorkMatchedRows[wi] : nullptr,
											  perWorkLoadedCounts[wi], perWorkMatchedCounts[wi], i,
											  readPropertyEntityPredicateMatch(entityBuffer, predicateGroups,
																			   predicateExpectations.size()));
					}
				}
			});

			size_t loadedCount = 0;
			for (size_t i = 0; i < work.size(); ++i) {
				loadedCount += perWorkLoadedCounts[i];
				result.matchedCount += perWorkMatchedCounts[i];
			}
			result.loadedCount = loadedCount;
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(loadedCount);
			}
			if (options.collectMatchedRows) {
				result.matchedRows.reserve(result.matchedCount);
			}
			for (size_t i = 0; i < work.size(); ++i) {
				if (options.collectLoadedRows) {
					result.loadedRows.insert(result.loadedRows.end(), perWorkLoadedRows[i].begin(),
											 perWorkLoadedRows[i].end());
				}
				if (options.collectMatchedRows) {
					result.matchedRows.insert(result.matchedRows.end(), perWorkMatchedRows[i].begin(),
											  perWorkMatchedRows[i].end());
				}
			}
		} else {
			if (options.collectLoadedRows) {
				result.loadedRows.reserve(refs.size());
			}
			for (const auto &w: work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0) {
					continue;
				}

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes)) {
					continue;
				}

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used) {
						continue;
					}
					const char *entityBuffer = buf.data() + slot * entitySize;
					if (readSerializedPropertyId(entityBuffer) != id) {
						continue;
					}
					appendPredicateResult(result.loadedRows, options.collectMatchedRows ? &result.matchedRows : nullptr,
										  result.loadedCount, result.matchedCount, i,
										  readPropertyEntityPredicateMatch(entityBuffer, predicateGroups,
																		   predicateExpectations.size()));
				}
			}
		}

		return result;
	}

	// --- Edge Operations (delegate to EdgeManager) ---

} // namespace graph::storage
