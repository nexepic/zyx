#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/StorageHeaders.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		constexpr size_t kEdgeIdOffset = 0;
		constexpr size_t kSourceNodeIdOffset = kEdgeIdOffset + sizeof(int64_t);
		constexpr size_t kTargetNodeIdOffset = kSourceNodeIdOffset + sizeof(int64_t);
		constexpr size_t kPropertyEntityIdOffset = kTargetNodeIdOffset + sizeof(int64_t) + sizeof(int64_t) * 4;
		constexpr size_t kTypeIdOffset = kPropertyEntityIdOffset + sizeof(int64_t);
		constexpr size_t kPropertyStorageTypeOffset = kTypeIdOffset + sizeof(int64_t);
		constexpr size_t kActiveOffset = kPropertyStorageTypeOffset + sizeof(uint32_t);

		int64_t readSerializedEdgeId(const char *buf) {
			int64_t edgeId = 0;
			std::memcpy(&edgeId, buf + kEdgeIdOffset, sizeof(int64_t));
			return edgeId;
		}

		int64_t readSerializedTypeId(const char *buf) {
			int64_t typeId = 0;
			std::memcpy(&typeId, buf + kTypeIdOffset, sizeof(int64_t));
			return typeId;
		}

		int64_t readSerializedPropertyEntityId(const char *buf) {
			int64_t propertyEntityId = 0;
			std::memcpy(&propertyEntityId, buf + kPropertyEntityIdOffset, sizeof(int64_t));
			return propertyEntityId;
		}

		PropertyStorageType readSerializedPropertyStorageType(const char *buf) {
			uint32_t storageType = 0;
			std::memcpy(&storageType, buf + kPropertyStorageTypeOffset, sizeof(uint32_t));
			return static_cast<PropertyStorageType>(storageType);
		}

		bool readSerializedActive(const char *buf) {
			bool active = false;
			std::memcpy(&active, buf + kActiveOffset, sizeof(bool));
			return active;
		}

		void readMetadataIntoBatch(const char *buf, RelationshipMetadataBatch &batch) {
			batch.appendDefault();
			const size_t row = batch.size() - 1;
			std::memcpy(&batch.edgeIds[row], buf + kEdgeIdOffset, sizeof(int64_t));
			std::memcpy(&batch.sourceNodeIds[row], buf + kSourceNodeIdOffset, sizeof(int64_t));
			std::memcpy(&batch.targetNodeIds[row], buf + kTargetNodeIdOffset, sizeof(int64_t));
			std::memcpy(&batch.propertyEntityIds[row], buf + kPropertyEntityIdOffset, sizeof(int64_t));
			std::memcpy(&batch.typeIds[row], buf + kTypeIdOffset, sizeof(int64_t));
			uint32_t storageType = 0;
			std::memcpy(&storageType, buf + kPropertyStorageTypeOffset, sizeof(uint32_t));
			batch.propertyStorageTypes[row] = static_cast<PropertyStorageType>(storageType);
			batch.active[row] = static_cast<uint8_t>(readSerializedActive(buf));
		}

		std::vector<size_t> collectEdgeWorkSegments(const std::shared_ptr<storage::DataManager> &dm,
		                                            int64_t beginId,
		                                            int64_t endId) {
			const auto &segmentIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
			std::vector<size_t> workSegmentIndices;
			workSegmentIndices.reserve(segmentIndex.size());
			for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
				const auto &entry = segmentIndex[segment];
				if (entry.endId >= beginId && entry.startId <= endId) {
					workSegmentIndices.push_back(segment);
				}
			}
			return workSegmentIndices;
		}

		struct EdgeSegmentScanWindow {
			const char *data = nullptr;
			int64_t first = 0;
			int64_t last = -1;
			int64_t segmentStartId = 0;
		};

		std::optional<EdgeSegmentScanWindow> edgeSegmentScanWindow(
				const storage::SegmentIndexManager::SegmentIndex &entry,
				const storage::SegmentHeader &header,
				const char *segmentBuffer,
				int64_t beginId,
				int64_t endId) {
			if (header.used == 0) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}
			EdgeSegmentScanWindow window;
			window.data = segmentBuffer + sizeof(storage::SegmentHeader);
			window.segmentStartId = header.start_id;
			const int64_t headerLastId = header.start_id + static_cast<int64_t>(header.used) - 1;
			window.first = std::max<int64_t>(beginId, std::max<int64_t>(entry.startId, header.start_id));
			window.last = std::min<int64_t>(endId, std::min<int64_t>(entry.endId, headerLastId));
			if (window.first > window.last) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}
			return window;
		}

		template<typename Visitor>
		void scanEdgeWindow(const EdgeSegmentScanWindow &window, Visitor &&visitor) {
			constexpr size_t entitySize = Edge::getTotalSize();
			for (int64_t edgeId = window.first; edgeId <= window.last; ++edgeId) {
				const auto slot = static_cast<uint32_t>(edgeId - window.segmentStartId);
				const char *serializedEdge = window.data + slot * entitySize;
				if (readSerializedEdgeId(serializedEdge) == edgeId) {
					visitor(edgeId, serializedEdge);
				}
			}
		}

		template<typename Visitor>
		bool scanSerializedEdges(const std::shared_ptr<storage::DataManager> &dm,
		                         int64_t beginId,
		                         int64_t endId,
		                         Visitor &&visitor) {
			const auto workSegmentIndices = collectEdgeWorkSegments(dm, beginId, endId);
			if (workSegmentIndices.empty()) { // ZYX_COV_EXCL_LINE
				return false;
			}

			const auto &segmentIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
			auto groups = storage::buildCoalescedGroups(workSegmentIndices, segmentIndex);
			for (const auto &group : groups) {
				const size_t totalBytes = group.segCount * storage::TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuffer(totalBytes);
				const auto groupOffset = static_cast<int64_t>(group.startOffset);
				const auto read = dm->preadBytes(groupBuffer.data(), totalBytes, groupOffset);
				if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return false;
				}

				for (size_t member = 0; member < group.memberIndices.size(); ++member) {
					const size_t segmentIndexInWork = group.memberIndices[member];
					const size_t segment = workSegmentIndices[segmentIndexInWork];
					const auto &entry = segmentIndex[segment];
					const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;
					storage::SegmentHeader header{};
					const char *segmentBuffer = groupBuffer.data() + bufferOffset;
					std::memcpy(&header, segmentBuffer, sizeof(storage::SegmentHeader));
					auto window = edgeSegmentScanWindow(entry, header, segmentBuffer, beginId, endId);
					if (window.has_value()) {
						scanEdgeWindow(*window, visitor);
					}
				}
			}
			return true;
		}

	} // namespace

	void RelationshipMetadataBatch::reserve(size_t rowCount) {
		edgeIds.reserve(rowCount);
		sourceNodeIds.reserve(rowCount);
		targetNodeIds.reserve(rowCount);
		typeIds.reserve(rowCount);
		propertyEntityIds.reserve(rowCount);
		propertyStorageTypes.reserve(rowCount);
		active.reserve(rowCount);
	}

	void RelationshipMetadataBatch::appendDefault() {
		edgeIds.push_back(0);
		sourceNodeIds.push_back(0);
		targetNodeIds.push_back(0);
		typeIds.push_back(0);
		propertyEntityIds.push_back(0);
		propertyStorageTypes.push_back(PropertyStorageType::NONE);
		active.push_back(0);
	}

	void RelationshipPropertyCandidateBatch::reserve(size_t rowCount) {
		edgeIds.reserve(rowCount);
		propertyEntityIds.reserve(rowCount);
		propertyRows.reserve(rowCount);
		fallbackRows.reserve(rowCount / 8);
	}

	void RelationshipPropertyCountCandidates::reserve(size_t rowCount) {
		propertyEntityIds.reserve(rowCount);
		fallbackEdgeIds.reserve(rowCount / 8);
	}

	void RelationshipMetadataBatch::setFromEdge(size_t row, const Edge &edge) {
		if (row >= size()) {
			return;
		}
		edgeIds[row] = edge.getId();
		sourceNodeIds[row] = edge.getSourceNodeId();
		targetNodeIds[row] = edge.getTargetNodeId();
		typeIds[row] = edge.getTypeId();
		propertyEntityIds[row] = edge.getPropertyEntityId();
		propertyStorageTypes[row] = edge.getPropertyStorageType();
		active[row] = edge.isActive() ? uint8_t{1} : uint8_t{0};
	}

	Edge RelationshipMetadataBatch::toEdge(size_t row) const {
		Edge edge;
		if (row >= size()) {
			return edge;
		}
		auto &metadata = edge.getMutableMetadata();
		metadata.id = edgeIds[row];
		metadata.sourceNodeId = sourceNodeIds[row];
		metadata.targetNodeId = targetNodeIds[row];
		metadata.typeId = typeIds[row];
		metadata.propertyEntityId = propertyEntityIds[row];
		metadata.propertyStorageType = static_cast<uint32_t>(propertyStorageTypes[row]);
		metadata.isActive = active[row] != 0;
		return edge;
	}

	RelationshipMetadataColumnLoader::RelationshipMetadataColumnLoader(std::shared_ptr<storage::DataManager> dm)
		: dm_(std::move(dm)) {}

	bool RelationshipMetadataColumnLoader::canLoad(int64_t beginId, int64_t endId) const {
		static constexpr int64_t METADATA_LOAD_THRESHOLD = 128;
		if (!dm_ || !dm_->hasPreadSupport() || dm_->hasUnsavedChanges() || beginId <= 0 || endId < beginId ||
		    endId - beginId + 1 < METADATA_LOAD_THRESHOLD) {
			return false;
		}
		const auto *snapshot = dm_->getCurrentSnapshot();
		const bool snapshotSafe = snapshot == nullptr || // ZYX_COV_EXCL_LINE
		                          (snapshot->edges.empty() && snapshot->properties.empty() && snapshot->blobs.empty()); // ZYX_COV_EXCL_LINE
		return snapshotSafe;
	}

	std::optional<RelationshipMetadataBatch> RelationshipMetadataColumnLoader::loadRange(int64_t beginId, int64_t endId) const {
		if (!canLoad(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		RelationshipMetadataBatch batch;
		batch.reserve(static_cast<size_t>(endId - beginId + 1));
		if (!scanSerializedEdges(dm_, beginId, endId, [&](int64_t, const char *serializedEdge) {
			readMetadataIntoBatch(serializedEdge, batch);
		})) {
			return std::nullopt;
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return batch;
	}

	std::optional<int64_t> RelationshipMetadataColumnLoader::countActiveByType(int64_t beginId,
	                                                                           int64_t endId,
	                                                                           int64_t typeId) const {
		if (!canLoad(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		int64_t count = 0;
		if (!scanSerializedEdges(dm_, beginId, endId, [&](int64_t, const char *serializedEdge) {
			if (readSerializedActive(serializedEdge) &&
			    (typeId == 0 || readSerializedTypeId(serializedEdge) == typeId)) {
				++count;
			}
		})) {
			return std::nullopt;
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return count;
	}

	std::optional<RelationshipPropertyCandidateBatch>
	RelationshipMetadataColumnLoader::collectPropertyCandidatesByType(int64_t beginId,
	                                                                  int64_t endId,
	                                                                  int64_t typeId) const {
		if (!canLoad(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		RelationshipPropertyCandidateBatch batch;
		batch.reserve(static_cast<size_t>(endId - beginId + 1));
		if (!scanSerializedEdges(dm_, beginId, endId, [&](int64_t edgeId, const char *serializedEdge) {
			if (!readSerializedActive(serializedEdge) ||
			    (typeId != 0 && readSerializedTypeId(serializedEdge) != typeId)) {
				return;
			}

			const int64_t propertyEntityId = readSerializedPropertyEntityId(serializedEdge);
			if (propertyEntityId == 0) {
				return;
			}

			const auto storageType = readSerializedPropertyStorageType(serializedEdge);
			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				const size_t row = batch.edgeIds.size();
				batch.edgeIds.push_back(edgeId);
				batch.propertyEntityIds.push_back(propertyEntityId);
				batch.propertyRows.push_back(row);
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				const size_t row = batch.edgeIds.size();
				batch.edgeIds.push_back(edgeId);
				batch.fallbackRows.push_back(row);
			}
		})) {
			return std::nullopt;
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return batch;
	}

	std::optional<RelationshipPropertyCountCandidates>
	RelationshipMetadataColumnLoader::collectPropertyCountCandidatesByType(int64_t beginId,
	                                                                       int64_t endId,
	                                                                       int64_t typeId) const {
		if (!canLoad(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		RelationshipPropertyCountCandidates candidates;
		candidates.reserve(static_cast<size_t>(endId - beginId + 1));
		if (!scanSerializedEdges(dm_, beginId, endId, [&](int64_t edgeId, const char *serializedEdge) {
			if (!readSerializedActive(serializedEdge) ||
			    (typeId != 0 && readSerializedTypeId(serializedEdge) != typeId)) {
				return;
			}

			++candidates.matchedEdges;
			const int64_t propertyEntityId = readSerializedPropertyEntityId(serializedEdge);
			if (propertyEntityId == 0) {
				return;
			}

			const auto storageType = readSerializedPropertyStorageType(serializedEdge);
			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				candidates.propertyEntityIds.push_back(propertyEntityId);
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				candidates.fallbackEdgeIds.push_back(edgeId);
			}
		})) {
			return std::nullopt;
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return candidates;
	}

} // namespace graph::query::execution
