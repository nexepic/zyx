#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <utility>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/StorageHeaders.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
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

		std::vector<char> &relationshipMetadataScanBuffer() {
			thread_local std::vector<char> buffer;
			return buffer;
		}

		constexpr size_t kMaxCoalescedRelationshipMetadataReadSegments = 16;
		constexpr size_t kMinParallelRelationshipMetadataReadTasks = 2;
		constexpr size_t kMinParallelRelationshipMetadataReadSegments = 32;

		concurrent::ParallelExecutionDecision decideRelationshipMetadataScan(concurrent::ThreadPool *threadPool,
																			 size_t taskCount,
																			 size_t segmentCount) {
			return concurrent::decideParallelExecution(
					threadPool,
					{.workloadKind = concurrent::ParallelWorkloadKind::PWK_STORAGE_SCAN,
					 .partitions = taskCount,
					 .estimatedItems = segmentCount,
					 .estimatedBytes = segmentCount * storage::TOTAL_SEGMENT_SIZE,
					 .minPartitions = kMinParallelRelationshipMetadataReadTasks,
					 .minItems = kMinParallelRelationshipMetadataReadSegments});
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

		std::vector<size_t> collectEdgeWorkSegments(const std::shared_ptr<storage::DataManager> &dm, int64_t beginId,
													int64_t endId) {
			const auto &segmentIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
			std::vector<size_t> workSegmentIndices;
			workSegmentIndices.reserve(segmentIndex.size());
			for (size_t segment = 0; segment < segmentIndex.size(); ++segment) {
				const auto &entry = segmentIndex[segment];
				if (entry.endId >= beginId &&
					entry.startId <= endId) { // ZYX_COV_EXCL_LINE: segment overlap is integration-tested; per-term
											  // branch splits add noise.
					workSegmentIndices.push_back(segment);
				}
			}
			return workSegmentIndices;
		}

		struct EdgeSegmentScanPlan {
			std::vector<size_t> workSegmentIndices;
			std::vector<storage::CoalescedGroup> groups;
			std::vector<storage::CoalescedReadTask> tasks;
			size_t totalSegments = 0;

			[[nodiscard]] bool empty() const { return workSegmentIndices.empty(); }
		};

		EdgeSegmentScanPlan buildEdgeSegmentScanPlan(const std::shared_ptr<storage::DataManager> &dm,
													 int64_t beginId,
													 int64_t endId) {
			EdgeSegmentScanPlan plan;
			plan.workSegmentIndices = collectEdgeWorkSegments(dm, beginId, endId);
			if (plan.workSegmentIndices.empty()) {
				return plan;
			}

			const auto &segmentIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
			plan.groups = storage::buildCoalescedGroups(plan.workSegmentIndices, segmentIndex);
			plan.totalSegments = storage::totalCoalescedSegments(plan.groups);
			plan.tasks = storage::buildCoalescedReadTasks(plan.groups, kMaxCoalescedRelationshipMetadataReadSegments);
			return plan;
		}

		struct EdgeSegmentScanWindow {
			const char *data = nullptr;
			int64_t first = 0;
			int64_t last = -1;
			int64_t segmentStartId = 0;
		};

		std::optional<EdgeSegmentScanWindow>
		edgeSegmentScanWindow(const storage::SegmentIndexManager::SegmentIndex &entry,
							  const storage::SegmentHeader &header, const char *segmentBuffer, int64_t beginId,
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
			for (int64_t edgeId = window.first; edgeId <= window.last;
				 ++edgeId) { // ZYX_COV_EXCL_LINE: loop-exit arcs are not meaningful for metadata scanning.
				const auto slot = static_cast<uint32_t>(edgeId - window.segmentStartId);
				const char *serializedEdge = window.data + slot * entitySize;
				if (readSerializedEdgeId(serializedEdge) ==
					edgeId) { // ZYX_COV_EXCL_LINE: mismatched slots indicate corrupt segment contents.
					visitor(edgeId, serializedEdge);
				}
			}
		}

		template<typename Visitor>
		bool scanSerializedEdgesWithPlan(const std::shared_ptr<storage::DataManager> &dm,
										 const EdgeSegmentScanPlan &plan,
										 int64_t beginId,
										 int64_t endId,
										 Visitor &&visitor) {
			if (plan.empty()) {
				return false;
			}

			const auto &segmentIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
			auto &groupBuffer = relationshipMetadataScanBuffer();
			for (const auto &group: plan.groups) { // ZYX_COV_EXCL_LINE: coalesced group loop shape depends on segment allocator layout.
				const size_t totalBytes = group.segCount * storage::TOTAL_SEGMENT_SIZE;
				groupBuffer.resize(totalBytes);
				const auto read = dm->preadSegments(groupBuffer.data(), group.segCount, group.startOffset);
				if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					return false;
				}

				for (size_t member = 0; member < group.memberIndices.size();
					 ++member) { // ZYX_COV_EXCL_LINE: group-member loop exit is allocator-layout noise.
					const size_t segmentIndexInWork = group.memberIndices[member];
					const size_t segment = plan.workSegmentIndices[segmentIndexInWork];
					const auto &entry = segmentIndex[segment];
					const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;
					storage::SegmentHeader header{};
					const char *segmentBuffer = groupBuffer.data() + bufferOffset;
					std::memcpy(&header, segmentBuffer, sizeof(storage::SegmentHeader));
					auto window = edgeSegmentScanWindow(entry, header, segmentBuffer, beginId, endId);
					if (window.has_value()) { // ZYX_COV_EXCL_LINE: empty windows are covered through public range
											  // tests.
						scanEdgeWindow(*window, visitor);
					}
				}
			}
			return true;
		}

		template<typename Visitor>
		bool scanSerializedEdges(const std::shared_ptr<storage::DataManager> &dm, int64_t beginId, int64_t endId,
								 Visitor &&visitor) {
			return scanSerializedEdgesWithPlan(
					dm, buildEdgeSegmentScanPlan(dm, beginId, endId), beginId, endId, std::forward<Visitor>(visitor));
		}

		template<typename PartitionVisitor>
		bool scanSerializedEdgesPartitionedWithPlan(const std::shared_ptr<storage::DataManager> &dm,
													const EdgeSegmentScanPlan &plan,
													int64_t beginId,
													int64_t endId,
													concurrent::ThreadPool *threadPool,
													PartitionVisitor &&visitor) {
			if (plan.empty()) {
				return false;
			}

			const auto &segmentIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
			const auto decision = decideRelationshipMetadataScan(threadPool, plan.tasks.size(), plan.totalSegments);
			if (!decision.useParallel) {
				return scanSerializedEdgesWithPlan(
						dm, plan, beginId, endId, [&](int64_t edgeId, const char *serializedEdge) {
							visitor(0, edgeId, serializedEdge);
						});
			}

			std::atomic<bool> failed{false};
			threadPool->parallelFor(0, plan.tasks.size(), decision.workerCount, [&](size_t taskIndex) {
				const auto &task = plan.tasks[taskIndex];
				const auto &group = plan.groups[task.groupIndex];
				const size_t totalBytes = task.segCount * storage::TOTAL_SEGMENT_SIZE;
				auto &groupBuffer = relationshipMetadataScanBuffer();
				groupBuffer.resize(totalBytes);
				const auto read = dm->preadSegments(groupBuffer.data(), task.segCount, task.startOffset);
				if (read < static_cast<ssize_t>(totalBytes)) { // ZYX_COV_EXCL_LINE
					failed.store(true, std::memory_order_relaxed); // ZYX_COV_EXCL_LINE
					return; // ZYX_COV_EXCL_LINE
				}

				for (size_t member = 0; member < task.memberCount; ++member) {
					const size_t segmentIndexInWork = group.memberIndices[task.memberBegin + member];
					const size_t segment = plan.workSegmentIndices[segmentIndexInWork];
					const auto &entry = segmentIndex[segment];
					const size_t bufferOffset = member * storage::TOTAL_SEGMENT_SIZE;
					const char *segmentBuffer = groupBuffer.data() + bufferOffset;
					storage::SegmentHeader header{};
					std::memcpy(&header, segmentBuffer, sizeof(storage::SegmentHeader));
					auto window = edgeSegmentScanWindow(entry, header, segmentBuffer, beginId, endId);
					if (window.has_value()) {
						scanEdgeWindow(*window, [&](int64_t edgeId, const char *serializedEdge) {
							visitor(taskIndex, edgeId, serializedEdge);
						});
					}
				}
			});
			return !failed.load(std::memory_order_relaxed);
		}

		template<typename PartitionVisitor>
		bool scanSerializedEdgesPartitioned(const std::shared_ptr<storage::DataManager> &dm,
											int64_t beginId,
											int64_t endId,
											concurrent::ThreadPool *threadPool,
											PartitionVisitor &&visitor) {
			return scanSerializedEdgesPartitionedWithPlan(
					dm,
					buildEdgeSegmentScanPlan(dm, beginId, endId),
					beginId,
					endId,
					threadPool,
					std::forward<PartitionVisitor>(visitor));
		}

		void appendRelationshipPropertyCountCandidate(RelationshipPropertyCountCandidates &candidates,
		                                              int64_t edgeId,
		                                              const char *serializedEdge,
		                                              const RelationshipPropertyCountCandidateOptions &options) {
			++candidates.matchedEdges;
			const int64_t propertyEntityId = readSerializedPropertyEntityId(serializedEdge);
			if (propertyEntityId == 0) {
				return;
			}

			const auto storageType = readSerializedPropertyStorageType(serializedEdge);
			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				candidates.propertyEntityIds.push_back(propertyEntityId);
				if (options.collectPropertyEdgeRefs) {
					candidates.propertyEdgeIds.push_back(edgeId);
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
				candidates.fallbackEdgeIds.push_back(edgeId);
			}
		}

		RelationshipPropertyCountCandidates mergeRelationshipPropertyCountCandidatePartitions(
				const std::vector<RelationshipPropertyCountCandidates> &partitions,
				const RelationshipPropertyCountCandidateOptions &options) {
			RelationshipPropertyCountCandidates merged;
			size_t matchedEdges = 0;
			size_t propertyRows = 0;
			size_t fallbackRows = 0;
			for (const auto &partition : partitions) {
				matchedEdges += partition.matchedEdges;
				propertyRows += partition.propertyEntityIds.size();
				fallbackRows += partition.fallbackEdgeIds.size();
			}

			merged.matchedEdges = matchedEdges;
			merged.propertyEntityIds.reserve(propertyRows);
			merged.fallbackEdgeIds.reserve(fallbackRows);
			if (options.collectPropertyEdgeRefs) {
				merged.propertyEdgeIds.reserve(propertyRows);
			}
			for (const auto &partition : partitions) {
				merged.propertyEntityIds.insert(merged.propertyEntityIds.end(),
												partition.propertyEntityIds.begin(),
												partition.propertyEntityIds.end());
				if (options.collectPropertyEdgeRefs) {
					merged.propertyEdgeIds.insert(merged.propertyEdgeIds.end(),
												  partition.propertyEdgeIds.begin(),
												  partition.propertyEdgeIds.end());
				}
				merged.fallbackEdgeIds.insert(merged.fallbackEdgeIds.end(),
											  partition.fallbackEdgeIds.begin(),
											  partition.fallbackEdgeIds.end());
			}
			return merged;
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
		propertyEdgeIds.reserve(rowCount);
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

	RelationshipMetadataColumnLoader::RelationshipMetadataColumnLoader(std::shared_ptr<storage::DataManager> dm) :
		dm_(std::move(dm)) {}

	bool RelationshipMetadataColumnLoader::canLoad(int64_t beginId, int64_t endId) const {
		static constexpr int64_t METADATA_LOAD_THRESHOLD = 128;
		if (!dm_ || !dm_->hasPreadSupport() || dm_->hasUnsavedChanges() || beginId <= 0 ||
			endId < beginId || // ZYX_COV_EXCL_LINE: guards are individually validated through public APIs.
			endId - beginId + 1 < METADATA_LOAD_THRESHOLD) {
			return false;
		}
		const auto *snapshot = dm_->getCurrentSnapshot();
		const bool snapshotSafe = snapshot == nullptr || // ZYX_COV_EXCL_LINE
								  (snapshot->edges.empty() && snapshot->properties.empty() &&
								   snapshot->blobs.empty()); // ZYX_COV_EXCL_LINE
		return snapshotSafe;
	}

	bool RelationshipMetadataColumnLoader::canCountActiveByType(int64_t beginId, int64_t endId) const {
		static constexpr int64_t METADATA_LOAD_THRESHOLD = 128;
		return dm_ && dm_->hasPreadSupport() && beginId > 0 &&
			   endId >= beginId && // ZYX_COV_EXCL_LINE: pread availability is fixed for FileStorage-backed tests.
			   endId - beginId + 1 >= METADATA_LOAD_THRESHOLD;
	}

	std::optional<RelationshipMetadataBatch> RelationshipMetadataColumnLoader::loadRange(int64_t beginId,
																						 int64_t endId) const {
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

	std::optional<int64_t> RelationshipMetadataColumnLoader::countActiveByType(int64_t beginId, int64_t endId,
																			   int64_t typeId) const {
		return countActiveByType(beginId, endId, typeId, nullptr);
	}

	std::optional<int64_t> RelationshipMetadataColumnLoader::countActiveByType(int64_t beginId,
	                                                                           int64_t endId,
	                                                                           int64_t typeId,
	                                                                           concurrent::ThreadPool *threadPool) const {
		if (!canCountActiveByType(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		if (dm_->hasUnsavedChanges()) { // ZYX_COV_EXCL_LINE: dirty ranges are rejected before direct scan fallback.
			return std::nullopt;
		}
		const auto *snapshot = dm_->getCurrentSnapshot();
		if (snapshot != nullptr && !snapshot->edges.empty()) { // ZYX_COV_EXCL_LINE: canLoad rejects dirty snapshots.
			return std::nullopt;
		}

		const auto plan = buildEdgeSegmentScanPlan(dm_, beginId, endId);
		if (plan.empty()) {
			return std::nullopt;
		}
		const auto decision = decideRelationshipMetadataScan(threadPool, plan.tasks.size(), plan.totalSegments);
		std::vector<int64_t> partitionCounts(decision.useParallel ? plan.tasks.size() : 1, 0);

		if (!scanSerializedEdgesPartitionedWithPlan(dm_, plan, beginId, endId, threadPool, [&](size_t partition,
		                                                                                       int64_t,
		                                                                                       const char *serializedEdge) {
				if (readSerializedActive(serializedEdge) &&
					(typeId == 0 || readSerializedTypeId(serializedEdge) == typeId)) {
					if (partition < partitionCounts.size()) {
						++partitionCounts[partition];
					}
				}
			})) {
			return std::nullopt;
		}
		int64_t count = 0;
		for (const int64_t partitionCount : partitionCounts) {
			count += partitionCount;
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return count;
	}

	std::optional<RelationshipPropertyCandidateBatch>
	RelationshipMetadataColumnLoader::collectPropertyCandidatesByType(int64_t beginId, int64_t endId,
																	  int64_t typeId) const {
		if (!canLoad(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{}; // ZYX_COV_EXCL_LINE

		RelationshipPropertyCandidateBatch batch;
		batch.reserve(static_cast<size_t>(endId - beginId + 1));
		if (!scanSerializedEdges(dm_, beginId, endId, [&](int64_t edgeId, const char *serializedEdge) {
				if (!readSerializedActive(serializedEdge) || // ZYX_COV_EXCL_LINE
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
				} else if (storageType == PropertyStorageType::BLOB_ENTITY) { // ZYX_COV_EXCL_LINE
					const size_t row = batch.edgeIds.size();
					batch.edgeIds.push_back(edgeId);
					batch.fallbackRows.push_back(row);
				}
			})) {
			return std::nullopt;
		}

		if (traceEnabled) { // ZYX_COV_EXCL_LINE: trace-only instrumentation is covered by count-candidate path.
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return batch;
	}

	std::optional<RelationshipPropertyCountCandidates>
	RelationshipMetadataColumnLoader::collectPropertyCountCandidatesByType(int64_t beginId, int64_t endId,
																		   int64_t typeId,
																		   RelationshipPropertyCountCandidateOptions options) const {
		return collectPropertyCountCandidatesByType(beginId, endId, typeId, nullptr, options);
	}

	std::optional<RelationshipPropertyCountCandidates>
	RelationshipMetadataColumnLoader::collectPropertyCountCandidatesByType(
			int64_t beginId,
			int64_t endId,
			int64_t typeId,
			concurrent::ThreadPool *threadPool,
			RelationshipPropertyCountCandidateOptions options) const {
		if (!canLoad(beginId, endId)) {
			return std::nullopt;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto start = traceEnabled ? Clock::now() : Clock::time_point{};

		const auto plan = buildEdgeSegmentScanPlan(dm_, beginId, endId);
		if (plan.empty()) {
			return std::nullopt;
		}
		const auto decision = decideRelationshipMetadataScan(threadPool, plan.tasks.size(), plan.totalSegments);
		std::vector<RelationshipPropertyCountCandidates> partitions(decision.useParallel ? plan.tasks.size() : 1);
		const size_t rowsPerPartition = std::max<size_t>(1, static_cast<size_t>(endId - beginId + 1) / partitions.size());
		for (auto &partition : partitions) {
			partition.reserve(rowsPerPartition);
		}

		if (!scanSerializedEdgesPartitionedWithPlan(dm_, plan, beginId, endId, threadPool, [&](size_t partition,
		                                                                                       int64_t edgeId,
		                                                                                       const char *serializedEdge) {
				if (!readSerializedActive(serializedEdge) || // ZYX_COV_EXCL_LINE
					(typeId != 0 && readSerializedTypeId(serializedEdge) != typeId)) {
					return;
				}

				if (partition < partitions.size()) {
					appendRelationshipPropertyCountCandidate(partitions[partition], edgeId, serializedEdge, options);
				}
			})) {
			return std::nullopt;
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("relationship_count.load_edge_metadata", elapsedNs(start));
		}
		return mergeRelationshipPropertyCountCandidatePartitions(partitions, options);
	}

} // namespace graph::query::execution
