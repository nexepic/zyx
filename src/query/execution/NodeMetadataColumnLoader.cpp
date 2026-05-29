#include "graph/query/execution/NodeMetadataColumnLoader.hpp"

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

		struct SegmentCandidateRange {
			size_t segmentIndex = 0;
			size_t idBegin = 0;
			size_t idEnd = 0;
		};

		int64_t readSerializedNodeId(const char *buf) {
			int64_t nodeId = 0;
			std::memcpy(&nodeId, buf, sizeof(int64_t));
			return nodeId;
		}

		void readMetadataIntoBatch(const char *buf, NodeMetadataBatch &batch, size_t row) {
			size_t off = sizeof(int64_t);
			std::memcpy(&batch.firstOutEdgeIds[row], buf + off, sizeof(int64_t));
			off += sizeof(int64_t);
			std::memcpy(&batch.firstInEdgeIds[row], buf + off, sizeof(int64_t));
			off += sizeof(int64_t);
			std::memcpy(&batch.propertyEntityIds[row], buf + off, sizeof(int64_t));
			off += sizeof(int64_t);
			std::memcpy(batch.labelIds[row].data(), buf + off, sizeof(int64_t) * Node::MAX_LABELS);
			off += sizeof(int64_t) * Node::MAX_LABELS;
			std::memcpy(&batch.labelCounts[row], buf + off, sizeof(uint8_t));
			off += sizeof(uint8_t);
			uint32_t storageType = 0;
			std::memcpy(&storageType, buf + off, sizeof(uint32_t));
			batch.propertyStorageTypes[row] = static_cast<PropertyStorageType>(storageType);
			off += sizeof(uint32_t);
			bool active = false;
			std::memcpy(&active, buf + off, sizeof(bool));
			batch.nodeIds[row] = readSerializedNodeId(buf);
			batch.active[row] = static_cast<uint8_t>(active);
		}
	} // namespace

	void NodeMetadataBatch::reserve(size_t rowCount) {
		nodeIds.reserve(rowCount);
		firstOutEdgeIds.reserve(rowCount);
		firstInEdgeIds.reserve(rowCount);
		active.reserve(rowCount);
		labelCounts.reserve(rowCount);
		labelIds.reserve(rowCount);
		propertyEntityIds.reserve(rowCount);
		propertyStorageTypes.reserve(rowCount);
	}

	void NodeMetadataBatch::appendDefault() {
		nodeIds.push_back(0);
		firstOutEdgeIds.push_back(0);
		firstInEdgeIds.push_back(0);
		active.push_back(0);
		labelCounts.push_back(0);
		labelIds.push_back({});
		propertyEntityIds.push_back(0);
		propertyStorageTypes.push_back(PropertyStorageType::NONE);
	}

	void NodeMetadataBatch::setFromNode(size_t row, const Node &node) {
		if (row >= size()) {
			return;
		}
		const auto &metadata = node.getMetadata();
		nodeIds[row] = metadata.id;
		firstOutEdgeIds[row] = metadata.firstOutEdgeId;
		firstInEdgeIds[row] = metadata.firstInEdgeId;
		active[row] = metadata.isActive ? uint8_t{1} : uint8_t{0};
		labelCounts[row] = metadata.labelCount;
		std::copy(std::begin(metadata.labelIds), std::end(metadata.labelIds), labelIds[row].begin());
		propertyEntityIds[row] = metadata.propertyEntityId;
		propertyStorageTypes[row] = node.getPropertyStorageType();
	}

	bool NodeMetadataBatch::hasLabelId(size_t row, int64_t labelId) const {
		if (row >= size() || labelId <= 0) {
			return false;
		}
		const uint8_t count = std::min<uint8_t>(labelCounts[row], Node::MAX_LABELS);
		for (uint8_t index = 0; index < count; ++index) {
			if (labelIds[row][index] == labelId) {
				return true;
			}
		}
		return false;
	}

	Node NodeMetadataBatch::toNode(size_t row) const {
		Node node;
		if (row >= size()) {
			return node;
		}
		auto &metadata = node.getMutableMetadata();
		metadata.id = nodeIds[row];
		metadata.firstOutEdgeId = firstOutEdgeIds[row];
		metadata.firstInEdgeId = firstInEdgeIds[row];
		metadata.propertyEntityId = propertyEntityIds[row];
		metadata.propertyStorageType = static_cast<uint32_t>(propertyStorageTypes[row]);
		metadata.labelCount = std::min<uint8_t>(labelCounts[row], Node::MAX_LABELS);
		std::copy(labelIds[row].begin(), labelIds[row].end(), std::begin(metadata.labelIds));
		metadata.isActive = active[row] != 0;
		return node;
	}

	NodeMetadataColumnLoader::NodeMetadataColumnLoader(std::shared_ptr<storage::DataManager> dm)
		: dm_(std::move(dm)) {}

	bool NodeMetadataColumnLoader::canLoad(const std::vector<int64_t> &candidateIds, size_t begin, size_t end) const {
		static constexpr size_t METADATA_LOAD_THRESHOLD = 128;
		if (!dm_ || !dm_->hasPreadSupport() || dm_->hasUnsavedChanges() ||
		    begin > candidateIds.size() || end > candidateIds.size() ||
		    end <= begin || end - begin < METADATA_LOAD_THRESHOLD) {
			return false;
		}
		const auto *snapshot = dm_->getCurrentSnapshot();
		const bool snapshotSafe = snapshot == nullptr || // ZYX_COV_EXCL_LINE
		                          (snapshot->nodes.empty() && snapshot->properties.empty() && snapshot->blobs.empty()); // ZYX_COV_EXCL_LINE
		return snapshotSafe &&
		       std::is_sorted(candidateIds.begin() + static_cast<std::ptrdiff_t>(begin),
		                      candidateIds.begin() + static_cast<std::ptrdiff_t>(end));
	}

	std::optional<NodeMetadataBatch> NodeMetadataColumnLoader::loadBatch(const std::vector<int64_t> &candidateIds,
	                                                                    size_t begin,
	                                                                    size_t end) const {
		const size_t clampedEnd = std::min(end, candidateIds.size());
		if (!canLoad(candidateIds, begin, clampedEnd)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		const auto &segmentIndex = dm_->getSegmentIndexManager()->getNodeSegmentIndex();

		std::vector<SegmentCandidateRange> work;
		std::vector<size_t> workSegmentIndices;
		work.reserve(segmentIndex.size());
		workSegmentIndices.reserve(segmentIndex.size());

		auto rangeBegin = candidateIds.begin() + static_cast<std::ptrdiff_t>(begin);
		auto rangeEnd = candidateIds.begin() + static_cast<std::ptrdiff_t>(clampedEnd);
		for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
			const auto &entry = segmentIndex[segment];
			auto lo = std::lower_bound(rangeBegin, rangeEnd, entry.startId);
			auto hi = std::upper_bound(lo, rangeEnd, entry.endId);
			if (lo == hi) { // ZYX_COV_EXCL_LINE
				continue;
			}
			work.push_back({segment,
			                static_cast<size_t>(lo - candidateIds.begin()),
			                static_cast<size_t>(hi - candidateIds.begin())});
			workSegmentIndices.push_back(segment);
		}

		if (work.empty()) { // ZYX_COV_EXCL_LINE
			return std::nullopt;
		}

		NodeMetadataBatch batch;
		batch.reserve(clampedEnd - begin);
		for (size_t index = begin; index < clampedEnd; ++index) {
			(void) index;
			batch.appendDefault();
		}
		auto groups = storage::buildCoalescedGroups(workSegmentIndices, segmentIndex);
		constexpr size_t entitySize = Node::getTotalSize();

		for (const auto &group : groups) {
			const size_t totalBytes = group.segCount * storage::TOTAL_SEGMENT_SIZE;
			std::vector<char> groupBuffer(totalBytes);
			const auto groupOffset = static_cast<int64_t>(group.startOffset);
			const auto read = dm_->preadBytes(groupBuffer.data(), totalBytes, groupOffset);
			if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
				return std::nullopt;
			}

			for (size_t member = 0; member < group.memberIndices.size(); ++member) {
				const size_t workIndex = group.memberIndices[member];
				const auto &range = work[workIndex];
				const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;

				storage::SegmentHeader header{};
				std::memcpy(&header, groupBuffer.data() + bufferOffset, sizeof(storage::SegmentHeader));
				if (header.used == 0) { // ZYX_COV_EXCL_LINE
					continue;
				}

				const char *data = groupBuffer.data() + bufferOffset + sizeof(storage::SegmentHeader);
				for (size_t index = range.idBegin; index < range.idEnd; ++index) {
					const int64_t nodeId = candidateIds[index];
					const auto slot = static_cast<uint32_t>(nodeId - header.start_id);
					if (slot >= header.used) { // ZYX_COV_EXCL_LINE
						continue;
					}

					const char *serializedNode = data + slot * entitySize;
					if (readSerializedNodeId(serializedNode) == nodeId) { // ZYX_COV_EXCL_LINE
						readMetadataIntoBatch(serializedNode, batch, index - begin);
					}
				}
			}
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.load_node_metadata", elapsedNs(start));
		}
		return batch;
	}

	std::optional<std::vector<Node>> NodeMetadataColumnLoader::load(const std::vector<int64_t> &candidateIds,
	                                                               size_t begin,
	                                                               size_t end) const {
		auto batch = loadBatch(candidateIds, begin, end);
		if (!batch.has_value()) {
			return std::nullopt;
		}

		std::vector<Node> nodes;
		nodes.reserve(batch->size());
		for (size_t row = 0; row < batch->size(); ++row) {
			nodes.push_back(batch->toNode(row));
		}
		return nodes;
	}

} // namespace graph::query::execution
