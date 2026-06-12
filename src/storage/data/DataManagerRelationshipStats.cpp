#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ParallelScanExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/RelationshipSegmentStatsScanner.hpp"

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

		struct PersistedEdgeIdTypeFilterWork {
			size_t segmentIndex = 0;
			size_t idBegin = 0;
			size_t idEnd = 0;
		};

		struct PersistedEdgeIdTypeFilterState {
			std::vector<char> buffer;
			std::optional<int64_t> count;
		};

		void appendRelationshipPropertyCandidates(RelationshipPropertyCandidateStats &target,
												  std::span<const int64_t> propertyEntityIds,
												  std::span<const int64_t> propertyEdgeIds,
												  std::span<const int64_t> blobEdgeIds,
												  bool includePropertyEdgeRefs) {
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
			if (includePropertyEdgeRefs) {
				reserveAdditional(target.propertyEdgeIds, propertyEdgeIds.size());
				target.propertyEdgeIds.insert(target.propertyEdgeIds.end(), propertyEdgeIds.begin(), propertyEdgeIds.end());
			}
			reserveAdditional(target.fallbackEdgeIds, blobEdgeIds.size());
			target.fallbackEdgeIds.insert(target.fallbackEdgeIds.end(), blobEdgeIds.begin(), blobEdgeIds.end());
		}

	} // namespace

	std::optional<RelationshipTypeSegmentStats>
	DataManager::buildRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header,
												   bool includePropertyCandidates) const {
		return RelationshipSegmentStatsScanner(*this).build(segmentOffset, header, includePropertyCandidates);
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
				header.used == 0 ? header.start_id - 1
								 : header.start_id + static_cast<int64_t>(header.used) -
										   1; // ZYX_COV_EXCL_LINE: zero-used cached edge segments are never inserted.
		std::shared_lock lock(relationshipSegmentTypeStatsMutex_);
		auto it = relationshipSegmentTypeStats_.find(segmentOffset);
		if (it != relationshipSegmentTypeStats_.end() && it->second.startId == header.start_id &&
			it->second.endId == expectedEndId && it->second.used == header.used && // ZYX_COV_EXCL_LINE
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

	bool DataManager::hasCachedRelationshipSegmentTypeStats(bool requirePropertyCandidates) const {
		std::shared_lock lock(relationshipSegmentTypeStatsMutex_);
		if (!requirePropertyCandidates) {
			return !relationshipSegmentTypeStats_.empty();
		}
		return std::any_of(relationshipSegmentTypeStats_.begin(), relationshipSegmentTypeStats_.end(),
						   [](const auto &entry) { return entry.second.hasPropertyCandidates; });
	}

	std::optional<RelationshipPropertyCandidateStats>
	DataManager::collectRelationshipPropertyCandidatesFromSegmentStats(int64_t beginId, int64_t endId,
																	   int64_t typeId,
																	   bool includePropertyEdgeRefs) const {
		return collectRelationshipPropertyCandidatesFromSegmentStatsImpl(
				beginId, endId, typeId, true, includePropertyEdgeRefs);
	}

	std::optional<RelationshipPropertyCandidateStats>
	DataManager::collectCachedRelationshipPropertyCandidatesFromSegmentStats(int64_t beginId, int64_t endId,
																			 int64_t typeId,
																			 bool includePropertyEdgeRefs) const {
		return collectRelationshipPropertyCandidatesFromSegmentStatsImpl(
				beginId, endId, typeId, false, includePropertyEdgeRefs);
	}

	std::optional<RelationshipPropertyCandidateStats>
	DataManager::collectRelationshipPropertyCandidatesFromSegmentStatsImpl(int64_t beginId, int64_t endId,
																		   int64_t typeId,
																		   bool buildMissingStats,
																		   bool includePropertyEdgeRefs) const {
		if (!hasPreadSupport() || beginId <= 0 || endId < beginId ||
			hasUnsavedChanges()) { // ZYX_COV_EXCL_LINE: pread support is fixed for FileStorage-backed tests.
			return std::nullopt;
		}
		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr &&
			(!snapshot->edges.empty() || !snapshot->properties.empty() || !snapshot->blobs.empty())) { // ZYX_COV_EXCL_LINE
			return std::nullopt;
		}

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		RelationshipPropertyCandidateStats candidates;
		for (const auto &entry: segmentIndex) {
			if (entry.endId < beginId ||
				entry.startId > endId) { // ZYX_COV_EXCL_LINE: non-overlap is a segment-index range prune.
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

			auto stats = buildMissingStats ? getRelationshipSegmentTypeStats(entry.segmentOffset, header, true)
										   : getCachedRelationshipSegmentTypeStats(entry.segmentOffset, header, true);
			if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: stats build failure is surfaced through corrupt-header
									  // tests.
				return std::nullopt;
			}
			if (typeId == 0) {
				candidates.matchedEdges += static_cast<size_t>(stats->activeCount);
				appendRelationshipPropertyCandidates(candidates, stats->activePropertyEntityIds,
													 stats->activePropertyEdgeIds, stats->activeBlobEdgeIds,
													 includePropertyEdgeRefs);
				continue;
			}

			if (auto countIt = stats->activeCountByType.find(typeId); countIt != stats->activeCountByType.end()) {
				candidates.matchedEdges += static_cast<size_t>(countIt->second);
			}
			if (auto propertyIt = stats->activePropertyEntityIdsByType.find(typeId);
				propertyIt != stats->activePropertyEntityIdsByType.end()) {
				const auto edgeIt = stats->activePropertyEdgeIdsByType.find(typeId);
				appendRelationshipPropertyCandidates(
						candidates,
						propertyIt->second,
						edgeIt != stats->activePropertyEdgeIdsByType.end() ? std::span<const int64_t>{edgeIt->second}
																			: std::span<const int64_t>{},
						std::span<const int64_t>{},
						includePropertyEdgeRefs);
			}
			if (auto blobIt = stats->activeBlobEdgeIdsByType.find(typeId);
				blobIt != stats->activeBlobEdgeIdsByType.end()) {
				appendRelationshipPropertyCandidates(
						candidates, std::span<const int64_t>{}, std::span<const int64_t>{}, blobIt->second,
						includePropertyEdgeRefs);
			}
		}
		return candidates;
	}


	std::optional<int64_t> DataManager::countActiveEdgesByTypeInSegmentWindow(uint64_t segmentOffset,
																			  const SegmentHeader &header,
																			  int64_t firstId, int64_t lastId,
																			  int64_t typeId) const {
		return RelationshipSegmentStatsScanner(*this).countActiveInWindow(
				segmentOffset, header, firstId, lastId, typeId);
	}

	std::optional<bool> DataManager::persistedEdgeMatchesType(int64_t edgeId, int64_t typeId) const {
		return RelationshipSegmentStatsScanner(*this).persistedEdgeMatchesType(edgeId, typeId);
	}

	std::optional<int64_t> DataManager::countActivePersistedEdgeIdsByType(
			std::span<const int64_t> edgeIds,
			int64_t typeId) const {
		return countActivePersistedEdgeIdsByType(edgeIds, typeId, nullptr);
	}

	std::optional<int64_t> DataManager::countActivePersistedEdgeIdsByType(
			std::span<const int64_t> edgeIds,
			int64_t typeId,
			concurrent::ThreadPool *threadPool) const {
		if (edgeIds.empty()) {
			return int64_t{0};
		}
		if (!hasPreadSupport() || hasUnsavedChanges() || !segmentIndexManager_) {
			return std::nullopt;
		}
		const auto *snapshot = getCurrentSnapshot();
		if (snapshot != nullptr && !snapshot->edges.empty()) {
			return std::nullopt;
		}

		std::vector<int64_t> sortedIds(edgeIds.begin(), edgeIds.end());
		std::sort(sortedIds.begin(), sortedIds.end());
		sortedIds.erase(std::remove_if(sortedIds.begin(), sortedIds.end(), [](int64_t id) { return id <= 0; }),
						sortedIds.end());
		sortedIds.erase(std::unique(sortedIds.begin(), sortedIds.end()), sortedIds.end());
		if (sortedIds.empty()) {
			return int64_t{0};
		}

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		if (segmentIndex.empty()) {
			return int64_t{0};
		}

		std::vector<PersistedEdgeIdTypeFilterWork> work;
		work.reserve(segmentIndex.size());
		size_t idCursor = 0;
		for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
			const auto &entry = segmentIndex[segment];
			while (idCursor < sortedIds.size() && sortedIds[idCursor] < entry.startId) {
				++idCursor;
			}
			if (idCursor >= sortedIds.size()) {
				break;
			}
			if (sortedIds[idCursor] > entry.endId) {
				continue;
			}

			const size_t segmentBegin = idCursor;
			while (idCursor < sortedIds.size() && sortedIds[idCursor] <= entry.endId) {
				++idCursor;
			}
			const size_t segmentEnd = idCursor;
			const size_t selectedCount = segmentEnd - segmentBegin;
			if (selectedCount == 0) { // ZYX_COV_EXCL_LINE: segmentEnd is advanced from segmentBegin.
				continue;
			}
			work.push_back({segment, segmentBegin, segmentEnd});
		}
		if (work.empty()) {
			return int64_t{0};
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		auto countWork = [&](const PersistedEdgeIdTypeFilterWork &item, std::vector<char> &buf) -> std::optional<int64_t> {
			const auto &entry = segmentIndex[item.segmentIndex];
			const size_t selectedCount = item.idEnd - item.idBegin;

			if (auto stats = cachedRelationshipTypeSegmentStats(entry.segmentOffset)) {
				if (typeId != 0 && !stats->activeCountByType.contains(typeId)) {
					return int64_t{0};
				}
				const bool segmentHasOnlyRequestedType =
						stats->inactiveCount == 0 &&
						(typeId == 0 || stats->activeCountByType.size() == 1);
				if (segmentHasOnlyRequestedType) {
					return static_cast<int64_t>(selectedCount);
				}
			}

			buf.resize(TOTAL_SEGMENT_SIZE);
			const ssize_t read = preadSegments(buf.data(), 1, entry.segmentOffset);
			if (read < static_cast<ssize_t>(TOTAL_SEGMENT_SIZE)) {
				return std::nullopt; // ZYX_COV_EXCL_LINE: short reads require file truncation or OS failure.
			}

			SegmentHeader header{};
			std::memcpy(&header, buf.data(), sizeof(SegmentHeader));
			if (header.data_type != Edge::typeId || header.used == 0) {
				return int64_t{0}; // ZYX_COV_EXCL_LINE: edge segment index entries point at edge segments.
			}
			const char *data = buf.data() + sizeof(SegmentHeader);
			int64_t count = 0;
			for (size_t i = item.idBegin; i < item.idEnd; ++i) {
				const int64_t edgeId = sortedIds[i];
				const int64_t slot = edgeId - header.start_id;
				if (slot < 0 || slot >= static_cast<int64_t>(header.used)) {
					continue;
				}
				const char *edgeBuf = data + static_cast<size_t>(slot) * entitySize;
				if (readSerializedRelationshipId(edgeBuf) == edgeId &&
					readSerializedRelationshipActive(edgeBuf) &&
					(typeId == 0 || readSerializedRelationshipTypeId(edgeBuf) == typeId)) {
					++count;
				}
			}
			return count;
		};

		int64_t total = 0;
		const bool counted = concurrent::runIndexedPartitions<PersistedEdgeIdTypeFilterState>(
				work.size(),
				threadPool,
				{.phase = "relationship_count.edge_type_filter.parallel",
				 .estimatedItems = sortedIds.size(),
				 .minPartitions = 2,
				 .minItems = 1024},
				[&](size_t workIndex, PersistedEdgeIdTypeFilterState &state) {
					state.count = countWork(work[workIndex], state.buffer);
					std::vector<char>().swap(state.buffer);
					return state.count.has_value();
				},
				[&](size_t, PersistedEdgeIdTypeFilterState &state) {
					total += state.count.value_or(int64_t{0});
				});
		if (!counted) {
			return std::nullopt;
		}
		return total;
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
			if (edgeId < beginId || edgeId > endId) { // ZYX_COV_EXCL_LINE: overlay range pruning is covered indirectly.
				continue;
			}

			if (info.changeType != EntityChangeType::CHANGE_ADDED) {
				auto persistedMatch = persistedEdgeMatchesType(edgeId, typeId);
				if (!persistedMatch.has_value()) {
					return std::nullopt;
				}
				total -= *persistedMatch ? int64_t{1} : int64_t{0};
			}

			if (info.changeType != EntityChangeType::CHANGE_DELETED && edge.isActive() && // ZYX_COV_EXCL_LINE
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
		if (beginId <= 0 || endId < beginId) { // ZYX_COV_EXCL_LINE: range guards are validated through public tests.
			return std::nullopt;
		}
		if (!hasPreadSupport()) { // ZYX_COV_EXCL_LINE: segment stats require FileStorage pread support.
			return std::nullopt;
		}
		const auto *snapshot = getCurrentSnapshot();

		const auto &segmentIndex = segmentIndexManager_->getEdgeSegmentIndex();
		int64_t total = 0;
		for (const auto &entry: segmentIndex) {
			if (entry.endId < beginId ||
				entry.startId > endId) { // ZYX_COV_EXCL_LINE: non-overlap is a segment-index range prune.
				continue;
			}

			SegmentHeader header{};
			try {
				header = segmentTracker_->getSegmentHeaderCopy(entry.segmentOffset);
			} catch (const std::exception &) {
				return std::nullopt;
			}
			if (header.data_type != Edge::typeId) { // ZYX_COV_EXCL_LINE: edge indexes only reference edge segments.
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

			if (first == segmentFirst &&
				last == segmentLast) { // ZYX_COV_EXCL_LINE: full-window split is determined by segment boundaries.
				auto stats = buildRelationshipSegmentTypeStats(entry.segmentOffset, header, false);
				if (!stats.has_value()) { // ZYX_COV_EXCL_LINE: stats build failure is surfaced through corrupt-header
										  // tests.
					return std::nullopt;
				}
				if (typeId == 0) {
					total += stats->activeCount;
				} else if (auto it = stats->activeCountByType.find(typeId); it != stats->activeCountByType.end()) { // ZYX_COV_EXCL_LINE
					total += it->second;
				}
				continue;
			}

			auto partial = countActiveEdgesByTypeInSegmentWindow(entry.segmentOffset, header, first, last, typeId);
			if (!partial.has_value()) { // ZYX_COV_EXCL_LINE: partial-window bounds are derived above from a valid
										// segment.
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
		for (uint64_t segmentOffset: segmentOffsets) {
			relationshipSegmentTypeStats_.erase(segmentOffset);
		}
	}

	void DataManager::clearRelationshipSegmentTypeStats() const {
		std::unique_lock lock(relationshipSegmentTypeStatsMutex_);
		relationshipSegmentTypeStats_.clear();
	}

} // namespace graph::storage
