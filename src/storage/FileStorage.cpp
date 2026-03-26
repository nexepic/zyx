/**
 * @file FileStorage.cpp
 * @author Nexepic
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/FileStorage.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <zlib.h>
#include "graph/core/Blob.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/DatabaseInspector.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {

	FileStorage::FileStorage(std::string path, size_t cacheSize, OpenMode mode) :
		dbFilePath(std::move(path)), fileStream(nullptr), cacheSize(cacheSize), openMode_(mode) {}

	FileStorage::~FileStorage() { close(); }

	void FileStorage::open() {
		if (isFileOpen)
			return;

		bool fileExists = std::filesystem::exists(dbFilePath);

		// --- Strict Mode Validation ---
		if (openMode_ == OpenMode::OPEN_EXISTING_FILE && !fileExists) {
			throw std::runtime_error("Database file does not exist: " + dbFilePath);
		}
		if (openMode_ == OpenMode::OPEN_CREATE_NEW_FILE && fileExists) {
			throw std::runtime_error("Database file already exists: " + dbFilePath);
		}
		// ------------------------------------

		// Logic for creating NEW database
		if (openMode_ == OpenMode::OPEN_CREATE_NEW_FILE || (openMode_ == OpenMode::OPEN_CREATE_OR_OPEN_FILE && !fileExists)) {
			// Create and open for Read/Write + Truncate (Wipe previous)
			fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out |
																			std::ios::trunc);

			if (!*fileStream) {
				throw std::runtime_error("Cannot create database file: " + dbFilePath);
			}

			fileHeaderManager = std::make_shared<FileHeaderManager>(fileStream, fileHeader);
			// This physically writes the header bytes, creating the "empty" DB file.
			fileHeaderManager->initializeFileHeader();
		}
		// Logic for OPENING existing database
		else {
			fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);

			if (!*fileStream) {
				throw std::runtime_error("Cannot open database file: " + dbFilePath);
			}

			fileHeaderManager = std::make_shared<FileHeaderManager>(fileStream, fileHeader);
			fileHeaderManager->validateAndReadHeader();
		}

		initializeComponents();
		isFileOpen = true;
	}

	void FileStorage::initializeComponents() {
		fileHeaderManager->extractFileHeaderInfo();

		segmentTracker = std::make_shared<SegmentTracker>(fileStream, fileHeader);

		// Initialize ID allocator
		idAllocator = std::make_unique<IDAllocator>(
				fileStream, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
				fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
				fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());
		idAllocator->initialize();

		// Then create the space manager
		spaceManager =
				std::make_shared<SpaceManager>(fileStream, dbFilePath, segmentTracker, fileHeaderManager, idAllocator);

		// Initialize data manager (pass filePath for pread-based parallel reads)
		dataManager = std::make_shared<DataManager>(fileStream, cacheSize, fileHeader, idAllocator, segmentTracker,
													spaceManager, dbFilePath);
		dataManager->initialize();
		dataManager->setDeletionFlagReference(&deleteOperationPerformed);

		// Always set up auto-flush callback
		dataManager->setAutoFlushCallback([this]() { this->flush(); });

		systemStateManager = std::make_shared<state::SystemStateManager>(dataManager);

		dataManager->setSystemStateManager(systemStateManager);

		databaseInspector = std::make_shared<DatabaseInspector>(fileHeader, fileStream, *dataManager);
	}

	void FileStorage::close() {
		if (isFileOpen) {
			flush(); // Ensure any pending changes are written

			if (fileStream) {
				fileStream->flush();
				fileStream->close();
				fileStream.reset();
			}

			dataManager->clearCache();
			dataManager.reset();
			(void) spaceManager->truncateFile();
			isFileOpen = false;
		}
	}

	template<typename EntityType>
	std::vector<EntityType> getEntitiesByType(const std::unordered_map<int64_t, DirtyEntityInfo<EntityType>> &map,
											  EntityChangeType type) {
		std::vector<EntityType> result;
		for (const auto &[id, info]: map) {
			if (info.changeType == type && info.backup.has_value()) {
				result.push_back(*info.backup);
			}
		}
		return result;
	}

	void FileStorage::save() {
		using Clock = std::chrono::steady_clock;

		if (!isFileOpen)
			throw std::runtime_error("Database must be open before saving");
		if (!dataManager->hasUnsavedChanges())
			return;

		auto totalStart = Clock::now();

		// 1. ATOMIC SNAPSHOT: Freeze current dirty state into snapshot
		auto snapshot = dataManager->prepareFlushSnapshot();

		if (snapshot.isEmpty())
			return;

		// 2. PARALLEL DATA PREPARATION PHASE
		// Classify entities by change type for all entity types in parallel.
		// This is pure in-memory work with no I/O dependencies.
		auto prepStart = Clock::now();
		std::vector<Node> newNodes, modNodes, delNodes;
		std::vector<Edge> newEdges, modEdges, delEdges;
		std::vector<Property> newProps, modProps, delProps;
		std::vector<Blob> newBlobs, modBlobs, delBlobs;
		std::vector<Index> newIndexes, modIndexes, delIndexes;
		std::vector<State> newStates, modStates, delStates;

		if (threadPool_ && !threadPool_->isSingleThreaded()) {
			std::vector<std::future<void>> prep;

			if (!snapshot.nodes.empty()) {
				prep.push_back(threadPool_->submit([&] {
					newNodes = getEntitiesByType(snapshot.nodes, EntityChangeType::CHANGE_ADDED);
					modNodes = getEntitiesByType(snapshot.nodes, EntityChangeType::CHANGE_MODIFIED);
					delNodes = getEntitiesByType(snapshot.nodes, EntityChangeType::CHANGE_DELETED);
				}));
			}
			if (!snapshot.edges.empty()) {
				prep.push_back(threadPool_->submit([&] {
					newEdges = getEntitiesByType(snapshot.edges, EntityChangeType::CHANGE_ADDED);
					modEdges = getEntitiesByType(snapshot.edges, EntityChangeType::CHANGE_MODIFIED);
					delEdges = getEntitiesByType(snapshot.edges, EntityChangeType::CHANGE_DELETED);
				}));
			}
			if (!snapshot.properties.empty()) {
				prep.push_back(threadPool_->submit([&] {
					newProps = getEntitiesByType(snapshot.properties, EntityChangeType::CHANGE_ADDED);
					modProps = getEntitiesByType(snapshot.properties, EntityChangeType::CHANGE_MODIFIED);
					delProps = getEntitiesByType(snapshot.properties, EntityChangeType::CHANGE_DELETED);
				}));
			}
			if (!snapshot.blobs.empty()) {
				prep.push_back(threadPool_->submit([&] {
					newBlobs = getEntitiesByType(snapshot.blobs, EntityChangeType::CHANGE_ADDED);
					modBlobs = getEntitiesByType(snapshot.blobs, EntityChangeType::CHANGE_MODIFIED);
					delBlobs = getEntitiesByType(snapshot.blobs, EntityChangeType::CHANGE_DELETED);
				}));
			}
			if (!snapshot.indexes.empty()) {
				prep.push_back(threadPool_->submit([&] {
					newIndexes = getEntitiesByType(snapshot.indexes, EntityChangeType::CHANGE_ADDED);
					modIndexes = getEntitiesByType(snapshot.indexes, EntityChangeType::CHANGE_MODIFIED);
					delIndexes = getEntitiesByType(snapshot.indexes, EntityChangeType::CHANGE_DELETED);
				}));
			}
			if (!snapshot.states.empty()) {
				prep.push_back(threadPool_->submit([&] {
					newStates = getEntitiesByType(snapshot.states, EntityChangeType::CHANGE_ADDED);
					modStates = getEntitiesByType(snapshot.states, EntityChangeType::CHANGE_MODIFIED);
					delStates = getEntitiesByType(snapshot.states, EntityChangeType::CHANGE_DELETED);
				}));
			}

			for (auto &f : prep)
				f.get();
		} else {
			// Sequential preparation
			if (!snapshot.nodes.empty()) {
				newNodes = getEntitiesByType(snapshot.nodes, EntityChangeType::CHANGE_ADDED);
				modNodes = getEntitiesByType(snapshot.nodes, EntityChangeType::CHANGE_MODIFIED);
				delNodes = getEntitiesByType(snapshot.nodes, EntityChangeType::CHANGE_DELETED);
			}
			if (!snapshot.edges.empty()) {
				newEdges = getEntitiesByType(snapshot.edges, EntityChangeType::CHANGE_ADDED);
				modEdges = getEntitiesByType(snapshot.edges, EntityChangeType::CHANGE_MODIFIED);
				delEdges = getEntitiesByType(snapshot.edges, EntityChangeType::CHANGE_DELETED);
			}
			if (!snapshot.properties.empty()) {
				newProps = getEntitiesByType(snapshot.properties, EntityChangeType::CHANGE_ADDED);
				modProps = getEntitiesByType(snapshot.properties, EntityChangeType::CHANGE_MODIFIED);
				delProps = getEntitiesByType(snapshot.properties, EntityChangeType::CHANGE_DELETED);
			}
			if (!snapshot.blobs.empty()) {
				newBlobs = getEntitiesByType(snapshot.blobs, EntityChangeType::CHANGE_ADDED);
				modBlobs = getEntitiesByType(snapshot.blobs, EntityChangeType::CHANGE_MODIFIED);
				delBlobs = getEntitiesByType(snapshot.blobs, EntityChangeType::CHANGE_DELETED);
			}
			if (!snapshot.indexes.empty()) {
				newIndexes = getEntitiesByType(snapshot.indexes, EntityChangeType::CHANGE_ADDED);
				modIndexes = getEntitiesByType(snapshot.indexes, EntityChangeType::CHANGE_MODIFIED);
				delIndexes = getEntitiesByType(snapshot.indexes, EntityChangeType::CHANGE_DELETED);
			}
			if (!snapshot.states.empty()) {
				newStates = getEntitiesByType(snapshot.states, EntityChangeType::CHANGE_ADDED);
				modStates = getEntitiesByType(snapshot.states, EntityChangeType::CHANGE_MODIFIED);
				delStates = getEntitiesByType(snapshot.states, EntityChangeType::CHANGE_DELETED);
			}
		}

		debug::PerfTrace::addDuration(
				"save.prepare", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													 prepStart)
												   .count()));

		// 3. SEQUENTIAL I/O PHASE (single fstream constraint)
		auto ioStart = Clock::now();
		if (!newNodes.empty()) {
			std::unordered_map<int64_t, Node> map;
			for (auto &e : newNodes) map[e.getId()] = e;
			saveData(map, fileHeader.node_segment_head, NODES_PER_SEGMENT);
		}
		for (const auto &n : modNodes) updateEntityInPlace(n);
		for (const auto &n : delNodes) deleteEntityOnDisk(n);

		if (!newEdges.empty()) {
			std::unordered_map<int64_t, Edge> map;
			for (auto &e : newEdges) map[e.getId()] = e;
			saveData(map, fileHeader.edge_segment_head, EDGES_PER_SEGMENT);
		}
		for (const auto &e : modEdges) updateEntityInPlace(e);
		for (const auto &e : delEdges) deleteEntityOnDisk(e);

		if (!newProps.empty()) {
			std::unordered_map<int64_t, Property> map;
			for (auto &e : newProps) map[e.getId()] = e;
			saveData(map, fileHeader.property_segment_head, PROPERTIES_PER_SEGMENT);
		}
		for (const auto &p : modProps) updateEntityInPlace(p);
		for (const auto &p : delProps) deleteEntityOnDisk(p);

		if (!newBlobs.empty()) {
			std::unordered_map<int64_t, Blob> map;
			for (auto &e : newBlobs) map[e.getId()] = e;
			saveData(map, fileHeader.blob_segment_head, BLOBS_PER_SEGMENT);
		}
		for (const auto &b : modBlobs) updateEntityInPlace(b);
		for (const auto &b : delBlobs) deleteEntityOnDisk(b);

		if (!newIndexes.empty()) {
			std::unordered_map<int64_t, Index> map;
			for (auto &e : newIndexes) map[e.getId()] = e;
			saveData(map, fileHeader.index_segment_head, INDEXES_PER_SEGMENT);
		}
		for (const auto &i : modIndexes) updateEntityInPlace(i);
		for (const auto &i : delIndexes) deleteEntityOnDisk(i);

		if (!newStates.empty()) {
			std::unordered_map<int64_t, State> map;
			for (auto &e : newStates) map[e.getId()] = e;
			saveData(map, fileHeader.state_segment_head, STATES_PER_SEGMENT);
		}
		for (const auto &s : modStates) updateEntityInPlace(s);
		for (const auto &s : delStates) deleteEntityOnDisk(s);

		debug::PerfTrace::addDuration(
				"save.io", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													ioStart)
												.count()));

		// 4. COMMIT: Clear the snapshot data
		dataManager->commitFlushSnapshot();
		debug::PerfTrace::addDuration(
				"save.total", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													  totalStart)
													.count()));
	}

	template<typename T>
	void FileStorage::saveData(std::unordered_map<int64_t, T> &data, uint64_t &segmentHead, uint32_t itemsPerSegment) {
		if (data.empty())
			return;

		// Group entities by whether they have pre-allocated slots or need new allocation
		std::vector<T> entitiesForNewSlots;
		std::unordered_map<uint64_t, std::vector<T>> entitiesBySegment; // Segment offset -> entities

		// First pass: determine if each entity has a pre-allocated slot
		for (const auto &[id, entity]: data) {
			uint64_t segmentOffset = 0;

			// Find if entity has a segment assignment (either from inactive slot reuse or existing entity)
			segmentOffset = dataManager->findSegmentForEntityId<T>(id);

			if (segmentOffset != 0) {
				// Entity has a pre-allocated slot - group by segment for batch processing
				entitiesBySegment[segmentOffset].push_back(entity);
			} else {
				// Entity needs a new slot
				entitiesForNewSlots.push_back(entity);
			}
		}

		// Process entities with pre-allocated slots
		for (auto &[segmentOffset, entities]: entitiesBySegment) {
			SegmentHeader header = readSegmentHeader(segmentOffset);

			// Sort entities by ID for efficient placement
			std::sort(entities.begin(), entities.end(), [](const T &a, const T &b) { return a.getId() < b.getId(); });

			// Process each entity
			for (const auto &entity: entities) {
				auto index = static_cast<uint32_t>(entity.getId() - header.start_id);

				// Calculate file offset for this entity
				uint64_t entityOffset = segmentOffset + sizeof(SegmentHeader) + index * T::getTotalSize();

				// Write entity
				fileStream->seekp(static_cast<std::streamoff>(entityOffset));
				utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, entity, T::getTotalSize());

				// Mark entity as active if it wasn't already
				if (!segmentTracker->getBitmapBit(segmentOffset, index)) {
					segmentTracker->setEntityActive(segmentOffset, index, true);

					// Update inactive count in the segment header if needed
					segmentTracker->updateSegmentHeader(segmentOffset, [](SegmentHeader &header) {
						if (header.inactive_count > 0) {
							header.inactive_count--;
						}
					});
				}
			}
		}

		// Exit if all entities were saved to pre-allocated slots
		if (entitiesForNewSlots.empty()) {
			return;
		}

		// For remaining entities, sort by ID for sequential storage
		std::sort(entitiesForNewSlots.begin(), entitiesForNewSlots.end(),
				  [](const T &a, const T &b) { return a.getId() < b.getId(); });

		// Process entities that need new slots
		uint64_t currentSegmentOffset = segmentHead;
		SegmentHeader currentSegHeader;
		bool isFirstSegment = (currentSegmentOffset == 0);

		// If we have a segment head, find the last segment in the chain
		if (currentSegmentOffset != 0) {
			// Find the last segment in the chain
			while (true) {
				currentSegHeader = readSegmentHeader(currentSegmentOffset);
				if (currentSegHeader.next_segment_offset == 0)
					break;
				currentSegmentOffset = currentSegHeader.next_segment_offset;
			}
		}

		auto dataIt = entitiesForNewSlots.begin();
		while (dataIt != entitiesForNewSlots.end()) {
			uint32_t remaining = 0;

			// Calculate remaining space in current segment (if it exists)
			if (currentSegmentOffset != 0) {
				currentSegHeader = readSegmentHeader(currentSegmentOffset);
				remaining = currentSegHeader.capacity - currentSegHeader.used;
			}

			if (currentSegmentOffset == 0 || remaining == 0) {
				// Allocate new segment
				// Note: SpaceManager will handle all segment linking automatically
				uint64_t newOffset = allocateSegment(T::typeId, itemsPerSegment);

				// If this is the first segment for this type, update the segment head
				if (isFirstSegment) {
					segmentHead = newOffset;
					segmentTracker->updateChainHead(T::typeId, newOffset);
					isFirstSegment = false;
				}

				// Update the start_id if needed (only for a new segment)
				SegmentHeader newSegHeader = readSegmentHeader(newOffset);
				if (newSegHeader.start_id != dataIt->getId()) {
					// Only update start_id if it differs from what SpaceManager set
					segmentTracker->updateSegmentHeader(
							newOffset, [dataIt](SegmentHeader &header) { header.start_id = dataIt->getId(); });
				}

				// Move to new segment
				currentSegmentOffset = newOffset;
				currentSegHeader = readSegmentHeader(newOffset);
				remaining = currentSegHeader.capacity;
			}

			// Calculate number of items to write
			uint32_t writeCount = std::min(remaining, static_cast<uint32_t>(entitiesForNewSlots.end() - dataIt));
			std::vector<T> batch(dataIt, dataIt + writeCount);

			// Write data
			writeSegmentData(currentSegmentOffset, batch, currentSegHeader.used);

			// Reload header to get updated used count
			currentSegHeader = readSegmentHeader(currentSegmentOffset);

			dataIt += writeCount;
		}
	}

	uint64_t FileStorage::allocateSegment(uint32_t type, uint32_t capacity) const {
		// Use SpaceManager's allocateSegment function instead of direct file operations
		return spaceManager->allocateSegment(type, capacity);
	}

	template<typename T>
	void FileStorage::writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t baseUsed) {
		// Calculate item size
		const size_t itemSize = T::getTotalSize();

		// Write data
		uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;
		fileStream->seekp(static_cast<std::streamoff>(dataOffset));
		for (const auto &item: data) {
			utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, item, itemSize);
			dataOffset += itemSize;
		}

		// Use the tracker to update the segment header
		segmentTracker->updateSegmentHeader(segmentOffset, [&](SegmentHeader &header) {
			// Update header fields
			header.used = baseUsed + static_cast<uint32_t>(data.size());
			if (baseUsed == 0 && !data.empty()) {
				header.start_id = data.front().getId();
			}
		});

		// Flush file stream to ensure data is written
		fileStream->flush();

		// Update bitmap for the newly added entities directly
		if (!data.empty()) {
			updateSegmentBitmap(segmentOffset, data.front().getId(), static_cast<uint32_t>(data.size()), true);
		}
	}

	template<typename T>
	void FileStorage::updateEntityInPlace(const T &entity, uint64_t knownSegmentOffset) {
		int64_t id = entity.getId();
		uint64_t segmentOffset = knownSegmentOffset;

		if (segmentOffset == 0) {
			segmentOffset = dataManager->findSegmentForEntityId<T>(id);
		}

		if (segmentOffset == 0) {
			throw std::runtime_error("Cannot update entity: entity not found via index lookup. ID: " +
									 std::to_string(id));
		}

		SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);

		// Calculate entity position within segment
		uint64_t entityIndex = id - header.start_id;

		if (entityIndex >= header.capacity) {
			std::stringstream ss;
			ss << "Entity index out of bounds for segment. "
			   << "ID: " << id << ", StartID: " << header.start_id << ", Index: " << entityIndex
			   << ", Capacity: " << header.capacity;
			throw std::runtime_error(ss.str());
		}

		// Calculate file offset for this entity
		uint64_t entityOffset = segmentOffset + sizeof(SegmentHeader) + entityIndex * T::getTotalSize();

		// Write entity
		fileStream->seekp(static_cast<std::streamoff>(entityOffset));
		utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, entity, T::getTotalSize());
		fileStream->flush();

		// Update bitmap to reflect entity's active state
		updateBitmapForEntity<T>(segmentOffset, id, entity.isActive());
	}

	template<typename T>
	void FileStorage::deleteEntityOnDisk(const T &entity) {
		int64_t id = entity.getId();

		if (id <= 0) {
			return;
		}

		// Mark the ID as available for reuse
		idAllocator->freeId(id, entity.typeId);

		if (uint64_t segmentOffset = dataManager->findSegmentForEntityId<T>(id); segmentOffset != 0) {
			// Entity exists on disk, update it in place
			updateEntityInPlace(entity, segmentOffset);
		}
		// If entity doesn't exist on disk, there's nothing to update
	}

	// Template specializations
	template void FileStorage::deleteEntityOnDisk<Node>(const Node &entity);
	template void FileStorage::deleteEntityOnDisk<Edge>(const Edge &entity);

	// Read segment header
	SegmentHeader FileStorage::readSegmentHeader(uint64_t segmentOffset) const {
		return segmentTracker->getSegmentHeader(segmentOffset);
	}

	// Write segment header
	void FileStorage::writeSegmentHeader(uint64_t segmentOffset, const SegmentHeader &header) const {
		segmentTracker->writeSegmentHeader(segmentOffset, header);
	}

	// Update bitmap for a specific entity in the segment
	template<typename EntityType>
	void FileStorage::updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive) {
		if (segmentOffset == 0) {
			return; // No segment to update
		}

		// Calculate entity position within segment
		SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);
		uint64_t entityIndex = entityId - header.start_id;
		if (entityIndex >= header.capacity) {
			throw std::runtime_error("Entity index out of bounds for segment bitmap update");
		}

		// Delegate bitmap update to SegmentTracker
		segmentTracker->setEntityActive(segmentOffset, entityIndex, isActive);
	}

	// Update bitmap for batch operations
	void FileStorage::updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count,
										  bool isActive) const {
		if (segmentOffset == 0 || count == 0) {
			return;
		}

		// Read segment header to get start_id
		SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);

		// Calculate relative start position
		uint64_t startIndex = startId - header.start_id;
		if (startIndex + count > header.capacity) {
			throw std::runtime_error("Entity range out of bounds for segment bitmap batch update");
		}

		// Create the activity map
		std::vector<bool> currentActivity = segmentTracker->getActivityBitmap(segmentOffset);

		// Ensure the vector is large enough
		if (currentActivity.size() < startIndex + count) {
			currentActivity.resize(startIndex + count);
		}

		// Update activity status for the specified range
		for (uint32_t i = 0; i < count; i++) {
			currentActivity[startIndex + i] = isActive;
		}

		// Delegate the update to SegmentTracker
		segmentTracker->updateActivityBitmap(segmentOffset, currentActivity);
	}

	// Template instantiations
	template void FileStorage::updateBitmapForEntity<Node>(uint64_t segmentOffset, uint64_t entityId, bool isActive);
	template void FileStorage::updateBitmapForEntity<Edge>(uint64_t segmentOffset, uint64_t entityId, bool isActive);

	bool FileStorage::verifyBitmapConsistency(uint64_t segmentOffset) const {
		if (segmentOffset == 0) {
			return false;
		}

		// Read segment header through the tracker
		SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);

		// Count inactive items in bitmap
		uint32_t inactiveCount = 0;
		for (uint32_t i = 0; i < header.used; i++) {
			if (!segmentTracker->getBitmapBit(segmentOffset, i)) {
				inactiveCount++;
			}
		}

		// Compare with header's inactive_count
		if (inactiveCount != header.inactive_count) {
			std::cerr << "Bitmap inconsistency detected: Counted " << inactiveCount
					  << " inactive items but header reports " << header.inactive_count << std::endl;
			return false;
		}

		return true;
	}

	void FileStorage::persistSegmentHeaders() const {
		// Simply delegate to the SegmentTracker to flush all dirty segments
		segmentTracker->flushDirtySegments();
	}

	void FileStorage::registerEventListener(std::weak_ptr<IStorageEventListener> listener) {
		std::lock_guard<std::mutex> lock(listenerMutex_);
		eventListeners_.push_back(std::move(listener));
	}

	// TODO: Subsequent flush operations should be executed in a queue
	void FileStorage::flush() {
		// Acquire lock to ensure atomic operation
		std::unique_lock<std::mutex> lock(flushMutex, std::try_to_lock);

		// If we couldn't acquire the lock or a flush is already in progress, return
		if (!lock.owns_lock() || flushInProgress.exchange(true)) {
			return;
		}

		try {
			// --- NOTIFY LISTENERS ---
			{
				std::lock_guard<std::mutex> listenerLock(listenerMutex_);
				// Iterate and notify valid listeners
				auto it = eventListeners_.begin();
				while (it != eventListeners_.end()) {
					if (auto listener = it->lock()) {
						// Notify IndexManager or others to persist their state
						listener->onStorageFlush();
						++it;
					} else {
						// Remove expired listeners (e.g., if IndexManager was destroyed)
						it = eventListeners_.erase(it);
					}
				}
			}

			// Ensure all pending changes are written
			save();

			// Only check for compaction if delete operations occurred
			if (deleteOperationPerformed.load() && compactionEnabled_.load()) {
				if (spaceManager->shouldCompact()) {
					// Use the thread-safe compaction method

					// Only perform post-compaction operations if compaction was successful
					if (spaceManager->safeCompactSegments()) {
						dataManager->clearCache();

						// Just clear the allocator cache. It will lazy-load on next insert.
						idAllocator->resetAfterCompaction();

						dataManager->getSegmentIndexManager()->buildSegmentIndexes();
					}
				}
				// Reset the flag after handling potential compaction
				deleteOperationPerformed.store(false);
			}

			// Persist segment headers
			persistSegmentHeaders();

			fileHeaderManager->flushFileHeader();
		} catch (const std::exception &e) {
			// Log the error
			std::cerr << "Exception during flush operation: " << e.what() << std::endl;
		} catch (...) {
			// Log unknown errors
			std::cerr << "Unknown exception during flush operation" << std::endl;
		}

		// Mark flush as complete
		flushInProgress.store(false);
	}

	void FileStorage::clearCache() const { dataManager->clearCache(); }

} // namespace graph::storage
