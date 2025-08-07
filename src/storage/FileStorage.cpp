/**
 * @file FileStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileStorage.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <graph/core/Database.hpp>
#include <graph/storage/data/EntityTraits.hpp>
#include <sstream>
#include <utility>
#include <zlib.h>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/storage/DatabaseInspector.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

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
				fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
				fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());
		idAllocator->initialize();

		// Then create the space manager
		spaceManager =
				std::make_shared<SpaceManager>(fileStream, dbFilePath, segmentTracker, fileHeaderManager, idAllocator);
		spaceManager->initialize(fileHeader);

		// Initialize data manager
		dataManager = std::make_shared<DataManager>(fileStream, cacheSize, fileHeader, idAllocator, segmentTracker,
													spaceManager);
		dataManager->initialize();
		dataManager->setDeletionFlagReference(&deleteOperationPerformed);

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

		auto newIndexes = dataManager->getDirtyEntitiesWithChangeTypes<Index>({EntityChangeType::ADDED});
		auto modifiedIndexes = dataManager->getDirtyEntitiesWithChangeTypes<Index>({EntityChangeType::MODIFIED});
		auto deletedIndexes = dataManager->getDirtyEntitiesWithChangeTypes<Index>({EntityChangeType::DELETED});

		auto newStates = dataManager->getDirtyEntitiesWithChangeTypes<State>({EntityChangeType::ADDED});
		auto modifiedStates = dataManager->getDirtyEntitiesWithChangeTypes<State>({EntityChangeType::MODIFIED});
		auto deletedStates = dataManager->getDirtyEntitiesWithChangeTypes<State>({EntityChangeType::DELETED});

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

		// Save new indexes
		if (!newIndexes.empty()) {
			std::unordered_map<int64_t, Index> indexesToSave;
			for (const auto &index: newIndexes) {
				indexesToSave[index.getId()] = index;
			}
			saveData(indexesToSave, fileHeader.index_segment_head, INDEXES_PER_SEGMENT);
		}

		// Save new states
		if (!newStates.empty()) {
			std::unordered_map<int64_t, State> statesToSave;
			for (const auto &state: newStates) {
				statesToSave[state.getId()] = state;
			}
			saveData(statesToSave, fileHeader.state_segment_head, STATES_PER_SEGMENT);
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

		// Update modified indexes in-place
		for (const auto &index: modifiedIndexes) {
			updateEntityInPlace(index);
		}

		// Update modified states in-place
		for (const auto &state: modifiedStates) {
			updateEntityInPlace(state);
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

		// Delete indexes
		for (const auto &index: deletedIndexes) {
			deleteEntityOnDisk(index);
		}

		// Delete states
		for (const auto &state: deletedStates) {
			deleteEntityOnDisk(state);
		}

		// Mark everything as saved
		dataManager->markAllSaved();
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

	// TODO: Subsequent flush operations should be executed in a queue
	void FileStorage::flush() {
		// Acquire lock to ensure atomic operation
		std::unique_lock<std::mutex> lock(flushMutex, std::try_to_lock);

		// If we couldn't acquire the lock or a flush is already in progress, return
		if (!lock.owns_lock() || flushInProgress.exchange(true)) {
			return;
		}

		try {
			queryEngine->persistIndexState();

			// Ensure all pending changes are written
			save();

			// Only check for compaction if delete operations occurred
			if (deleteOperationPerformed.load()) {
				if (spaceManager->shouldCompact()) {
					// Use the thread-safe compaction method

					// Only perform post-compaction operations if compaction was successful
					if (spaceManager->safeCompactSegments()) {
						dataManager->clearCache();

						// Refresh inactive IDs cache for all entity types
						idAllocator->refreshInactiveIdsCache(Node::typeId);
						idAllocator->refreshInactiveIdsCache(Edge::typeId);
						idAllocator->refreshInactiveIdsCache(Property::typeId);
						idAllocator->refreshInactiveIdsCache(Blob::typeId);
						idAllocator->refreshInactiveIdsCache(Index::typeId);
						idAllocator->refreshInactiveIdsCache(State::typeId);

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

	Node FileStorage::insertNode(const std::string &label) const {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before inserting data");
		}

		Node node(0, label);

		dataManager->addNode(node);
		return node;
	}

	Edge FileStorage::insertEdge(const int64_t &from, const int64_t &to, const std::string &label) const {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before inserting data");
		}

		Node fromNode = dataManager->getNode(from);
		Node toNode = dataManager->getNode(to);

		if (!fromNode.isActive() || !toNode.isActive()) {
			throw std::runtime_error("Cannot create edge between inactive or non-existent nodes");
		}

		// Check for existing edge first
		auto existingEdges = dataManager->findEdgesByNode(from, "outgoing");
		for (const auto &edge: existingEdges) {
			if (edge.getTargetNodeId() == to && edge.getLabel() == label && edge.isActive()) {
				return edge; // Return existing edge
			}
		}

		// Create new edge if none exists
		Edge edge(0, from, to, label);
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

	void FileStorage::updateNode(const Node &node) const {
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

	void FileStorage::updateEdge(const Edge &edge) const {
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
	}

	void FileStorage::deleteEdge(int64_t edgeId) {
		if (!isFileOpen) {
			open();
		}
		auto edge = dataManager->getEdge(edgeId);
		dataManager->deleteEdge(edge);
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
