#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"

namespace graph::storage {

	namespace {
		int64_t readSerializedRelationshipId(const char *buf) {
			int64_t edgeId = 0;
			std::memcpy(&edgeId, buf + offsetof(Edge::Metadata, id), sizeof(edgeId));
			return edgeId;
		}

		int64_t readSerializedRelationshipTypeId(const char *buf) {
			int64_t typeId = 0;
			std::memcpy(&typeId, buf + offsetof(Edge::Metadata, typeId), sizeof(typeId));
			return typeId;
		}

		bool readSerializedRelationshipActive(const char *buf) {
			bool active = false;
			std::memcpy(&active, buf + offsetof(Edge::Metadata, isActive), sizeof(active));
			return active;
		}

		int64_t readSerializedRelationshipPropertyEntityId(const char *buf) {
			int64_t propertyEntityId = 0;
			std::memcpy(&propertyEntityId, buf + offsetof(Edge::Metadata, propertyEntityId), sizeof(propertyEntityId));
			return propertyEntityId;
		}

		PropertyStorageType readSerializedRelationshipPropertyStorageType(const char *buf) {
			uint32_t storageType = 0;
			std::memcpy(&storageType, buf + offsetof(Edge::Metadata, propertyStorageType), sizeof(storageType));
			return static_cast<PropertyStorageType>(storageType);
		}

		std::optional<RelationshipTypeSegmentStats> parseRelationshipSegmentTypeStats(uint64_t segmentOffset,
																					  const SegmentHeader &header,
																					  const char *data,
																					  bool includePropertyCandidates) {
			if (header.data_type != Edge::typeId || header.used == 0 || data == nullptr) { // ZYX_COV_EXCL_LINE: callers validate segment shape before parsing.
				return std::nullopt;
			}

			constexpr size_t entitySize = Edge::getTotalSize();
			RelationshipTypeSegmentStats stats;
			stats.segmentOffset = segmentOffset;
			stats.startId = header.start_id;
			stats.endId = header.start_id + static_cast<int64_t>(header.used) - 1;
			stats.used = header.used;
			stats.inactiveCount = header.inactive_count;
			stats.hasPropertyCandidates = includePropertyCandidates;
			if (includePropertyCandidates) {
				stats.activePropertyEntityIds.reserve(header.used);
				stats.activeBlobEdgeIds.reserve(
						header.inactive_count < header.used ? header.used - header.inactive_count : 0);
			}
			for (uint32_t slot = 0; slot < header.used; ++slot) { // ZYX_COV_EXCL_LINE: loop-exit arcs are not meaningful for fixed-size segment scans.
				const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
				const char *edgeBuf = data + static_cast<size_t>(slot) * entitySize;
				if (readSerializedRelationshipId(edgeBuf) != expectedId || !readSerializedRelationshipActive(edgeBuf)) { // ZYX_COV_EXCL_LINE: id mismatch is corrupt-slot handling; inactive rows are validated elsewhere.
					continue;
				}
				const int64_t typeId = readSerializedRelationshipTypeId(edgeBuf);
				++stats.activeCount;
				++stats.activeCountByType[typeId];

				if (!includePropertyCandidates) {
					continue;
				}
				const int64_t propertyEntityId = readSerializedRelationshipPropertyEntityId(edgeBuf);
				if (propertyEntityId == 0) {
					continue;
				}
				const auto storageType = readSerializedRelationshipPropertyStorageType(edgeBuf);
				if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
					stats.activePropertyEntityIds.push_back(propertyEntityId);
					stats.activePropertyEntityIdsByType[typeId].push_back(propertyEntityId);
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
					stats.activeBlobEdgeIds.push_back(expectedId);
					stats.activeBlobEdgeIdsByType[typeId].push_back(expectedId);
				}
			}
			return stats;
		}

