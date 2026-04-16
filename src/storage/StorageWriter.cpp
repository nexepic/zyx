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
#include <cstring>
#include <future>
#include <sstream>
#include <vector>
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
		for (const auto &[id, info] : map) {
			if (!info.backup.has_value()) continue;
			switch (info.changeType) {
				case EntityChangeType::CHANGE_ADDED:
					batch.added.push_back(*info.backup);
					break;
				case EntityChangeType::CHANGE_MODIFIED:
					batch.modified.push_back(*info.backup);
					break;
				case EntityChangeType::CHANGE_DELETED:
					batch.deleted.push_back(*info.backup);
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
		using Clock = std::chrono::steady_clock;

		// Classify entities by change type for all entity types.
		auto prepStart = Clock::now();

		auto allBatches = std::make_tuple(
				classifyEntities(snapshot.nodes),
				classifyEntities(snapshot.edges),
				classifyEntities(snapshot.properties),
				classifyEntities(snapshot.blobs),
				classifyEntities(snapshot.indexes),
				classifyEntities(snapshot.states));

		debug::PerfTrace::addDuration(
				"save.prepare", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													 prepStart)
												   .count()));

		// Step 1: Sequential — new entities (need segment allocation)
		std::apply([this](auto &...batch) { (saveNewEntities(batch.added), ...); }, allBatches);

		// Step 2: Modified + deleted entities (known offsets, non-overlapping regions)
		if (threadPool && !threadPool->isSingleThreaded()) {
			std::vector<std::future<void>> ioTasks;
			std::apply(
					[this, &ioTasks, threadPool](auto &...batch) {
						auto submit = [this, &ioTasks, threadPool](auto &b) {
							if (!b.modified.empty() || !b.deleted.empty()) {
								ioTasks.push_back(threadPool->submit(
										[this, &b] { saveModifiedAndDeleted(b.modified, b.deleted); }));
							}
						};
						(submit(batch), ...);
					},
					allBatches);
			for (auto &f : ioTasks)
				f.get();
		} else {
			std::apply([this](auto &...batch) { (saveModifiedAndDeleted(batch.modified, batch.deleted), ...); },
					   allBatches);
		}

		// Flush accumulated shadow buffers as pre-computed CRCs
		flushPendingCrcs();
	}

	// ── flushPendingCrcs ───────────────────────────────────────────────────

	void StorageWriter::flushPendingCrcs() {
		for (auto &[segmentOffset, buf] : pendingSegmentData_) {
			uint32_t crc = utils::calculateCrc(buf.data(), SEGMENT_SIZE);
			tracker_->setPendingCrc(segmentOffset, crc);
		}
		pendingSegmentData_.clear();
	}

	// ── saveNewEntities ─────────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::saveNewEntities(std::vector<T> &entities) {
		if (entities.empty()) return;
		uint64_t &segHead = getSegmentHead(fileHeader_, T::typeId);
		constexpr uint32_t perSeg = storage::itemsPerSegment<T>();
		saveData(entities, segHead, perSeg);
	}

	// ── saveModifiedAndDeleted ──────────────────────────────────────────────

	template<typename T>
	void StorageWriter::saveModifiedAndDeleted(const std::vector<T> &modified, const std::vector<T> &deleted) {
		for (const auto &e : modified) updateEntityInPlace(e);
		for (const auto &e : deleted) deleteEntityOnDisk(e);
	}

	// ── saveData ────────────────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::saveData(std::vector<T> &data, uint64_t &segmentHead, uint32_t itemsPerSegment) {
		if (data.empty())
			return;

		// Group entities by whether they have pre-allocated slots or need new allocation
		std::vector<T> entitiesForNewSlots;
		std::unordered_map<uint64_t, std::vector<T>> entitiesBySegment;

		for (const auto &entity : data) {
			uint64_t segmentOffset = dataManager_->findSegmentForEntityId<T>(entity.getId());

			if (segmentOffset != 0) {
				entitiesBySegment[segmentOffset].push_back(entity);
			} else {
				entitiesForNewSlots.push_back(entity);
			}
		}

		writePreAllocatedEntities(entitiesBySegment);

		if (!entitiesForNewSlots.empty()) {
			writeNewSlotEntities(entitiesForNewSlots, segmentHead, itemsPerSegment);
		}
	}

	// ── writePreAllocatedEntities ────────────────────────────────────────────

	template<typename T>
	void StorageWriter::writePreAllocatedEntities(
			const std::unordered_map<uint64_t, std::vector<T>> &entitiesBySegment) {
		for (auto &[segmentOffset, entities] : entitiesBySegment) {
			SegmentHeader header = readSegmentHeader(segmentOffset);
			const size_t itemSize = T::getTotalSize();

			auto sorted = entities;
			std::sort(sorted.begin(), sorted.end(), [](const T &a, const T &b) { return a.getId() < b.getId(); });

			// Group consecutive entities for batch I/O
			std::vector<std::pair<uint32_t, bool>> bitmapUpdates;
			size_t i = 0;
			while (i < sorted.size()) {
				size_t batchStart = i;
				auto startIndex = static_cast<uint32_t>(sorted[i].getId() - header.start_id);

				// Find consecutive run
				while (i + 1 < sorted.size() &&
					   sorted[i + 1].getId() == sorted[i].getId() + 1) {
					i++;
				}
				i++;
				size_t batchSize = i - batchStart;

				// Serialize all into one buffer
				std::vector<char> buf(batchSize * itemSize);
				for (size_t j = 0; j < batchSize; j++) {
					utils::FixedSizeSerializer::serializeInto(buf.data() + j * itemSize,
															  sorted[batchStart + j], itemSize);
				}

				// Single pwrite for the batch
				uint64_t offset = segmentOffset + sizeof(SegmentHeader) + startIndex * itemSize;
				io_->writeAt(offset, buf.data(), buf.size());

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
	void StorageWriter::writeNewSlotEntities(std::vector<T> &entitiesForNewSlots,
											  uint64_t &segmentHead, uint32_t itemsPerSegment) {
		std::sort(entitiesForNewSlots.begin(), entitiesForNewSlots.end(),
				  [](const T &a, const T &b) { return a.getId() < b.getId(); });

		uint64_t currentSegmentOffset = segmentHead;
		SegmentHeader currentSegHeader;
		bool isFirstSegment = (currentSegmentOffset == 0);

		// If we have a segment head, find the last segment via O(1) tail lookup
		if (currentSegmentOffset != 0) {
			uint64_t tail = tracker_->getChainTail(T::typeId);
			if (tail != 0) {
				currentSegmentOffset = tail;
			} else {
				// Fallback: walk the chain (should not happen after init)
				while (true) {
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
				currentSegHeader = readSegmentHeader(currentSegmentOffset);
				remaining = currentSegHeader.capacity - currentSegHeader.used;

				int64_t expectedNextId = currentSegHeader.start_id + currentSegHeader.used;
				if (remaining == 0 || dataIt->getId() != expectedNextId) {
					needNewSegment = true;
				}
			}

			if (needNewSegment) {
				uint64_t newOffset = allocator_->allocateSegment(T::typeId, itemsPerSegment);

				if (isFirstSegment) {
					segmentHead = newOffset;
					tracker_->updateChainHead(T::typeId, newOffset);
					isFirstSegment = false;
				}

				SegmentHeader newSegHeader = readSegmentHeader(newOffset);
				if (newSegHeader.start_id != dataIt->getId()) {
					tracker_->updateSegmentHeader(
							newOffset, [dataIt](SegmentHeader &header) { header.start_id = dataIt->getId(); });
				}

				currentSegmentOffset = newOffset;
				currentSegHeader = readSegmentHeader(newOffset);
				remaining = currentSegHeader.capacity;
			}

			uint32_t maxCount = (std::min)(remaining, static_cast<uint32_t>(entitiesForNewSlots.end() - dataIt));
			int64_t expectedId = dataIt->getId();
			uint32_t writeCount = 0;
			for (auto it = dataIt; writeCount < maxCount; ++it, ++writeCount) {
				if (it->getId() != expectedId) break;
				expectedId++;
			}

			std::vector<T> batch(dataIt, dataIt + writeCount);

			writeSegmentData(currentSegmentOffset, batch, currentSegHeader.used);

			currentSegHeader = readSegmentHeader(currentSegmentOffset);

			dataIt += writeCount;
		}
	}

	// ── writeSegmentData ────────────────────────────────────────────────────

	template<typename T>
	void StorageWriter::writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t baseUsed) {
		const size_t itemSize = T::getTotalSize();
		uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;

		size_t totalSize = data.size() * itemSize;
		std::vector<char> buf(totalSize);
		for (size_t i = 0; i < data.size(); i++) {
			utils::FixedSizeSerializer::serializeInto(buf.data() + i * itemSize, data[i], itemSize);
		}
		io_->writeAt(dataOffset, buf.data(), totalSize);

		// Accumulate into shadow buffer for write-time CRC computation
		auto &segBuf = pendingSegmentData_[segmentOffset];
		if (segBuf.empty()) {
			segBuf.resize(SEGMENT_SIZE, 0);
		}
		size_t bufOffset = baseUsed * itemSize;
		std::memcpy(segBuf.data() + bufOffset, buf.data(), totalSize);

		tracker_->updateSegmentHeader(segmentOffset, [&](SegmentHeader &header) {
			header.used = baseUsed + static_cast<uint32_t>(data.size());
			if (baseUsed == 0 && !data.empty()) {
				header.start_id = data.front().getId();
			}
		});

		if (!data.empty()) {
			updateSegmentBitmap(segmentOffset, data.front().getId(), static_cast<uint32_t>(data.size()), true);
		}
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

		auto buf = utils::FixedSizeSerializer::serializeToBuffer(entity, T::getTotalSize());
		io_->writeAt(entityOffset, buf.data(), buf.size());

		updateBitmapForEntity<T>(segmentOffset, id, entity.isActive());
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

	// ── updateBitmapForEntity ───────────────────────────────────────────────

	template<typename EntityType>
	void StorageWriter::updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive) {
		if (segmentOffset == 0) {
			return;
		}

		SegmentHeader header = tracker_->getSegmentHeader(segmentOffset);
		uint64_t entityIndex = entityId - header.start_id;
		if (entityIndex >= header.capacity) {
			throw std::runtime_error("Entity index out of bounds for segment bitmap update");
		}

		tracker_->setEntityActive(segmentOffset, entityIndex, isActive);
	}

	// ── updateSegmentBitmap ─────────────────────────────────────────────────

	void StorageWriter::updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count,
											bool isActive) const {
		if (segmentOffset == 0 || count == 0) {
			return;
		}

		SegmentHeader header = tracker_->getSegmentHeader(segmentOffset);

		uint64_t startIndex = startId - header.start_id;
		if (startIndex + count > header.capacity) {
			throw std::runtime_error("Entity range out of bounds for segment bitmap batch update");
		}

		tracker_->setEntityActiveRange(segmentOffset, static_cast<uint32_t>(startIndex), count, isActive);
	}

	// ── readSegmentHeader ───────────────────────────────────────────────────

	SegmentHeader StorageWriter::readSegmentHeader(uint64_t segmentOffset) const {
		return tracker_->getSegmentHeader(segmentOffset);
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

	template void StorageWriter::updateBitmapForEntity<Node>(uint64_t, uint64_t, bool);
	template void StorageWriter::updateBitmapForEntity<Edge>(uint64_t, uint64_t, bool);

} // namespace graph::storage
