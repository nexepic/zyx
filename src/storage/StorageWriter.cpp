/**
 * @file StorageWriter.cpp
 * @brief Entity write engine implementation
 *
 * Methods moved from FileStorage.cpp with mechanical reference adjustments.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/StorageWriter.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <future>
#include <sstream>
#include <type_traits>
#include <vector>
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/PersistenceManager.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {
	namespace {
		template<typename T>
		void appendPod(char *&dest, const T &value) {
			std::memcpy(dest, &value, sizeof(T));
			dest += sizeof(T);
		}

		class FixedBufferWriter {
		public:
			FixedBufferWriter(char *dest, size_t size) : cursor_(dest), end_(dest + size) {}

			template<typename T>
			void writePod(const T &value) {
				static_assert(std::is_trivial_v<T>, "writePod expects a trivial value");
				writeBytes(&value, sizeof(T));
			}

			void writeString(const std::string &value) {
				writePod(static_cast<uint32_t>(value.size()));
				writeBytes(value.data(), value.size());
			}

		private:
			void writeBytes(const void *src, size_t size) {
				if (static_cast<size_t>(end_ - cursor_) < size) {
					throw std::runtime_error("Object serialized size exceeds allocated fixed size");
				}
				if (size > 0) {
					std::memcpy(cursor_, src, size);
					cursor_ += size;
				}
			}

			char *cursor_;
			char *end_;
		};

		void writePropertyValue(FixedBufferWriter &writer, const PropertyValue &value) {
			std::visit(
					[&writer](const auto &arg) {
						using ValueType = std::decay_t<decltype(arg)>;

						if constexpr (std::is_same_v<ValueType, std::monostate>) {
							writer.writePod(PropertyType::NULL_TYPE);
						} else if constexpr (std::is_same_v<ValueType, bool>) {
							writer.writePod(PropertyType::BOOLEAN);
							writer.writePod(arg);
						} else if constexpr (std::is_same_v<ValueType, int64_t>) {
							writer.writePod(PropertyType::INTEGER);
							writer.writePod(arg);
						} else if constexpr (std::is_same_v<ValueType, double>) {
							writer.writePod(PropertyType::DOUBLE);
							writer.writePod(arg);
						} else if constexpr (std::is_same_v<ValueType, std::string>) {
							writer.writePod(PropertyType::STRING);
							writer.writeString(arg);
						} else if constexpr (std::is_same_v<ValueType, std::vector<PropertyValue>>) {
							writer.writePod(PropertyType::LIST);
							writer.writePod(static_cast<uint32_t>(arg.size()));
							for (const auto &element: arg) {
								writePropertyValue(writer, element);
							}
						} else if constexpr (std::is_same_v<ValueType, PropertyValue::MapType>) {
							writer.writePod(PropertyType::MAP);
							writer.writePod(static_cast<uint32_t>(arg.size()));
							for (const auto &[key, mapValue]: arg) {
								writer.writeString(key);
								writePropertyValue(writer, mapValue);
							}
						} else if constexpr (std::is_same_v<ValueType, TemporalDate>) {
							writer.writePod(PropertyType::DATE);
							writer.writePod(arg.epochDays);
						} else if constexpr (std::is_same_v<ValueType, TemporalDateTime>) {
							writer.writePod(PropertyType::DATETIME);
							writer.writePod(arg.epochMillis);
						} else if constexpr (std::is_same_v<ValueType, TemporalDuration>) {
							writer.writePod(PropertyType::DURATION);
							writer.writePod(arg.months);
							writer.writePod(arg.days);
							writer.writePod(arg.nanos);
						}
					},
					value.getVariant());
		}

		void serializeFixedEntityInto(char *dest, const Node &node, size_t fixedSize) {
			if (fixedSize < Node::getTotalSize()) { // ZYX_COV_EXCL_LINE
				throw std::runtime_error("Node fixed-size buffer is too small");
			}

			std::memset(dest, 0, fixedSize);
			char *out = dest;
			const auto &metadata = node.getMetadata();
			appendPod(out, metadata.id);
			appendPod(out, metadata.firstOutEdgeId);
			appendPod(out, metadata.firstInEdgeId);
			appendPod(out, metadata.propertyEntityId);
			for (uint8_t i = 0; i < Node::MAX_LABELS; ++i) {
				appendPod(out, metadata.labelIds[i]);
			}
			appendPod(out, metadata.labelCount);
			appendPod(out, metadata.propertyStorageType);
			appendPod(out, metadata.isActive);
		}

		void serializeFixedEntityInto(char *dest, const Edge &edge, size_t fixedSize) {
			if (fixedSize < Edge::getTotalSize()) { // ZYX_COV_EXCL_LINE
				throw std::runtime_error("Edge fixed-size buffer is too small");
			}

			std::memset(dest, 0, fixedSize);
			char *out = dest;
			const auto &metadata = edge.getMetadata();
			appendPod(out, metadata.id);
			appendPod(out, metadata.sourceNodeId);
			appendPod(out, metadata.targetNodeId);
			appendPod(out, metadata.nextOutEdgeId);
			appendPod(out, metadata.prevOutEdgeId);
			appendPod(out, metadata.nextInEdgeId);
			appendPod(out, metadata.prevInEdgeId);
			appendPod(out, metadata.propertyEntityId);
			appendPod(out, metadata.typeId);
			appendPod(out, metadata.propertyStorageType);
			appendPod(out, metadata.isActive);
		}

		void serializeFixedEntityInto(char *dest, const Blob &blob, size_t fixedSize) {
			if (fixedSize < Blob::getTotalSize()) { // ZYX_COV_EXCL_LINE
				throw std::runtime_error("Blob fixed-size buffer is too small");
			}

			std::memset(dest, 0, fixedSize);
			char *out = dest;
			const auto &metadata = blob.getMetadata();
			appendPod(out, metadata.id);
			appendPod(out, metadata.entityId);
			appendPod(out, metadata.nextBlobId);
			appendPod(out, metadata.prevBlobId);
			appendPod(out, metadata.dataSize);
			appendPod(out, metadata.entityType);
			appendPod(out, metadata.originalSize);
			appendPod(out, metadata.chainPosition);
			appendPod(out, metadata.compressed);
			appendPod(out, metadata.isActive);
			std::memcpy(out, blob.getData(), Blob::CHUNK_SIZE);
		}

		void serializeFixedEntityInto(char *dest, const State &state, size_t fixedSize) {
			if (fixedSize < State::getTotalSize()) { // ZYX_COV_EXCL_LINE
				throw std::runtime_error("State fixed-size buffer is too small");
			}
			if (state.getSize() > State::CHUNK_SIZE) { // ZYX_COV_EXCL_LINE
				throw std::runtime_error("State data exceeds fixed-size inline storage");
			}

			std::memset(dest, 0, fixedSize);
			char *out = dest;
			const auto &metadata = state.getMetadata();
			appendPod(out, metadata.id);
			appendPod(out, metadata.nextStateId);
			appendPod(out, metadata.prevStateId);
			appendPod(out, metadata.externalId);
			appendPod(out, metadata.dataSize);
			appendPod(out, metadata.chainPosition);
			std::memcpy(out, metadata.key, State::MAX_KEY_LENGTH);
			out += State::MAX_KEY_LENGTH;
			appendPod(out, metadata.isActive);
			if (metadata.dataSize > 0) {
				std::memcpy(out, state.getData(), metadata.dataSize);
			}
		}

		void serializeFixedEntityInto(char *dest, const Property &property, size_t fixedSize) {
			if (fixedSize < Property::getTotalSize()) { // ZYX_COV_EXCL_LINE
				throw std::runtime_error("Property fixed-size buffer is too small");
			}

			std::memset(dest, 0, fixedSize);
			char *out = dest;
			const auto &metadata = property.getMetadata();
			appendPod(out, metadata.id);
			appendPod(out, metadata.entityId);
			appendPod(out, metadata.entityType);
			appendPod(out, metadata.isActive);

			const auto &payload = property.getSerializedPropertyPayload();
			if (!payload.empty()) {
				const auto used = static_cast<size_t>(out - dest);
				if (payload.size() > fixedSize - used) {
					throw std::runtime_error("Property serialized payload exceeds allocated fixed size");
				}
				std::memcpy(out, payload.data(), payload.size());
				return;
			}

			FixedBufferWriter writer(out, fixedSize - static_cast<size_t>(out - dest));
			const auto &values = property.getPropertyValues();
			writer.writePod(static_cast<uint32_t>(values.size()));
			for (const auto &[key, value]: values) {
				writer.writeString(key);
				writePropertyValue(writer, value);
			}
		}

		template<typename T>
		void serializeFixedEntityInto(char *dest, const T &entity, size_t fixedSize) {
			utils::FixedSizeSerializer::serializeInto(dest, entity, fixedSize);
		}

		template<typename T>
		void orderEntityRefsById(std::vector<const T *> &entities) {
			if (entities.size() < 2) {
				return;
			}

			auto sortById = [](std::vector<const T *> &refs) {
				std::sort(refs.begin(), refs.end(), [](const T *a, const T *b) { return a->getId() < b->getId(); });
			};
			auto [minIt, maxIt] = std::minmax_element(
					entities.begin(), entities.end(), [](const T *a, const T *b) { return a->getId() < b->getId(); });
			const int64_t minId = (*minIt)->getId();
			const int64_t maxId = (*maxIt)->getId();
			if (minId < 0 || maxId < minId) { // ZYX_COV_EXCL_LINE
				sortById(entities);
				return;
			}

			const auto range = static_cast<uint64_t>(maxId - minId) + 1;
			constexpr uint64_t kMaxDenseOrderingRange = 4 * 1024 * 1024;
			if (range > kMaxDenseOrderingRange || range > entities.size() * 4ULL) { // ZYX_COV_EXCL_LINE
				sortById(entities);
				return;
			}

			std::vector<const T *> buckets(static_cast<size_t>(range), nullptr);
			size_t filled = 0;
			for (const T *entity : entities) {
				auto index = static_cast<size_t>(entity->getId() - minId);
				if (buckets[index] == nullptr) {
					++filled;
				}
				buckets[index] = entity;
			}

			if (filled != entities.size()) { // ZYX_COV_EXCL_LINE
				sortById(entities);
				return;
			}

			entities.clear();
			for (const T *entity : buckets) {
				if (entity != nullptr) {
					entities.push_back(entity);
				}
			}
		}
	} // namespace

	StorageWriter::StorageWriter(std::shared_ptr<StorageIO> io,
								 std::shared_ptr<SegmentTracker> tracker,
								 std::shared_ptr<SegmentAllocator> allocator,
								 std::shared_ptr<DataManager> dataManager,
								 IDAllocators allocators,
								 FileHeader &fileHeader) :
		io_(std::move(io)),
		tracker_(std::move(tracker)),
		allocator_(std::move(allocator)),
		dataManager_(std::move(dataManager)),
		allocators_(std::move(allocators)),
		fileHeader_(fileHeader) {}

	// ── classifyEntities (free function) ────────────────────────────────────

	template<typename EntityType>
	SaveBatch<EntityType> classifyEntities(
			const std::unordered_map<int64_t, DirtyEntityInfo<EntityType>> &map) {
		SaveBatch<EntityType> batch;
		batch.added.reserve(map.size());
		for (const auto &[id, info] : map) {
			if (!info.backup.has_value()) continue;
			switch (info.changeType) {
				case EntityChangeType::CHANGE_ADDED:
					batch.added.push_back(&*info.backup);
					break;
				case EntityChangeType::CHANGE_MODIFIED:
					batch.modified.push_back(&*info.backup);
					break;
				case EntityChangeType::CHANGE_DELETED:
					batch.deleted.push_back(&*info.backup);
					break;
			}
		}
		return batch;
	}

	// Explicit instantiations for classifyEntities
	template SaveBatch<Node> classifyEntities(const std::unordered_map<int64_t, DirtyEntityInfo<Node>> &);
	template SaveBatch<Edge> classifyEntities(const std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &);
	template SaveBatch<Property> classifyEntities(const std::unordered_map<int64_t, DirtyEntityInfo<Property>> &);
	template SaveBatch<Blob> classifyEntities(const std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &);
	template SaveBatch<Index> classifyEntities(const std::unordered_map<int64_t, DirtyEntityInfo<Index>> &);
	template SaveBatch<State> classifyEntities(const std::unordered_map<int64_t, DirtyEntityInfo<State>> &);

	// ── writeSnapshot ───────────────────────────────────────────────────────

	void StorageWriter::writeSnapshot(FlushSnapshot &snapshot, concurrent::ThreadPool *threadPool) {
		{
			std::lock_guard lock(pendingSegmentStateMutex_);
			touchedSegments_.clear();
		}
		writeSnapshotMaps(snapshot.nodes, snapshot.edges, snapshot.properties, snapshot.blobs, snapshot.indexes,
						  snapshot.states, threadPool);
	}

	void StorageWriter::writeSnapshot(FlushSnapshotView &snapshot, concurrent::ThreadPool *threadPool) {
		{
			std::lock_guard lock(pendingSegmentStateMutex_);
			touchedSegments_.clear();
		}
		if (snapshot.isEmpty()) {
			return;
		}
		writeSnapshotMaps(*snapshot.nodes, *snapshot.edges, *snapshot.properties, *snapshot.blobs, *snapshot.indexes,
						  *snapshot.states, threadPool);
	}

	std::vector<uint64_t> StorageWriter::takeTouchedSegments() {
		std::lock_guard lock(pendingSegmentStateMutex_);
		std::vector<uint64_t> segments(touchedSegments_.begin(), touchedSegments_.end());
		std::ranges::sort(segments);
		touchedSegments_.clear();
		return segments;
	}

	template<typename NodeMap, typename EdgeMap, typename PropertyMap, typename BlobMap, typename IndexMap,
			 typename StateMap>
	void StorageWriter::writeSnapshotMaps(const NodeMap &nodes, const EdgeMap &edges, const PropertyMap &properties,
										  const BlobMap &blobs, const IndexMap &indexes, const StateMap &states,
										  concurrent::ThreadPool *threadPool) {
		using Clock = std::chrono::steady_clock;

		// Classify entities by change type for all entity types.
		auto prepStart = Clock::now();

		auto allBatches = std::tuple<SaveBatch<Node>, SaveBatch<Edge>, SaveBatch<Property>, SaveBatch<Blob>,
									 SaveBatch<Index>, SaveBatch<State>>{};
		const size_t dirtyEntityCount =
				nodes.size() + edges.size() + properties.size() + blobs.size() + indexes.size() + states.size();
		const auto classifySerial = [&]() {
			std::get<0>(allBatches) = classifyEntities(nodes);
			std::get<1>(allBatches) = classifyEntities(edges);
			std::get<2>(allBatches) = classifyEntities(properties);
			std::get<3>(allBatches) = classifyEntities(blobs);
			std::get<4>(allBatches) = classifyEntities(indexes);
			std::get<5>(allBatches) = classifyEntities(states);
		};
		if (concurrent::hasParallelWorkers(threadPool) && dirtyEntityCount >= 4096) {
			struct SavePrepareState {};
			const concurrent::ParallelOperatorOptions options{
					.phase = "save.prepare.parallel",
					.workloadKind = concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
					.estimatedItems = dirtyEntityCount,
					.minPartitions = 2,
					.minItems = 4096,
					.minItemsPerWorker = 1024,
					.maxWorkers = 6};
			(void) concurrent::ParallelOperatorExecutor::runIndexedPartitions<SavePrepareState>(
					6,
					threadPool,
					options,
					[&](size_t partition, SavePrepareState &) {
						switch (partition) {
							case 0:
								std::get<0>(allBatches) = classifyEntities(nodes);
								break;
							case 1:
								std::get<1>(allBatches) = classifyEntities(edges);
								break;
							case 2:
								std::get<2>(allBatches) = classifyEntities(properties);
								break;
							case 3:
								std::get<3>(allBatches) = classifyEntities(blobs);
								break;
							case 4:
								std::get<4>(allBatches) = classifyEntities(indexes);
								break;
							case 5:
								std::get<5>(allBatches) = classifyEntities(states);
								break;
							default: // ZYX_COV_EXCL_LINE
								break; // ZYX_COV_EXCL_LINE
						}
					},
					[](size_t, SavePrepareState &) {});
		} else {
			classifySerial();
		}

		debug::PerfTrace::addDuration(
				"save.prepare", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													 prepStart)
												   .count()));

		// Step 1: Sequential — new entities (need segment allocation)
		std::apply([this](auto &...batch) { (this->saveNewEntityRefs(batch.added), ...); }, allBatches);

		// Step 2: Modified + deleted entities (known offsets, non-overlapping regions)
		if (threadPool && !threadPool->isSingleThreaded()) {
			std::vector<std::future<void>> ioTasks;
			std::apply(
					[this, &ioTasks, threadPool](auto &...batch) {
						auto submit = [this, &ioTasks, threadPool](auto &b) {
							if (!b.modified.empty() || !b.deleted.empty()) {
								ioTasks.push_back(threadPool->submit(
										[this, &b] { saveModifiedAndDeletedRefs(b.modified, b.deleted); }));
							}
						};
						(submit(batch), ...);
					},
					allBatches);
			for (auto &f : ioTasks)
				f.get();
		} else {
			std::apply([this](auto &...batch) { (this->saveModifiedAndDeletedRefs(batch.modified, batch.deleted), ...); },
					   allBatches);
		}

		// Flush accumulated shadow buffers as pre-computed CRCs
		flushPendingCrcs();
	}

	// ── flushPendingCrcs ───────────────────────────────────────────────────

	void StorageWriter::flushPendingCrcs() {
		std::vector<std::pair<uint64_t, uint32_t>> crcs;
		{
			std::lock_guard lock(pendingSegmentStateMutex_);
			crcs.reserve(pendingSegmentCrcs_.size());
			for (const auto &[segmentOffset, crc] : pendingSegmentCrcs_) {
				crcs.emplace_back(segmentOffset, crc);
			}
			pendingSegmentCrcs_.clear();
		}
		tracker_->setPendingCrcs(crcs);
	}

	std::vector<char> &StorageWriter::prepareSegmentBuffer(uint64_t segmentOffset, size_t preservedBytes) {
		if (segmentScratchBuffer_.size() != SEGMENT_SIZE) {
			segmentScratchBuffer_.resize(SEGMENT_SIZE);
		}
		if (preservedBytes > 0) {
			if (preservedBytes > SEGMENT_SIZE) { // ZYX_COV_EXCL_LINE: write callers derive this from segment capacity.
				throw std::runtime_error("Preserved segment prefix exceeds segment data size");
			}
			const uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader);
			size_t bytesRead = io_->readAt(dataOffset, segmentScratchBuffer_.data(), preservedBytes);
			if (bytesRead < preservedBytes) { // ZYX_COV_EXCL_LINE: a short prefix read means the segment was externally truncated.
				std::fill(segmentScratchBuffer_.begin() + static_cast<std::ptrdiff_t>(bytesRead),
						  segmentScratchBuffer_.begin() + static_cast<std::ptrdiff_t>(preservedBytes), 0);
			}
		}
		return segmentScratchBuffer_;
	}

	void StorageWriter::markTouchedSegmentWithCrc(uint64_t segmentOffset, uint32_t crc) {
		std::lock_guard lock(pendingSegmentStateMutex_);
		touchedSegments_.insert(segmentOffset);
		pendingSegmentCrcs_[segmentOffset] = crc;
	}

	void StorageWriter::invalidatePendingSegmentCrc(uint64_t segmentOffset) {
		std::lock_guard lock(pendingSegmentStateMutex_);
		pendingSegmentCrcs_.erase(segmentOffset);
	}

	// ── saveNewEntities ─────────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::saveNewEntities(std::vector<T> &entities) {
		if (entities.empty()) return;
		std::vector<const T *> refs;
		refs.reserve(entities.size());
		for (const auto &entity : entities) {
			refs.push_back(&entity);
		}
		saveNewEntityRefs(refs);
	}

	template<typename T>
	void StorageWriter::saveNewEntityRefs(std::vector<const T *> &entities) {
		if (entities.empty()) return;
		uint64_t &segHead = getSegmentHead(fileHeader_, T::typeId);
		constexpr uint32_t perSeg = storage::itemsPerSegment<T>();
		saveDataRefs(entities, segHead, perSeg);
	}

	// ── saveModifiedAndDeleted ──────────────────────────────────────────────

	template<typename T>
	void StorageWriter::saveModifiedAndDeleted(const std::vector<T> &modified, const std::vector<T> &deleted) {
		for (const auto &e : modified) updateEntityInPlace(e);
		for (const auto &e : deleted) deleteEntityOnDisk(e);
	}

	template<typename T>
	void StorageWriter::saveModifiedAndDeletedRefs(const std::vector<const T *> &modified,
												   const std::vector<const T *> &deleted) {
		for (const T *e : modified) updateEntityInPlace(*e);
		for (const T *e : deleted) deleteEntityOnDisk(*e);
	}

	// ── saveData ────────────────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::saveData(std::vector<T> &data, uint64_t &segmentHead, uint32_t itemsPerSegment) {
		if (data.empty())
			return;

		std::vector<const T *> refs;
		refs.reserve(data.size());
		for (const auto &entity : data) {
			refs.push_back(&entity);
		}
		saveDataRefs(refs, segmentHead, itemsPerSegment);
	}

	template<typename T>
	void StorageWriter::saveDataRefs(std::vector<const T *> &data, uint64_t &segmentHead, uint32_t itemsPerSegment) {
		if (segmentHead == 0) {
			writeNewSlotEntityRefs(data, segmentHead, itemsPerSegment);
			return;
		}

		// Group entities by whether they have pre-allocated slots or need new allocation.
		// A contiguous suffix that starts at the chain tail's next ID is known to be append-only,
		// so it can bypass per-entity segment-index lookup without changing reused-ID behavior.
		std::vector<const T *> ordered = data;
		orderEntityRefsById(ordered);
		auto appendOnlySuffix = ordered.end();
		if (const uint64_t tail = tracker_->getChainTail(T::typeId); tail != 0) {
			const SegmentHeader tailHeader = readSegmentHeader(tail);
			const int64_t expectedNextId = tailHeader.start_id + tailHeader.used;
			auto candidate = std::lower_bound(ordered.begin(), ordered.end(), expectedNextId,
											  [](const T *entity, int64_t id) { return entity->getId() < id; });
			if (candidate != ordered.end() && (*candidate)->getId() == expectedNextId) {
				bool contiguous = true;
				int64_t nextId = expectedNextId;
				for (auto it = candidate; it != ordered.end(); ++it, ++nextId) {
					if ((*it)->getId() != nextId) {
						contiguous = false;
						break;
					}
				}
				if (contiguous) {
					appendOnlySuffix = candidate;
				}
			}
		}

		std::vector<const T *> entitiesForNewSlots;
		std::unordered_map<uint64_t, std::vector<const T *>> entitiesBySegment;
		entitiesForNewSlots.reserve(data.size());

		for (auto it = ordered.begin(); it != appendOnlySuffix; ++it) {
			const T *entity = *it;
			uint64_t segmentOffset = dataManager_->findSegmentForEntityId<T>(entity->getId());

			if (segmentOffset != 0) {
				entitiesBySegment[segmentOffset].push_back(entity);
			} else {
				entitiesForNewSlots.push_back(entity);
			}
		}
		entitiesForNewSlots.insert(entitiesForNewSlots.end(), appendOnlySuffix, ordered.end());

		writePreAllocatedEntityRefs(entitiesBySegment);

		if (!entitiesForNewSlots.empty()) {
			writeNewSlotEntityRefs(entitiesForNewSlots, segmentHead, itemsPerSegment);
		}
	}

	// ── writePreAllocatedEntities ────────────────────────────────────────────

	template<typename T>
	void StorageWriter::writePreAllocatedEntityRefs(
			const std::unordered_map<uint64_t, std::vector<const T *>> &entitiesBySegment) {
		for (const auto &[segmentOffset, entities] : entitiesBySegment) {
			SegmentHeader header = readSegmentHeader(segmentOffset);
			const size_t itemSize = T::getTotalSize();

			auto sorted = entities;
			std::sort(sorted.begin(), sorted.end(), [](const T *a, const T *b) { return a->getId() < b->getId(); });

			// Group consecutive entities for batch I/O
			std::vector<std::pair<uint32_t, bool>> bitmapUpdates;
			size_t i = 0;
			while (i < sorted.size()) {
				size_t batchStart = i;
				auto startIndex = static_cast<uint32_t>(sorted[i]->getId() - header.start_id);

				// Find consecutive run
				while (i + 1 < sorted.size() &&
					   sorted[i + 1]->getId() == sorted[i]->getId() + 1) {
					i++;
				}
				i++;
				size_t batchSize = i - batchStart;

				// Serialize all into one buffer
				std::vector<char> buf(batchSize * itemSize);
				for (size_t j = 0; j < batchSize; j++) {
					serializeFixedEntityInto(buf.data() + j * itemSize, *sorted[batchStart + j], itemSize);
				}

				// Single pwrite for the batch
				uint64_t offset = segmentOffset + sizeof(SegmentHeader) + startIndex * itemSize;
				io_->writeAt(offset, buf.data(), buf.size());
				markTouchedSegment(segmentOffset);
				invalidatePendingSegmentCrc(segmentOffset);

				// Collect bitmap updates
				for (size_t j = 0; j < batchSize; j++) {
					bitmapUpdates.emplace_back(startIndex + static_cast<uint32_t>(j), true);
				}
			}

			// Batch bitmap update (single lock acquisition)
			tracker_->batchSetEntityActive(segmentOffset, bitmapUpdates);
		}
	}

	// ── writeNewSlotEntities ─────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::writeNewSlotEntityRefs(std::vector<const T *> &entitiesForNewSlots,
											   uint64_t &segmentHead, uint32_t itemsPerSegment) {
		orderEntityRefsById(entitiesForNewSlots);

		uint64_t currentSegmentOffset = segmentHead;
		SegmentHeader currentSegHeader;
		bool currentSegHeaderValid = false;
		bool isFirstSegment = (currentSegmentOffset == 0);

		// If we have a segment head, find the last segment via O(1) tail lookup
		if (currentSegmentOffset != 0) {
			uint64_t tail = tracker_->getChainTail(T::typeId);
			if (tail != 0) {
				currentSegmentOffset = tail;
			} else {
				// Fallback: walk the chain (should not happen after init)
				for (;;) {
					currentSegHeader = readSegmentHeader(currentSegmentOffset);
					if (currentSegHeader.next_segment_offset == 0)
						break;
					currentSegmentOffset = currentSegHeader.next_segment_offset;
				}
			}
		}

		auto dataIt = entitiesForNewSlots.begin();
		while (dataIt != entitiesForNewSlots.end()) {
			uint32_t remaining = 0;
			bool needNewSegment = (currentSegmentOffset == 0);

			if (currentSegmentOffset != 0) {
				if (!currentSegHeaderValid) {
					currentSegHeader = readSegmentHeader(currentSegmentOffset);
					currentSegHeaderValid = true;
				}
				remaining = currentSegHeader.capacity - currentSegHeader.used;

				int64_t expectedNextId = currentSegHeader.start_id + currentSegHeader.used;
				if (remaining == 0 || (*dataIt)->getId() != expectedNextId) {
					needNewSegment = true;
				}
			}

			if (needNewSegment) {
				const int64_t segmentStartId = (*dataIt)->getId();
				uint64_t newOffset = allocator_->allocateSegmentWithStartId(T::typeId, itemsPerSegment, segmentStartId);

				if (isFirstSegment) {
					segmentHead = newOffset;
					tracker_->updateChainHead(T::typeId, newOffset);
					isFirstSegment = false;
				}

				currentSegmentOffset = newOffset;
				currentSegHeader = {};
				currentSegHeader.capacity = itemsPerSegment;
				currentSegHeader.used = 0;
				currentSegHeader.start_id = segmentStartId;
				currentSegHeaderValid = true;
				remaining = currentSegHeader.capacity;
			}

			uint32_t maxCount = (std::min)(remaining, static_cast<uint32_t>(entitiesForNewSlots.end() - dataIt));
			int64_t expectedId = (*dataIt)->getId();
			uint32_t writeCount = 0;
			for (auto it = dataIt; writeCount < maxCount; ++it, ++writeCount) {
				if ((*it)->getId() != expectedId) break;
				expectedId++;
			}

			writeSegmentDataRefs(currentSegmentOffset, std::span<const T *const>(&*dataIt, writeCount),
								 currentSegHeader.used);

			currentSegHeader.used += writeCount;
			currentSegHeaderValid = true;

			dataIt += writeCount;
		}
	}

	// ── writeSegmentData ────────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t baseUsed) {
		writeSegmentDataSpan(segmentOffset, std::span<const T>(data.data(), data.size()), baseUsed);
	}

	template<typename T>
	void StorageWriter::writeSegmentDataSpan(uint64_t segmentOffset, std::span<const T> data, uint32_t baseUsed) {
		if (data.empty()) {
			return;
		}
		writeSegmentDataWithAccessor<T>(
				segmentOffset, data.size(), baseUsed, [&data](size_t index) -> const T & { return data[index]; });
	}

	template<typename T>
	void StorageWriter::writeSegmentDataRefs(uint64_t segmentOffset, std::span<const T *const> data,
											 uint32_t baseUsed) {
		writeSegmentDataWithAccessor<T>(
				segmentOffset, data.size(), baseUsed, [&data](size_t index) -> const T & { return *data[index]; });
	}

	template<typename T, typename EntityAt>
	void StorageWriter::writeSegmentDataWithAccessor(uint64_t segmentOffset, size_t count, uint32_t baseUsed,
													 EntityAt entityAt) {
		const size_t itemSize = T::getTotalSize();
		uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;

		size_t totalSize = count * itemSize;
		const size_t bufOffset = baseUsed * itemSize;
		auto &segBuf = prepareSegmentBuffer(segmentOffset, bufOffset);
		for (size_t i = 0; i < count; i++) {
			serializeFixedEntityInto(segBuf.data() + bufOffset + i * itemSize, entityAt(i), itemSize);
		}
		const size_t writeEnd = bufOffset + totalSize;
		if (writeEnd < SEGMENT_SIZE) {
			std::fill(segBuf.begin() + static_cast<std::ptrdiff_t>(writeEnd), segBuf.end(), 0);
		}
		io_->writeAt(dataOffset, segBuf.data() + bufOffset, totalSize);
		markTouchedSegmentWithCrc(segmentOffset, utils::calculateCrc(segBuf.data(), SEGMENT_SIZE));
		tracker_->appendEntityRange(segmentOffset, entityAt(0).getId(), baseUsed, static_cast<uint32_t>(count));
	}

	// ── updateEntityInPlace ─────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::updateEntityInPlace(const T &entity, uint64_t knownSegmentOffset) {
		int64_t id = entity.getId();
		uint64_t segmentOffset = knownSegmentOffset;

		if (segmentOffset == 0) {
			segmentOffset = dataManager_->findSegmentForEntityId<T>(id);
		}

		if (segmentOffset == 0) {
			throw std::runtime_error("Cannot update entity: entity not found via index lookup. ID: " +
									 std::to_string(id));
		}

		SegmentHeader header = tracker_->getSegmentHeader(segmentOffset);

		uint64_t entityIndex = id - header.start_id;

		if (entityIndex >= header.capacity) {
			std::stringstream ss;
			ss << "Entity index out of bounds for segment. "
			   << "ID: " << id << ", StartID: " << header.start_id << ", Index: " << entityIndex
			   << ", Capacity: " << header.capacity;
			throw std::runtime_error(ss.str());
		}

		uint64_t entityOffset = segmentOffset + sizeof(SegmentHeader) + entityIndex * T::getTotalSize();

		std::array<char, T::getTotalSize()> buf{};
		serializeFixedEntityInto(buf.data(), entity, buf.size());
		io_->writeAt(entityOffset, buf.data(), buf.size());
		markTouchedSegment(segmentOffset);
		invalidatePendingSegmentCrc(segmentOffset);

		tracker_->setEntityActive(segmentOffset, entityIndex, entity.isActive());
	}

	// ── deleteEntityOnDisk ──────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::deleteEntityOnDisk(const T &entity) {
		int64_t id = entity.getId();

		if (id <= 0) {
			return;
		}

		allocators_[entity.typeId]->free(id);

		if (uint64_t segmentOffset = dataManager_->findSegmentForEntityId<T>(id); segmentOffset != 0) {
			updateEntityInPlace(entity, segmentOffset);
		}
	}

	// ── readSegmentHeader ───────────────────────────────────────────────────

	SegmentHeader StorageWriter::readSegmentHeader(uint64_t segmentOffset) const {
		return tracker_->getSegmentHeader(segmentOffset);
	}

	void StorageWriter::markTouchedSegment(uint64_t segmentOffset) {
		std::lock_guard lock(pendingSegmentStateMutex_);
		touchedSegments_.insert(segmentOffset);
	}

	// ── Template instantiations ─────────────────────────────────────────────

	template void StorageWriter::saveData<Node>(std::vector<Node> &, uint64_t &, uint32_t);
	template void StorageWriter::saveData<Edge>(std::vector<Edge> &, uint64_t &, uint32_t);
	template void StorageWriter::saveData<Property>(std::vector<Property> &, uint64_t &, uint32_t);
	template void StorageWriter::saveData<Blob>(std::vector<Blob> &, uint64_t &, uint32_t);
	template void StorageWriter::saveData<Index>(std::vector<Index> &, uint64_t &, uint32_t);
	template void StorageWriter::saveData<State>(std::vector<State> &, uint64_t &, uint32_t);

	template void StorageWriter::writeSegmentData<Node>(uint64_t, const std::vector<Node> &, uint32_t);
	template void StorageWriter::writeSegmentData<Edge>(uint64_t, const std::vector<Edge> &, uint32_t);
	template void StorageWriter::writeSegmentData<Property>(uint64_t, const std::vector<Property> &, uint32_t);
	template void StorageWriter::writeSegmentData<Blob>(uint64_t, const std::vector<Blob> &, uint32_t);
	template void StorageWriter::writeSegmentData<Index>(uint64_t, const std::vector<Index> &, uint32_t);
	template void StorageWriter::writeSegmentData<State>(uint64_t, const std::vector<State> &, uint32_t);

	template void StorageWriter::updateEntityInPlace<Node>(const Node &, uint64_t);
	template void StorageWriter::updateEntityInPlace<Edge>(const Edge &, uint64_t);
	template void StorageWriter::updateEntityInPlace<Property>(const Property &, uint64_t);
	template void StorageWriter::updateEntityInPlace<Blob>(const Blob &, uint64_t);
	template void StorageWriter::updateEntityInPlace<Index>(const Index &, uint64_t);
	template void StorageWriter::updateEntityInPlace<State>(const State &, uint64_t);

	template void StorageWriter::deleteEntityOnDisk<Node>(const Node &);
	template void StorageWriter::deleteEntityOnDisk<Edge>(const Edge &);

	template void StorageWriter::saveNewEntities<Node>(std::vector<Node> &);
	template void StorageWriter::saveNewEntities<Edge>(std::vector<Edge> &);
	template void StorageWriter::saveNewEntities<Property>(std::vector<Property> &);
	template void StorageWriter::saveNewEntities<Blob>(std::vector<Blob> &);
	template void StorageWriter::saveNewEntities<Index>(std::vector<Index> &);
	template void StorageWriter::saveNewEntities<State>(std::vector<State> &);

	template void StorageWriter::saveModifiedAndDeleted<Node>(const std::vector<Node> &, const std::vector<Node> &);
	template void StorageWriter::saveModifiedAndDeleted<Edge>(const std::vector<Edge> &, const std::vector<Edge> &);
	template void StorageWriter::saveModifiedAndDeleted<Property>(const std::vector<Property> &, const std::vector<Property> &);
	template void StorageWriter::saveModifiedAndDeleted<Blob>(const std::vector<Blob> &, const std::vector<Blob> &);
	template void StorageWriter::saveModifiedAndDeleted<Index>(const std::vector<Index> &, const std::vector<Index> &);
	template void StorageWriter::saveModifiedAndDeleted<State>(const std::vector<State> &, const std::vector<State> &);

} // namespace graph::storage
