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
#include <unordered_map>
#include <vector>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/StorageHeaders.hpp"

namespace graph::storage {

	// Forward declarations
	class StorageIO;
	class SegmentTracker;
	class SegmentAllocator;
	class DataManager;
	class IDAllocator;
	struct FlushSnapshot;

	template<typename EntityType>
	struct DirtyEntityInfo;

	// Classify entities from a dirty map into added/modified/deleted vectors
	template<typename EntityType>
	struct SaveBatch {
		std::vector<EntityType> added, modified, deleted;
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
					  std::shared_ptr<IDAllocator> idAllocator,
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
		template<typename EntityType>
		void updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive);

		void updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive) const;

		SegmentHeader readSegmentHeader(uint64_t segmentOffset) const;

		/// Flush accumulated segment data as pre-computed CRCs to SegmentTracker.
		void flushPendingCrcs();

		std::shared_ptr<StorageIO> io_;
		std::shared_ptr<SegmentTracker> tracker_;
		std::shared_ptr<SegmentAllocator> allocator_;
		std::shared_ptr<DataManager> dataManager_;
		std::shared_ptr<IDAllocator> idAllocator_;
		FileHeader &fileHeader_;

		/// Shadow buffers for segments written via writeSegmentData (new entities).
		/// Used to compute CRC at write time, avoiding 128KB read-back at flush.
		std::unordered_map<uint64_t, std::vector<char>> pendingSegmentData_;
	};

} // namespace graph::storage
