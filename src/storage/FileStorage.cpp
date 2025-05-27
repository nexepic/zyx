/**
 * @file FileStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileStorage.h"
#include <algorithm> // for std::sort
#include <filesystem>
#include <fstream>
#include <sstream>
#include <zlib.h>
#include "graph/core/Blob.h"
#include "graph/core/Edge.h"
#include "graph/core/Node.h"
#include "graph/core/Property.h"
#include "graph/storage/DatabaseInspector.h"
#include "graph/storage/EntityReferenceUpdater.h"
#include "graph/utils/FixedSizeSerializer.h"

namespace graph::storage {

	FileStorage::FileStorage(std::string path, size_t cacheSize) :
		dbFilePath(std::move(path)), fileStream(nullptr), cacheSize(cacheSize) {}

	FileStorage::~FileStorage() { close(); }

	void FileStorage::open() {
		if (isFileOpen)
			return;

		bool fileExists = std::filesystem::exists(dbFilePath);

		// Create a new file with header if it doesn't exist
		if (!fileExists) {
			// Create and open the file for reading and writing
			fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out |
																			std::ios::trunc);
			if (!*fileStream) {
				throw std::runtime_error("Cannot create or open database file");
			}

			// Initialize file header through FileHeaderManager
			fileHeaderManager = std::make_shared<FileHeaderManager>(fileStream, fileHeader);
			fileHeaderManager->initializeFileHeader();
		} else {
			// Open file for reading and writing
			fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);
			if (!*fileStream) {
				throw std::runtime_error("Cannot open database file");
			}

			// Validate and read header
			fileHeaderManager = std::make_shared<FileHeaderManager>(fileStream, fileHeader);
			fileHeaderManager->validateAndReadHeader();
		}
		// Initialize components
		initializeComponents();

		isFileOpen = true;
	}

	void FileStorage::initializeComponents() {
		fileHeaderManager->extractFileHeaderInfo();

		segmentTracker = std::make_shared<SegmentTracker>(fileStream);

		// Initialize ID allocator
		idAllocator = std::make_unique<IDAllocator>(
				fileStream, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
				fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef());
		idAllocator->initialize();

		// Set up ID update callback
		idAllocator->setIdUpdateCallback([this](int64_t tempId, int64_t permId, uint32_t entityType) {
			// Handle ID updates by immediately updating entities in the DataManager
			dataManager->handleIdUpdate(tempId, permId, entityType);
		});

		// Then create the space manager
		spaceManager =
				std::make_shared<SpaceManager>(fileStream, dbFilePath, segmentTracker, fileHeaderManager, idAllocator);
		spaceManager->initialize(fileHeader);

		// Initialize data manager
		dataManager = std::make_shared<DataManager>(dbFilePath, cacheSize, fileHeader, idAllocator, segmentTracker, spaceManager);
		dataManager->initialize();

		// Always set up auto-flush callback
		dataManager->setAutoFlushCallback([this]() { this->flush(); });

		databaseInspector = std::make_shared<DatabaseInspector>(fileHeader, fileStream, *dataManager);
		databaseInspector->displayDatabaseStructure();
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
			spaceManager->truncateFile();
			isFileOpen = false;
		}
	}

	void FileStorage::save() {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before saving");
		}

		// Check if we have any changes to save
		if (!dataManager->hasUnsavedChanges()) {
			return;
		}

		// First, allocate permanent IDs for all entities
		allocatePermanentIdsForAllEntities();

		updateEntityReferencesToPermanent();

		// Get entities by their change type
		auto newNodes = dataManager->getDirtyEntitiesWithChangeTypes<Node>({EntityChangeType::ADDED});
		auto modifiedNodes = dataManager->getDirtyEntitiesWithChangeTypes<Node>({EntityChangeType::MODIFIED});
		auto deletedNodes = dataManager->getDirtyEntitiesWithChangeTypes<Node>({EntityChangeType::DELETED});

		auto newEdges = dataManager->getDirtyEntitiesWithChangeTypes<Edge>({EntityChangeType::ADDED});
		auto modifiedEdges = dataManager->getDirtyEntitiesWithChangeTypes<Edge>({EntityChangeType::MODIFIED});
		auto deletedEdges = dataManager->getDirtyEntitiesWithChangeTypes<Edge>({EntityChangeType::DELETED});

		auto newProperties = dataManager->getDirtyEntitiesWithChangeTypes<Property>({EntityChangeType::ADDED});
		auto modifiedProperties = dataManager->getDirtyEntitiesWithChangeTypes<Property>({EntityChangeType::MODIFIED});
		auto deletedProperties = dataManager->getDirtyEntitiesWithChangeTypes<Property>({EntityChangeType::DELETED});

		auto newBlobs = dataManager->getDirtyEntitiesWithChangeTypes<Blob>({EntityChangeType::ADDED});
		auto modifiedBlobs = dataManager->getDirtyEntitiesWithChangeTypes<Blob>({EntityChangeType::MODIFIED});
		auto deletedBlobs = dataManager->getDirtyEntitiesWithChangeTypes<Blob>({EntityChangeType::DELETED});

		// Save new properties first
		if (!newProperties.empty()) {
			std::unordered_map<int64_t, Property> propertiesToSave;
			for (const auto &property: newProperties) {
				propertiesToSave[property.getId()] = property;
			}
			saveData(propertiesToSave, fileHeader.property_segment_head, PROPERTIES_PER_SEGMENT);
		}

		// Save new blobs
		if (!newBlobs.empty()) {
			std::unordered_map<int64_t, Blob> blobsToSave;
			for (const auto &blob: newBlobs) {
				blobsToSave[blob.getId()] = blob;
			}
			saveData(blobsToSave, fileHeader.blob_segment_head, BLOBS_PER_SEGMENT);
		}

		// Save new nodes
		if (!newNodes.empty()) {
			std::unordered_map<int64_t, Node> nodesToSave;
			for (const auto &node: newNodes) {
				nodesToSave[node.getId()] = node;
			}
			saveData(nodesToSave, fileHeader.node_segment_head, NODES_PER_SEGMENT);
		}

		// Save new edges
		if (!newEdges.empty()) {
			std::unordered_map<int64_t, Edge> edgesToSave;
			for (const auto &edge: newEdges) {
				edgesToSave[edge.getId()] = edge;
			}
			saveData(edgesToSave, fileHeader.edge_segment_head, EDGES_PER_SEGMENT);
		}

		// update properties in-place
		for (const auto &property: modifiedProperties) {
			updateEntityInPlace(property);
		}

		// update blobs in-place
		for (const auto &blob: modifiedBlobs) {
			updateEntityInPlace(blob);
		}

		// Update modified nodes in-place
		for (const auto &node: modifiedNodes) {
			updateEntityInPlace(node);
		}

		// Update modified edges in-place
		for (const auto &edge: modifiedEdges) {
			updateEntityInPlace(edge);
		}

		// Delete properties
		for (const auto &property: deletedProperties) {
			deleteEntityOnDisk(property);
		}

		// Delete blobs
		for (const auto &blob: deletedBlobs) {
			deleteEntityOnDisk(blob);
		}

		// Delete nodes
		for (const auto &node: deletedNodes) {
			deleteEntityOnDisk(node);
		}

		// Delete edges
		for (const auto &edge: deletedEdges) {
			deleteEntityOnDisk(edge);
		}

		// Mark everything as saved
		dataManager->markAllSaved();

		// Clean up any remaining temporary ID mappings
		idAllocator->clearTempIdMappings();
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
				bool wasInactive = !segmentTracker->getBitmapBit(segmentOffset, index);
				if (wasInactive) {
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

		// Process entities that need new slots in segments
		uint64_t currentSegmentOffset = segmentHead;
		SegmentHeader currentSegHeader;

		// Locate the last segment if segmentHead != 0
		if (currentSegmentOffset != 0) {
			while (true) {
				fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
				fileStream->read(reinterpret_cast<char *>(&currentSegHeader), sizeof(SegmentHeader));
				if (currentSegHeader.next_segment_offset == 0)
					break;
				currentSegmentOffset = currentSegHeader.next_segment_offset;
			}
		} else {
			// Allocate first segment
			currentSegmentOffset = allocateSegment(T::typeId, itemsPerSegment);
			segmentHead = currentSegmentOffset;
			fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
			fileStream->read(reinterpret_cast<char *>(&currentSegHeader), sizeof(SegmentHeader));
		}

		auto dataIt = entitiesForNewSlots.begin();
		while (dataIt != entitiesForNewSlots.end()) {
			// Calculate remaining space
			uint32_t remaining = currentSegHeader.capacity - currentSegHeader.used;

			if (remaining == 0) {
				// Allocate new segment and link
				uint64_t newOffset = allocateSegment(T::typeId, itemsPerSegment);

				// Update next pointer in current segment
				currentSegHeader.next_segment_offset = newOffset;

				// Write updated segment header
				fileStream->seekp(static_cast<std::streamoff>(currentSegmentOffset));
				fileStream->write(reinterpret_cast<char *>(&currentSegHeader), sizeof(SegmentHeader));

				// Get the new segment header
				SegmentHeader newSegHeader;
				fileStream->seekg(static_cast<std::streamoff>(newOffset));
				fileStream->read(reinterpret_cast<char *>(&newSegHeader), sizeof(SegmentHeader));

				// Update prev pointer in new segment
				newSegHeader.prev_segment_offset = currentSegmentOffset;

				// Set the start ID for the new segment
				newSegHeader.start_id = dataIt->getId();

				// Write updated new segment header
				fileStream->seekp(static_cast<std::streamoff>(newOffset));
				fileStream->write(reinterpret_cast<char *>(&newSegHeader), sizeof(SegmentHeader));
				fileStream->flush();

				// Move to new segment
				currentSegmentOffset = newOffset;
				currentSegHeader = newSegHeader;
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
	void FileStorage::updateEntityInPlace(const T &entity) {
		int64_t id = entity.getId();
		uint64_t segmentOffset = 0;

		segmentOffset = dataManager->findSegmentForEntityId<T>(id);

		if (segmentOffset == 0) {
			throw std::runtime_error("Cannot update entity: entity not found");
		}

		// Read segment header
		SegmentHeader header;
		fileStream->seekg(static_cast<std::streamoff>(segmentOffset));
		fileStream->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		// Calculate entity position within segment
		uint64_t entityIndex = id - header.start_id;
		if (entityIndex >= header.capacity) {
			throw std::runtime_error("Entity index out of bounds for segment");
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

	// Specializations for Node and Edge
	template void FileStorage::updateEntityInPlace<Node>(const Node &entity);
	template void FileStorage::updateEntityInPlace<Edge>(const Edge &entity);

	template<typename T>
	void FileStorage::deleteEntityOnDisk(const T &entity) {
		int64_t id = entity.getId();
		uint64_t segmentOffset = 0;

		segmentOffset = dataManager->findSegmentForEntityId<T>(id);

		if (segmentOffset != 0) {
			// Entity exists on disk, update it in place
			updateEntityInPlace(entity);

			// Mark the ID as available for reuse
			idAllocator->freeId(id, entity.typeId);
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
	void FileStorage::writeSegmentHeader(uint64_t segmentOffset, const SegmentHeader &header) {
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
	void FileStorage::updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive) {
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

	bool FileStorage::verifyBitmapConsistency(uint64_t segmentOffset) {
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

	void FileStorage::flush() {
		// Acquire lock to ensure atomic operation
		std::unique_lock<std::mutex> lock(flushMutex, std::try_to_lock);

		// If we couldn't acquire the lock or a flush is already in progress, return
		if (!lock.owns_lock() || flushInProgress.exchange(true)) {
			return;
		}

		try {
			// Ensure all pending changes are written
			save();

			// Only check for compaction if delete operations occurred
			if (deleteOperationPerformed.load()) {
				if (spaceManager->shouldCompact()) {
					spaceManager->compactSegments();

					idAllocator->refreshInactiveIdsCache(Node::typeId);
					idAllocator->refreshInactiveIdsCache(Edge::typeId);
					idAllocator->refreshInactiveIdsCache(Property::typeId);
					idAllocator->refreshInactiveIdsCache(Blob::typeId);

					dataManager->buildNodeSegmentIndex();
					dataManager->buildEdgeSegmentIndex();
					dataManager->buildPropertySegmentIndex();
					dataManager->buildBlobSegmentIndex();
				}
				// Reset the flag after handling potential compaction
				deleteOperationPerformed.store(false);
			}

			// Persist segment headers
			persistSegmentHeaders();

			fileHeaderManager->updateFileHeader();
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

	void FileStorage::allocatePermanentIdsForAllEntities() {
		// Get entities with temporary IDs
		auto newNodes = dataManager->getDirtyEntitiesWithChangeTypes<Node>({EntityChangeType::ADDED});
		auto newEdges = dataManager->getDirtyEntitiesWithChangeTypes<Edge>({EntityChangeType::ADDED});
		auto newProperties = dataManager->getDirtyEntitiesWithChangeTypes<Property>({EntityChangeType::ADDED});
		auto newBlobs = dataManager->getDirtyEntitiesWithChangeTypes<Blob>({EntityChangeType::ADDED});

		// Process in order: properties, blobs, nodes, edges (dependencies go first)
		std::sort(newProperties.begin(), newProperties.end(),
				  [](const auto &a, const auto &b) { return a.getId() > b.getId(); });
		for (auto &property: newProperties) {
			if (property.hasTemporaryId()) {
				idAllocator->allocatePermanentId(property.getId(), Property::typeId);
				// No need to update property here, callback will handle it
			}
		}

		std::sort(newBlobs.begin(), newBlobs.end(), [](const auto &a, const auto &b) { return a.getId() > b.getId(); });
		for (auto &blob: newBlobs) {
			if (blob.hasTemporaryId()) {
				idAllocator->allocatePermanentId(blob.getId(), Blob::typeId);
				// No need to update blob here, callback will handle it
			}
		}

		std::sort(newNodes.begin(), newNodes.end(), [](const auto &a, const auto &b) { return a.getId() > b.getId(); });
		for (auto &node: newNodes) {
			if (node.hasTemporaryId()) {
				idAllocator->allocatePermanentId(node.getId(), Node::typeId);
				// No need to update node here, callback will handle it
			}
		}

		std::sort(newEdges.begin(), newEdges.end(), [](const auto &a, const auto &b) { return a.getId() > b.getId(); });
		for (auto &edge: newEdges) {
			if (edge.hasTemporaryId()) {
				idAllocator->allocatePermanentId(edge.getId(), Edge::typeId);
				// No need to update edge here, callback will handle it
			}
		}
	}

	void FileStorage::updateEntityReferencesToPermanent() const {
		// Get entities that need reference updates (both ADDED and MODIFIED)
		auto dirtyNodes = dataManager->getDirtyEntitiesWithChangeTypes<Node>(
				{EntityChangeType::ADDED, EntityChangeType::MODIFIED});

		auto dirtyEdges = dataManager->getDirtyEntitiesWithChangeTypes<Edge>(
				{EntityChangeType::ADDED, EntityChangeType::MODIFIED});

		auto dirtyProperties = dataManager->getDirtyEntitiesWithChangeTypes<Property>(
				{EntityChangeType::ADDED, EntityChangeType::MODIFIED});

		auto dirtyBlobs = dataManager->getDirtyEntitiesWithChangeTypes<Blob>(
				{EntityChangeType::ADDED, EntityChangeType::MODIFIED});

		// Update references for properties first
		for (auto &property: dirtyProperties) {
			if (EntityReferenceUpdater::updatePropertyReferencesToPermanent(property, idAllocator)) {
				dataManager->updatePropertyEntity(property);
			}
		}

		// Update references for blobs next
		for (auto &blob: dirtyBlobs) {
			if (EntityReferenceUpdater::updateBlobReferencesToPermanent(blob, idAllocator)) {
				// Ensure your DataManager has this method
				dataManager->updateBlobEntity(blob);
			}
		}

		// Update references for nodes
		for (auto &node: dirtyNodes) {
			if (EntityReferenceUpdater::updateNodeReferencesToPermanent(node, idAllocator)) {
				dataManager->updateNode(node);
			}
		}

		// Update references for edges last
		for (auto &edge: dirtyEdges) {
			if (EntityReferenceUpdater::updateEdgeReferencesToPermanent(edge, idAllocator)) {
				dataManager->updateEdge(edge);
			}
		}
	}

	Node FileStorage::getNode(int64_t id) {
		if (!isFileOpen) {
			open();
		}
		// Load from data manager
		Node node = dataManager->getNode(id);
		return node;
	}

	Edge FileStorage::getEdge(int64_t id) {
		if (!isFileOpen) {
			open();
		}
		Edge edge = dataManager->getEdge(id);
		return edge;
	}

	std::vector<Node> FileStorage::getNodes(const std::vector<int64_t> &ids) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getNodeBatch(ids);
	}

	std::vector<Edge> FileStorage::getEdges(const std::vector<int64_t> &ids) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getEdgeBatch(ids);
	}

	std::vector<Node> FileStorage::getNodesInRange(int64_t startId, int64_t endId, size_t limit) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getNodesInRange(startId, endId, limit);
	}

	std::vector<Edge> FileStorage::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getEdgesInRange(startId, endId, limit);
	}

	std::vector<Edge> FileStorage::findEdgesByNode(int64_t nodeId, const std::string &direction) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->findEdgesByNode(nodeId, direction);
	}

	std::vector<int64_t> FileStorage::findConnectedNodeIds(int64_t nodeId, const std::string &direction) {
		std::vector<Edge> edges = findEdgesByNode(nodeId, direction);
		std::vector<int64_t> connectedNodeIds;

		for (const auto &edge: edges) {
			if (direction == "outgoing" || direction == "both") {
				if (edge.getFromNodeId() == nodeId) {
					connectedNodeIds.push_back(edge.getToNodeId());
				}
			}
			if (direction == "incoming" || direction == "both") {
				if (edge.getToNodeId() == nodeId) {
					connectedNodeIds.push_back(edge.getFromNodeId());
				}
			}
		}
		return connectedNodeIds;
	}

	std::unordered_map<std::string, PropertyValue> FileStorage::getNodeProperties(int64_t nodeId) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getNodeProperties(nodeId);
	}

	std::unordered_map<std::string, PropertyValue> FileStorage::getEdgeProperties(int64_t edgeId) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getEdgeProperties(edgeId);
	}

	Node FileStorage::insertNode(const std::string &label) {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before inserting data");
		}

		// Get a temporary ID (negative number)
		int64_t tempId = dataManager->reserveTemporaryNodeId();

		// Create a new node with the temporary ID

		Node node(tempId, label);

		dataManager->addNode(node);
		return node;
	}

	Edge FileStorage::insertEdge(const int64_t &from, const int64_t &to, const std::string &label) {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before inserting data");
		}

		Node fromNode = dataManager->getNode(from);
		Node toNode = dataManager->getNode(to);

		if (!fromNode.isActive() || !toNode.isActive()) {
			throw std::runtime_error("Cannot create edge between inactive or non-existent nodes");
		}

		// Get a temporary ID
		int64_t tempId = dataManager->reserveTemporaryEdgeId();

		// Create a new edge with the temporary ID
		Edge edge(tempId, from, to, label);

		dataManager->addEdge(edge);
		return edge;
	}

	void FileStorage::insertProperties(int64_t entityId, uint32_t entityType,
									   const std::unordered_map<std::string, PropertyValue> &properties) const {
		// Check the entity type and call the appropriate method
		if (entityType == Node::typeId) {
			dataManager->addNodeProperties(entityId, properties);
		} else if (entityType == Edge::typeId) {
			dataManager->addEdgeProperties(entityId, properties);
		} else {
			throw std::runtime_error("Unsupported entity type for properties: " + std::to_string(entityType));
		}
	}

	void FileStorage::updateNode(const Node &node) {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before updating data");
		}

		// Ensure node exists
		int64_t nodeId = node.getId();
		if (nodeId == 0 || dataManager->findSegmentForEntityId<Node>(nodeId) == 0) {
			throw std::runtime_error("Cannot update node with ID " + std::to_string(nodeId) + ": node doesn't exist");
		}

		// Update the node in DataManager
		dataManager->updateNode(node);
	}

	void FileStorage::updateEdge(const Edge &edge) {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before updating data");
		}

		// Ensure edge exists
		int64_t edgeId = edge.getId();
		if (edgeId == 0 || dataManager->findSegmentForEntityId<Edge>(edgeId) == 0) {
			throw std::runtime_error("Cannot update edge with ID " + std::to_string(edgeId) + ": edge doesn't exist");
		}

		// Update the edge in DataManager
		dataManager->updateEdge(edge);
	}

	void FileStorage::deleteNode(int64_t nodeId) {
		if (!isFileOpen) {
			open();
		}
		auto node = dataManager->getNode(nodeId);
		dataManager->deleteNode(node);
		// Set the flag if deletion was successful
		deleteOperationPerformed.store(true);
	}

	void FileStorage::deleteEdge(int64_t edgeId) {
		if (!isFileOpen) {
			open();
		}
		auto edge = dataManager->getEdge(edgeId);
		dataManager->deleteEdge(edge);
		// Set the flag if deletion was successful
		deleteOperationPerformed.store(true);
	}

	void FileStorage::beginWrite() {
		// Placeholder for transaction management
	}

	void FileStorage::commitWrite() { save(); }

	void FileStorage::rollbackWrite() {
		// Placeholder for transaction management
	}

	void FileStorage::clearCache() const { dataManager->clearCache(); }

	std::unordered_map<int64_t, Node> FileStorage::getAllNodes() {
		if (!isFileOpen) {
			open();
		}
		// Example approach: fetch IDs in range [1..max_node_id], then batch-load
		std::vector<int64_t> allIds;
		allIds.reserve(static_cast<size_t>(idAllocator->getCurrentMaxNodeId()));
		for (int64_t i = 1; i <= idAllocator->getCurrentMaxNodeId(); ++i) {
			// You might maintain a deleted-list, but this is a simple approach
			allIds.push_back(i);
		}

		auto nodeVec = dataManager->getNodeBatch(allIds);
		std::unordered_map<int64_t, Node> result;
		result.reserve(nodeVec.size());
		for (auto &n: nodeVec) {
			if (n.getId() != 0) { // skip invalid / missing
				result[n.getId()] = std::move(n);
			}
		}
		return result;
	}

	std::unordered_map<int64_t, Edge> FileStorage::getAllEdges() {
		if (!isFileOpen) {
			open();
		}
		// Similar approach for edges
		std::vector<int64_t> allIds;
		allIds.reserve(static_cast<size_t>(idAllocator->getCurrentMaxEdgeId()));
		for (int64_t i = 1; i <= idAllocator->getCurrentMaxEdgeId(); ++i) {
			allIds.push_back(i);
		}

		auto edgeVec = dataManager->getEdgeBatch(allIds);
		std::unordered_map<int64_t, Edge> result;
		result.reserve(edgeVec.size());
		for (auto &e: edgeVec) {
			if (e.getId() != 0) {
				result[e.getId()] = std::move(e);
			}
		}
		return result;
	}

} // namespace graph::storage
