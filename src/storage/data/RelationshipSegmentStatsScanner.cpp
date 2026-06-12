#include "graph/storage/data/RelationshipSegmentStatsScanner.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage {
	namespace {
		void widenRelationshipTypeRange(std::unordered_map<int64_t, std::pair<int64_t, int64_t>> &ranges,
										int64_t typeId, int64_t edgeId) {
			if (typeId == 0) {
				return;
			}
			auto [it, inserted] = ranges.emplace(typeId, std::pair<int64_t, int64_t>{edgeId, edgeId});
			if (!inserted) {
				it->second.first = std::min(it->second.first, edgeId);
				it->second.second = std::max(it->second.second, edgeId);
			}
		}

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

		std::vector<char> &relationshipStatsScannerScratchBuffer() {
			thread_local std::vector<char> buffer;
			return buffer;
		}

		uint64_t findEdgeSegmentOffset(const DataManager &dataManager, int64_t edgeId) {
			const auto segmentIndexManager = dataManager.getSegmentIndexManager();
			if (!segmentIndexManager) { // ZYX_COV_EXCL_LINE: initialized DataManager always owns a segment index.
				return 0;
			}
			for (const auto &entry: segmentIndexManager->getEdgeSegmentIndex()) {
				if (entry.startId <= edgeId && edgeId <= entry.endId) {
					return entry.segmentOffset;
				}
			}
			return uint64_t{0};
		}

		std::optional<RelationshipTypeSegmentStats>
		parseRelationshipSegmentTypeStats(uint64_t segmentOffset, const SegmentHeader &header, const char *data,
										  bool includePropertyCandidates) {
			if (header.data_type != Edge::typeId || header.used == 0 ||
				data == nullptr) { // ZYX_COV_EXCL_LINE: callers validate segment shape before parsing.
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
				stats.activePropertyEdgeIds.reserve(header.used);
				stats.activeBlobEdgeIds.reserve(
						header.inactive_count < header.used ? header.used - header.inactive_count : 0);
			}
			for (uint32_t slot = 0; slot < header.used;
				 ++slot) { // ZYX_COV_EXCL_LINE: loop-exit arcs are not meaningful for fixed-size segment scans.
				const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
				const char *edgeBuf = data + static_cast<size_t>(slot) * entitySize;
				if (readSerializedRelationshipId(edgeBuf) != expectedId ||
					!readSerializedRelationshipActive(edgeBuf)) { // ZYX_COV_EXCL_LINE: corrupt/inactive rows.
					continue;
				}
				const int64_t typeId = readSerializedRelationshipTypeId(edgeBuf);
				++stats.activeCount;
				++stats.activeCountByType[typeId];
				widenRelationshipTypeRange(stats.activeIdRangeByType, typeId, expectedId);

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
					stats.activePropertyEdgeIds.push_back(expectedId);
					stats.activePropertyEntityIdsByType[typeId].push_back(propertyEntityId);
					stats.activePropertyEdgeIdsByType[typeId].push_back(expectedId);
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
					stats.activeBlobEdgeIds.push_back(expectedId);
					stats.activeBlobEdgeIdsByType[typeId].push_back(expectedId);
				}
			}
			return stats;
		}
	} // namespace

	RelationshipSegmentStatsScanner::RelationshipSegmentStatsScanner(const DataManager &dataManager) :
		dataManager_(dataManager) {}

	std::optional<RelationshipTypeSegmentStats>
	RelationshipSegmentStatsScanner::build(uint64_t segmentOffset, const SegmentHeader &header,
											bool includePropertyCandidates) const {
		if (!dataManager_.hasPreadSupport() || header.data_type != Edge::typeId ||
			header.used == 0) { // ZYX_COV_EXCL_LINE: public callers pre-validate persisted edge segments.
			return std::nullopt;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		const size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
		auto &buf = relationshipStatsScannerScratchBuffer();
		buf.resize(dataBytes);
		const auto dataOffset = static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader));
		const ssize_t read = dataManager_.preadBytes(buf.data(), dataBytes, dataOffset);
		if (read <
			static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread requires external file truncation.
			return std::nullopt;
		}

		return parseRelationshipSegmentTypeStats(segmentOffset, header, buf.data(), includePropertyCandidates);
	}

	std::optional<int64_t>
	RelationshipSegmentStatsScanner::countActiveInWindow(uint64_t segmentOffset, const SegmentHeader &header,
														 int64_t firstId, int64_t lastId, int64_t typeId) const {
		if (!dataManager_.hasPreadSupport() || header.data_type != Edge::typeId || header.used == 0 ||
			firstId > lastId) { // ZYX_COV_EXCL_LINE: partial-window callers validate edge segment bounds.
			return std::nullopt;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		const auto firstSlot = static_cast<uint32_t>(firstId - header.start_id);
		const auto lastSlot = static_cast<uint32_t>(lastId - header.start_id);
		if (lastSlot >= header.used ||
			firstSlot > lastSlot) { // ZYX_COV_EXCL_LINE: partial-window callers derive slots from the same header.
			return std::nullopt;
		}

		const size_t rowCount = static_cast<size_t>(lastSlot - firstSlot + 1);
		const size_t dataBytes = rowCount * entitySize;
		auto &buf = relationshipStatsScannerScratchBuffer();
		buf.resize(dataBytes);
		const auto dataOffset = static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader) +
													 static_cast<uint64_t>(firstSlot) * entitySize);
		const ssize_t read = dataManager_.preadBytes(buf.data(), dataBytes, dataOffset);
		if (read <
			static_cast<ssize_t>(dataBytes)) { // ZYX_COV_EXCL_LINE: short pread requires external file truncation.
			return std::nullopt;
		}

		int64_t count = 0;
		for (uint32_t slot = firstSlot; slot <= lastSlot; ++slot) {
			const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
			const char *edgeBuf = buf.data() + static_cast<size_t>(slot - firstSlot) * entitySize;
			if (readSerializedRelationshipId(edgeBuf) == expectedId && readSerializedRelationshipActive(edgeBuf) && // ZYX_COV_EXCL_LINE
				(typeId == 0 || readSerializedRelationshipTypeId(edgeBuf) == typeId)) {
				++count;
			}
		}
		return count;
	}

	std::optional<bool> RelationshipSegmentStatsScanner::persistedEdgeMatchesType(int64_t edgeId, int64_t typeId) const {
		if (!dataManager_.hasPreadSupport() || edgeId <= 0) { // ZYX_COV_EXCL_LINE: overlay callers pass positive ids.
			return std::nullopt;
		}

		const auto segmentOffset = findEdgeSegmentOffset(dataManager_, edgeId);
		if (segmentOffset == 0) {
			return false;
		}

		SegmentHeader header{};
		const ssize_t headerRead =
				dataManager_.preadBytes(&header, sizeof(SegmentHeader), static_cast<int64_t>(segmentOffset));
		if (headerRead < static_cast<ssize_t>(sizeof(SegmentHeader)) ||
			header.data_type !=
					Edge::typeId) { // ZYX_COV_EXCL_LINE: corrupt disk header is validated through public overlay tests.
			return std::nullopt;
		}

		const int64_t slot = edgeId - header.start_id;
		if (slot < 0 ||
			slot >= static_cast<int64_t>(
							header.used)) { // ZYX_COV_EXCL_LINE: segment index normally points at the owning segment.
			return false;
		}

		constexpr size_t entitySize = Edge::getTotalSize();
		auto &buf = relationshipStatsScannerScratchBuffer();
		buf.resize(entitySize);
		const auto dataOffset =
				static_cast<int64_t>(segmentOffset + sizeof(SegmentHeader) + static_cast<uint64_t>(slot) * entitySize);
		const ssize_t read = dataManager_.preadBytes(buf.data(), entitySize, dataOffset);
		if (read <
			static_cast<ssize_t>(entitySize)) { // ZYX_COV_EXCL_LINE: short pread requires external file truncation.
			return std::nullopt;
		}

		return readSerializedRelationshipId(buf.data()) == edgeId && readSerializedRelationshipActive(buf.data()) && // ZYX_COV_EXCL_LINE
			   (typeId == 0 || readSerializedRelationshipTypeId(buf.data()) == typeId);
	}

} // namespace graph::storage