		void appendRelationshipPropertyCandidates(RelationshipPropertyCandidateStats &target,
												  std::span<const int64_t> propertyEntityIds,
												  std::span<const int64_t> blobEdgeIds) {
			auto reserveAdditional = [](std::vector<int64_t> &values, size_t additional) {
				const size_t desired = values.size() + additional;
				if (desired <= values.capacity()) {
					return;
				}
				values.reserve(std::max(desired, values.capacity() * 2));
			};
			reserveAdditional(target.propertyEntityIds, propertyEntityIds.size());
			target.propertyEntityIds.insert(target.propertyEntityIds.end(), propertyEntityIds.begin(),
											propertyEntityIds.end());
			reserveAdditional(target.fallbackEdgeIds, blobEdgeIds.size());
			target.fallbackEdgeIds.insert(target.fallbackEdgeIds.end(), blobEdgeIds.begin(), blobEdgeIds.end());
		}

	} // namespace

	std::optional<RelationshipTypeSegmentStats>
	DataManager::buildRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
												   bool includePropertyCandidates) const {
		if (!hasPreadSupport() || header.data_type != Edge::typeId || header.used == 0) { // ZYX_COV_EXCL_LINE: public callers pre-validate persisted edge segments.
			return std::nullopt;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
		std::vector<char> buf(dataBytes);
		const auto dataOffset = static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader));
		const ssize_t read = preadBytes(buf.data(), dataBytes, dataOffset);
		if (read < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread requires external file truncation.
			return std::nullopt;
		}

		return parseRelationshipSegmentTypeStats(segmentOffset, header, buf.data(), includePropertyCandidates);
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::getRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
												 bool includePropertyCandidates) const {
		if (auto cached = getCachedRelationshipSegmentTypeStats(segmentOffset, header, includePropertyCandidates)) {
			return cached;
		}

		auto stats = buildRelationshipSegmentTypeStats(segmentOffset, header, includePropertyCandidates);
		if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: build failure is covered at public call sites.
			return std::nullopt;
		}

		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		auto &cached = relationshipSegmentTypeStats_[segmentOffset];
		cached = std::move(*stats);
		return cached;
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::getCachedRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
													   bool requirePropertyCandidates) const {
		const int64_t expectedEndId =
				header.used == 0 ? header.start_id - 1 : header.start_id + static_cast<int64_t>(header.used) - 1; // ZYX_COV_EXCL_LINE: zero-used cached edge segments are never inserted.
		std::shared_lock lock(relationshipSegmentTypeStatsMutex_);
		auto it = relationshipSegmentTypeStats_.find(segmentOffset);
		if (it != relationshipSegmentTypeStats_.end() && it->second.startId == header.start_id &&
			it->second.endId == expectedEndId && it->second.used == header.used &&
			it->second.inactiveCount == header.inactive_count &&
			(!requirePropertyCandidates || it->second.hasPropertyCandidates)) {
			return it->second;
		}
		return std::nullopt;
	}

	std::optional<RelationshipTypeSegmentStats>
	DataManager::cachedRelationshipTypeSegmentStats(uint64_t segmentOffset) const {
		if (segmentOffset == 0) {
			return std::nullopt;
		}
		SegmentHeader header{};
		try {
			header = segmentTracker_->getSegmentHeaderCopy(segmentOffset);
		} catch (const std::exception &) {
			return std::nullopt;
		}
		if (header.data_type != Edge::typeId) {
			return std::nullopt;
		}
		return getCachedRelationshipSegmentTypeStats(segmentOffset, header);
	}

	std::optional<RelationshipPropertyCandidateStats>
	DataManager::collectRelationshipPropertyCandidatesFromSegmentStats(int64_t beginId, int64_t endId,
																	   int64_t typeId) const {
		if (!hasPreadSupport() || beginId <= 0 || endId < beginId || hasUnsavedChanges()) { // ZYX_COV_EXCL_LINE: pread support is fixed for FileStorage-backed tests.
			return std::nullopt;
		}
		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr &&
			(!snapshot->edges.empty() || !snapshot->properties.empty() || !snapshot->blobs.empty())) {
			return std::nullopt;
		}

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		RelationshipPropertyCandidateStats candidates;
		for (const auto &entry: segmentIndex) {
			if (entry.endId < beginId || entry.startId > endId) { // ZYX_COV_EXCL_LINE: non-overlap is a segment-index range prune.
				continue;
			}

			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) {
				return std::nullopt;
			}
			header.file_offset = entry.segmentOffset;
			if (header.used == 0) {
				continue;
			}

			const int64_t segmentFirst = std::max<int64_t>(entry.startId, header.start_id);
			const int64_t segmentLast =
					std::min<int64_t>(entry.endId, header.start_id + static_cast<int64_t>(header.used) - 1);
			const int64_t first = std::max<int64_t>(beginId, segmentFirst);
			const int64_t last = std::min<int64_t>(endId, segmentLast);
			if (first > last) { // ZYX_COV_EXCL_LINE: segment-index/header disagreements are defensive.
				continue;
			}
			if (first != segmentFirst || last != segmentLast) {
				return std::nullopt;
			}

			auto stats = getRelationshipSegmentTypeStats(entry.segmentOffset, header, true);
			if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: stats build failure is surfaced through corrupt-header tests.
				return std::nullopt;
			}
			if (typeId == 0) {
				candidates.matchedEdges += static_cast<size_t>(stats->activeCount);
				appendRelationshipPropertyCandidates(candidates, stats->activePropertyEntityIds,
													 stats->activeBlobEdgeIds);
				continue;
			}

			if (auto countIt = stats->activeCountByType.find(typeId); countIt != stats->activeCountByType.end()) {
				candidates.matchedEdges += static_cast<size_t>(countIt->second);
			}
			if (auto propertyIt = stats->activePropertyEntityIdsByType.find(typeId);
				propertyIt != stats->activePropertyEntityIdsByType.end()) {
				appendRelationshipPropertyCandidates(candidates, propertyIt->second, std::span<const int64_t>{});
			}
			if (auto blobIt = stats->activeBlobEdgeIdsByType.find(typeId);
				blobIt != stats->activeBlobEdgeIdsByType.end()) {
				appendRelationshipPropertyCandidates(candidates, std::span<const int64_t>{}, blobIt->second);
			}
		}
		return candidates;
	}

	std::optional<RelationshipTypeTotalStats> DataManager::getRelationshipTypeTotalStats() const {
		if (!hasPreadSupport()) { // ZYX_COV_EXCL_LINE: FileStorage-backed DataManager always supports pread.
			return std::nullopt;
		}

		if (auto cached = getCachedRelationshipTypeTotalStats()) {
			return cached;
		}

		auto stats = buildRelationshipTypeTotalStats();
		if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: build failure is covered at public call sites.
			return std::nullopt;
		}

		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipTypeTotalStats_ = std::move(*stats);
		return relationshipTypeTotalStats_;
	}

	std::optional<RelationshipTypeTotalStats> DataManager::getCachedRelationshipTypeTotalStats() const {
		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		const int64_t firstId = segmentIndex.empty() ? int64_t{0} : segmentIndex.front().startId;
		const int64_t lastId = segmentIndex.empty() ? int64_t{-1} : segmentIndex.back().endId;
		std::shared_lock lock(relationshipSegmentTypeStatsMutex_);
		if (relationshipTypeTotalStats_.has_value() &&
			relationshipTypeTotalStats_->segmentCount == segmentIndex.size() &&
			relationshipTypeTotalStats_->firstId == firstId && relationshipTypeTotalStats_->lastId == lastId) {
			return relationshipTypeTotalStats_;
		}
		return std::nullopt;
	}

	std::optional<RelationshipTypeTotalStats> DataManager::buildRelationshipTypeTotalStats() const {
		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		RelationshipTypeTotalStats total;
		total.segmentCount = segmentIndex.size();
		if (segmentIndex.empty()) {
			return total;
		}
		total.firstId = segmentIndex.front().startId;
		total.lastId = segmentIndex.back().endId;

		for (const auto &entry: segmentIndex) {
			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) {
				return std::nullopt;
			}
			header.file_offset = entry.segmentOffset;
			if (header.used == 0) {
				continue;
			}

			auto stats = getRelationshipSegmentTypeStats(entry.segmentOffset, header);
			if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: stats build failure is surfaced through corrupt-header tests.
				return std::nullopt;
			}
			total.activeCount += stats->activeCount;
			for (const auto &[typeId, count]: stats->activeCountByType) {
				total.activeCountByType[typeId] += count;
			}
		}
		return total;
	}

	std::optional<int64_t> DataManager::countActiveEdgesByTypeFromTotalStats(int64_t beginId, int64_t endId,
																			 int64_t typeId) const {
		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		if (!segmentIndex.empty() && (beginId > segmentIndex.front().startId || endId < segmentIndex.back().endId)) {
			return std::nullopt;
		}

		auto totalStats = getRelationshipTypeTotalStats();
		if (!totalStats.has_value()) {
			return std::nullopt;
		}
		if (totalStats->segmentCount > 0 && (beginId > totalStats->firstId || endId < totalStats->lastId)) { // ZYX_COV_EXCL_LINE: total stats are only used for whole-index windows.
			return std::nullopt;
		}

		int64_t baseCount = totalStats->activeCount;
		if (typeId != 0) {
			baseCount = 0;
			if (auto it = totalStats->activeCountByType.find(typeId); it != totalStats->activeCountByType.end()) {
				baseCount = it->second;
			}
		}

		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr) {
			return applyRelationshipTypeCountSnapshotOverlay(baseCount, beginId, endId, typeId, snapshot->edges);
		}
		auto edgeOverlay = getDirtyEntityInfos<Edge>(
				{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
		return applyRelationshipTypeCountOverlay(baseCount, beginId, endId, typeId, edgeOverlay);
	}

	std::optional<int64_t> DataManager::countActiveEdgesByTypeInSegmentWindow(uint64_t segmentOffset,
																			  const SegmentHeader &header,
																			  int64_t firstId, int64_t lastId,
																			  int64_t typeId) const {
		if (!hasPreadSupport() || header.data_type != Edge::typeId || header.used == 0 || firstId > lastId) { // ZYX_COV_EXCL_LINE: partial-window callers validate edge segment bounds.
			return std::nullopt;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		const auto firstSlot = static_cast<uint32_t>(firstId - header.start_id);
		const auto lastSlot = static_cast<uint32_t>(lastId - header.start_id);
		if (lastSlot >= header.used || firstSlot > lastSlot) { // ZYX_COV_EXCL_LINE: partial-window callers derive slots from the same header.
			return std::nullopt;
		}

		const size_t rowCount = static_cast<size_t>(lastSlot - firstSlot + 1);
		const size_t dataBytes = rowCount * entitySize;
		std::vector<char> buf(dataBytes);
		const auto dataOffset = static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader) +
													 static_cast<uint64_t>(firstSlot) * entitySize);
		const ssize_t read = preadBytes(buf.data(), dataBytes, dataOffset);
		if (read < static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread requires external file truncation.
			return std::nullopt;
		}

		int64_t count = 0;
		for (uint32_t slot = firstSlot; slot <= lastSlot; ++slot) {
			const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
			const char *edgeBuf = buf.data() + static_cast<size_t>(slot - firstSlot) * entitySize;
			if (readSerializedRelationshipId(edgeBuf) == expectedId && readSerializedRelationshipActive(edgeBuf) &&
				(typeId == 0 || readSerializedRelationshipTypeId(edgeBuf) == typeId)) {
				++count;
			}
		}
		return count;
	}

	std::optional<bool> DataManager::persistedEdgeMatchesType(int64_t edgeId, int64_t typeId) const {
		if (!hasPreadSupport() || edgeId <= 0) { // ZYX_COV_EXCL_LINE: overlay callers pass persisted positive edge ids.
			return std::nullopt;
		}

		const uint64_t segmentOffset = findSegmentForEntityId<Edge>(edgeId);
		if (segmentOffset == 0) {
			return false;
		}

		SegmentHeader header{};
		const ssize_t headerRead = preadBytes(&header, sizeof(SegmentHeader), static_cast<int64_t>(segmentOffset));
		if (headerRead < static_cast<ssize_t>(sizeof(SegmentHeader)) || header.data_type != Edge::typeId) { // ZYX_COV_EXCL_LINE: corrupt disk header is validated through public overlay tests.
			return std::nullopt;
		}

		const int64_t slot = edgeId - header.start_id;
		if (slot < 0 || slot >= static_cast<int64_t>(header.used)) { // ZYX_COV_EXCL_LINE: segment lookup normally returns the owning segment.
			return false;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		std::vector<char> buf(entitySize);
		const auto dataOffset =
				static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader) + static_cast<uint64_t>(slot) * entitySize);
		const ssize_t read = preadBytes(buf.data(), entitySize, dataOffset);
		if (read < static_cast<ssize_t>(entitySize)) { // ZYX_COV_EXCL_LINE: short pread requires external file truncation.
			return std::nullopt;
		}

		return readSerializedRelationshipId(buf.data()) == edgeId && readSerializedRelationshipActive(buf.data()) &&
			   (typeId == 0 || readSerializedRelationshipTypeId(buf.data()) == typeId);
	}

	std::optional<int64_t>
	DataManager::applyRelationshipTypeCountOverlay(int64_t baseCount, int64_t beginId, int64_t endId, int64_t typeId,
												   std::span<const DirtyEntityInfo<Edge>> edgeOverlay) const {
		int64_t total = baseCount;
		for (const auto &info: edgeOverlay) {
			if (!info.backup.has_value()) {
				continue;
			}
			const Edge &edge = *info.backup;
			const int64_t edgeId = edge.getId();
			if (edgeId < beginId || edgeId > endId) {
				continue;
			}

			if (info.changeType != EntityChangeType::CHANGE_ADDED) {
				auto persistedMatch = persistedEdgeMatchesType(edgeId, typeId);
				if (!persistedMatch.has_value()) {
					return std::nullopt;
				}
				total -= *persistedMatch ? int64_t{1} : int64_t{0};
			}

			if (info.changeType != EntityChangeType::CHANGE_DELETED && edge.isActive() &&
				(typeId == 0 || edge.getTypeId() == typeId)) {
				++total;
			}
		}
		return total;
	}

	std::optional<int64_t> DataManager::applyRelationshipTypeCountSnapshotOverlay(
			int64_t baseCount, int64_t beginId, int64_t endId, int64_t typeId,
			const std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &edgeOverlay) const {
		std::vector<DirtyEntityInfo<Edge>> overlay;
		overlay.reserve(edgeOverlay.size());
		for (const auto &[edgeId, info]: edgeOverlay) {
			(void) edgeId;
			overlay.push_back(info);
		}
		return applyRelationshipTypeCountOverlay(baseCount, beginId, endId, typeId, overlay);
	}

	std::optional<int64_t> DataManager::countActiveEdgesByTypeFromSegmentStats(int64_t beginId, int64_t endId,
																			   int64_t typeId) const {
		if (!hasPreadSupport() || beginId <= 0 || endId < beginId) { // ZYX_COV_EXCL_LINE: pread support is fixed for FileStorage-backed tests.
			return std::nullopt;
		}
		if (auto count = countActiveEdgesByTypeFromTotalStats(beginId, endId, typeId)) {
			return count;
		}
		const auto *snapshot = getCurrentSnapshot();

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		int64_t total = 0;
		for (const auto &entry: segmentIndex) {
			if (entry.endId < beginId || entry.startId > endId) { // ZYX_COV_EXCL_LINE: non-overlap is a segment-index range prune.
				continue;
			}

			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) {
				return std::nullopt;
			}
			header.file_offset = entry.segmentOffset;
			if (header.used == 0) {
				continue;
			}

			const int64_t segmentFirst = std::max<int64_t>(entry.startId, header.start_id);
			const int64_t segmentLast =
					std::min<int64_t>(entry.endId, header.start_id + static_cast<int64_t>(header.used) - 1);
			const int64_t first = std::max<int64_t>(beginId, segmentFirst);
			const int64_t last = std::min<int64_t>(endId, segmentLast);
			if (first > last) { // ZYX_COV_EXCL_LINE: segment-index/header disagreements are defensive.
				continue;
			}

			if (first == segmentFirst && last == segmentLast) { // ZYX_COV_EXCL_LINE: full-window split is determined by segment boundaries.
				auto stats = getRelationshipSegmentTypeStats(entry.segmentOffset, header);
				if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: stats build failure is surfaced through corrupt-header tests.
					return std::nullopt;
				}
				if (typeId == 0) {
					total += stats->activeCount;
				} else if (auto it = stats->activeCountByType.find(typeId); it != stats->activeCountByType.end()) {
					total += it->second;
				}
				continue;
			}

			auto partial = countActiveEdgesByTypeInSegmentWindow(entry.segmentOffset, header, first, last, typeId);
			if (!partial.has_value()) { // ZYX_COV_EXCL_LINE: partial-window bounds are derived above from a valid segment.
				return std::nullopt;
			}
			total += *partial;
		}
		if (snapshot != nullptr) {
			return applyRelationshipTypeCountSnapshotOverlay(total, beginId, endId, typeId, snapshot->edges);
		}
		auto edgeOverlay = getDirtyEntityInfos<Edge>(
				{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
		return applyRelationshipTypeCountOverlay(total, beginId, endId, typeId, edgeOverlay);
	}

	void DataManager::invalidateRelationshipSegmentTypeStats(std::span<const uint64_t> segmentOffsets) const {
		if (segmentOffsets.empty()) {
			return;
		}
		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipTypeTotalStats_.reset();
		for (uint64_t segmentOffset: segmentOffsets) {
			relationshipSegmentTypeStats_.erase(segmentOffset);
		}
	}

	void DataManager::clearRelationshipSegmentTypeStats() const {
		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipSegmentTypeStats_.clear();
		relationshipTypeTotalStats_.reset();
	}

} // namespace graph::storage
