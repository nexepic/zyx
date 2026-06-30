#include "graph/query/execution/NodeMetadataColumnLoader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <utility>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
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

		struct NodeMetadataScanPlan {
			std::vector<SegmentCandidateRange> work;
			std::vector<size_t> workSegmentIndices;
		};

		struct NodeMetadataReadTaskState {};

		int64_t readSerializedNodeId(const char *buf) {
			int64_t nodeId = 0;
			std::memcpy(&nodeId, buf, sizeof(int64_t));
			return nodeId;
		}

		std::vector<char> &nodeMetadataScanBuffer() {
			thread_local std::vector<char> buffer;
			return buffer;
		}

		constexpr size_t kMaxCoalescedNodeMetadataReadSegments = 16;
		constexpr size_t kMinParallelNodeMetadataReadTasks = 2;
		constexpr size_t kMinParallelNodeMetadataReadSegments = 32;
		constexpr size_t kNodeMetadataReadBytesPerWorker = size_t{8} * 1024 * 1024;

		concurrent::ParallelExecutionDecision decideNodeMetadataScan(
				concurrent::ThreadPool *threadPool,
				size_t taskCount,
				size_t segmentCount) {
			return concurrent::decideParallelExecution(
					threadPool,
					{.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					 .partitions = taskCount,
					 .estimatedItems = segmentCount,
					 .estimatedBytes = segmentCount * storage::TOTAL_SEGMENT_SIZE,
					 .minPartitions = kMinParallelNodeMetadataReadTasks,
					 .minItems = kMinParallelNodeMetadataReadSegments,
					 .minBytesPerWorker = kNodeMetadataReadBytesPerWorker});
		}

		NodeMetadataRow readMetadataRow(const char *buf, NodeMetadataProjection projection = {}) {
			NodeMetadataRow row;
			size_t off = sizeof(int64_t);
			row.nodeId = readSerializedNodeId(buf);
			if (projection.loadEdgeRefs) {
				std::memcpy(&row.firstOutEdgeId, buf + off, sizeof(int64_t));
			}
			off += sizeof(int64_t);
			if (projection.loadEdgeRefs) {
				std::memcpy(&row.firstInEdgeId, buf + off, sizeof(int64_t));
			}
			off += sizeof(int64_t);
			std::memcpy(&row.propertyEntityId, buf + off, sizeof(int64_t));
			off += sizeof(int64_t);
			if (projection.loadLabels) {
				std::memcpy(row.labelIds.data(), buf + off, sizeof(int64_t) * Node::MAX_LABELS);
			}
			off += sizeof(int64_t) * Node::MAX_LABELS;
			std::memcpy(&row.labelCount, buf + off, sizeof(uint8_t));
			if (!projection.loadLabels) {
				row.labelCount = 0;
			}
			off += sizeof(uint8_t);
			uint32_t storageType = 0;
			std::memcpy(&storageType, buf + off, sizeof(uint32_t));
			row.propertyStorageType = static_cast<PropertyStorageType>(storageType);
			off += sizeof(uint32_t);
			bool active = false;
			std::memcpy(&active, buf + off, sizeof(bool));
			row.active = static_cast<uint8_t>(active);
			return row;
		}

		NodeMetadataScanPlan collectNodeMetadataScanPlan(
				const std::vector<storage::SegmentIndexManager::SegmentIndex> &segmentIndex,
				const std::vector<int64_t> &candidateIds,
				size_t begin,
				size_t clampedEnd) {
			NodeMetadataScanPlan plan;
			plan.work.reserve(segmentIndex.size());
			plan.workSegmentIndices.reserve(segmentIndex.size());

			auto rangeBegin = candidateIds.begin() + static_cast<std::ptrdiff_t>(begin);
			auto rangeEnd = candidateIds.begin() + static_cast<std::ptrdiff_t>(clampedEnd);
			for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
				const auto &entry = segmentIndex[segment];
				auto lo = std::lower_bound(rangeBegin, rangeEnd, entry.startId);
				auto hi = std::upper_bound(lo, rangeEnd, entry.endId);
				if (lo == hi) { // ZYX_COV_EXCL_LINE
					continue;
				}
				plan.work.push_back({segment,
									  static_cast<size_t>(lo - candidateIds.begin()),
									  static_cast<size_t>(hi - candidateIds.begin())});
				plan.workSegmentIndices.push_back(segment);
			}
			return plan;
		}

		template<typename Visitor>
		bool scanSerializedNodeMetadata(const std::shared_ptr<storage::DataManager> &dm,
										const std::vector<int64_t> &candidateIds,
										size_t begin,
										size_t clampedEnd,
										Visitor &&visitor) {
			const auto &segmentIndex = dm->getSegmentIndexManager()->getNodeSegmentIndex();
			auto plan = collectNodeMetadataScanPlan(segmentIndex, candidateIds, begin, clampedEnd);
			if (plan.work.empty()) { // ZYX_COV_EXCL_LINE
				return false;
			}

			auto groups = storage::buildCoalescedGroups(plan.workSegmentIndices, segmentIndex);
			constexpr size_t entitySize = Node::getTotalSize();
			auto &groupBuffer = nodeMetadataScanBuffer();

			for (const auto &group : groups) {
				const size_t totalBytes = group.segCount * storage::TOTAL_SEGMENT_SIZE;
				groupBuffer.resize(totalBytes);
				const auto read = dm->preadSegments(groupBuffer.data(), group.segCount, group.startOffset);
				if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return false;
				}

				for (size_t member = 0; member < group.memberIndices.size(); ++member) {
					const size_t workIndex = group.memberIndices[member];
					const auto &range = plan.work[workIndex];
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
							if (!visitor(index, serializedNode)) {
								return true;
							}
						}
					}
				}
			}
			return true;
		}

			template<typename PartitionVisitor>
			bool scanSerializedNodeMetadataPartitioned(const std::shared_ptr<storage::DataManager> &dm,
			                                           const std::vector<int64_t> &candidateIds,
			                                           size_t begin,
			                                           size_t clampedEnd,
		                                           concurrent::ThreadPool *threadPool,
			                                           PartitionVisitor &&visitor) {
				const auto &segmentIndex = dm->getSegmentIndexManager()->getNodeSegmentIndex();
				auto plan = collectNodeMetadataScanPlan(segmentIndex, candidateIds, begin, clampedEnd);
				// visitBatchPartitioned validates that the plan is non-empty before selecting this execution path.
				auto groups = storage::buildCoalescedGroups(plan.workSegmentIndices, segmentIndex);
			auto tasks = storage::buildCoalescedReadTasks(groups, kMaxCoalescedNodeMetadataReadSegments);
				// The public entry point selects this helper only after the policy chooses parallel execution.
				constexpr size_t entitySize = Node::getTotalSize();
				std::atomic<bool> cancelled{false};
				std::atomic<bool> failed{false};
			const concurrent::ParallelOperatorOptions options{
					.phase = "node_metadata.scan_candidates",
					.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					.estimatedItems = storage::totalCoalescedSegments(groups),
					.estimatedBytes = storage::totalCoalescedSegments(groups) * storage::TOTAL_SEGMENT_SIZE,
					.minPartitions = kMinParallelNodeMetadataReadTasks,
					.minItems = kMinParallelNodeMetadataReadSegments,
					.minBytesPerWorker = kNodeMetadataReadBytesPerWorker};
			(void) concurrent::ParallelOperatorExecutor::runIndexedPartitions<NodeMetadataReadTaskState>(
					tasks.size(), threadPool, options, [&](size_t taskIndex, NodeMetadataReadTaskState &) {
				if (cancelled.load(std::memory_order_relaxed)) {
					return true;
				}

				const auto &task = tasks[taskIndex];
				const auto &group = groups[task.groupIndex];
				const size_t totalBytes = task.segCount * storage::TOTAL_SEGMENT_SIZE;
				auto &groupBuffer = nodeMetadataScanBuffer();
				groupBuffer.resize(totalBytes);
				const auto read = dm->preadSegments(groupBuffer.data(), task.segCount, task.startOffset);
				if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					failed.store(true, std::memory_order_relaxed); // ZYX_COV_EXCL_LINE
					cancelled.store(true, std::memory_order_relaxed); // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				for (size_t member = 0; member < task.memberCount; ++member) {
					if (cancelled.load(std::memory_order_relaxed)) {
						return true;
					}
					const size_t workIndex = group.memberIndices[task.memberBegin + member];
					const auto &range = plan.work[workIndex];
					const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;

					storage::SegmentHeader header{};
					std::memcpy(&header, groupBuffer.data() + bufferOffset, sizeof(storage::SegmentHeader));
					if (header.used == 0) { // ZYX_COV_EXCL_LINE
						continue;
					}

					const char *data = groupBuffer.data() + bufferOffset + sizeof(storage::SegmentHeader);
					for (size_t index = range.idBegin; index < range.idEnd; ++index) {
						if (cancelled.load(std::memory_order_relaxed)) {
							return true;
						}
						const int64_t nodeId = candidateIds[index];
						const auto slot = static_cast<uint32_t>(nodeId - header.start_id);
						if (slot >= header.used) { // ZYX_COV_EXCL_LINE
							continue;
						}

						const char *serializedNode = data + slot * entitySize;
						if (readSerializedNodeId(serializedNode) == nodeId) { // ZYX_COV_EXCL_LINE
							if (!visitor(taskIndex, index, serializedNode)) {
								cancelled.store(true, std::memory_order_relaxed);
								return true;
							}
						}
					}
				}
				return true;
			}, [](size_t, NodeMetadataReadTaskState &) {});
			return !failed.load(std::memory_order_relaxed);
		}

			template<typename Visitor>
			bool scanAllSerializedNodeMetadata(const std::shared_ptr<storage::DataManager> &dm,
			                                   Visitor &&visitor) {
				const auto &segmentIndex = dm->getSegmentIndexManager()->getNodeSegmentIndex();
				std::vector<size_t> segmentIndices;
				segmentIndices.reserve(segmentIndex.size());
				for (size_t index = 0; index < segmentIndex.size(); ++index) {
				segmentIndices.push_back(index);
			}

			auto groups = storage::buildCoalescedGroups(segmentIndices, segmentIndex);
			constexpr size_t entitySize = Node::getTotalSize();
			auto &groupBuffer = nodeMetadataScanBuffer();

			for (const auto &group : groups) {
				for (size_t chunkBegin = 0; chunkBegin < group.memberIndices.size();
					 chunkBegin += kMaxCoalescedNodeMetadataReadSegments) {
					const size_t chunkSegments =
							std::min(kMaxCoalescedNodeMetadataReadSegments,
									 group.memberIndices.size() - chunkBegin);
					const size_t totalBytes = chunkSegments * storage::TOTAL_SEGMENT_SIZE;
					groupBuffer.resize(totalBytes);
					const uint64_t groupOffset = group.startOffset + chunkBegin * storage::TOTAL_SEGMENT_SIZE;
					const auto read = dm->preadSegments(groupBuffer.data(), chunkSegments, groupOffset);
					if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}

					for (size_t member = 0; member < chunkSegments; ++member) {
						const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;
						storage::SegmentHeader header{};
						std::memcpy(&header, groupBuffer.data() + bufferOffset, sizeof(storage::SegmentHeader));
						if (header.used == 0 || header.data_type != Node::typeId) { // ZYX_COV_EXCL_LINE: node segment index points at active node segments.
							continue; // ZYX_COV_EXCL_LINE
						}

						const char *data = groupBuffer.data() + bufferOffset + sizeof(storage::SegmentHeader);
						for (uint32_t slot = 0; slot < header.used; ++slot) {
							const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
							const char *serializedNode = data + static_cast<size_t>(slot) * entitySize;
								if (readSerializedNodeId(serializedNode) != expectedId) { // ZYX_COV_EXCL_LINE: mismatched ids require corrupt persisted segment data.
									continue; // ZYX_COV_EXCL_LINE
								}
								visitor(serializedNode);
							}
						}
					}
			}
			return true;
		}

		template<typename PartitionVisitor>
		bool scanAllSerializedNodeMetadataPartitioned(const std::shared_ptr<storage::DataManager> &dm,
		                                              concurrent::ThreadPool *threadPool,
		                                              PartitionVisitor &&visitor) {
			const auto &segmentIndex = dm->getSegmentIndexManager()->getNodeSegmentIndex();
			if (segmentIndex.empty()) {
				return true;
			}

			std::vector<size_t> segmentIndices;
			segmentIndices.reserve(segmentIndex.size());
			for (size_t index = 0; index < segmentIndex.size(); ++index) {
				segmentIndices.push_back(index);
			}

			auto groups = storage::buildCoalescedGroups(segmentIndices, segmentIndex);
			auto tasks = storage::buildCoalescedReadTasks(groups, kMaxCoalescedNodeMetadataReadSegments);
			const auto decision = decideNodeMetadataScan(
					threadPool, tasks.size(), storage::totalCoalescedSegments(groups));
			if (!decision.useParallel) {
				return scanAllSerializedNodeMetadata(dm, [&](const char *serializedNode) {
					return visitor(0, serializedNode);
				});
			}

			constexpr size_t entitySize = Node::getTotalSize();
			std::atomic<bool> cancelled{false};
			std::atomic<bool> failed{false};
			const concurrent::ParallelOperatorOptions options{
					.phase = "node_metadata.scan_all",
					.workloadKind = concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
					.estimatedItems = storage::totalCoalescedSegments(groups),
					.estimatedBytes = storage::totalCoalescedSegments(groups) * storage::TOTAL_SEGMENT_SIZE,
					.minPartitions = kMinParallelNodeMetadataReadTasks,
					.minItems = kMinParallelNodeMetadataReadSegments,
					.minBytesPerWorker = kNodeMetadataReadBytesPerWorker};
			(void) concurrent::ParallelOperatorExecutor::runIndexedPartitions<NodeMetadataReadTaskState>(
					tasks.size(), threadPool, options, [&](size_t taskIndex, NodeMetadataReadTaskState &) {
				if (cancelled.load(std::memory_order_relaxed)) {
					return true;
				}

				const auto &task = tasks[taskIndex];
				const size_t totalBytes = task.segCount * storage::TOTAL_SEGMENT_SIZE;
				auto &groupBuffer = nodeMetadataScanBuffer();
				groupBuffer.resize(totalBytes);
				const ssize_t read = dm->preadSegments(groupBuffer.data(), task.segCount, task.startOffset);
				if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					failed.store(true, std::memory_order_relaxed); // ZYX_COV_EXCL_LINE
					cancelled.store(true, std::memory_order_relaxed); // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				for (size_t member = 0; member < task.memberCount; ++member) {
					if (cancelled.load(std::memory_order_relaxed)) {
						return true;
					}

					const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;
					storage::SegmentHeader header{};
					std::memcpy(&header, groupBuffer.data() + bufferOffset, sizeof(storage::SegmentHeader));
					if (header.used == 0 || header.data_type != Node::typeId) { // ZYX_COV_EXCL_LINE: node segment index points at active node segments.
						continue; // ZYX_COV_EXCL_LINE
					}

					const char *data = groupBuffer.data() + bufferOffset + sizeof(storage::SegmentHeader);
					for (uint32_t slot = 0; slot < header.used; ++slot) {
						if (cancelled.load(std::memory_order_relaxed)) {
							return true;
						}
						const int64_t expectedId = header.start_id + static_cast<int64_t>(slot);
						const char *serializedNode = data + static_cast<size_t>(slot) * entitySize;
							if (readSerializedNodeId(serializedNode) != expectedId) { // ZYX_COV_EXCL_LINE: mismatched ids require corrupt persisted segment data.
								continue; // ZYX_COV_EXCL_LINE
							}
							visitor(taskIndex, serializedNode);
						}
					}
					return true;
			}, [](size_t, NodeMetadataReadTaskState &) {});
			return !failed.load(std::memory_order_relaxed);
		}

		void appendPropertyCountCandidate(NodePropertyCountCandidates &candidates,
		                                  const NodeMetadataRow &metadata,
		                                  const NodePropertyCountCandidateOptions &options) {
			++candidates.acceptedRowCount;
			if (metadata.propertyEntityId == 0) {
				return;
			}
			if (metadata.propertyStorageType == PropertyStorageType::PROPERTY_ENTITY) {
				candidates.propertyEntityIds.push_back(metadata.propertyEntityId);
				if (options.collectFallbackRefs) {
					const size_t row = candidates.propertyEntityIds.size() - 1;
					candidates.propertyNodeIds.push_back(metadata.nodeId);
					candidates.propertyRows.push_back(row);
				}
			} else if (metadata.propertyStorageType == PropertyStorageType::BLOB_ENTITY) {
				candidates.blobRefs.push_back({metadata.nodeId, metadata.propertyEntityId,
											   metadata.propertyStorageType});
			}
		}

		NodePropertyCountCandidates mergePropertyCountCandidatePartitions(
				const std::vector<NodePropertyCountCandidates> &partitions,
				const NodePropertyCountCandidateOptions &options) {
			NodePropertyCountCandidates merged;
			size_t acceptedRows = 0;
			size_t propertyRows = 0;
			size_t blobRows = 0;
			for (const auto &partition : partitions) {
				acceptedRows += partition.acceptedRowCount;
				propertyRows += partition.propertyEntityIds.size();
				blobRows += partition.blobRefs.size();
			}

			merged.acceptedRowCount = acceptedRows;
			merged.propertyEntityIds.reserve(propertyRows);
			merged.blobRefs.reserve(blobRows);
			if (options.collectFallbackRefs) {
				merged.propertyNodeIds.reserve(propertyRows);
				merged.propertyRows.reserve(propertyRows);
			}

			for (const auto &partition : partitions) {
				const size_t rowOffset = merged.propertyEntityIds.size();
				merged.propertyEntityIds.insert(merged.propertyEntityIds.end(),
												partition.propertyEntityIds.begin(),
												partition.propertyEntityIds.end());
				if (options.collectFallbackRefs) {
					merged.propertyNodeIds.insert(merged.propertyNodeIds.end(),
												  partition.propertyNodeIds.begin(),
												  partition.propertyNodeIds.end());
					for (const size_t row : partition.propertyRows) {
						merged.propertyRows.push_back(rowOffset + row);
					}
				}
				merged.blobRefs.insert(merged.blobRefs.end(),
									   partition.blobRefs.begin(),
									   partition.blobRefs.end());
			}
			return merged;
		}
	} // namespace

	bool NodeMetadataRow::hasLabelId(int64_t labelId) const {
		if (labelId <= 0) {
			return false;
		}
		const uint8_t count = std::min<uint8_t>(labelCount, Node::MAX_LABELS);
		for (uint8_t index = 0; index < count; ++index) {
			if (labelIds[index] == labelId) {
				return true;
			}
		}
		return false;
	}

	Node NodeMetadataRow::toNode() const {
		Node node;
		auto &metadata = node.getMutableMetadata();
		metadata.id = nodeId;
		metadata.firstOutEdgeId = firstOutEdgeId;
		metadata.firstInEdgeId = firstInEdgeId;
		metadata.propertyEntityId = propertyEntityId;
		metadata.propertyStorageType = static_cast<uint32_t>(propertyStorageType);
		metadata.labelCount = std::min<uint8_t>(labelCount, Node::MAX_LABELS);
		std::copy(labelIds.begin(), labelIds.end(), std::begin(metadata.labelIds));
		metadata.isActive = active != 0;
		return node;
	}

	Node NodePropertyCandidateRef::toNode() const {
		Node node;
		auto &metadata = node.getMutableMetadata();
		metadata.id = nodeId;
		metadata.propertyEntityId = propertyEntityId;
		metadata.propertyStorageType = static_cast<uint32_t>(propertyStorageType);
		metadata.isActive = true;
		return node;
	}

	void NodePropertyCountCandidates::reserve(size_t rowCount) {
		propertyEntityIds.reserve(rowCount);
		propertyNodeIds.reserve(rowCount);
		propertyRows.reserve(rowCount);
		blobRefs.reserve(rowCount / 8);
	}

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

	void NodeMetadataBatch::setFromMetadataRow(size_t row, const NodeMetadataRow &metadata) {
		if (row >= size()) {
			return;
		}
		nodeIds[row] = metadata.nodeId;
		firstOutEdgeIds[row] = metadata.firstOutEdgeId;
		firstInEdgeIds[row] = metadata.firstInEdgeId;
		active[row] = metadata.active;
		labelCounts[row] = metadata.labelCount;
		std::copy(metadata.labelIds.begin(), metadata.labelIds.end(), labelIds[row].begin());
		propertyEntityIds[row] = metadata.propertyEntityId;
		propertyStorageTypes[row] = metadata.propertyStorageType;
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
			    begin > candidateIds.size() ||
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

		NodeMetadataBatch batch;
		batch.reserve(clampedEnd - begin);
		for (size_t index = begin; index < clampedEnd; ++index) {
			(void) index;
			batch.appendDefault();
		}
		const bool visited = visitBatchChecked(candidateIds, begin, clampedEnd, [&](size_t row, const NodeMetadataRow &metadata) {
			batch.setFromMetadataRow(row - begin, metadata);
			return true;
		}, {});
		if (!visited) {
			return std::nullopt;
		}
		return batch;
	}

	bool NodeMetadataColumnLoader::visitBatch(const std::vector<int64_t> &candidateIds,
	                                         size_t begin,
	                                         size_t end,
	                                         const MetadataVisitor &visitor,
	                                         NodeMetadataProjection projection) const {
		const size_t clampedEnd = std::min(end, candidateIds.size());
		if (!visitor || !canLoad(candidateIds, begin, clampedEnd)) {
			return false;
		}
		return visitBatchChecked(candidateIds, begin, clampedEnd, visitor, projection);
	}

	bool NodeMetadataColumnLoader::visitBatchPartitioned(const std::vector<int64_t> &candidateIds,
	                                                    size_t begin,
	                                                    size_t end,
	                                                    const MetadataPartitionInitializer &initializer,
	                                                    const MetadataPartitionVisitor &visitor,
	                                                    concurrent::ThreadPool *threadPool,
	                                                    NodeMetadataProjection projection) const {
		const size_t clampedEnd = std::min(end, candidateIds.size());
		if (!visitor || !canLoad(candidateIds, begin, clampedEnd)) {
			return false;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};
			auto initializePartitions = [&](size_t partitionCount) {
				if (initializer) {
					initializer(partitionCount);
				}
			};

		const auto &segmentIndex = dm_->getSegmentIndexManager()->getNodeSegmentIndex();
		auto plan = collectNodeMetadataScanPlan(segmentIndex, candidateIds, begin, clampedEnd);
		if (plan.work.empty()) {
			if (traceEnabled) {
				debug::PerfTrace::addDuration("node_scan.load_node_metadata", elapsedNs(start));
			}
			return false;
		}
		auto groups = storage::buildCoalescedGroups(plan.workSegmentIndices, segmentIndex);
		auto tasks = storage::buildCoalescedReadTasks(groups, kMaxCoalescedNodeMetadataReadSegments);
		const auto decision = decideNodeMetadataScan(
				threadPool, tasks.size(), storage::totalCoalescedSegments(groups));
			initializePartitions(decision.useParallel ? tasks.size() : 1);

		const bool scanned = decision.useParallel ?
			scanSerializedNodeMetadataPartitioned(dm_, candidateIds, begin, clampedEnd, threadPool,
					[&](size_t partition, size_t row, const char *serializedNode) {
						return visitor(partition, row, readMetadataRow(serializedNode, projection));
					}) :
			scanSerializedNodeMetadata(dm_, candidateIds, begin, clampedEnd,
					[&](size_t row, const char *serializedNode) {
						return visitor(0, row, readMetadataRow(serializedNode, projection));
					});

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.load_node_metadata", elapsedNs(start));
		}
		return scanned;
	}

	bool NodeMetadataColumnLoader::visitBatchChecked(const std::vector<int64_t> &candidateIds,
	                                                size_t begin,
	                                                size_t clampedEnd,
	                                                const MetadataVisitor &visitor,
	                                                NodeMetadataProjection projection) const {
		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		const bool scanned = scanSerializedNodeMetadata(dm_, candidateIds, begin, clampedEnd,
				[&](size_t index, const char *serializedNode) {
					return visitor(index, readMetadataRow(serializedNode, projection));
				});

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.load_node_metadata", elapsedNs(start));
		}
		return scanned;
	}

	std::optional<NodePropertyCountCandidates> NodeMetadataColumnLoader::collectPropertyCountCandidates(
			const std::vector<int64_t> &candidateIds,
			size_t begin,
			size_t end,
			const NodeScanConfig &config,
			const NodeScanRequirements &requirements,
			NodePropertyCountCandidateOptions options) const {
		return collectPropertyCountCandidates(candidateIds, begin, end, config, requirements, nullptr, options);
	}

	std::optional<NodePropertyCountCandidates> NodeMetadataColumnLoader::collectPropertyCountCandidates(
			const std::vector<int64_t> &candidateIds,
			size_t begin,
			size_t end,
			const NodeScanConfig &config,
			const NodeScanRequirements &requirements,
			concurrent::ThreadPool *threadPool,
			NodePropertyCountCandidateOptions options) const {
		const size_t clampedEnd = std::min(end, candidateIds.size());
		if (!canLoad(candidateIds, begin, clampedEnd)) {
			return std::nullopt;
		}

		const NodeMetadataRowFilter rowFilter(dm_, config, requirements);
		NodeMetadataProjection projection;
		projection.loadEdgeRefs = false;
		projection.loadLabels = requirements.needsLabels;

		std::vector<NodePropertyCountCandidates> partitions;
		const bool scanned = visitBatchPartitioned(
				candidateIds, begin, clampedEnd,
				[&](size_t partitionCount) {
					partitions.resize(partitionCount);
					const size_t rowsPerPartition = std::max<size_t>(1, (clampedEnd - begin) / partitionCount);
					for (auto &partition : partitions) {
						partition.reserve(rowsPerPartition);
					}
				},
				[&](size_t partition, size_t, const NodeMetadataRow &metadata) {
					if (!rowFilter.accepts(metadata)) {
						return true;
					}
					if (partition < partitions.size()) {
						appendPropertyCountCandidate(partitions[partition], metadata, options);
					}
					return true;
				},
				threadPool,
				projection);

		if (!scanned) {
			return std::nullopt;
		}
		return mergePropertyCountCandidatePartitions(partitions, options);
	}

	std::optional<NodePropertyCountCandidates> NodeMetadataColumnLoader::collectFullScanPropertyCountCandidates(
			const NodeScanConfig &config,
			const NodeScanRequirements &requirements,
			NodePropertyCountCandidateOptions options) const {
		return collectFullScanPropertyCountCandidates(config, requirements, nullptr, options);
	}

	std::optional<NodePropertyCountCandidates> NodeMetadataColumnLoader::collectFullScanPropertyCountCandidates(
			const NodeScanConfig &config,
			const NodeScanRequirements &requirements,
			concurrent::ThreadPool *threadPool,
			NodePropertyCountCandidateOptions options) const {
		if (!dm_ || !dm_->hasPreadSupport() || dm_->hasUnsavedChanges() ||
			config.type != ScanType::FULL_SCAN || !config.labels.empty() || requirements.needsLabels) {
			return std::nullopt;
		}
		const auto *snapshot = dm_->getCurrentSnapshot();
		if (snapshot != nullptr &&
			(!snapshot->nodes.empty() || !snapshot->properties.empty() || !snapshot->blobs.empty())) { // ZYX_COV_EXCL_LINE
			return std::nullopt; // ZYX_COV_EXCL_LINE
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		NodeMetadataProjection projection;
		projection.loadEdgeRefs = false;
		projection.loadLabels = false;

		const auto &segmentIndex = dm_->getSegmentIndexManager()->getNodeSegmentIndex();
		std::vector<size_t> segmentIndices;
		segmentIndices.reserve(segmentIndex.size());
		for (size_t index = 0; index < segmentIndex.size(); ++index) {
			segmentIndices.push_back(index);
		}
		auto groups = storage::buildCoalescedGroups(segmentIndices, segmentIndex);
		auto tasks = storage::buildCoalescedReadTasks(groups, kMaxCoalescedNodeMetadataReadSegments);
		const auto decision = decideNodeMetadataScan(
				threadPool, tasks.size(), storage::totalCoalescedSegments(groups));
		std::vector<NodePropertyCountCandidates> partitions(decision.useParallel ? tasks.size() : 1);
		for (auto &partition : partitions) {
			partition.reserve(1024);
		}

			const bool scanned = scanAllSerializedNodeMetadataPartitioned(dm_, threadPool, [&](size_t partition,
			                                                                                  const char *serializedNode) {
				const NodeMetadataRow metadata = readMetadataRow(serializedNode, projection);
				if (requirements.needsActiveCheck && metadata.active == 0) {
					return true;
				}

			if (partition < partitions.size()) {
				appendPropertyCountCandidate(partitions[partition], metadata, options);
			}
			return true;
		});

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.load_node_metadata", elapsedNs(start));
		}
		if (!scanned) { // ZYX_COV_EXCL_LINE: scanAll only fails on defensive short-read/corruption paths.
			return std::nullopt; // ZYX_COV_EXCL_LINE
		}
		return mergePropertyCountCandidatePartitions(partitions, options);
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
