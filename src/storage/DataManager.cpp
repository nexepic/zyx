/**
 * @file DataManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/DataManager.h"
#include <algorithm>
#include <graph/storage/PropertyStorage.h>
#include <graph/storage/SegmentTracker.h>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace graph::storage {

	DataManager::DataManager(const std::string &dbFilePath, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator, std::shared_ptr<SegmentTracker> segmentTracker) :
		dbFilePath_(dbFilePath), nodeCache_(cacheSize), edgeCache_(cacheSize), propertyCache_(cacheSize * 2),
		blobCache_(cacheSize / 4), fileHeader_(fileHeader), idAllocator_(std::move(idAllocator)),
		segmentTracker_(std::move(segmentTracker)) {
		file_ = std::make_shared<std::ifstream>(dbFilePath, std::ios::binary);
		if (!file_->good()) {
			throw std::runtime_error("Cannot open database file");
		}
	}

	DataManager::~DataManager() {
		if (file_ && file_->is_open()) {
			file_->close();
		}
	}

	void DataManager::initialize() {
		// Build segment indexes for efficient lookups
		buildNodeSegmentIndex();
		buildEdgeSegmentIndex();

		propertySegmentHead_ = fileHeader_.property_segment_head;
		if (propertySegmentHead_ != 0) {
			buildPropertySegmentIndex();
		}

		if (fileHeader_.blob_section_head != 0) {
			// Reopen file with read-write access for BlobStore
			auto fileStream =
					std::make_shared<std::fstream>(dbFilePath_, std::ios::binary | std::ios::in | std::ios::out);
			blobStore_ = std::make_unique<BlobStore>(fileStream, fileHeader_.blob_section_head);
		}
	}

	template<typename EntityType>
	struct EntityGetter {
		// This will be specialized for Node and Edge
	};

	// Specialization for Node
	template<>
	struct EntityGetter<Node> {
		static Node get(DataManager *manager, uint64_t id) {
			Node node = manager->getNode(id);
			if (node.getId() != 0 && !node.isActive()) {
				return {}; // Return empty node if inactive
			}
			return node;
		}

		static void refresh(DataManager *manager, const Node &entity) { manager->refreshNode(entity); }

		static void markEntityDirty(DataManager *manager, Node &node) { manager->markNodeDirty(node); }

		static uint64_t findSegmentForId(const DataManager *manager, int64_t id) {
			return manager->findSegmentForNodeId(id);
		}

		static constexpr uint8_t typeId = Node::typeId;
	};

	// Specialization for Edge
	template<>
	struct EntityGetter<Edge> {
		static Edge get(DataManager *manager, uint64_t id) {
			Edge edge = manager->getEdge(id);
			if (edge.getId() != 0 && !edge.isActive()) {
				return {}; // Return empty edge if inactive
			}
			return edge;
		}

		static void refresh(DataManager *manager, const Edge &entity) { manager->refreshEdge(entity); }

		static void markEntityDirty(DataManager *manager, Edge &edge) { manager->markEdgeDirty(edge); }

		static uint64_t findSegmentForId(const DataManager *manager, uint64_t id) {
			return manager->findSegmentForEdgeId(id);
		}

		static constexpr uint8_t typeId = Edge::typeId;
	};

	// Generic method to build segment index
	void DataManager::buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const {
		segmentIndex.clear();
		uint64_t currentOffset = segmentHead;

		while (currentOffset != 0) {
			// Use the segment tracker to read the header if available
			SegmentHeader header = segmentTracker_->readSegmentHeader(currentOffset);

			SegmentIndex index{};
			index.startId = header.start_id;
			index.endId = header.start_id + header.used - 1;
			index.segmentOffset = currentOffset;

			segmentIndex.push_back(index);
			currentOffset = header.next_segment_offset;
		}

		// Sort by startId to enable binary search
		std::sort(segmentIndex.begin(), segmentIndex.end(),
				  [](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });
	}

	void DataManager::buildNodeSegmentIndex() { buildSegmentIndex(nodeSegmentIndex_, fileHeader_.node_segment_head); }

	void DataManager::buildEdgeSegmentIndex() { buildSegmentIndex(edgeSegmentIndex_, fileHeader_.edge_segment_head); }

	void DataManager::buildPropertySegmentIndex() {
		propertySegmentIndex_.clear();
		uint64_t currentOffset = propertySegmentHead_;

		while (currentOffset != 0) {
			PropertySegmentHeader header = segmentTracker_->readPropertySegmentHeader(currentOffset);

			// Here we do not store ID ranges like nodes and edges, because property entries are organized by entity ID
			SegmentIndex index{};
			index.segmentOffset = currentOffset;
			index.startId = 0; // For property segments, we do not use ID ranges
			index.endId = 0;

			propertySegmentIndex_.push_back(index);
			currentOffset = header.next_segment_offset;
		}
	}

	// Generic method to find segment for ID
	uint64_t DataManager::findSegmentForId(const std::vector<SegmentIndex> &segmentIndex, int64_t id) {
		// Binary search to find the segment containing this ID
		auto it = std::lower_bound(segmentIndex.begin(), segmentIndex.end(), id,
								   [](const SegmentIndex &index, const int64_t value) { return index.endId < value; });

		if (it != segmentIndex.end() && id >= it->startId && id <= it->endId) {
			return it->segmentOffset;
		}

		return 0; // Not found
	}

	uint64_t DataManager::findSegmentForNodeId(int64_t id) const { return findSegmentForId(nodeSegmentIndex_, id); }

	uint64_t DataManager::findSegmentForEdgeId(int64_t id) const { return findSegmentForId(edgeSegmentIndex_, id); }

	uint64_t DataManager::findPropertyEntry(int64_t entityId, uint8_t entityType) const {
		// Find the property entry for the given entity in the property segments
		for (const auto &segmentIndex: propertySegmentIndex_) {
			uint64_t segmentOffset = segmentIndex.segmentOffset;

			// Read the segment header
			PropertySegmentHeader header = segmentTracker_->readPropertySegmentHeader(segmentOffset);

			// Iterate through all entries in the segment
			uint64_t currentPos = segmentOffset + sizeof(PropertySegmentHeader);
			uint64_t endPos = segmentOffset + sizeof(PropertySegmentHeader) + header.used;

			while (currentPos < endPos) {
				file_->seekg(static_cast<std::streamoff>(currentPos));

				// Read the entry header
				PropertyEntryHeader entryHeader;
				file_->read(reinterpret_cast<char *>(&entryHeader), sizeof(entryHeader));

				// Check if it matches
				if (entryHeader.entity_id == entityId && entryHeader.entity_type == entityType) {
					return currentPos;
				}

				// Move to the next entry
				currentPos += sizeof(PropertyEntryHeader) + entryHeader.data_size;
			}
		}

		// Not found
		return 0;
	}

	int64_t DataManager::reserveTemporaryNodeId() { return idAllocator_->reserveTemporaryId(Node::typeId); }

	int64_t DataManager::reserveTemporaryEdgeId() { return idAllocator_->reserveTemporaryId(Edge::typeId); }

	void DataManager::addNode(const Node &node) {
		// Add to cache
		nodeCache_.put(node.getId(), node);

		// Mark as dirty (needs to be persisted)
		dirtyNodes_[node.getId()] = {DirtyEntityInfo::ChangeType::ADDED, node, std::nullopt};

		// Check if auto-flush is needed
		checkAndTriggerAutoFlush();
	}

	void DataManager::addEdge(const Edge &edge) {
		// Add to cache
		edgeCache_.put(edge.getId(), edge);

		// Mark as dirty (needs to be persisted)
		dirtyEdges_[edge.getId()] = {DirtyEntityInfo::ChangeType::ADDED, std::nullopt, edge};

		// Check if auto-flush is needed
		checkAndTriggerAutoFlush();
	}

	void DataManager::updateNode(const Node &node) {
		// Check if the node is active
		if (!node.isActive()) {
			throw std::runtime_error("Cannot update inactive node " + std::to_string(node.getId()));
		}

		// Add to cache
		nodeCache_.put(node.getId(), node);

		// Check if the node is already marked as ADDED in dirtyNodes_
		auto it = dirtyNodes_.find(node.getId());
		if (it != dirtyNodes_.end() && it->second.changeType == DirtyEntityInfo::ChangeType::ADDED) {
			// Keep ADDED state but update the node backup with the latest version
			dirtyNodes_[node.getId()] = {DirtyEntityInfo::ChangeType::ADDED,
										 node, // This captures the updated node with properties
										 std::nullopt};
			return;
		}

		// Mark as dirty (needs to be persisted)
		dirtyNodes_[node.getId()] = {DirtyEntityInfo::ChangeType::MODIFIED, node, std::nullopt};

		// Check if auto-flush is needed
		checkAndTriggerAutoFlush();
	}

	void DataManager::updateEdge(const Edge &edge) {
		// Check if the edge is active
		if (!edge.isActive()) {
			throw std::runtime_error("Cannot update inactive edge " + std::to_string(edge.getId()));
		}

		// Add to cache
		edgeCache_.put(edge.getId(), edge);

		// Check if the edge is already marked as ADDED in dirtyEdges_
		auto it = dirtyEdges_.find(edge.getId());
		if (it != dirtyEdges_.end() && it->second.changeType == DirtyEntityInfo::ChangeType::ADDED) {
			dirtyEdges_[edge.getId()] = {DirtyEntityInfo::ChangeType::ADDED, std::nullopt, edge};
			return;
		}

		// Mark as dirty (needs to be persisted)
		dirtyEdges_[edge.getId()] = {DirtyEntityInfo::ChangeType::MODIFIED, std::nullopt, edge};

		// Check if auto-flush is needed
		checkAndTriggerAutoFlush();
	}

	void DataManager::deleteNode(Node &node) {
		node.markInactive();
		// Keep a backup copy before removing from cache
		dirtyNodes_[node.getId()] = {DirtyEntityInfo::ChangeType::DELETED, node, std::nullopt};

		// Remove from cache
		nodeCache_.remove(node.getId());

		// Check if auto-flush is needed
		checkAndTriggerAutoFlush();
	}

	void DataManager::deleteEdge(Edge &edge) {
		edge.markInactive();
		// Keep a backup copy before removing from cache
		dirtyEdges_[edge.getId()] = {DirtyEntityInfo::ChangeType::DELETED, std::nullopt, edge};

		// Remove from cache
		edgeCache_.remove(edge.getId());

		// Check if auto-flush is needed
		checkAndTriggerAutoFlush();
	}

	std::vector<Node> DataManager::getDirtyNodesWithChangeType(DirtyEntityInfo::ChangeType type) const {
		std::vector<Node> result;
		for (const auto &[id, info]: dirtyNodes_) {
			if (info.changeType == type) {
				if (info.nodeBackup.has_value()) {
					result.push_back(*info.nodeBackup);
				} else if (nodeCache_.contains(id)) {
					result.push_back(nodeCache_.peek(id));
				}
			}
		}
		return result;
	}

	std::vector<Edge> DataManager::getDirtyEdgesWithChangeType(DirtyEntityInfo::ChangeType type) const {
		std::vector<Edge> result;
		for (const auto &[id, info]: dirtyEdges_) {
			if (info.changeType == type) {
				if (info.edgeBackup.has_value()) {
					result.push_back(*info.edgeBackup);
				} else if (edgeCache_.contains(id)) {
					result.push_back(edgeCache_.peek(id));
				}
			}
		}
		return result;
	}

	void DataManager::refreshNode(const Node &node) {
		// Add to cache
		nodeCache_.put(node.getId(), node);
	}

	void DataManager::refreshEdge(const Edge &edge) {
		// Add to cache
		edgeCache_.put(edge.getId(), edge);
	}

	void DataManager::markAllSaved() {
		dirtyNodes_.clear();
		dirtyEdges_.clear();
	}

	void DataManager::checkAndTriggerAutoFlush() const {
		if (needsAutoFlush() && autoFlushCallback_) {
			autoFlushCallback_();
		}
	}

	Node DataManager::getNode(int64_t id) { return getEntityFromMemoryOrDisk<Node>(id); }

	Edge DataManager::getEdge(int64_t id) { return getEntityFromMemoryOrDisk<Edge>(id); }

	Node DataManager::loadNodeFromDisk(int64_t id) const {
		auto nodeOpt = findAndReadEntity<Node>(id);
		return nodeOpt.value_or(Node()); // Return empty node if not found or deleted
	}

	Edge DataManager::loadEdgeFromDisk(int64_t id) const {
		auto edgeOpt = findAndReadEntity<Edge>(id);
		return edgeOpt.value_or(Edge()); // Return empty edge if not found or deleted
	}

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) {
		std::vector<Node> result;
		result.reserve(ids.size());
		std::vector<int64_t> missedIds;

		// First check dirty collections and cache
		for (int64_t id: ids) {
			// Check dirty collections
			auto dirtyNode = getEntityFromDirty<Node>(id);
			if (dirtyNode.has_value()) {
				result.push_back(*dirtyNode);
				continue;
			}

			// Check cache
			if (nodeCache_.contains(id)) {
				result.push_back(nodeCache_.get(id));
			} else {
				missedIds.push_back(id);
			}
		}

		// Group missed IDs by segment for efficient loading
		std::unordered_map<uint64_t, std::vector<int64_t>> segmentToIds;
		for (int64_t id: missedIds) {
			uint64_t segmentOffset = findSegmentForNodeId(id);
			if (segmentOffset != 0) {
				segmentToIds[segmentOffset].push_back(id);
			}
		}

		// Load nodes segment by segment
		for (const auto &[segmentOffset, segmentIds]: segmentToIds) {
			for (int64_t id: segmentIds) {
				auto nodeOpt = findAndReadEntity<Node>(id);
				if (nodeOpt.has_value()) {
					const Node &node = nodeOpt.value();
					result.push_back(node);
					// Add to cache
					nodeCache_.put(id, node);
				}
			}
		}

		return result;
	}

	std::vector<Edge> DataManager::getEdgeBatch(const std::vector<int64_t> &ids) {
		std::vector<Edge> result;
		result.reserve(ids.size());
		std::vector<int64_t> missedIds;

		// First check dirty collections and cache
		for (int64_t id: ids) {
			// Check dirty collections
			auto dirtyEdge = getEntityFromDirty<Edge>(id);
			if (dirtyEdge.has_value()) {
				result.push_back(*dirtyEdge);
				continue;
			}

			// Check cache
			if (edgeCache_.contains(id)) {
				result.push_back(edgeCache_.get(id));
			} else {
				missedIds.push_back(id);
			}
		}

		// Group missed IDs by segment for efficient loading
		std::unordered_map<uint64_t, std::vector<int64_t>> segmentToIds;
		for (int64_t id: missedIds) {
			uint64_t segmentOffset = findSegmentForEdgeId(id);
			if (segmentOffset != 0) {
				segmentToIds[segmentOffset].push_back(id);
			}
		}

		// Load edges segment by segment
		for (const auto &[segmentOffset, segmentIds]: segmentToIds) {
			for (int64_t id: segmentIds) {
				auto edgeOpt = findAndReadEntity<Edge>(id);
				if (edgeOpt.has_value()) {
					const Edge &edge = edgeOpt.value();
					result.push_back(edge);
					// Add to cache
					edgeCache_.put(id, edge);
				}
			}
		}

		return result;
	}

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) {
		std::vector<Node> result;
		if (startId > endId || limit == 0) {
			return result;
		}

		// Find relevant segments that contain the range
		std::vector<uint64_t> relevantSegments;
		for (const auto &segmentIndex: nodeSegmentIndex_) {
			if ((segmentIndex.startId <= endId && segmentIndex.endId >= startId) && (result.size() < limit)) {
				relevantSegments.push_back(segmentIndex.segmentOffset);
			}
		}

		// Load nodes from each relevant segment
		for (uint64_t segmentOffset: relevantSegments) {
			std::vector<Node> segmentNodes = loadNodesFromSegment(segmentOffset, startId, endId, limit - result.size());

			// Add to result and cache
			for (const Node &node: segmentNodes) {
				result.push_back(node);
				nodeCache_.put(node.getId(), node);
			}

			if (result.size() >= limit) {
				break;
			}
		}

		return result;
	}

	std::vector<Edge> DataManager::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) {
		// Implementation similar to getNodesInRange
		std::vector<Edge> result;
		if (startId > endId || limit == 0) {
			return result;
		}

		// Find relevant segments that contain the range
		std::vector<uint64_t> relevantSegments;
		for (const auto &segmentIndex: edgeSegmentIndex_) {
			if ((segmentIndex.startId <= endId && segmentIndex.endId >= startId) && (result.size() < limit)) {
				relevantSegments.push_back(segmentIndex.segmentOffset);
			}
		}

		// Load edges from each relevant segment
		for (uint64_t segmentOffset: relevantSegments) {
			std::vector<Edge> segmentEdges = loadEdgesFromSegment(segmentOffset, startId, endId, limit - result.size());

			// Add to result and cache
			for (const Edge &edge: segmentEdges) {
				result.push_back(edge);
				edgeCache_.put(edge.getId(), edge);
			}

			if (result.size() >= limit) {
				break;
			}
		}

		return result;
	}

	// Core method to read a single entity from a specific file offset
	template<typename EntityType>
	std::optional<EntityType> DataManager::readEntityFromDisk(int64_t fileOffset) const {
		file_->seekg(static_cast<std::streamoff>(fileOffset));
		EntityType entity = EntityType::deserialize(*file_);

		// Check if the entity is marked as inactive
		if (!entity.isActive()) {
			return std::nullopt; // Return empty optional if marked as inactive
		}

		return entity;
	}

	// Core method to find and read a single entity by ID
	template<typename EntityType>
	std::optional<EntityType> DataManager::findAndReadEntity(int64_t id) const {
		// Find segment for this entity type and ID
		uint64_t segmentOffset = EntityGetter<EntityType>::findSegmentForId(this, id);

		if (segmentOffset == 0) {
			return std::nullopt; // Segment not found
		}

		// Read segment header using segment tracker if available
		SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);

		// Calculate position of entity within segment
		uint64_t relativePosition = id - header.start_id;
		if (relativePosition >= header.used) {
			return std::nullopt; // ID is out of range for this segment
		}

		// Check if the entity is active
		if (!segmentTracker_->isEntityActive(segmentOffset, relativePosition)) {
			return std::nullopt; // Entity is marked as inactive
		}

		// Calculate file offset for this entity
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														relativePosition * sizeof(EntityType));

		return readEntityFromDisk<EntityType>(entityOffset);
	}

	// Core method to read multiple entities from a segment with filtering
	template<typename EntityType>
	std::vector<EntityType> DataManager::readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit, bool filterDeleted) const {

		std::vector<EntityType> result;
		if (segmentOffset == 0 || limit == 0) {
			return result;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);

		// Calculate effective start and end positions
		uint64_t effectiveStartId = std::max(startId, header.start_id);
		uint64_t effectiveEndId = std::min(endId, header.start_id + header.used - 1);

		if (effectiveStartId > effectiveEndId) {
			return result;
		}

		// Calculate offsets
		uint64_t startOffset = effectiveStartId - header.start_id;
		uint64_t count = std::min(effectiveEndId - effectiveStartId + 1, static_cast<uint64_t>(limit));

		// Reserve space for the maximum possible entities
		result.reserve(count);

		// Seek to the first entity
		auto entityOffset =
				static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) + startOffset * sizeof(EntityType));

		// Read entities
		for (uint64_t i = 0; i < count; ++i) {
			auto entityOpt = readEntityFromDisk<EntityType>(entityOffset);
			if (entityOpt.has_value()) {
				result.push_back(entityOpt.value());
			} else if (!filterDeleted) {
				// If we're not filtering deleted entities, read directly
				file_->seekg(entityOffset);
				EntityType entity = EntityType::deserialize(*file_);
				result.push_back(entity);
			}

			// Move to next entity
			entityOffset += sizeof(EntityType);
		}

		return result;
	}

	std::vector<Node> DataManager::loadNodesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit) const {
		return readEntitiesFromSegment<Node>(segmentOffset, startId, endId, limit);
	}

	std::vector<Edge> DataManager::loadEdgesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit) const {
		return readEntitiesFromSegment<Edge>(segmentOffset, startId, endId, limit);
	}

	std::vector<Edge> DataManager::findEdgesByNode(int64_t nodeId, const std::string &direction) {
		std::vector<Edge> result;

		// First check dirty edges
		for (const auto &[edgeId, info]: dirtyEdges_) {
			if (info.changeType != DirtyEntityInfo::ChangeType::DELETED) {
				Edge edge;
				if (info.edgeBackup.has_value()) {
					edge = *info.edgeBackup;
				} else if (edgeCache_.contains(edgeId)) {
					edge = edgeCache_.peek(edgeId);
				} else {
					continue; // Skip if we can't get the edge
				}

				bool match = false;
				if (direction == "outgoing" && edge.getFromNodeId() == nodeId) {
					match = true;
				} else if (direction == "incoming" && edge.getToNodeId() == nodeId) {
					match = true;
				} else if (direction == "both" && (edge.getFromNodeId() == nodeId || edge.getToNodeId() == nodeId)) {
					match = true;
				}

				if (match) {
					result.push_back(edge);
				}
			}
		}

		// Then proceed with disk scan
		// Scan through edge segments
		for (const auto &segmentIndex: edgeSegmentIndex_) {
			uint64_t segmentOffset = segmentIndex.segmentOffset;
			SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);

			// Read all edges in this segment (allowing for potential filtering)
			// We set filterDeleted to true to automatically skip deleted edges
			std::vector<Edge> segmentEdges = readEntitiesFromSegment<Edge>(
					segmentOffset, header.start_id, header.start_id + header.used - 1, header.used);

			// Filter edges based on direction criteria
			for (const Edge &edge: segmentEdges) {
				// Skip edges that are in dirty collection (already processed)
				if (dirtyEdges_.find(edge.getId()) != dirtyEdges_.end()) {
					continue;
				}

				bool match = false;
				if (direction == "outgoing" && edge.getFromNodeId() == nodeId) {
					match = true;
				} else if (direction == "incoming" && edge.getToNodeId() == nodeId) {
					match = true;
				} else if (direction == "both" && (edge.getFromNodeId() == nodeId || edge.getToNodeId() == nodeId)) {
					match = true;
				}

				if (match) {
					result.push_back(edge);
					edgeCache_.put(edge.getId(), edge);
				}
			}
		}

		return result;
	}

	template<typename EntityType>
	void DataManager::updateEntityProperty(int64_t entityId, const std::string &key, const PropertyValue &value) {
		// Get the entity
		EntityType entity = EntityGetter<EntityType>::get(this, entityId);

		if (entity.getId() == 0) {
			throw std::runtime_error("Entity " + std::to_string(entityId) + " not found");
		}

		// Check if entity is active
		if (!entity.isActive()) {
			throw std::runtime_error("Cannot update property of inactive entity " + std::to_string(entityId));
		}

		// Get all current properties (from memory and disk, with memory taking precedence)
		std::unordered_map<std::string, PropertyValue> currentProperties = getMergedProperties<EntityType>(entityId);

		// Clear existing properties from the entity and add all current ones back
		entity.clearProperties();
		for (const auto &[propKey, propValue]: currentProperties) {
			if (propKey != key) { // Skip the key we're about to update
				entity.addProperty(propKey, propValue);
			}
		}

		// Now add/update the property
		entity.addProperty(key, value);

		// Cache the updated property
		std::pair cacheKey = {entityId, key};
		propertyCache_.put(cacheKey, value);

		// Check if total property size exceeds threshold, but DON'T store immediately
		if (entity.getTotalPropertySize() > MAX_INLINE_PROPERTY_SIZE) {
			// Just mark that this entity needs property storage during next save
			// Keep any existing property reference if it exists
			if (entity.getPropertyReference().type == PropertyReference::StorageType::NONE) {
				entity.setPropertyReference({PropertyReference::StorageType::NEEDS_STORE, 0, 0});
			}
		}

		// Mark entity as dirty
		EntityGetter<EntityType>::markEntityDirty(this, entity);
	}

	void DataManager::updateNodeProperty(int64_t nodeId, const std::string &key, const PropertyValue &value) {
		updateEntityProperty<Node>(nodeId, key, value);
	}

	void DataManager::updateEdgeProperty(int64_t edgeId, const std::string &key, const PropertyValue &value) {
		updateEntityProperty<Edge>(edgeId, key, value);
	}

	// Generic method to handle entity properties
	template<typename EntityType>
	PropertyValue DataManager::getEntityProperty(int64_t entityId, const std::string &key, uint8_t entityTypeId) {
		// First check the property cache
		std::pair<uint64_t, std::string> cacheKey = {entityId, key};
		if (propertyCache_.contains(cacheKey)) {
			return propertyCache_.get(cacheKey);
		}

		// Get the entity (only load basic information and property reference)
		EntityType entity = EntityGetter<EntityType>::get(this, entityId);

		if (entity.getId() == 0) {
			throw std::out_of_range("Entity " + std::to_string(entityId) + " not found");
		}

		// Check if entity is active
		if (!entity.isActive()) {
			throw std::out_of_range("Entity " + std::to_string(entityId) + " is not active");
		}

		// Get the property reference
		const PropertyReference &propRef = entity.getPropertyReference();

		// If the entity has no property reference, check for inline properties
		if (propRef.type == PropertyReference::StorageType::NONE) {
			if (entity.hasProperty(key)) {
				PropertyValue value = entity.getProperty(key);
				propertyCache_.put(cacheKey, value);
				return value;
			}
			throw std::out_of_range("Property " + key + " not found for entity " + std::to_string(entityId));
		}

		// If there is a property reference, load properties from disk
		uint64_t entryOffset = 0;
		if (propRef.type == PropertyReference::StorageType::SEGMENT) {
			entryOffset = propRef.reference;
		} else if (propRef.type == PropertyReference::StorageType::BLOB) {
			// Properties are stored in a BLOB, load all properties directly
			auto properties = loadPropertiesFromDisk(propRef);
			// Update the entity's property set
			for (const auto &[propKey, propValue]: properties) {
				entity.addProperty(propKey, propValue);
				propertyCache_.put({entityId, propKey}, propValue);
			}

			// Check for the specific property needed
			if (properties.find(key) != properties.end()) {
				return properties.at(key);
			}
			throw std::out_of_range("Property " + key + " not found for entity " + std::to_string(entityId));
		}

		// If we have a property entry offset, load it
		if (entryOffset != 0 || (entryOffset = findPropertyEntry(entityId, entityTypeId)) != 0) {
			file_->seekg(static_cast<std::streamoff>(entryOffset));

			// Read the property entry header
			PropertyEntryHeader entryHeader;
			file_->read(reinterpret_cast<char *>(&entryHeader), sizeof(entryHeader));

			// Deserialize properties
			auto properties = property_storage::PropertyStorage::deserialize(*file_);

			// Update the entity's property set and cache all properties
			for (const auto &[propKey, propValue]: properties) {
				entity.addProperty(propKey, propValue);
				propertyCache_.put({entityId, propKey}, propValue);
			}

			// Check for the specific property needed
			if (properties.find(key) != properties.end()) {
				return properties.at(key);
			}
		}

		throw std::out_of_range("Property " + key + " not found for entity " + std::to_string(entityId));
	}

	PropertyValue DataManager::getNodeProperty(int64_t nodeId, const std::string &key) {
		// First check if property exists in dirty collections or cache
		auto dirtyProp = getPropertyFromMemory<Node>(nodeId, key);
		if (dirtyProp.has_value()) {
			return *dirtyProp;
		}

		// If not found in dirty collections, proceed with the original logic
		return getEntityProperty<Node>(nodeId, key, Node::typeId);
	}

	PropertyValue DataManager::getEdgeProperty(int64_t edgeId, const std::string &key) {
		// First check if property exists in dirty collections or cache
		auto dirtyProp = getPropertyFromMemory<Edge>(edgeId, key);
		if (dirtyProp.has_value()) {
			return *dirtyProp;
		}

		// If not found in dirty collections, proceed with the original logic
		return getEntityProperty<Edge>(edgeId, key, Edge::typeId);
	}

	void DataManager::markNodeDirty(Node &node) { updateNode(node); }

	void DataManager::markEdgeDirty(Edge &edge) { updateEdge(edge); }

	void DataManager::updateEntitiesWithPermanentIds() {
		// Process nodes
		std::vector<std::pair<int64_t, Node>> updatedNodes;

		for (const auto &[id, node]: nodeCache_) {
			if (node.getId() != 0 && IDAllocator::isTemporaryId(node.getId())) {
				int64_t tempId = node.getId(); // Store the temporary ID
				int64_t permanentId = idAllocator_->getPermanentId(tempId, Node::typeId);

				if (permanentId > 0) {
					// Clone the node and update its ID
					Node updatedNode = node;
					updatedNode.setPermanentId(permanentId);

					// Store both the original temp ID and the updated node
					updatedNodes.emplace_back(tempId, updatedNode);
				}
			}
		}

		// Update node cache with permanent IDs
		for (const auto &[tempId, node]: updatedNodes) {
			// Remove the entry with the temporary ID
			nodeCache_.remove(tempId);

			// Add new entry with the permanent ID
			nodeCache_.put(node.getId(), node);

			// Clean up the mapping
			idAllocator_->clearTempIdMapping(tempId, Node::typeId);
		}

		// Process edges - similar logic
		std::vector<std::pair<int64_t, Edge>> updatedEdges;

		for (const auto &[id, edge]: edgeCache_) {
			if (edge.getId() != 0 && IDAllocator::isTemporaryId(edge.getId())) {
				int64_t tempId = edge.getId();
				int64_t permanentId = idAllocator_->getPermanentId(tempId, Edge::typeId);

				if (permanentId > 0) {
					Edge updatedEdge = edge;
					updatedEdge.setPermanentId(permanentId);
					updatedEdges.emplace_back(tempId, updatedEdge);
				}
			}
		}

		// Update edge cache with permanent IDs
		for (const auto &[tempId, edge]: updatedEdges) {
			edgeCache_.remove(tempId);
			edgeCache_.put(edge.getId(), edge);
			idAllocator_->clearTempIdMapping(tempId, Edge::typeId);
		}

		// If all temporary IDs have been processed, we can clear the maps completely
		if (updatedNodes.empty() && updatedEdges.empty()) {
			idAllocator_->clearTempIdMappings();
		}
	}

	template<typename EntityType>
	bool DataManager::entityNeedsPropertyStorage(const EntityType &entity) const {
		return entity.getPropertyReference().type == PropertyReference::StorageType::NEEDS_STORE ||
			   entity.getTotalPropertySize() > MAX_INLINE_PROPERTY_SIZE;
	}

	template<typename EntityType>
	void DataManager::storeEntityProperties(EntityType &entity, const std::shared_ptr<std::fstream> &fileStream) {
		// Only store if needed
		if (entityNeedsPropertyStorage(entity)) {
			// Find entity's file offset if it exists on disk
			uint64_t entityOffset = 0;
			uint64_t segmentOffset = EntityGetter<EntityType>::findSegmentForId(this, entity.getId());

			if (segmentOffset != 0) {
				// Read segment header
				SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);

				// Calculate entity's offset within the segment
				uint64_t relativePosition = entity.getId() - header.start_id;
				if (relativePosition < header.capacity) {
					entityOffset = segmentOffset + sizeof(SegmentHeader) + relativePosition * sizeof(EntityType);
				}
			}

			PropertyReference newPropRef = property_storage::PropertyStorage::storeProperties(
					fileStream, entity.getId(), EntityType::typeId,
					entityOffset, // Pass the entity's file offset
					entity.getProperties(), propertySegmentHead_, *this, entity.getPropertyReference());

			// Update property reference
			entity.setPropertyReference(newPropRef);

			// Mark entity as dirty (again) to ensure the updated reference is saved
			EntityGetter<EntityType>::markEntityDirty(this, entity);
		}
	}

	// Template instantiations
	template bool DataManager::entityNeedsPropertyStorage<Node>(const Node &entity) const;
	template bool DataManager::entityNeedsPropertyStorage<Edge>(const Edge &entity) const;
	template void DataManager::storeEntityProperties<Node>(Node &entity,
														   const std::shared_ptr<std::fstream> &fileStream);
	template void DataManager::storeEntityProperties<Edge>(Edge &entity,
														   const std::shared_ptr<std::fstream> &fileStream);

	template<typename EntityType>
	void DataManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		// Get the entity
		EntityType entity = EntityGetter<EntityType>::get(this, entityId);

		if (entity.getId() == 0) {
			throw std::runtime_error("Entity " + std::to_string(entityId) + " not found");
		}

		// Check if entity is active
		if (!entity.isActive()) {
			throw std::runtime_error("Cannot remove property from inactive entity " + std::to_string(entityId));
		}

		// Get all current properties (from memory and disk, with memory taking precedence)
		std::unordered_map<std::string, PropertyValue> currentProperties = getMergedProperties<EntityType>(entityId);

		// Check if the property exists
		if (currentProperties.find(key) == currentProperties.end()) {
			return; // Property doesn't exist, nothing to do
		}

		// Remove the property
		currentProperties.erase(key);

		// Clear existing properties from the entity and add all remaining ones back
		entity.clearProperties();
		for (const auto &[propKey, propValue]: currentProperties) {
			entity.addProperty(propKey, propValue);
		}

		// Remove from cache
		std::pair cacheKey = {entityId, key};
		if (propertyCache_.contains(cacheKey)) {
			propertyCache_.remove(cacheKey);
		}

		// If the entity still has properties and the total size is large, mark for storage
		if (!entity.getProperties().empty() && entity.getTotalPropertySize() > MAX_INLINE_PROPERTY_SIZE) {
			if (entity.getPropertyReference().type == PropertyReference::StorageType::NONE) {
				entity.setPropertyReference({PropertyReference::StorageType::NEEDS_STORE, 0, 0});
			}
		} else {
			// For small or empty property sets, clear external reference
			entity.setPropertyReference(PropertyReference{});
		}

		// Mark as property dirty
		EntityGetter<EntityType>::markEntityDirty(this, entity);
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) {
		removeEntityProperty<Node>(nodeId, key);
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) {
		removeEntityProperty<Edge>(edgeId, key);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(int64_t nodeId) {
		return getMergedProperties<Node>(nodeId);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(int64_t edgeId) {
		return getMergedProperties<Edge>(edgeId);
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::getEntityFromDirty(int64_t id) const {
		if constexpr (std::is_same_v<EntityType, Node>) {
			auto it = dirtyNodes_.find(id);
			if (it != dirtyNodes_.end()) {
				const auto &info = it->second;
				// Return the entity if it's not deleted
				if (info.changeType != DirtyEntityInfo::ChangeType::DELETED) {
					// If we have a backup, return it, otherwise try to get from cache
					if (info.nodeBackup.has_value()) {
						return *info.nodeBackup;
					} else if (nodeCache_.contains(id)) {
						return nodeCache_.peek(id);
					}
				}
				// If it's deleted or not available, return nullopt
				return std::nullopt;
			}
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			auto it = dirtyEdges_.find(id);
			if (it != dirtyEdges_.end()) {
				const auto &info = it->second;
				// Return the entity if it's not deleted
				if (info.changeType != DirtyEntityInfo::ChangeType::DELETED) {
					// If we have a backup, return it, otherwise try to get from cache
					if (info.edgeBackup.has_value()) {
						return *info.edgeBackup;
					} else if (edgeCache_.contains(id)) {
						return edgeCache_.peek(id);
					}
				}
				// If it's deleted or not available, return nullopt
				return std::nullopt;
			}
		}

		return std::nullopt;
	}

	template<typename EntityType>
	EntityType DataManager::getEntityFromMemoryOrDisk(int64_t id) {
		// First check dirty collections
		auto dirtyEntity = getEntityFromDirty<EntityType>(id);
		if (dirtyEntity.has_value()) {
			return *dirtyEntity;
		}

		// Then check cache
		if constexpr (std::is_same_v<EntityType, Node>) {
			if (nodeCache_.contains(id)) {
				Node node = nodeCache_.get(id);
				// Check if node is active
				if (!node.isActive()) {
					return EntityType(); // Return empty entity if inactive
				}
				return node;
			}
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			if (edgeCache_.contains(id)) {
				Edge edge = edgeCache_.get(id);
				// Check if edge is active
				if (!edge.isActive()) {
					return EntityType(); // Return empty entity if inactive
				}
				return edge;
			}
		}

		// Finally, load from disk
		if constexpr (std::is_same_v<EntityType, Node>) {
			auto node = loadNodeFromDisk(id);
			if (node.getId() != 0 && node.isActive()) { // Valid and active node
				nodeCache_.put(id, node);
				return node;
			}
			return EntityType(); // Return empty entity if not found or inactive
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			auto edge = loadEdgeFromDisk(id);
			if (edge.getId() != 0 && edge.isActive()) { // Valid and active edge
				edgeCache_.put(id, edge);
				return edge;
			}
			return EntityType(); // Return empty entity if not found or inactive
		}

		// Return empty entity if not found
		return EntityType();
	}

	template<typename EntityType>
	std::optional<PropertyValue> DataManager::getPropertyFromMemory(int64_t entityId, const std::string &key) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			auto it = dirtyNodes_.find(entityId);
			if (it != dirtyNodes_.end()) {
				const auto &info = it->second;

				if (info.changeType == DirtyEntityInfo::ChangeType::DELETED) {
					return std::nullopt; // Entity is deleted
				}

				// Try to get from nodeBackup if available
				if (info.nodeBackup.has_value() && info.nodeBackup->hasProperty(key)) {
					return info.nodeBackup->getProperty(key);
				}

				// Try from cache
				if (nodeCache_.contains(entityId)) {
					const Node &node = nodeCache_.peek(entityId);
					if (node.hasProperty(key)) {
						return node.getProperty(key);
					}
				}
			}
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			auto it = dirtyEdges_.find(entityId);
			if (it != dirtyEdges_.end()) {
				const auto &info = it->second;

				if (info.changeType == DirtyEntityInfo::ChangeType::DELETED) {
					return std::nullopt; // Entity is deleted
				}

				// Try to get from edgeBackup if available
				if (info.edgeBackup.has_value() && info.edgeBackup->hasProperty(key)) {
					return info.edgeBackup->getProperty(key);
				}

				// Try from cache
				if (edgeCache_.contains(entityId)) {
					const Edge &edge = edgeCache_.peek(entityId);
					if (edge.hasProperty(key)) {
						return edge.getProperty(key);
					}
				}
			}
		}

		// Check property cache
		std::pair cacheKey = {entityId, key};
		if (propertyCache_.contains(cacheKey)) {
			return propertyCache_.get(cacheKey);
		}

		return std::nullopt;
	}

	template<typename EntityType>
	std::unordered_map<std::string, PropertyValue> DataManager::getMergedProperties(int64_t entityId) {
		// First get properties from disk or cache
		EntityType entity = getEntityFromMemoryOrDisk<EntityType>(entityId);

		if (entity.getId() == 0) {
			throw std::runtime_error("Entity " + std::to_string(entityId) + " not found");
		}

		// Get all properties, including those that might be externally stored
		std::unordered_map<std::string, PropertyValue> properties;

		// If entity has a property reference, load properties from disk
		const PropertyReference &propRef = entity.getPropertyReference();
		if (propRef.type != PropertyReference::StorageType::NONE) {
			properties = loadPropertiesFromDisk(propRef);

			// Update the entity's property set and cache
			for (const auto &[key, value]: properties) {
				entity.addProperty(key, value);
				propertyCache_.put({entityId, key}, value);
			}
		} else {
			// Otherwise just use entity's properties
			properties = entity.getProperties();
		}

		// Now check for dirty properties and overlay them
		if constexpr (std::is_same_v<EntityType, Node>) {
			auto it = dirtyNodes_.find(entityId);
			if (it != dirtyNodes_.end() && it->second.changeType != DirtyEntityInfo::ChangeType::DELETED) {
				if (it->second.nodeBackup.has_value()) {
					// Overlay dirty properties from backup
					for (const auto &[key, value]: it->second.nodeBackup->getProperties()) {
						properties[key] = value;
					}
				}
			}
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			auto it = dirtyEdges_.find(entityId);
			if (it != dirtyEdges_.end() && it->second.changeType != DirtyEntityInfo::ChangeType::DELETED) {
				if (it->second.edgeBackup.has_value()) {
					// Overlay dirty properties from backup
					for (const auto &[key, value]: it->second.edgeBackup->getProperties()) {
						properties[key] = value;
					}
				}
			}
		}

		return properties;
	}

	std::unordered_map<std::string, PropertyValue> DataManager::loadPropertiesFromDisk(const PropertyReference &ref) {
		// If the reference is invalid, return an empty map
		if (ref.type == PropertyReference::StorageType::NONE) {
			return {};
		}

		// Load from property segment
		if (ref.type == PropertyReference::StorageType::SEGMENT) {
			file_->seekg(static_cast<std::streamoff>(ref.reference));

			// Skip PropertyEntryHeader
			PropertyEntryHeader entryHeader;
			file_->read(reinterpret_cast<char *>(&entryHeader), sizeof(entryHeader));

			// Deserialize the property data
			return property_storage::PropertyStorage::deserialize(*file_);
		}
		// Load from BLOB
		if (ref.type == PropertyReference::StorageType::BLOB) {
			// Ensure BlobStore is initialized
			ensureBlobStoreInitialized();

			// Get the BLOB data
			std::string blobData = getBlob(ref.reference);
			std::stringstream ss(blobData);
			return property_storage::PropertyStorage::deserialize(ss);
		}

		return {};
	}

	std::string DataManager::getBlob(uint64_t blobId) {
		// Check cache first
		if (blobCache_.contains(blobId)) {
			return blobCache_.get(blobId);
		}

		ensureBlobStoreInitialized();

		// Load blob from store
		std::string blobData = blobStore_->getBlob(blobId);

		// Add to cache
		blobCache_.put(blobId, blobData);

		return blobData;
	}

	uint64_t DataManager::storeBlob(const std::string &data, const std::string &contentType) {
		ensureBlobStoreInitialized();

		// Store blob in BlobStore
		uint64_t blobId = blobStore_->storeBlob(data, contentType);

		// Update file header
		fileHeader_.max_blob_id = std::max(fileHeader_.max_blob_id, blobId);

		// Add to cache
		blobCache_.put(blobId, data);

		return blobId;
	}

	void DataManager::ensureBlobStoreInitialized() {
		if (!blobStore_) {
			auto fileStream =
					std::make_shared<std::fstream>(dbFilePath_, std::ios::binary | std::ios::in | std::ios::out);

			blobStore_ = std::make_unique<BlobStore>(fileStream, fileHeader_.blob_section_head);

			// Allocate blob section if needed
			if (fileHeader_.blob_section_head == 0) {
				fileHeader_.blob_section_head = blobStore_->allocateSection(BLOB_SECTION_SIZE);
			}
		}
	}

	BlobStore &DataManager::getBlobStore() const {
		if (!blobStore_) {
			throw std::runtime_error("BlobStore is not initialized");
		}
		return *blobStore_;
	}

	void DataManager::flushToDisk(std::fstream &file) {

		// Flush blob store if needed
		if (blobStore_) {
			blobStore_->flush();
		}

		// Update file header with blob section info
		if (blobStore_ && fileHeader_.blob_section_head == 0) {
			fileHeader_.blob_section_head = file.tellp();
		}
	}

	void DataManager::clearCache() {
		nodeCache_.clear();
		edgeCache_.clear();
		propertyCache_.clear();
		blobCache_.clear();
	}

} // namespace graph::storage
