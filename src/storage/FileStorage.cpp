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
#include <fstream>
#include <graph/storage/BlobStore.h>
#include <graph/storage/PropertyStorage.h>
#include <sstream>
#include <zlib.h>
#include "graph/core/Edge.h"
#include "graph/core/Node.h"
#include "graph/storage/DatabaseInspector.h"
#include "graph/utils/ChecksumUtils.h"
#include "graph/utils/FixedSizeSerializer.h"
#include "graph/utils/Serializer.h"

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

		auto segmentTracker = std::make_shared<SegmentTracker>(fileStream);

		// Initialize ID allocator
		idAllocator = std::make_unique<IDAllocator>(fileStream, segmentTracker, fileHeaderManager->getMaxNodeIdRef(),
													fileHeaderManager->getMaxEdgeIdRef());
		idAllocator->initialize();

		// Initialize data manager
		dataManager = std::make_unique<DataManager>(dbFilePath, cacheSize, fileHeader, idAllocator);
		dataManager->initialize();

		// Always set up auto-flush callback
		dataManager->setAutoFlushCallback([this]() { this->flush(); });

		// Then create the space manager
		spaceManager =
				std::make_shared<SpaceManager>(fileStream, dbFilePath, segmentTracker, fileHeaderManager, idAllocator);
		spaceManager->initialize(fileHeader);

		// After dataManager initialization
		deletionManager = std::make_unique<DeletionManager>(fileStream, *this, *dataManager, spaceManager);
		deletionManager->initialize();

		databaseInspector = std::make_shared<DatabaseInspector>(fileHeader, fileStream);
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
		auto newNodes = dataManager->getDirtyNodesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::ADDED);
		auto modifiedNodes =
				dataManager->getDirtyNodesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::MODIFIED);
		auto deletedNodes = dataManager->getDirtyNodesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::DELETED);

		auto newEdges = dataManager->getDirtyEdgesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::ADDED);
		auto modifiedEdges =
				dataManager->getDirtyEdgesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::MODIFIED);
		auto deletedEdges = dataManager->getDirtyEdgesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::DELETED);

		// First, store properties for all entities that need it
		// For new nodes
		for (auto &node: newNodes) {
			dataManager->storeEntityProperties<Node>(node, fileStream);
		}

		// For modified nodes
		for (auto &node: modifiedNodes) {
			dataManager->storeEntityProperties<Node>(node, fileStream);
		}

		// For new edges
		for (auto &edge: newEdges) {
			dataManager->storeEntityProperties<Edge>(edge, fileStream);
		}

		// For modified edges
		for (auto &edge: modifiedEdges) {
			dataManager->storeEntityProperties<Edge>(edge, fileStream);
		}

		// Now save the actual entities
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

		// Update modified nodes in-place
		for (const auto &node: modifiedNodes) {
			updateEntityInPlace(node);
		}

		// Update modified edges in-place
		for (const auto &edge: modifiedEdges) {
			updateEntityInPlace(edge);
		}

		// Delete nodes
		for (const auto &node: deletedNodes) {
			deleteEntityOnDisk(node);
		}

		// Delete edges
		for (const auto &edge: deletedEdges) {
			deleteEntityOnDisk(edge);
		}

		// Persist segment headers
		persistSegmentHeaders();

		// Mark everything as saved
		dataManager->markAllSaved();

		// Process any pending ID updates for objects in the database
		dataManager->updateEntitiesWithPermanentIds();

		// Clean up any remaining temporary ID mappings
		idAllocator->clearTempIdMappings();
	}

	template<typename T>
	void FileStorage::saveData(std::unordered_map<int64_t, T> &data, uint64_t &segmentHead, uint32_t itemsPerSegment) {
		if (data.empty())
			return;

		// Process entities with temporary IDs first
		std::unordered_map<int64_t, uint64_t> idMapping; // Maps temporary IDs to permanent IDs
		std::unordered_map<uint64_t, T> permanentIdData; // Entities with permanent IDs

		for (auto &[id, entity]: data) {
			if (IDAllocator::isTemporaryId(id)) {
				// Allocate a permanent ID
				uint64_t permanentId = idAllocator->allocatePermanentId(id, T::typeId);

				// Update the entity with its permanent ID
				entity.setPermanentId(permanentId);

				// Store in mapping
				idMapping[id] = permanentId;

				// Add to permanent ID data
				permanentIdData[permanentId] = entity;
			} else {
				// Already has a permanent ID
				permanentIdData[id] = entity;
			}
		}

		// Sort data by permanent ID
		std::vector<T> sortedData;
		sortedData.reserve(permanentIdData.size());
		for (const auto &[id, item]: permanentIdData) {
			sortedData.push_back(item);
		}
		std::sort(sortedData.begin(), sortedData.end(), [](const T &a, const T &b) { return a.getId() < b.getId(); });

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

		auto dataIt = sortedData.begin();
		while (dataIt != sortedData.end()) {
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
			uint32_t writeCount = std::min(remaining, static_cast<uint32_t>(sortedData.end() - dataIt));
			std::vector<T> batch(dataIt, dataIt + writeCount);

			// // If this is a new segment, set its start ID
			// if (currentSegHeader.used == 0 && !batch.empty()) {
			//     currentSegHeader.start_id = batch.front().getId();
			//     fileStream->seekp(static_cast<std::streamoff>(currentSegmentOffset));
			//     fileStream->write(reinterpret_cast<char *>(&currentSegHeader), sizeof(SegmentHeader));
			// }

			// Write data
			writeSegmentData(currentSegmentOffset, batch, currentSegHeader.used);

			// Reload header to get updated used count
			fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
			fileStream->read(reinterpret_cast<char *>(&currentSegHeader), sizeof(SegmentHeader));

			dataIt += writeCount;
		}

		// // Update max IDs in header if needed
		// if constexpr (std::is_same_v<T, Node>) {
		// 	fileHeader.max_node_id = idAllocator_->getCurrentMaxNodeId();
		// } else if constexpr (std::is_same_v<T, Edge>) {
		// 	fileHeader.max_edge_id = idAllocator_->getCurrentMaxEdgeId();
		// }
	}

	uint64_t FileStorage::allocateSegment(uint8_t type, uint32_t capacity) const {
		fileStream->seekp(0, std::ios::end);
		if (!*fileStream) {
			throw std::runtime_error("Failed to seek to end of file.");
		}

		const uint64_t baseOffset = fileStream->tellp();

		SegmentHeader header;
		header.data_type = type;
		header.capacity = capacity;
		header.used = 0;
		header.inactive_count = 0;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;
		header.start_id = 0;

		// Initialize bitmap
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);
		memset(header.activity_bitmap, 0, MAX_BITMAP_SIZE); // Initialize bitmap to all 0s

		// Calculate header CRC
		header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

		// Write segment header
		fileStream->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		if (!*fileStream) {
			throw std::runtime_error("Failed to write SegmentHeader.");
		}

		// Write empty data area
		size_t itemSize = (type == 0) ? sizeof(Node) : sizeof(Edge);
		size_t dataSize = capacity * itemSize;
		std::vector<char> emptyData(dataSize, 0);
		fileStream->write(emptyData.data(), static_cast<std::streamsize>(dataSize));
		if (!*fileStream) {
			throw std::runtime_error("Failed to write segment data.");
		}

		fileStream->flush();
		return baseOffset;
	}

	template<typename T>
	void FileStorage::writeSegmentData(uint64_t segmentOffset, const std::vector<T> &data, uint32_t baseUsed) {
		// Read segment header
		SegmentHeader header;
		fileStream->seekg(static_cast<std::streamoff>(segmentOffset));
		fileStream->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		// Calculate item size
		const size_t itemSize = (header.data_type == 0) ? sizeof(Node) : sizeof(Edge);

		// Write data
		uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;
		fileStream->seekp(static_cast<std::streamoff>(dataOffset));
		for (const auto &item: data) {
			utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, item, itemSize);
			dataOffset += itemSize;
		}

		// Update header
		header.used = baseUsed + static_cast<uint32_t>(data.size());
		if (baseUsed == 0 && !data.empty()) {
			header.start_id = data.front().getId();
		}

		// Ensure bitmap_size is correctly set
		if (header.bitmap_size == 0) {
			header.bitmap_size = bitmap::calculateBitmapSize(header.capacity);
		}

		// Calculate and write CRC of segment data
		header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

		// Write updated header back to file
		fileStream->seekp(static_cast<std::streamoff>(segmentOffset));
		fileStream->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		fileStream->flush();

		// Update bitmap for the newly added entities
		if (!data.empty()) {
			updateSegmentBitmap(segmentOffset, data.front().getId(), static_cast<uint32_t>(data.size()), true);
		}
	}

	template<typename T>
	void FileStorage::updateEntityInPlace(const T &entity) {
		uint64_t id = entity.getId();
		uint64_t segmentOffset;

		if constexpr (std::is_same_v<T, Node>) {
			segmentOffset = dataManager->findSegmentForNodeId(id);
		} else {
			segmentOffset = dataManager->findSegmentForEdgeId(id);
		}

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
		uint64_t entityOffset = segmentOffset + sizeof(SegmentHeader) + entityIndex * sizeof(T);

		// Write entity
		fileStream->seekp(static_cast<std::streamoff>(entityOffset));
		utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, entity, sizeof(T));
		fileStream->flush();

		// Update bitmap to reflect entity's active state
		updateBitmapForEntity<T>(segmentOffset, id, entity.isActive());
	}

	// Specializations for Node and Edge
	template void FileStorage::updateEntityInPlace<Node>(const Node &entity);
	template void FileStorage::updateEntityInPlace<Edge>(const Edge &entity);

	template<typename T>
	void FileStorage::deleteEntityOnDisk(const T &entity) {
		uint64_t id = entity.getId();
		uint64_t segmentOffset;

		if constexpr (std::is_same_v<T, Node>) {
			segmentOffset = dataManager->findSegmentForNodeId(id);
		} else {
			segmentOffset = dataManager->findSegmentForEdgeId(id);
		}

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

	// Read segment header from disk
	SegmentHeader FileStorage::readSegmentHeader(uint64_t segmentOffset) const {
		SegmentHeader header;
		fileStream->seekg(static_cast<std::streamoff>(segmentOffset));
		fileStream->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));
		if (!*fileStream) {
			throw std::runtime_error("Failed to read segment header at offset " + std::to_string(segmentOffset));
		}
		return header;
	}

	// Write segment header to disk
	void FileStorage::writeSegmentHeader(uint64_t segmentOffset, const SegmentHeader &header) {
		fileStream->seekp(static_cast<std::streamoff>(segmentOffset));
		fileStream->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		if (!*fileStream) {
			throw std::runtime_error("Failed to write segment header at offset " + std::to_string(segmentOffset));
		}
		fileStream->flush();
	}

	// Update bitmap for a specific entity in the segment
	template<typename EntityType>
	void FileStorage::updateBitmapForEntity(uint64_t segmentOffset, uint64_t entityId, bool isActive) {
		if (segmentOffset == 0) {
			return; // No segment to update
		}

		// Read segment header
		SegmentHeader header = readSegmentHeader(segmentOffset);

		// Calculate entity position within segment
		uint64_t entityIndex = entityId - header.start_id;
		if (entityIndex >= header.capacity) {
			throw std::runtime_error("Entity index out of bounds for segment bitmap update");
		}

		// Ensure bitmap_size is correctly set if it's not already
		if (header.bitmap_size == 0) {
			header.bitmap_size = bitmap::calculateBitmapSize(header.capacity);
		}

		// Update bitmap - set bit to 1 if active, 0 if inactive
		bitmap::setBit(header.activity_bitmap, entityIndex, isActive);

		// Update inactive count if needed
		if (!isActive) {
			header.inactive_count++;
		} else if (header.inactive_count > 0) {
			header.inactive_count--;
		}

		// Recalculate segment CRC
		header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

		// Write updated header
		writeSegmentHeader(segmentOffset, header);

		// Update segment usage in SpaceManager
		spaceManager->getTracker()->updateSegmentUsage(segmentOffset, header.used, header.inactive_count);
	}

	// Update bitmap for batch operations
	void FileStorage::updateSegmentBitmap(uint64_t segmentOffset, uint64_t startId, uint32_t count, bool isActive) {
		if (segmentOffset == 0 || count == 0) {
			return;
		}

		// Read segment header
		SegmentHeader header = readSegmentHeader(segmentOffset);

		// Calculate relative start position
		uint64_t startIndex = startId - header.start_id;
		if (startIndex + count > header.capacity) {
			throw std::runtime_error("Entity range out of bounds for segment bitmap batch update");
		}

		// Ensure bitmap_size is correctly set
		if (header.bitmap_size == 0) {
			header.bitmap_size = bitmap::calculateBitmapSize(header.capacity);
		}

		// Update bitmap for each entity in range
		for (uint32_t i = 0; i < count; i++) {
			uint64_t entityIndex = startIndex + i;
			bool currentState = bitmap::getBit(header.activity_bitmap, entityIndex);

			// Only adjust counts if the state is changing
			if (currentState != isActive) {
				if (isActive) {
					// Activating an inactive entity
					if (header.inactive_count > 0) {
						header.inactive_count--;
					}
				} else {
					// Deactivating an active entity
					header.inactive_count++;
				}
			}

			bitmap::setBit(header.activity_bitmap, entityIndex, isActive);
		}

		// Recalculate segment CRC
		header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

		// Write updated header
		writeSegmentHeader(segmentOffset, header);
	}

	// Template instantiations
	template void FileStorage::updateBitmapForEntity<Node>(uint64_t segmentOffset, uint64_t entityId, bool isActive);
	template void FileStorage::updateBitmapForEntity<Edge>(uint64_t segmentOffset, uint64_t entityId, bool isActive);

	bool FileStorage::verifyBitmapConsistency(uint64_t segmentOffset) {
		if (segmentOffset == 0) {
			return false;
		}

		// Read segment header
		SegmentHeader header = readSegmentHeader(segmentOffset);

		// Count inactive items in bitmap
		uint32_t inactiveCount = 0;
		for (uint32_t i = 0; i < header.used; i++) {
			if (!bitmap::getBit(header.activity_bitmap, i)) {
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

	void FileStorage::persistSegmentHeaders() {
		// Persist segment headers for any segments marked for compaction
		auto nodeSegments = spaceManager->getTracker()->getSegmentsByType(0);
		for (const auto &info: nodeSegments) {
			if (info.needsCompaction) {
				SegmentHeader header;
				fileStream->seekg(static_cast<std::streamoff>(info.offset));
				fileStream->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

				// Update header if needed (e.g., update used count)
				fileStream->seekp(static_cast<std::streamoff>(info.offset));
				fileStream->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
			}
		}

		// Do the same for edge segments
		auto edgeSegments = spaceManager->getTracker()->getSegmentsByType(1);
		for (const auto &info: edgeSegments) {
			if (info.needsCompaction) {
				SegmentHeader header;
				fileStream->seekg(static_cast<std::streamoff>(info.offset));
				fileStream->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

				// Update header if needed
				fileStream->seekp(static_cast<std::streamoff>(info.offset));
				fileStream->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
			}
		}
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

					dataManager->buildNodeSegmentIndex();
					dataManager->buildEdgeSegmentIndex();
					dataManager->buildPropertySegmentIndex();
				}
				// Reset the flag after handling potential compaction
				deleteOperationPerformed.store(false);
			}

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

	void FileStorage::updateNodeProperty(uint64_t nodeId, const std::string &key, const PropertyValue &value) {
		if (!isFileOpen) {
			open();
		}
		dataManager->updateNodeProperty(nodeId, key, value);
	}

	void FileStorage::updateEdgeProperty(uint64_t edgeId, const std::string &key, const PropertyValue &value) {
		if (!isFileOpen) {
			open();
		}
		dataManager->updateEdgeProperty(edgeId, key, value);
	}

	PropertyValue FileStorage::getNodeProperty(uint64_t nodeId, const std::string &key) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getNodeProperty(nodeId, key);
	}

	PropertyValue FileStorage::getEdgeProperty(uint64_t edgeId, const std::string &key) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getEdgeProperty(edgeId, key);
	}

	std::unordered_map<std::string, PropertyValue> FileStorage::getNodeProperties(uint64_t nodeId) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getNodeProperties(nodeId);
	}

	std::unordered_map<std::string, PropertyValue> FileStorage::getEdgeProperties(uint64_t edgeId) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getEdgeProperties(edgeId);
	}

	void FileStorage::removeNodeProperty(uint64_t nodeId, const std::string &key) {
		if (!isFileOpen) {
			open();
		}
		dataManager->removeNodeProperty(nodeId, key);
	}

	void FileStorage::removeEdgeProperty(uint64_t edgeId, const std::string &key) {
		if (!isFileOpen) {
			open();
		}
		dataManager->removeEdgeProperty(edgeId, key);
	}

	uint64_t FileStorage::storeBlob(const std::string &data, const std::string &contentType) {
		if (!isFileOpen) {
			open();
		}
		return dataManager->storeBlob(data, contentType);
	}

	BlobStore &FileStorage::getBlobStore() {
		if (!isFileOpen) {
			open();
		}
		return dataManager->getBlobStore();
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

	void FileStorage::updateNode(const Node &node) {
		if (!isFileOpen) {
			throw std::runtime_error("Database must be open before updating data");
		}

		// Ensure node exists
		int64_t nodeId = node.getId();
		if (nodeId == 0 || dataManager->findSegmentForNodeId(nodeId) == 0) {
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
		uint64_t edgeId = edge.getId();
		if (edgeId == 0 || dataManager->findSegmentForEdgeId(edgeId) == 0) {
			throw std::runtime_error("Cannot update edge with ID " + std::to_string(edgeId) + ": edge doesn't exist");
		}

		// Update the edge in DataManager
		dataManager->updateEdge(edge);
	}

	bool FileStorage::deleteNode(uint64_t nodeId, bool cascadeEdges) {
		if (!isFileOpen) {
			open();
		}
		bool result = deletionManager->deleteNode(nodeId, cascadeEdges);
		// Set the flag if deletion was successful
		if (result) {
			deleteOperationPerformed.store(true);
		}
		return result;
	}

	bool FileStorage::deleteEdge(uint64_t edgeId) {
		if (!isFileOpen) {
			open();
		}
		bool result = deletionManager->deleteEdge(edgeId);
		// Set the flag if deletion was successful
		if (result) {
			deleteOperationPerformed.store(true);
		}
		return result;
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
