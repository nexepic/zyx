/**
 * @file StorageWriter.hpp
 * @brief Entity write engine — serializes entities to disk segments
 *
 * Extracted from FileStorage to separate "how to write entities" from lifecycle
 * orchestration. Handles saveData, writeSegmentData, updateEntityInPlace,
 * deleteEntityOnDisk, and bitmap maintenance.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/DirtyEntityRegistry.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/StorageHeaders.hpp"

namespace graph::storage {

	// Forward declarations
	class StorageIO;
	class SegmentTracker;
	class SegmentAllocator;
	class DataManager;
	struct FlushSnapshot;
	struct FlushSnapshotView;

	template<typename EntityType>
	struct DirtyEntityInfo;

	// Classify entities from a dirty map into added/modified/deleted vectors
	template<typename EntityType>
	struct SaveBatch {
		std::vector<const EntityType *> added, modified, deleted;
	};

	template<typename EntityType>
	SaveBatch<EntityType> classifyEntities(
			const std::unordered_map<int64_t, DirtyEntityInfo<EntityType>> &map);

	class StorageWriter {
	public:
		StorageWriter(std::shared_ptr<StorageIO> io,
					  std::shared_ptr<SegmentTracker> tracker,
					  std::shared_ptr<SegmentAllocator> allocator,
					  std::shared_ptr<DataManager> dataManager,
					  IDAllocators allocators,
					  FileHeader &fileHeader);

		~StorageWriter() = default;

		// Non-copyable
		StorageWriter(const StorageWriter &) = delete;
		StorageWriter &operator=(const StorageWriter &) = delete;

		/**
		 * @brief High-level: classify + write all entity types from a flush snapshot.
		 *
		 * Called by FileStorage::save(). Replaces the inline classify+saveNew+saveMod logic.
		 */
		void writeSnapshot(FlushSnapshot &snapshot, concurrent::ThreadPool *threadPool);
		void writeSnapshot(FlushSnapshotView &snapshot, concurrent::ThreadPool *threadPool);
		std::vector<uint64_t> takeTouchedSegments();

		/**
		 * @brief Append new entities to a segment chain, allocating new segments as needed.
		 */
		template<typename T>
		void saveData(std::vector<T> &data, uint64_t &segmentHead, uint32_t maxSegmentSize);

		/**
		 * @brief Write a batch of entities into a segment at a given base offset.
		 */
		template<typename T>
		void writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t baseUsed);

		/**
		 * @brief Overwrite a single entity at its on-disk location.
		 */
		template<typename T>
		void updateEntityInPlace(const T &entity, uint64_t knownSegmentOffset = 0);

		/**
		 * @brief Mark an entity as deleted on disk and free its ID.
		 */
		template<typename T>
		void deleteEntityOnDisk(const T &entity);

		/**
		 * @brief Save new entities to their segment chain.
		 */
		template<typename T>
		void saveNewEntities(std::vector<T> &entities);

		/**
		 * @brief Update modified entities in place and delete removed entities.
		 */
		template<typename T>
		void saveModifiedAndDeleted(const std::vector<T> &modified, const std::vector<T> &deleted);

	private:
		template<typename NodeMap, typename EdgeMap, typename PropertyMap, typename BlobMap, typename IndexMap,
				 typename StateMap>
		void writeSnapshotMaps(const NodeMap &nodes, const EdgeMap &edges, const PropertyMap &properties,
							   const BlobMap &blobs, const IndexMap &indexes, const StateMap &states,
							   concurrent::ThreadPool *threadPool);

		/**
		 * @brief Save new entities referenced by a flush snapshot without copying entity payloads.
		 */
		template<typename T>
		void saveNewEntityRefs(std::vector<const T *> &entities);

		/**
		 * @brief Append referenced entities to a segment chain, allocating new segments as needed.
		 */
		template<typename T>
		void saveDataRefs(std::vector<const T *> &data, uint64_t &segmentHead, uint32_t maxSegmentSize);

		/**
		 * @brief Update/delete referenced snapshot entities without materializing copies.
		 */
		template<typename T>
		void saveModifiedAndDeletedRefs(const std::vector<const T *> &modified,
										const std::vector<const T *> &deleted);

		/**
		 * @brief Write entities that already have pre-allocated segment slots.
		 */
		template<typename T>
		void writePreAllocatedEntityRefs(
				const std::unordered_map<uint64_t, std::vector<const T *>> &entitiesBySegment);

		/**
		 * @brief Append entities that need new segment slot allocation.
		 */
		template<typename T>
		void writeNewSlotEntityRefs(std::vector<const T *> &entitiesForNewSlots,
									uint64_t &segmentHead, uint32_t itemsPerSegment);

		template<typename T>
		void writeSegmentDataSpan(uint64_t segmentOffset, std::span<const T> data, uint32_t baseUsed);

		template<typename T>
		void writeSegmentDataRefs(uint64_t segmentOffset, std::span<const T *const> data, uint32_t baseUsed);

		template<typename T, typename EntityAt>
		void writeSegmentDataWithAccessor(uint64_t segmentOffset, size_t count, uint32_t baseUsed,
										  EntityAt entityAt);

		template<typename EntityType>
		void updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive);

		void updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive) const;

		SegmentHeader readSegmentHeader(uint64_t segmentOffset) const;
		void markTouchedSegment(uint64_t segmentOffset);

		/// Flush accumulated segment data as pre-computed CRCs to SegmentTracker.
		void flushPendingCrcs();

		/// Returns a reusable full-segment buffer, seeding from disk when appending
		/// to an already-populated segment.
		std::vector<char> &prepareSegmentBuffer(uint64_t segmentOffset, size_t preservedBytes);
		void markTouchedSegmentWithCrc(uint64_t segmentOffset, uint32_t crc);
		void invalidatePendingSegmentCrc(uint64_t segmentOffset);

		std::shared_ptr<StorageIO> io_;
		std::shared_ptr<SegmentTracker> tracker_;
		std::shared_ptr<SegmentAllocator> allocator_;
		std::shared_ptr<DataManager> dataManager_;
		IDAllocators allocators_;
		FileHeader &fileHeader_;

		/// Reusable full-segment scratch buffer for write-time CRC computation.
		std::vector<char> segmentScratchBuffer_;
		std::mutex pendingSegmentStateMutex_;
		std::unordered_map<uint64_t, uint32_t> pendingSegmentCrcs_;
		std::unordered_set<uint64_t> touchedSegments_;
	};

} // namespace graph::storage
