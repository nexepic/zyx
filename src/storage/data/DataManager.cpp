/**
 * @file DataManager.cpp
 * @author Nexepic
 * @date 2025/7/24
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

#include "graph/storage/data/DataManager.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/BlobChainManager.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/StateChainManager.hpp"
#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/DeletionManager.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"
#include "graph/storage/data/StateManager.hpp"
#include "graph/storage/dictionaries/TokenRegistry.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph::storage {

	// Thread-local snapshot pointer for read-only transactions
	thread_local const CommittedSnapshot *DataManager::currentSnapshot_ = nullptr;
	thread_local bool DataManager::readOnlyMode_ = false;

	namespace {
		template<typename EntityType>
		class WalEntityViewBatchWriter {
		public:
			WalEntityViewBatchWriter(const TransactionContext &txnContext, EntityChangeType changeType, size_t expectedCount) :
				txnContext_(txnContext), changeType_(static_cast<uint8_t>(changeType)) {
				reserveFor((std::min)(expectedCount, kWalBatchSize));
			}

			void add(const EntityType &entity) {
				if (views_.size() >= kWalBatchSize) {
					flush();
				}

				const size_t offset = serialized_.size();
				serialized_.resize(offset + kEntitySize);
				auto *dest = serialized_.data() + offset;
				utils::FixedSizeSerializer::serializeInto(reinterpret_cast<char *>(dest), entity, kEntitySize);
				views_.push_back({static_cast<uint8_t>(EntityType::typeId),
								  changeType_,
								  entity.getId(),
								  dest,
								  static_cast<uint32_t>(kEntitySize)});
			}

			void flush() {
				if (views_.empty()) {
					return;
				}
				txnContext_.flushWalChangeViews(views_);
				views_.clear();
				serialized_.clear();
			}

		private:
			static constexpr size_t kWalBatchSize = 4096;
			static constexpr size_t kEntitySize = EntityType::getTotalSize();

			void reserveFor(size_t count) {
				if (count == 0) {
					return;
				}
				serialized_.reserve(count * kEntitySize);
				views_.reserve(count);
			}

			const TransactionContext &txnContext_;
			uint8_t changeType_;
			std::vector<uint8_t> serialized_;
			std::vector<wal::WALEntityChangeView> views_;
		};
	} // namespace

	DataManager::DataManager(std::shared_ptr<std::fstream> file, size_t cacheSize, FileHeader &fileHeader,
							 IDAllocators allocators, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<StorageIO> storageIO) :
		file_(std::move(file)), fileHeader_(fileHeader), pagePool_(std::make_unique<PageBufferPool>(cacheSize)),
		allocators_(std::move(allocators)), segmentTracker_(std::move(segmentTracker)),
		storageIO_(std::move(storageIO)) {

		persistenceManager_ = std::make_shared<PersistenceManager>();
		segmentIndexManager_ = std::make_shared<SegmentIndexManager>(segmentTracker_);
		segmentTracker_->setSegmentIndexManager(std::weak_ptr(segmentIndexManager_));
	}

	DataManager::~DataManager() { closeFileHandles(); }

	void DataManager::closeFileHandles() {
		// readFd_ is now owned by StorageIO — nothing to close here.
	}

	ssize_t DataManager::preadBytes(void *buf, size_t count, int64_t offset) const {
		if (!storageIO_ || !storageIO_->hasPreadSupport())
			return -1;
		return static_cast<ssize_t>(storageIO_->readAt(static_cast<uint64_t>(offset), buf, count));
	}

	ssize_t DataManager::preadSegments(void *buf, size_t segmentCount, uint64_t startSegmentOffset,
									   SegmentReadCachePolicy cachePolicy) const {
		if (buf == nullptr) {
			return -1;
		}
		if (segmentCount == 0) {
			return 0;
		}

		const size_t totalBytes = segmentCount * TOTAL_SEGMENT_SIZE;
		auto *out = static_cast<uint8_t *>(buf);
		const bool readThrough = cachePolicy == SegmentReadCachePolicy::SRCP_READ_THROUGH;
		if (readThrough && pagePool_->capacity() > 0) {
			if (pagePool_->copyContiguousPages(startSegmentOffset, segmentCount, out, TOTAL_SEGMENT_SIZE)) {
				return static_cast<ssize_t>(totalBytes);
			}
		}

		const ssize_t read = preadBytes(buf, totalBytes, static_cast<int64_t>(startSegmentOffset));
		if (readThrough && read >= static_cast<ssize_t>(totalBytes) && pagePool_->capacity() > 0) {
			pagePool_->putContiguousPages(startSegmentOffset, segmentCount, out, TOTAL_SEGMENT_SIZE);
		}
		return read;
	}

	// Helper streambuf that wraps an existing memory buffer for zero-copy deserialization.
	// This lets us pread() into a stack buffer, then deserialize via the existing istream API.
	namespace {
		class membuf : public std::streambuf {
		public:
			membuf(char *base, size_t size) { this->setg(base, base, base + size); }
			[[nodiscard]] size_t consumed() const { return static_cast<size_t>(this->gptr() - this->eback()); }
		};

		template<typename EntityType>
		std::vector<DirtyEntityInfo<EntityType>>
		filterDirtyInfosByType(std::vector<DirtyEntityInfo<EntityType>> allInfos,
							   const std::vector<EntityChangeType> &types) {
			if (types.size() == 3) {
				return allInfos;
			}

			std::vector<DirtyEntityInfo<EntityType>> result;
			result.reserve(allInfos.size());
			for (const auto &info: allInfos) {
				const bool typeMatch = std::any_of(types.begin(), types.end(),
												   [&](EntityChangeType type) { return info.changeType == type; });
				if (typeMatch) {
					result.push_back(info);
				}
			}
			return result;
		}

	} // namespace


	void DataManager::initialize(bool skipSegmentIndexBuild) {
		// Initialize low-level components
		deletionManager_ = std::make_shared<DeletionManager>(shared_from_this(), segmentTracker_, allocators_);
		entityReferenceUpdater_ = std::make_shared<EntityReferenceUpdater>(shared_from_this());
		relationshipTraversal_ = std::make_shared<traversal::RelationshipTraversal>(shared_from_this());

		// Initialize segment indexes (unless pre-built by StorageBootstrap)
		if (!skipSegmentIndexBuild) {
			initializeSegmentIndexes();
		}

		// Initialize entity managers
		initializeManagers();
	}

	void DataManager::setSystemStateManager(const std::shared_ptr<state::SystemStateManager> &systemStateManager) {
		systemStateManager_ = systemStateManager;

		// Now that we have the high-level state manager, we can initialize the token registry
		initializeTokenRegistry(systemStateManager);
	}

	void DataManager::initializeTokenRegistry(std::shared_ptr<state::SystemStateManager> sm) {
		// Pass the correct high-level SystemStateManager to the registry
		tokenRegistry_ = std::make_unique<TokenRegistry>(shared_from_this(), sm);
	}

	void DataManager::initializeSegmentIndexes() const {
		// Initialize all segment indexes at once
		segmentIndexManager_->initialize(fileHeader_.node_segment_head, fileHeader_.edge_segment_head,
										 fileHeader_.property_segment_head, fileHeader_.blob_segment_head,
										 fileHeader_.index_segment_head, fileHeader_.state_segment_head);
	}

	void DataManager::initializeManagers() {
		// Initialize chain managers
		blobChainManager_ = std::make_shared<BlobChainManager>(shared_from_this());
		stateChainManager_ = std::make_shared<StateChainManager>(shared_from_this());

		// Create property manager first as others depend on it
		propertyManager_ = std::make_shared<PropertyManager>(this, deletionManager_);

		// Create entity managers
		nodeManager_ = std::make_shared<NodeManager>(this, deletionManager_);
		edgeManager_ = std::make_shared<EdgeManager>(this, deletionManager_);
		blobManager_ = std::make_shared<BlobManager>(this, blobChainManager_, deletionManager_);
		indexEntityManager_ = std::make_shared<IndexEntityManager>(this, deletionManager_);
		stateManager_ = std::make_shared<StateManager>(this, stateChainManager_, deletionManager_);
	}

	// Observer registration is now inline in DataManager.hpp, delegating to observerManager_.
	// All notification methods are now on EntityObserverManager.

	int64_t DataManager::getOrCreateTokenId(const std::string &name) const {
		if (name.empty())
			return 0;
		if (!tokenRegistry_) {
			throw std::runtime_error("TokenRegistry not initialized. Ensure SystemStateManager is set.");
		}
		return tokenRegistry_->getOrCreateTokenId(name);
	}

	int64_t DataManager::resolveTokenId(const std::string &name) const {
		if (name.empty())
			return 0;
		if (!tokenRegistry_) {
			throw std::runtime_error("TokenRegistry not initialized. Ensure SystemStateManager is set.");
		}
		return tokenRegistry_->resolveTokenId(name);
	}

	std::string DataManager::resolveTokenName(int64_t tokenId) const {
		if (tokenId == 0)
			return "";
		if (!tokenRegistry_) {
			return "";
		}

		return tokenRegistry_->resolveTokenName(tokenId);
	}

	// --- Transaction State Management ---

	// setActiveTransaction / clearActiveTransaction are now inline in DataManager.hpp,
	// delegating to txnContext_.

	// --- Node Operations (delegate to NodeManager) ---

	void DataManager::addNode(Node &node) const {
		guardReadOnly();
		for (const auto &v: observerManager_.getValidators()) {
			v->validateNodeInsert(node, node.getProperties());
		}
		nodeManager_->add(node);
		txnContext_.recordAdd(node);
		observerManager_.notifyNodeAdded(node);
		markGraphMutated();
	}

	void DataManager::addNodes(std::vector<Node> &nodes) const {
		guardReadOnly();
		if (nodes.empty())
			return;

		// Phase 1: Validate all nodes before any writes (atomicity)
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes.validate");
			for (const auto &node: nodes) {
				for (const auto &v: observerManager_.getValidators()) {
					v->validateNodeInsert(node, node.getProperties());
				}
			}
		}


		// Phase 2: Write
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes.prepare_ids");
			nodeManager_->prepareAddBatch(nodes);
		}
		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes.notify_indexes");
			observerManager_.notifyNodesAdded(nodes);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes.store_properties");
			(void) propertyManager_->storePropertiesBatch(nodes);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes.entity_write");
			nodeManager_->persistPreparedAddBatch(nodes);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_nodes.txn_record");
			txnContext_.recordAdds(nodes);
		}
		markGraphMutated();
	}

	void DataManager::updateNode(const Node &node) {
		guardReadOnly();
		Node oldNode = nodeManager_->get(node.getId());
		txnContext_.recordUpdate<Node>(node, oldNode);
		nodeManager_->update(node);
		observerManager_.notifyNodeUpdated(oldNode, node);
		markGraphMutated();
	}

	void DataManager::updateNodes(const std::vector<Node> &nodes) {
		guardReadOnly();
		if (nodes.empty()) {
			return;
		}

		std::vector<Node> oldNodes;
		oldNodes.reserve(nodes.size());
		for (const auto &node: nodes) {
			oldNodes.push_back(nodeManager_->get(node.getId()));
		}

		txnContext_.recordUpdates<Node>(nodes, oldNodes);
		nodeManager_->updateBatch(nodes);
		for (size_t index = 0; index < nodes.size(); ++index) {
			observerManager_.notifyNodeUpdated(oldNodes[index], nodes[index]);
		}
		markGraphMutated();
	}

	void DataManager::updateNodesWithBeforeImages(const std::vector<Node> &nodes, const std::vector<Node> &oldNodes) {
		guardReadOnly();
		if (nodes.empty()) {
			return;
		}
		if (nodes.size() != oldNodes.size()) {
			throw std::invalid_argument("updateNodesWithBeforeImages requires matching new/old node counts");
		}

		txnContext_.recordUpdates<Node>(nodes, oldNodes);
		nodeManager_->updateBatch(nodes);
		for (size_t index = 0; index < nodes.size(); ++index) {
			observerManager_.notifyNodeUpdated(oldNodes[index], nodes[index]);
		}
		markGraphMutated();
	}

	void DataManager::deleteNode(Node &node) const {
		guardReadOnly();
		nodeManager_->remove(node);
		observerManager_.notifyNodeDeleted(node);
		markGraphMutated();
	}

	Node DataManager::getNode(int64_t id) const { return nodeManager_->get(id); }

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) const {
		if (ids.empty()) {
			return {};
		}

		if (currentSnapshot_ != nullptr) {
			return nodeManager_->getBatch(ids);
		}

		std::vector<std::optional<Node>> rows(ids.size());
		std::vector<size_t> diskRows;
		std::vector<int64_t> diskIds;
		diskRows.reserve(ids.size());
		diskIds.reserve(ids.size());

		size_t row = 0;
		persistenceManager_->visitDirtyInfos<Node>(
				ids,
				[&](int64_t id, const DirtyEntityInfo<Node> *dirtyInfo) {
					if (dirtyInfo != nullptr) {
						if (dirtyInfo->changeType != EntityChangeType::CHANGE_DELETED &&
							dirtyInfo->backup.has_value()) {
							rows[row] = *dirtyInfo->backup;
						}
					} else {
						diskRows.push_back(row);
						diskIds.push_back(id);
					}
					++row;
				});

		for (size_t i = 0; i < diskIds.size(); ++i) {
			Node node = nodeManager_->get(diskIds[i]);
			if (node.getId() != 0 && node.isActive()) {
				rows[diskRows[i]] = std::move(node);
			}
		}

		std::vector<Node> result;
		result.reserve(ids.size());
		for (auto &node: rows) {
			if (node.has_value()) {
				result.push_back(std::move(*node));
			}
		}
		return result;
	}

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return nodeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addNodeProperties(int64_t nodeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		addEntityPropertiesImpl<Node>(
				nodeId, properties, *nodeManager_,
				[this](const Node &node, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateNodeUpdate(node, oldProps, newProps);
				},
				[this](const Node &o, const Node &n) { observerManager_.notifyNodeUpdated(o, n); });
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) const {
		removeEntityPropertyImpl<Node>(
				nodeId, key, *nodeManager_,
				[this](const Node &node, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateNodeUpdate(node, oldProps, newProps);
				},
				[this](const Node &o, const Node &n) { observerManager_.notifyNodeUpdated(o, n); });
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(int64_t nodeId) const {
		return nodeManager_->getProperties(nodeId);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodePropertiesDirect(const Node &node) {
		if (node.getId() == 0 || !node.isActive())
			return {};

		// Start with inline properties (no I/O needed)
		auto allProperties = EntityPropertyTraits<Node>::getProperties(node);

		// Load external properties via direct disk read (bypasses cache)
		if (EntityPropertyTraits<Node>::hasPropertyEntity(node)) {
			auto storageType = EntityPropertyTraits<Node>::getPropertyStorageType(node);
			auto propertyEntityId = EntityPropertyTraits<Node>::getPropertyEntityId(node);

			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				Property property = loadEntityDirect<Property>(propertyEntityId);
				if (property.getId() != 0) {
					for (const auto &[key, value]: property.getPropertyValues()) {
						allProperties[key] = value;
					}
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				// Blob chain reads are more complex; fall back to regular path
				auto blobProperties = propertyManager_->getPropertiesFromBlob(propertyEntityId);
				for (const auto &[key, value]: blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}

	std::unordered_map<std::string, PropertyValue>
	DataManager::getNodePropertiesFromMap(const Node &node, const std::unordered_map<int64_t, Property> &propertyMap) {
		if (node.getId() == 0 || !node.isActive())
			return {};

		auto allProperties = EntityPropertyTraits<Node>::getProperties(node);

		if (EntityPropertyTraits<Node>::hasPropertyEntity(node)) {
			auto storageType = EntityPropertyTraits<Node>::getPropertyStorageType(node);
			auto propertyEntityId = EntityPropertyTraits<Node>::getPropertyEntityId(node);

			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				auto it = propertyMap.find(propertyEntityId);
				if (it != propertyMap.end() && it->second.getId() != 0) {
					for (const auto &[key, value]: it->second.getPropertyValues()) {
						allProperties[key] = value;
					}
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				auto blobProperties = propertyManager_->getPropertiesFromBlob(propertyEntityId);
				for (const auto &[key, value]: blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}


	void DataManager::addEdge(Edge &edge) const {
		guardReadOnly();
		for (const auto &v: observerManager_.getValidators()) {
			v->validateEdgeInsert(edge, edge.getProperties());
		}
		edgeManager_->add(edge);
		txnContext_.recordAdd(edge);
		observerManager_.notifyEdgeAdded(edge);
		markGraphMutated();
	}

	void DataManager::addEdges(std::vector<Edge> &edges) const {
		guardReadOnly();
		if (edges.empty())
			return;

		// Phase 1: Validate all edges before any writes (atomicity)
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges.validate");
			for (const auto &edge: edges) {
				for (const auto &v: observerManager_.getValidators()) {
					v->validateEdgeInsert(edge, edge.getProperties());
				}
			}
		}


		// 1. Assign IDs and initial persistence
		traversal::RelationshipBatchLinkUpdates linkUpdates;
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges.prepare_links");
			linkUpdates = edgeManager_->prepareAddBatch(edges);
		}

		// 2. Indexing (needs IDs + Props)
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges.notify_indexes");
			observerManager_.notifyEdgesAdded(edges);
		}

		// 3. Property Storage Handling
		{
			debug::ScopedPerfTimer timer("datamanager.add_edges.store_properties");
			(void) propertyManager_->storePropertiesBatch(edges);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_edges.entity_write");
			edgeManager_->persistPreparedAddBatch(edges, linkUpdates);
		}

		{
			debug::ScopedPerfTimer timer("datamanager.add_edges.txn_record");
			txnContext_.recordAdds(edges);
		}
		markGraphMutated();
	}

	void DataManager::updateEdge(const Edge &edge) {
		guardReadOnly();
		Edge oldEdge = edgeManager_->get(edge.getId());
		txnContext_.recordUpdate<Edge>(edge, oldEdge);
		edgeManager_->update(edge);
		observerManager_.notifyEdgeUpdated(oldEdge, edge);
		markGraphMutated();
	}

	void DataManager::updateEdges(const std::vector<Edge> &edges) {
		guardReadOnly();
		if (edges.empty()) {
			return;
		}

		std::vector<Edge> oldEdges;
		oldEdges.reserve(edges.size());
		for (const auto &edge: edges) {
			oldEdges.push_back(edgeManager_->get(edge.getId()));
		}

		txnContext_.recordUpdates<Edge>(edges, oldEdges);
		edgeManager_->updateBatch(edges);
		for (size_t index = 0; index < edges.size(); ++index) {
			observerManager_.notifyEdgeUpdated(oldEdges[index], edges[index]);
		}
		markGraphMutated();
	}

	void DataManager::updateEdgesWithBeforeImages(const std::vector<Edge> &edges, const std::vector<Edge> &oldEdges) {
		guardReadOnly();
		if (edges.empty()) {
			return;
		}
		if (edges.size() != oldEdges.size()) {
			throw std::invalid_argument("updateEdgesWithBeforeImages requires matching new/old edge counts");
		}

		txnContext_.recordUpdates<Edge>(edges, oldEdges);
		edgeManager_->updateBatch(edges);
		for (size_t index = 0; index < edges.size(); ++index) {
			observerManager_.notifyEdgeUpdated(oldEdges[index], edges[index]);
		}
		markGraphMutated();
	}

	void DataManager::deleteEdge(Edge &edge) const {
		guardReadOnly();
		edgeManager_->remove(edge);
		observerManager_.notifyEdgeDeleted(edge);
		markGraphMutated();
	}

	Edge DataManager::getEdge(int64_t id) const { return edgeManager_->get(id); }

	std::vector<Edge> DataManager::getEdgeBatch(const std::vector<int64_t> &ids) const {
		return edgeManager_->getBatch(ids);
	}

	std::vector<Edge> DataManager::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return edgeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addEdgeProperties(int64_t edgeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		addEntityPropertiesImpl<Edge>(
				edgeId, properties, *edgeManager_,
				[this](const Edge &edge, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateEdgeUpdate(edge, oldProps, newProps);
				},
				[this](const Edge &o, const Edge &n) { observerManager_.notifyEdgeUpdated(o, n); });
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) const {
		removeEntityPropertyImpl<Edge>(
				edgeId, key, *edgeManager_,
				[this](const Edge &edge, const auto &oldProps, const auto &newProps) {
					for (const auto &v: observerManager_.getValidators())
						v->validateEdgeUpdate(edge, oldProps, newProps);
				},
				[this](const Edge &o, const Edge &n) { observerManager_.notifyEdgeUpdated(o, n); });
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(int64_t edgeId) const {
		return edgeManager_->getProperties(edgeId);
	}

	std::vector<Edge> DataManager::findEdgesByNode(int64_t nodeId, const std::string &direction) const {
		if (direction == "out") {
			return relationshipTraversal_->getOutgoingEdges(nodeId);
		} else if (direction == "in") {
			return relationshipTraversal_->getIncomingEdges(nodeId);
		} else { // "both" is the default
			return relationshipTraversal_->getAllConnectedEdges(nodeId);
		}
	}

	size_t DataManager::visitEdgesByNode(
			int64_t nodeId,
			const std::function<bool(const Edge &)> &visitor,
			const std::string &direction) const {
		if (!visitor) {
			return 0;
		}
		if (direction == "out") {
			return relationshipTraversal_->visitOutgoingEdges(nodeId, visitor);
		}
		if (direction == "in") {
			return relationshipTraversal_->visitIncomingEdges(nodeId, visitor);
		}
		return relationshipTraversal_->visitAllConnectedEdges(nodeId, visitor);
	}

	// --- Property Operation Templates ---

	template<typename EntityType, typename ManagerType>
	void DataManager::addEntityPropertiesImpl(
			int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties, ManagerType &manager,
			std::function<void(const EntityType &, const std::unordered_map<std::string, PropertyValue> &,
							   const std::unordered_map<std::string, PropertyValue> &)>
					validate,
			std::function<void(const EntityType &, const EntityType &)> notify) const {
		guardReadOnly();
		EntityType oldEntity = manager.get(entityId);
		auto existingProps = manager.getProperties(entityId);
		oldEntity.setProperties(existingProps);

		auto mergedProps = existingProps;
		for (const auto &[key, val]: properties) {
			mergedProps[key] = val;
		}
		validate(oldEntity, existingProps, mergedProps);

		observerManager_.setSuppressNotifications(true);
		manager.addProperties(entityId, properties);
		observerManager_.setSuppressNotifications(false);

		EntityType newEntity = manager.get(entityId);
		auto newProps = manager.getProperties(entityId);
		newEntity.setProperties(newProps);
		notify(oldEntity, newEntity);
		if constexpr (std::is_same_v<EntityType, Node> || std::is_same_v<EntityType, Edge>) {
			markGraphMutated();
		}
	}

	template<typename EntityType, typename ManagerType>
	void DataManager::removeEntityPropertyImpl(
			int64_t entityId, const std::string &key, ManagerType &manager,
			std::function<void(const EntityType &, const std::unordered_map<std::string, PropertyValue> &,
							   const std::unordered_map<std::string, PropertyValue> &)>
					validate,
			std::function<void(const EntityType &, const EntityType &)> notify) const {
		guardReadOnly();
		EntityType oldEntity = manager.get(entityId);
		auto existingProps = manager.getProperties(entityId);
		oldEntity.setProperties(existingProps);

		auto removedProps = existingProps;
		removedProps.erase(key);
		validate(oldEntity, existingProps, removedProps);

		observerManager_.setSuppressNotifications(true);
		manager.removeProperty(entityId, key);
		observerManager_.setSuppressNotifications(false);

		EntityType newEntity = manager.get(entityId);
		auto newProps = manager.getProperties(entityId);
		newEntity.setProperties(newProps);
		notify(oldEntity, newEntity);
		if constexpr (std::is_same_v<EntityType, Node> || std::is_same_v<EntityType, Edge>) {
			markGraphMutated();
		}
	}

	// --- Property Entity Operations ---

	void DataManager::addPropertyEntity(Property &property) const {
		propertyManager_->add(property);
		txnContext_.recordAdd(property);
	}

	void DataManager::addPropertyEntities(std::vector<Property> &properties) const {
		propertyManager_->addBatch(properties);
		txnContext_.recordAdds(properties);
	}

	void DataManager::updatePropertyEntity(const Property &property) const {
		Property oldProperty = propertyManager_->get(property.getId());
		txnContext_.recordUpdate<Property>(property, oldProperty);
		propertyManager_->update(property);
	}

	void DataManager::deleteProperty(Property &property) const { propertyManager_->remove(property); }

	Property DataManager::getProperty(int64_t id) const { return propertyManager_->get(id); }

	// --- Blob Operations ---

	void DataManager::addBlobEntity(Blob &blob) const {
		blobManager_->add(blob);
		txnContext_.recordAdd(blob);
	}

	void DataManager::updateBlobEntity(const Blob &blob) const {
		Blob oldBlob = blobManager_->get(blob.getId());
		txnContext_.recordUpdate<Blob>(blob, oldBlob);
		blobManager_->update(blob);
	}

	void DataManager::deleteBlob(Blob &blob) const { blobManager_->remove(blob); }

	Blob DataManager::getBlob(int64_t id) const { return blobManager_->get(id); }

	// --- Index Operations ---

	void DataManager::addIndexEntity(Index &index) const {
		indexEntityManager_->add(index);
		txnContext_.recordAdd(index);
	}

	void DataManager::updateIndexEntity(const Index &index) const {
		Index oldIndex = indexEntityManager_->get(index.getId());
		txnContext_.recordUpdate<Index>(index, oldIndex);
		indexEntityManager_->update(index);
	}

	void DataManager::deleteIndex(Index &index) const { indexEntityManager_->remove(index); }

	Index DataManager::getIndex(int64_t id) const { return indexEntityManager_->get(id); }

	// --- State Operations ---

	void DataManager::addStateEntity(State &state) const {
		stateManager_->add(state);
		txnContext_.recordAdd(state);
	}

	void DataManager::updateStateEntity(const State &state) const {
		State oldState = stateManager_->get(state.getId());
		txnContext_.recordUpdate<State>(state, oldState);
		stateManager_->update(state);
	}

	void DataManager::deleteState(State &state) const { stateManager_->remove(state); }

	State DataManager::getState(int64_t id) const { return stateManager_->get(id); }

	State DataManager::findStateByKey(const std::string &key) const { return stateManager_->findByKey(key); }

	void DataManager::addStateProperties(const std::string &stateKey,
										 const std::unordered_map<std::string, PropertyValue> &properties,
										 bool useBlobStorage) const {
		// 1. Capture OLD state (Snapshot)
		const State oldState = stateManager_->findByKey(stateKey);

		// 2. Perform the update (This effectively replaces the chain)
		// Pass the flag to StateManager
		stateManager_->addStateProperties(stateKey, properties, useBlobStorage);

		// 3. Capture NEW state
		const State newState = stateManager_->findByKey(stateKey);

		// 4. Notify Listeners
		observerManager_.notifyStateUpdated(oldState, newState);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getStateProperties(const std::string &stateKey) const {
		return stateManager_->getStateProperties(stateKey);
	}

	void DataManager::removeState(const std::string &stateKey) const { stateManager_->removeState(stateKey); }

	// --- Template Method Implementations ---

	template<typename EntityType>
	void DataManager::addToCache(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(this, entity);
	}

	template<typename EntityType>
	EntityType DataManager::getEntity(int64_t id) {
		return EntityTraits<EntityType>::get(this, id);
	}

	template<typename EntityType>
	void DataManager::updateEntity(const EntityType &entity) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			updateNode(entity);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			updateEdge(entity);
		}
		// Note: Property, Blob, Index, State updates use specialized methods directly
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::getEntitiesInRange(int64_t startId, int64_t endId, size_t limit) {
		std::vector<EntityType> result;
		if (startId > endId || limit == 0) {
			return result;
		}

		// Reserve memory to avoid reallocations.
		result.reserve((std::min) (static_cast<size_t>(endId - startId + 1), limit));

		// This set will keep track of IDs that have already been processed (from memory)
		// to avoid adding them again from the disk-based load.
		std::unordered_set<int64_t> processedIds;

		// --- PASS 1: Populate from Memory (PersistenceManager dirty entries) ---
		// This pass ensures data consistency by prioritizing in-memory changes over stale disk data.

		for (int64_t currentId = startId; currentId <= endId; ++currentId) {
			if (result.size() >= limit) {
				break;
			}

			// Check PersistenceManager first (Highest Priority)
			auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(currentId);

			if (dirtyInfo.has_value()) {
				processedIds.insert(currentId);

				if (dirtyInfo->changeType != EntityChangeType::CHANGE_DELETED &&
					dirtyInfo->backup.has_value()) { // ZYX_COV_EXCL_LINE: dirty registry always stores backups for
													 // non-delete range reads
					result.push_back(*dirtyInfo->backup);
				}
			}
		}

		// If we have already reached the limit just from memory, we can return early.
		if (result.size() >= limit) {
			return result;
		}

		// --- PASS 2: Load remaining entities from Disk using Segment-based reads ---
		// This pass leverages data locality for performance by reading from segments in bulk.
		// It will skip any entities that were already loaded from memory in Pass 1.

		// Find all segments that overlap with the requested ID range.
		auto entityType = EntityType::typeId;
		auto segments = segmentTracker_->getSegmentsByType(entityType);

		for (const auto &header: segments) {
			// Calculate the intersection between the segment's ID range and the user's requested range.
			int64_t segmentStartId = header.start_id;
			int64_t segmentEndId = header.start_id + header.used - 1;

			int64_t intersectStart = (std::max) (startId, segmentStartId);
			int64_t intersectEnd = (std::min) (endId, segmentEndId);

			if (intersectStart > intersectEnd) {
				continue; // No overlap with this segment.
			}

			// Load a batch of entities from this segment within the intersecting range.
			size_t needed = limit - result.size();
			std::vector<EntityType> segmentEntities =
					loadEntitiesFromSegment<EntityType>(header.file_offset, intersectStart, intersectEnd, needed);

			for (const EntityType &entity: segmentEntities) {
				// IMPORTANT: Only add the entity if it was not already processed from memory.
				if (!processedIds.contains(entity.getId())) { // ZYX_COV_EXCL_LINE: segment loader range is already
															  // filtered against dirty entries in tests
					result.push_back(entity);

					// Add the newly loaded entity to the cache for future queries.
					addToCache(entity);

					if (result.size() >= limit) {
						break;
					}
				}
			}

			if (result.size() >= limit) {
				break; // Stop iterating through segments if limit is reached.
			}
		}

		return result;
	}

	template<typename EntityType>
	uint64_t DataManager::findSegmentForEntityId(int64_t id) const {
		uint32_t type = EntityType::typeId;
		return segmentIndexManager_->findSegmentForId(type, id);
	}

	// --- Entity Loading from Disk ---

	template<typename EntityType>
	std::vector<EntityType> DataManager::loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		return readEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit);
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::readEntityFromDisk(const int64_t fileOffset) const {
		EntityType entity;

		if (hasPreadSupport()) { // ZYX_COV_EXCL_LINE: non-pread fallback is platform defensive and not hit on POSIX CI
			// Thread-safe path: pread() is atomic and needs no synchronization
			constexpr size_t entitySize = EntityType::getTotalSize();
			char buf[entitySize];
			ssize_t n = preadBytes(buf, entitySize, fileOffset);
			if (n < static_cast<ssize_t>(
							entitySize)) // ZYX_COV_EXCL_LINE: short pread requires corrupt/truncated segment file
				return std::nullopt;
			membuf mb(buf, entitySize);
			std::istream stream(&mb);
			entity = EntityType::deserialize(stream);
		} else {
			// Legacy path: requires external synchronization
			file_->seekg(fileOffset);
			entity = EntityType::deserialize(*file_);
		}

		if (!entity.isActive()) { // ZYX_COV_EXCL_LINE: inactive entities are filtered by segment metadata before direct
								  // reads
			return std::nullopt;
		}

		return entity;
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::findAndReadEntity(int64_t id) const {
		// Find segment for this entity type and ID
		uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);

		if (segmentOffset == 0) {
			return std::nullopt; // Segment not found
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate position of entity within segment
		uint64_t relativePosition = id - header.start_id;
		if (relativePosition >=
			header.used) { // ZYX_COV_EXCL_LINE: segment index lookup returns matching ranges for public reads
			return std::nullopt; // ID is out of range for this segment
		}

		// Calculate file offset for this entity
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														relativePosition * EntityType::getTotalSize());

		if (hasPreadSupport()) {
			// pread path: skip bitmap check — readEntityFromDisk checks isActive() on the
			// deserialized entity itself. This avoids a shared_lock on SegmentTracker per read.
			return readEntityFromDisk<EntityType>(entityOffset);
		}

		// fstream path: use bitmap to avoid unnecessary disk seek
		if (!segmentTracker_->isEntityActive(segmentOffset, relativePosition)) {
			return std::nullopt;
		}

		return readEntityFromDisk<EntityType>(entityOffset);
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		std::vector<EntityType> result;
		if (segmentOffset == 0 || limit == 0) {
			return result;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate effective start and end positions
		uint64_t effectiveStartId = (std::max) (startId, header.start_id);
		uint64_t effectiveEndId = (std::min) (endId, header.start_id + header.used - 1);

		if (effectiveStartId > effectiveEndId) {
			return result;
		}

		// Calculate offsets
		uint64_t startOffset = effectiveStartId - header.start_id;
		uint64_t count = (std::min) (effectiveEndId - effectiveStartId + 1, static_cast<uint64_t>(limit));

		// Reserve space for the maximum possible entities
		result.reserve(count);

		// Calculate starting file offset
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														startOffset * EntityType::getTotalSize());

		// Read entities (only active entities are returned)
		for (uint64_t i = 0; i < count; ++i) {
			auto entityOpt = readEntityFromDisk<EntityType>(entityOffset);
			if (entityOpt.has_value()) {
				result.push_back(entityOpt.value());
			}

			// Move to next entity
			entityOffset += EntityType::getTotalSize();
		}

		return result;
	}

	// --- Entity Memory Operations ---

	template<typename EntityType>
	void DataManager::setEntityDirty(const DirtyEntityInfo<EntityType> &info) {
		if (info.backup.has_value() && info.backup->getId() == 0) {
			return;
		}
		persistenceManager_->upsert(info);
	}

	template<typename EntityType>
	std::optional<DirtyEntityInfo<EntityType>> DataManager::getDirtyInfo(int64_t id) {
		return persistenceManager_->getDirtyInfo<EntityType>(id);
	}

	bool DataManager::hasUnsavedChanges() const { return persistenceManager_->hasUnsavedChanges(); }

	std::shared_ptr<CommittedSnapshot> DataManager::captureCommittedSnapshot() const {
		return persistenceManager_->captureCommittedSnapshot();
	}

	FlushSnapshot DataManager::prepareFlushSnapshot() const { return persistenceManager_->createSnapshot(); }

	FlushSnapshotView DataManager::prepareFlushSnapshotView() const {
		return persistenceManager_->createSnapshotView();
	}

	void DataManager::commitFlushSnapshot() const { persistenceManager_->commitSnapshot(); }

	void DataManager::setMaxDirtyEntities(size_t max) const { persistenceManager_->setMaxDirtyEntities(max); }

	void DataManager::setAutoFlushCallback(std::function<void()> cb) const {
		persistenceManager_->setAutoFlushCallback(std::move(cb));
	}

	void DataManager::checkAndTriggerAutoFlush() const {
		if (txnContext_.isActive())
			return; // Suppress auto-flush during active transaction
		persistenceManager_->checkAndTriggerAutoFlush();
	}

	void DataManager::setActiveTransaction(uint64_t txnId) {
		txnContext_.setActive(txnId);
		if (hasUnsavedChanges()) {
			txnContext_.setRollbackBase(captureCommittedSnapshot());
		}
	}

	bool DataManager::hasTransactionWALRecordsToFlush() const {
		if (!txnContext_.isActive() || !txnContext_.hasPendingWalRecords()) {
			return false;
		}
		if (!txnContext_.pendingWalChanges().empty()) {
			return true;
		}

		auto hasEffectiveAdds = [this]<typename EntityType>(std::span<const int64_t> entityIds) {
			if (entityIds.empty()) {
				return false;
			}
			bool found = false;
			persistenceManager_->visitDirtyInfos<EntityType>(
					entityIds,
					[&](int64_t, const DirtyEntityInfo<EntityType> *dirtyInfo) {
						if (found || !dirtyInfo || dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED ||
							!dirtyInfo->backup.has_value()) {
							return;
						}
						found = true;
					});
			return found;
		};

		const auto &added = txnContext_.pendingWalAddsByType();
		return hasEffectiveAdds.template operator()<Node>(added[Node::typeId]) ||
			   hasEffectiveAdds.template operator()<Edge>(added[Edge::typeId]) ||
			   hasEffectiveAdds.template operator()<Property>(added[Property::typeId]) ||
			   hasEffectiveAdds.template operator()<Blob>(added[Blob::typeId]) ||
			   hasEffectiveAdds.template operator()<Index>(added[Index::typeId]) ||
			   hasEffectiveAdds.template operator()<State>(added[State::typeId]);
	}

	void DataManager::flushTransactionWALRecords() const {
		if (!txnContext_.isActive()) {
			return;
		}
		auto *wal = txnContext_.getWALManager();
		if (!wal || !wal->isOpen() || !txnContext_.hasPendingWalRecords()) {
			return;
		}

		debug::ScopedPerfTimer timer("wal.entity_flush");
		const auto &added = txnContext_.pendingWalAddsByType();
		flushPendingWALAddRecordsForType<Node>(added[Node::typeId]);
		flushPendingWALAddRecordsForType<Edge>(added[Edge::typeId]);
		flushPendingWALAddRecordsForType<Property>(added[Property::typeId]);
		flushPendingWALAddRecordsForType<Blob>(added[Blob::typeId]);
		flushPendingWALAddRecordsForType<Index>(added[Index::typeId]);
		flushPendingWALAddRecordsForType<State>(added[State::typeId]);

		const auto &changes = txnContext_.pendingWalChanges();
		std::array<std::vector<const TransactionContext::PendingWalChange *>, getMaxEntityType() + 1> changesByType;
		for (auto &[key, change]: changes) {
			(void) key;
			if (change.entityType < changesByType.size()) { // ZYX_COV_EXCL_LINE: TransactionContext only records known entity type IDs.
				changesByType[change.entityType].push_back(&change);
			}
		}

		flushPendingWALRecordsForType<Node>(changesByType[Node::typeId]);
		flushPendingWALRecordsForType<Edge>(changesByType[Edge::typeId]);
		flushPendingWALRecordsForType<Property>(changesByType[Property::typeId]);
		flushPendingWALRecordsForType<Blob>(changesByType[Blob::typeId]);
		flushPendingWALRecordsForType<Index>(changesByType[Index::typeId]);
		flushPendingWALRecordsForType<State>(changesByType[State::typeId]);
	}

	template<typename EntityType>
	void DataManager::flushPendingWALAddRecordsForType(std::span<const int64_t> entityIds) const {
		if (entityIds.empty()) {
			return;
		}

		WalEntityViewBatchWriter<EntityType> writer(
				txnContext_, EntityChangeType::CHANGE_ADDED, entityIds.size());
		const auto entityType = static_cast<uint8_t>(EntityType::typeId);
		const bool hasCanceledAdds = txnContext_.hasCanceledPendingWalAdds(entityType);
		persistenceManager_->visitDirtyInfos<EntityType>(
				entityIds,
				[&](int64_t entityId, const DirtyEntityInfo<EntityType> *dirtyInfo) {
					if (hasCanceledAdds && !txnContext_.wasEntityAddedInActiveTransaction(entityType, entityId)) {
						return;
					}
					if (!dirtyInfo || dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED ||
						!dirtyInfo->backup.has_value()) {
						return;
					}
					writer.add(*dirtyInfo->backup);
				});
		writer.flush();
	}

		template<typename EntityType>
		void DataManager::flushPendingWALRecordsForType(
				std::span<const TransactionContext::PendingWalChange *const> changes) const {
			std::vector<int64_t> ids;
			std::vector<const TransactionContext::PendingWalChange *> typedChanges;
			ids.reserve(changes.size());
			typedChanges.reserve(changes.size());

			for (const auto *changePtr: changes) {
				ids.push_back(changePtr->entityId);
				typedChanges.push_back(changePtr);
			}

			if (ids.empty()) {
				return;
			}

		std::vector<wal::WALEntityChangeView> stagedViews;
		std::vector<int64_t> materializedIds;
		std::vector<const TransactionContext::PendingWalChange *> materializedChanges;
		stagedViews.reserve(typedChanges.size());
		materializedIds.reserve(ids.size());
		materializedChanges.reserve(typedChanges.size());
		for (const auto *changePtr: typedChanges) {
			if (changePtr->serializedData.empty()) {
				materializedIds.push_back(changePtr->entityId);
				materializedChanges.push_back(changePtr);
				continue;
			}
			stagedViews.push_back({changePtr->entityType,
								   static_cast<uint8_t>(changePtr->changeType),
								   changePtr->entityId,
								   changePtr->serializedData.data(),
								   static_cast<uint32_t>(changePtr->serializedData.size())});
		}
		txnContext_.flushWalChangeViews(stagedViews);
			if (materializedIds.empty()) {
				return;
			}

			WalEntityViewBatchWriter<EntityType> modified(
					txnContext_, EntityChangeType::CHANGE_MODIFIED, materializedChanges.size());
			WalEntityViewBatchWriter<EntityType> deleted(
					txnContext_, EntityChangeType::CHANGE_DELETED, materializedChanges.size());
			size_t changeIndex = 0;

			persistenceManager_->visitDirtyInfos<EntityType>(
					materializedIds, [&](int64_t, const DirtyEntityInfo<EntityType> *dirtyInfo) {
					const auto &change = *materializedChanges[changeIndex++];
					if (!dirtyInfo || !dirtyInfo->backup.has_value()) {
						if (change.changeType == EntityChangeType::CHANGE_DELETED) {
							txnContext_.flushSerializedWalChange(change);
						}
						return;
					}

						switch (change.changeType) {
							case EntityChangeType::CHANGE_ADDED:
								txnContext_.flushSerializedWalChange(change);
								break;
							case EntityChangeType::CHANGE_MODIFIED:
								modified.add(*dirtyInfo->backup);
								break;
						case EntityChangeType::CHANGE_DELETED:
							deleted.add(*dirtyInfo->backup);
							break;
					}
					});

			modified.flush();
			deleted.flush();
		}

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>>
	DataManager::getDirtyEntityInfos(const std::vector<EntityChangeType> &types) {
		return filterDirtyInfosByType(persistenceManager_->getAllDirtyInfos<EntityType>(), types);
	}

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>>
	DataManager::getDirtyEntityInfos(const std::vector<EntityChangeType> &types) const {
		return filterDirtyInfosByType(persistenceManager_->getAllDirtyInfos<EntityType>(), types);
	}

	template<typename EntityType>
	bool DataManager::hasDirtyEntityInfos(std::span<const EntityChangeType> types) const {
		return persistenceManager_->hasDirtyInfoOfTypes<EntityType>(types);
	}

	template<typename T>
	T make_inactive() {
		T entity;
		entity.markInactive();
		return entity;
	}

	// Helper: deserialize an entity from a cached page's raw bytes.
	// Returns default entity (id==0) if the entity is not in range or inactive.
	template<typename EntityType>
	EntityType deserializeEntityFromPage(const Page &page, int64_t id) {
		const auto *header = reinterpret_cast<const SegmentHeader *>(page.data.data());

		int64_t relativePosition = id - header->start_id;

		constexpr size_t entitySize = EntityType::getTotalSize();
		size_t entityOffset = SEGMENT_HEADER_SIZE + static_cast<size_t>(relativePosition) * entitySize;

		membuf mb(const_cast<char *>(reinterpret_cast<const char *>(page.data.data() + entityOffset)), entitySize);
		std::istream stream(&mb);
		return EntityType::deserialize(stream);
	}

	template<typename EntityType>
	EntityType DataManager::readEntityFromCacheOrSegment(int64_t id) {
		auto normalize = [](EntityType entity) {
			if (entity.getId() != 0 && entity.isActive()) {
				return entity;
			}
			return make_inactive<EntityType>();
		};

		const uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);
		if (segmentOffset != 0) {
			if (const Page *page = pagePool_->getPage(segmentOffset); page != nullptr) {
				return normalize(deserializeEntityFromPage<EntityType>(*page, id));
			}

			if (hasPreadSupport()) { // ZYX_COV_EXCL_LINE: StorageIO supplies positional reads on supported targets.
				std::vector<uint8_t> segData(TOTAL_SEGMENT_SIZE);
				const ssize_t n = preadBytes(segData.data(), TOTAL_SEGMENT_SIZE, static_cast<int64_t>(segmentOffset));
				if (n >= static_cast<ssize_t>(TOTAL_SEGMENT_SIZE)) { // ZYX_COV_EXCL_LINE: short reads require truncated/corrupt files.
					pagePool_->putPage(segmentOffset, std::vector<uint8_t>(segData));
					return normalize(deserializeEntityFromPage<EntityType>(
							Page{segmentOffset, std::move(segData)}, id));
				}
			}
		}

		return normalize(EntityTraits<EntityType>::loadFromDisk(this, id));
	}

	template<typename EntityType>
	EntityType DataManager::getEntityFromMemoryOrDisk(int64_t id) {
		// Check if we're in a read-only transaction with a snapshot
		const auto *snapshot = currentSnapshot_;
		if (snapshot != nullptr) {
			return getEntityWithSnapshot<EntityType>(id, snapshot);
		}

		// 1. Check dirty info via PersistenceManager (write transaction or no-txn context)
		{
			auto dirtyInfo = getDirtyInfo<EntityType>(id);

			if (dirtyInfo.has_value()) {
				if (dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED) {
					return make_inactive<EntityType>();
				}
				if (dirtyInfo->backup.has_value()) {
					return *dirtyInfo->backup;
				}
			}
		}

		return readEntityFromCacheOrSegment<EntityType>(id);
	}

	// Helper: get the snapshot map for a given entity type
	namespace {
		template<typename EntityType>
		const std::unordered_map<int64_t, DirtyEntityInfo<EntityType>> &
		getSnapshotMap(const CommittedSnapshot &snapshot);

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Node>> &
		getSnapshotMap<Node>(const CommittedSnapshot &snapshot) {
			return snapshot.nodes;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &
		getSnapshotMap<Edge>(const CommittedSnapshot &snapshot) {
			return snapshot.edges;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Property>> &
		getSnapshotMap<Property>(const CommittedSnapshot &snapshot) {
			return snapshot.properties;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &
		getSnapshotMap<Blob>(const CommittedSnapshot &snapshot) {
			return snapshot.blobs;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Index>> &
		getSnapshotMap<Index>(const CommittedSnapshot &snapshot) {
			return snapshot.indexes;
		}

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<State>> &
		getSnapshotMap<State>(const CommittedSnapshot &snapshot) {
			return snapshot.states;
		}
	} // namespace

	template<typename EntityType>
	EntityType DataManager::getEntityWithSnapshot(int64_t id, const CommittedSnapshot *snapshot) {
		// 1. Check snapshot dirty state
		const auto &snapshotMap = getSnapshotMap<EntityType>(*snapshot);
		auto it = snapshotMap.find(id);
		if (it != snapshotMap.end()) {
			const auto &info = it->second;
			if (info.changeType == EntityChangeType::CHANGE_DELETED) {
				return make_inactive<EntityType>();
			}
			if (info.backup.has_value()) {
				return *info.backup;
			}
		}

		return readEntityFromCacheOrSegment<EntityType>(id);
	}

	// Explicit instantiations for getEntityWithSnapshot
	template Node DataManager::getEntityWithSnapshot<Node>(int64_t, const CommittedSnapshot *);
	template Edge DataManager::getEntityWithSnapshot<Edge>(int64_t, const CommittedSnapshot *);
	template Property DataManager::getEntityWithSnapshot<Property>(int64_t, const CommittedSnapshot *);
	template Blob DataManager::getEntityWithSnapshot<Blob>(int64_t, const CommittedSnapshot *);
	template Index DataManager::getEntityWithSnapshot<Index>(int64_t, const CommittedSnapshot *);
	template State DataManager::getEntityWithSnapshot<State>(int64_t, const CommittedSnapshot *);

	template<typename EntityType>
	EntityType DataManager::loadEntityDirect(int64_t id) {
		// 1. Check dirty info (uncommitted changes must be visible)
		auto dirtyInfo = getDirtyInfo<EntityType>(id);
		if (dirtyInfo.has_value()) {
			if (dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED)
				return make_inactive<EntityType>();
			if (dirtyInfo->backup.has_value())
				return *dirtyInfo->backup;
		}

		// 2. Read directly from disk via pread (no cache, no locks)
		return EntityTraits<EntityType>::loadFromDisk(this, id);
	}

	// Explicit instantiations for loadEntityDirect
	template Node DataManager::loadEntityDirect<Node>(int64_t);
	template Edge DataManager::loadEntityDirect<Edge>(int64_t);
	template Property DataManager::loadEntityDirect<Property>(int64_t);
	template Blob DataManager::loadEntityDirect<Blob>(int64_t);
	template Index DataManager::loadEntityDirect<Index>(int64_t);
	template State DataManager::loadEntityDirect<State>(int64_t);

	Edge DataManager::getEdgeForAdjacency(int64_t id) const {
		if (id <= 0) {
			return make_inactive<Edge>();
		}

		if (const auto *snapshot = currentSnapshot_; snapshot != nullptr) {
			const auto &edges = getSnapshotMap<Edge>(*snapshot);
			if (const auto it = edges.find(id); it != edges.end() && it->second.backup.has_value()) {
				return *it->second.backup;
			}
		}

		if (auto dirtyInfo = persistenceManager_->getDirtyInfo<Edge>(id);
			dirtyInfo.has_value() && dirtyInfo->backup.has_value()) {
			return *dirtyInfo->backup;
		}

		const uint64_t segmentOffset = findSegmentForEntityId<Edge>(id);
		if (segmentOffset == 0) {
			return make_inactive<Edge>();
		}

		if (const Page *page = pagePool_->getPage(segmentOffset); page != nullptr) {
			return deserializeEntityFromPage<Edge>(*page, id);
		}

		const SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
		const auto relativePosition = static_cast<uint64_t>(id - header.start_id);
		if (relativePosition >= header.used) { // ZYX_COV_EXCL_LINE: segment index lookups return matching id ranges.
			return make_inactive<Edge>();
		}

		const auto entityOffset = static_cast<int64_t>(
				segmentOffset + sizeof(SegmentHeader) + relativePosition * Edge::getTotalSize());
		char buf[Edge::getTotalSize()];
		if (hasPreadSupport()) { // ZYX_COV_EXCL_LINE: StorageIO supplies positional reads on supported targets.
			const ssize_t n = preadBytes(buf, sizeof(buf), entityOffset);
			if (n < static_cast<ssize_t>(sizeof(buf))) { // ZYX_COV_EXCL_LINE: short reads require truncated/corrupt files.
				return make_inactive<Edge>();
			}
		} else {
			file_->seekg(entityOffset);
			file_->read(buf, static_cast<std::streamsize>(sizeof(buf)));
			if (file_->fail() || file_->gcount() < static_cast<std::streamsize>(sizeof(buf))) { // ZYX_COV_EXCL_LINE
				return make_inactive<Edge>();
			}
		}

		membuf mb(buf, sizeof(buf));
		std::istream stream(&mb);
		Edge edge = Edge::deserialize(stream);
		return edge.getId() == id ? edge : make_inactive<Edge>();
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::bulkLoadEntities(int64_t filterStartId, int64_t filterEndId) const {
		if (!hasPreadSupport())
			return {}; // Requires pread support

		// Get sorted segment list for this entity type
		const auto &segIndex = EntityTraits<EntityType>::getSegmentIndex(this);
		constexpr size_t entitySize = EntityType::getTotalSize();

		std::vector<EntityType> result;
		result.reserve(segIndex.size() * 4); // Rough estimate

		for (const auto &seg: segIndex) {
			// Skip segments entirely outside the ID range
			if (seg.endId < filterStartId || seg.startId > filterEndId)
				continue;

			// Get segment header (cached in memory, no lock needed)
			SegmentHeader header = segmentTracker_->getSegmentHeader(seg.segmentOffset);
			if (header.used == 0)
				continue;

			// Read entire segment data area in one pread syscall
			size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
			std::vector<char> buf(dataBytes);
			auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
			ssize_t n = preadBytes(buf.data(), dataBytes, dataOffset);
			if (n < static_cast<ssize_t>(dataBytes))
				continue;

			// Deserialize all entities from the buffer
			for (uint32_t i = 0; i < header.used; ++i) {
				int64_t entityId = header.start_id + i;
				if (entityId < filterStartId || entityId > filterEndId)
					continue;

				membuf mb(buf.data() + i * entitySize, entitySize);
				std::istream stream(&mb);
				EntityType entity = EntityType::deserialize(stream);

				if (entity.isActive()) {
					result.push_back(std::move(entity));
				}
			}
		}

		return result;
	}

	// Explicit instantiations for bulkLoadEntities
	template std::vector<Node> DataManager::bulkLoadEntities<Node>(int64_t, int64_t) const;
	template std::vector<Edge> DataManager::bulkLoadEntities<Edge>(int64_t, int64_t) const;
	template std::vector<Property> DataManager::bulkLoadEntities<Property>(int64_t, int64_t) const;

	// --- Loading Entities from Disk ---

	Node DataManager::loadNodeFromDisk(const int64_t id) const {
		return findAndReadEntity<Node>(id).value_or(make_inactive<Node>());
	}
	Edge DataManager::loadEdgeFromDisk(const int64_t id) const {
		return findAndReadEntity<Edge>(id).value_or(make_inactive<Edge>());
	}
	Property DataManager::loadPropertyFromDisk(const int64_t id) const {
		return findAndReadEntity<Property>(id).value_or(make_inactive<Property>());
	}
	Blob DataManager::loadBlobFromDisk(const int64_t id) const {
		return findAndReadEntity<Blob>(id).value_or(make_inactive<Blob>());
	}
	Index DataManager::loadIndexFromDisk(const int64_t id) const {
		return findAndReadEntity<Index>(id).value_or(make_inactive<Index>());
	}
	State DataManager::loadStateFromDisk(const int64_t id) const {
		return findAndReadEntity<State>(id).value_or(make_inactive<State>());
	}

	// --- Rollback Template Helpers ---

	template<typename EntityType, typename ManagerType>
	void DataManager::rollbackAddedEntry(const wal::UndoEntry &entry, ManagerType &manager,
										 std::function<void(const EntityType &)> notifyDeleted) const {
		try {
			EntityType entity = manager.get(entry.entityId);
			notifyDeleted(entity);
		} catch (...) {
		}
	}

	template<typename EntityType, typename ManagerType>
	void DataManager::rollbackModifiedEntry(
			const wal::UndoEntry &entry, ManagerType &manager,
			std::function<void(const EntityType &, const EntityType &)> notifyUpdated) const {
		if (entry.beforeImage.empty())
			return;
		try {
			std::string imgStr(entry.beforeImage.begin(), entry.beforeImage.end());
			std::istringstream iss(imgStr, std::ios::binary);
			EntityType oldEntity =
					utils::FixedSizeSerializer::deserializeWithFixedSize<EntityType>(iss, EntityType::getTotalSize());
			EntityType currentEntity = manager.get(entry.entityId);
			notifyUpdated(currentEntity, oldEntity);
		} catch (...) {
		}
	}

	template<typename EntityType>
	void DataManager::rollbackDeletedEntry(const wal::UndoEntry &entry,
										   std::function<void(const EntityType &)> notifyAdded) const {
		if (entry.beforeImage.empty())
			return;
		try {
			std::string imgStr(entry.beforeImage.begin(), entry.beforeImage.end());
			std::istringstream iss(imgStr, std::ios::binary);
			EntityType oldEntity =
					utils::FixedSizeSerializer::deserializeWithFixedSize<EntityType>(iss, EntityType::getTotalSize());
			notifyAdded(oldEntity);
		} catch (...) {
		}
	}

	// --- Transaction Rollback ---

	void DataManager::rollbackActiveTransaction() {
		persistenceManager_->setTransactionActive(true);

		const auto &entries = txnContext_.undoLog().entries();
		for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
			auto entityType = static_cast<EntityType>(it->entityType);
			bool isNode = (entityType == EntityType::Node);
			bool isEdge = (entityType == EntityType::Edge);

			switch (it->changeType) {
				case wal::UndoChangeType::UNDO_ADDED:
					if (isNode) {
						rollbackAddedEntry<Node>(*it, *nodeManager_,
												 [this](const Node &n) { observerManager_.notifyNodeDeleted(n); });
					} else if (isEdge) {
						rollbackAddedEntry<Edge>(*it, *edgeManager_,
												 [this](const Edge &e) { observerManager_.notifyEdgeDeleted(e); });
					}
					break;
				case wal::UndoChangeType::UNDO_MODIFIED:
					if (isNode) {
						rollbackModifiedEntry<Node>(*it, *nodeManager_, [this](const Node &c, const Node &o) {
							observerManager_.notifyNodeUpdated(c, o);
						});
					} else if (isEdge) {
						rollbackModifiedEntry<Edge>(*it, *edgeManager_, [this](const Edge &c, const Edge &o) {
							observerManager_.notifyEdgeUpdated(c, o);
						});
					}
					break;
				case wal::UndoChangeType::UNDO_DELETED:
					if (isNode) {
						rollbackDeletedEntry<Node>(*it, [this](const Node &n) { observerManager_.notifyNodeAdded(n); });
					} else if (isEdge) {
						rollbackDeletedEntry<Edge>(*it, [this](const Edge &e) { observerManager_.notifyEdgeAdded(e); });
					}
					break;
			}
		}

		if (const auto &rollbackBase = txnContext_.rollbackBase()) {
			persistenceManager_->restoreCommittedSnapshot(*rollbackBase);
		} else {
			persistenceManager_->clearAll();
		}
		persistenceManager_->setTransactionActive(false);
		clearCache();
	}

	// --- Cache and Transaction Management ---

	void DataManager::clearCache() const {
		pagePool_->clear();
		clearRelationshipSegmentTypeStats();
	}


	void DataManager::invalidateDirtySegments(const FlushSnapshot &snapshot) const {
		std::unordered_set<uint64_t> dirtySegments;

		auto collect = [&](const auto &entityMap, auto findSeg) {
			for (const auto &[id, info]: entityMap) {
				uint64_t seg = findSeg(id);
				if (seg != 0) {
					dirtySegments.insert(seg);
				}
			}
		};

		collect(snapshot.nodes, [&](int64_t id) { return findSegmentForEntityId<Node>(id); });
		collect(snapshot.edges, [&](int64_t id) { return findSegmentForEntityId<Edge>(id); });
		collect(snapshot.properties, [&](int64_t id) { return findSegmentForEntityId<Property>(id); });
		collect(snapshot.blobs, [&](int64_t id) { return findSegmentForEntityId<Blob>(id); });
		collect(snapshot.indexes, [&](int64_t id) { return findSegmentForEntityId<Index>(id); });
		collect(snapshot.states, [&](int64_t id) { return findSegmentForEntityId<State>(id); });

		for (uint64_t seg: dirtySegments) {
			pagePool_->invalidate(seg);
		}
		std::vector<uint64_t> dirtySegmentList(dirtySegments.begin(), dirtySegments.end());
		invalidateRelationshipSegmentTypeStats(dirtySegmentList);
	}

	void DataManager::invalidateDirtySegments(const FlushSnapshotView &snapshot) const {
		std::unordered_set<uint64_t> dirtySegments;

		auto collect = [&](const auto *entityMap, auto findSeg) {
			if (!entityMap) {
				return;
			}
			for (const auto &[id, info]: *entityMap) {
				uint64_t seg = findSeg(id);
				if (seg != 0) {
					dirtySegments.insert(seg);
				}
			}
		};

		collect(snapshot.nodes, [&](int64_t id) { return findSegmentForEntityId<Node>(id); });
		collect(snapshot.edges, [&](int64_t id) { return findSegmentForEntityId<Edge>(id); });
		collect(snapshot.properties, [&](int64_t id) { return findSegmentForEntityId<Property>(id); });
		collect(snapshot.blobs, [&](int64_t id) { return findSegmentForEntityId<Blob>(id); });
		collect(snapshot.indexes, [&](int64_t id) { return findSegmentForEntityId<Index>(id); });
		collect(snapshot.states, [&](int64_t id) { return findSegmentForEntityId<State>(id); });

		for (uint64_t seg: dirtySegments) {
			pagePool_->invalidate(seg);
		}
		std::vector<uint64_t> dirtySegmentList(dirtySegments.begin(), dirtySegments.end());
		invalidateRelationshipSegmentTypeStats(dirtySegmentList);
	}

	void DataManager::invalidateSegments(std::span<const uint64_t> segmentOffsets) const {
		for (uint64_t segmentOffset: segmentOffsets) {
			if (segmentOffset != 0) {
				pagePool_->invalidate(segmentOffset);
			}
		}
		invalidateRelationshipSegmentTypeStats(segmentOffsets);
	}

	template<typename EntityType>
	void DataManager::markEntityDeleted(EntityType &entity) {
		txnContext_.recordDelete<EntityType>(entity.getId(), [&entity](int64_t) { return entity; });

		auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(entity.getId());

		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED) {
			// Dirty ADD has no disk slot yet. Removing it keeps the checkpoint
			// overlay compact; if it was committed by an earlier transaction,
			// recordDelete() has already staged a WAL tombstone for recovery.
			const int64_t id = entity.getId();
			persistenceManager_->remove<EntityType>(id);
		} else {
			entity.markInactive();
			persistenceManager_->upsert(DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_DELETED, entity));
		}

		EntityTraits<EntityType>::removeFromCache(this, entity.getId());
		checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void DataManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		propertyManager_->removeEntityProperty<EntityType>(entityId, key);
	}

	// --- Template Instantiations ---

	// Node-specific instantiations
	template void DataManager::addToCache<Node>(const Node &entity);
	template Node DataManager::getEntity<Node>(int64_t id);
	template void DataManager::updateEntity<Node>(const Node &entity);
	template std::optional<DirtyEntityInfo<Node>> DataManager::getDirtyInfo<Node>(int64_t);
	template std::vector<Node> DataManager::getEntitiesInRange<Node>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Node>(int64_t id) const;
	template Node DataManager::getEntityFromMemoryOrDisk<Node>(int64_t id);
	template void DataManager::removeEntityProperty<Node>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Node>(Node &entity);
	template std::vector<Node> DataManager::loadEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Node> DataManager::readEntityFromDisk<Node>(int64_t fileOffset) const;
	template std::optional<Node> DataManager::findAndReadEntity<Node>(int64_t id) const;
	template std::vector<Node> DataManager::readEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t) const;
	template std::vector<DirtyEntityInfo<Node>>
	DataManager::getDirtyEntityInfos<Node>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Node>>
	DataManager::getDirtyEntityInfos<Node>(const std::vector<EntityChangeType> &) const;
	template bool DataManager::hasDirtyEntityInfos<Node>(std::span<const EntityChangeType>) const;
	template void DataManager::setEntityDirty<Node>(const DirtyEntityInfo<Node> &);

	// Edge-specific instantiations
	template void DataManager::addToCache<Edge>(const Edge &entity);
	template Edge DataManager::getEntity<Edge>(int64_t id);
	template void DataManager::updateEntity<Edge>(const Edge &entity);
	template std::optional<DirtyEntityInfo<Edge>> DataManager::getDirtyInfo<Edge>(int64_t);
	template std::vector<Edge> DataManager::getEntitiesInRange<Edge>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Edge>(int64_t id) const;
	template Edge DataManager::getEntityFromMemoryOrDisk<Edge>(int64_t id);
	template void DataManager::removeEntityProperty<Edge>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Edge>(Edge &entity);
	template std::vector<Edge> DataManager::loadEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Edge> DataManager::readEntityFromDisk<Edge>(int64_t fileOffset) const;
	template std::optional<Edge> DataManager::findAndReadEntity<Edge>(int64_t id) const;
	template std::vector<Edge> DataManager::readEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t) const;
	template std::vector<DirtyEntityInfo<Edge>>
	DataManager::getDirtyEntityInfos<Edge>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Edge>>
	DataManager::getDirtyEntityInfos<Edge>(const std::vector<EntityChangeType> &) const;
	template bool DataManager::hasDirtyEntityInfos<Edge>(std::span<const EntityChangeType>) const;
	template void DataManager::setEntityDirty<Edge>(const DirtyEntityInfo<Edge> &);

	// Property-specific instantiations
	template void DataManager::addToCache<Property>(const Property &entity);
	template std::optional<DirtyEntityInfo<Property>> DataManager::getDirtyInfo<Property>(int64_t);
	template Property DataManager::getEntityFromMemoryOrDisk<Property>(int64_t id);
	template std::vector<Property> DataManager::getEntitiesInRange<Property>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Property>(int64_t id) const;
	template void DataManager::markEntityDeleted<Property>(Property &entity);
	template std::vector<DirtyEntityInfo<Property>>
	DataManager::getDirtyEntityInfos<Property>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Property>>
	DataManager::getDirtyEntityInfos<Property>(const std::vector<EntityChangeType> &) const;
	template bool DataManager::hasDirtyEntityInfos<Property>(std::span<const EntityChangeType>) const;
	template void DataManager::setEntityDirty<Property>(const DirtyEntityInfo<Property> &);

	// Blob-specific instantiations
	template Blob DataManager::getEntityFromMemoryOrDisk<Blob>(int64_t id);
	template std::optional<DirtyEntityInfo<Blob>> DataManager::getDirtyInfo<Blob>(int64_t);
	template std::vector<Blob> DataManager::getEntitiesInRange<Blob>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Blob>(int64_t id) const;
	template void DataManager::markEntityDeleted<Blob>(Blob &entity);
	template std::vector<DirtyEntityInfo<Blob>>
	DataManager::getDirtyEntityInfos<Blob>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Blob>>
	DataManager::getDirtyEntityInfos<Blob>(const std::vector<EntityChangeType> &) const;
	template bool DataManager::hasDirtyEntityInfos<Blob>(std::span<const EntityChangeType>) const;
	template void DataManager::setEntityDirty<Blob>(const DirtyEntityInfo<Blob> &);

	// Index-specific instantiations
	template Index DataManager::getEntityFromMemoryOrDisk<Index>(int64_t id);
	template std::optional<DirtyEntityInfo<Index>> DataManager::getDirtyInfo<Index>(int64_t);
	template std::vector<Index> DataManager::getEntitiesInRange<Index>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Index>(int64_t id) const;
	template void DataManager::markEntityDeleted<Index>(Index &entity);
	template std::vector<DirtyEntityInfo<Index>>
	DataManager::getDirtyEntityInfos<Index>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<Index>>
	DataManager::getDirtyEntityInfos<Index>(const std::vector<EntityChangeType> &) const;
	template bool DataManager::hasDirtyEntityInfos<Index>(std::span<const EntityChangeType>) const;
	template void DataManager::setEntityDirty<Index>(const DirtyEntityInfo<Index> &);

	// State-specific instantiations
	template State DataManager::getEntityFromMemoryOrDisk<State>(int64_t id);
	template std::optional<DirtyEntityInfo<State>> DataManager::getDirtyInfo<State>(int64_t);
	template std::vector<State> DataManager::getEntitiesInRange<State>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<State>(int64_t id) const;
	template void DataManager::markEntityDeleted<State>(State &entity);
	template std::vector<DirtyEntityInfo<State>>
	DataManager::getDirtyEntityInfos<State>(const std::vector<EntityChangeType> &);
	template std::vector<DirtyEntityInfo<State>>
	DataManager::getDirtyEntityInfos<State>(const std::vector<EntityChangeType> &) const;
	template bool DataManager::hasDirtyEntityInfos<State>(std::span<const EntityChangeType>) const;
	template void DataManager::setEntityDirty<State>(const DirtyEntityInfo<State> &);
} // namespace graph::storage
