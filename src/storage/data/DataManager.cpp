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
#include <cstring>
#include <map>
#include <sstream>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/core/BlobChainManager.hpp"
#include "graph/core/EntityPropertyTraits.hpp"
#include "graph/core/StateChainManager.hpp"
#include "graph/core/Types.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/SegmentReadUtils.hpp"
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/DeletionManager.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"
#include "graph/storage/data/StateManager.hpp"
#include "graph/storage/dictionaries/LabelTokenRegistry.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::storage {

	// Thread-local snapshot pointer for read-only transactions
	thread_local const CommittedSnapshot *DataManager::currentSnapshot_ = nullptr;

	DataManager::DataManager(std::shared_ptr<std::fstream> file, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<SpaceManager> spaceManager,
							 const std::string &filePath) :
		file_(std::move(file)), fileHeader_(fileHeader), nodeCache_(cacheSize), edgeCache_(cacheSize),
		propertyCache_(cacheSize * 2), blobCache_(cacheSize / 4), indexCache_(cacheSize), stateCache_(cacheSize),
		idAllocator_(std::move(idAllocator)), segmentTracker_(std::move(segmentTracker)),
		spaceManager_(std::move(spaceManager)) {

		// Open a read-only file descriptor for pread()-based parallel reads.
		// pread() is atomic (no seek+read race) so multiple threads can call it
		// concurrently on the same fd without any synchronization.
		if (!filePath.empty()) {
			readFd_ = storage::portable_open(filePath.c_str(), O_RDONLY);
			// Share the fd with SegmentTracker so its ensureSegmentCached() is also lock-free
			if (readFd_ >= 0) {
				segmentTracker_->setReadFd(readFd_);
			}
		}

		persistenceManager_ = std::make_shared<PersistenceManager>();
		segmentIndexManager_ = std::make_shared<SegmentIndexManager>(segmentTracker_);
		segmentTracker_->setSegmentIndexManager(std::weak_ptr(segmentIndexManager_));
	}

	DataManager::~DataManager() {
		if (readFd_ >= 0) {
			storage::portable_close(readFd_);
			readFd_ = -1;
		}
	}

	ssize_t DataManager::preadBytes(void *buf, size_t count, int64_t offset) const {
		if (readFd_ < 0)
			return -1;
		return storage::portable_pread(readFd_, buf, count, offset);
	}

	// Helper streambuf that wraps an existing memory buffer for zero-copy deserialization.
	// This lets us pread() into a stack buffer, then deserialize via the existing istream API.
	namespace {
		class membuf : public std::streambuf {
		public:
			membuf(char *base, size_t size) { this->setg(base, base, base + size); }
		};
	} // namespace

	void DataManager::initialize() {
		// Initialize low-level components
		deletionManager_ = std::make_shared<DeletionManager>(shared_from_this(), spaceManager_, idAllocator_);
		entityReferenceUpdater_ = std::make_shared<EntityReferenceUpdater>(shared_from_this());
		spaceManager_->setEntityReferenceUpdater(entityReferenceUpdater_);
		relationshipTraversal_ = std::make_shared<traversal::RelationshipTraversal>(shared_from_this());

		// Initialize segment indexes
		initializeSegmentIndexes();

		// Initialize entity managers
		initializeManagers();
	}

	void DataManager::setSystemStateManager(const std::shared_ptr<state::SystemStateManager> &systemStateManager) {
		systemStateManager_ = systemStateManager;

		// Now that we have the high-level state manager, we can initialize the label registry
		initializeLabelRegistry(systemStateManager);
	}

	void DataManager::initializeLabelRegistry(std::shared_ptr<state::SystemStateManager> sm) {
		// Pass the correct high-level SystemStateManager to the registry
		labelRegistry_ = std::make_unique<LabelTokenRegistry>(shared_from_this(), sm);
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

	void DataManager::registerObserver(std::shared_ptr<IEntityObserver> observer) {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		observers_.push_back(std::move(observer));
	}

	void DataManager::registerValidator(std::shared_ptr<constraints::IEntityValidator> validator) {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		validators_.push_back(std::move(validator));
	}

	// --- Notification Helper Implementations ---

	void DataManager::notifyNodeAdded(const Node &node) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onNodeAdded(node);
		}
	}

	void DataManager::notifyNodesAdded(const std::vector<Node> &nodes) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			// Use the new batch interface!
			observer->onNodesAdded(nodes);
		}
	}

	void DataManager::notifyNodeUpdated(const Node &oldNode, const Node &newNode) const {
		if (suppressNotifications_) return;
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onNodeUpdated(oldNode, newNode);
		}
	}

	void DataManager::notifyNodeDeleted(const Node &node) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onNodeDeleted(node);
		}
	}

	void DataManager::notifyEdgeAdded(const Edge &edge) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onEdgeAdded(edge);
		}
	}

	void DataManager::notifyEdgesAdded(const std::vector<Edge> &edges) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			for (const auto &edge: edges) {
				observer->onEdgeAdded(edge);
			}
		}
	}

	void DataManager::notifyEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) const {
		if (suppressNotifications_) return;
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onEdgeUpdated(oldEdge, newEdge);
		}
	}

	void DataManager::notifyEdgeDeleted(const Edge &edge) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onEdgeDeleted(edge);
		}
	}

	void DataManager::notifyStateUpdated(const State &oldState, const State &newState) const {
		std::lock_guard<std::recursive_mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onStateUpdated(oldState, newState);
		}
	}

	template<typename T>
	void DataManager::updateEntityImpl(const T &entity, std::function<T(int64_t)> getOldFunc,
									   std::function<void(T)> internalUpdateFunc,
									   std::function<void(const T &, const T &)> notifyFunc) {
		// Check dirty info via persistence manager
		auto dirtyInfo = persistenceManager_->getDirtyInfo<T>(entity.getId());

		// If entity is marked as ADDED, this update is part of creation.
		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED) {
			// Even if it's "ADDED", the content (Label/Props) might have changed since the initial addNode().
			// We MUST notify observers (IndexManager) to update their in-memory structures.

			// 1. Get the current state from memory (Old Label)
			T oldEntity = getOldFunc(entity.getId());

			// 2. Perform the update (New Label)
			internalUpdateFunc(entity);

			// 3. Notify (Old vs New)
			notifyFunc(oldEntity, entity);
			return;
		}

		// Standard update flow for existing entities
		T oldEntity = getOldFunc(entity.getId());

		internalUpdateFunc(entity);

		notifyFunc(oldEntity, entity);
	}

	int64_t DataManager::getOrCreateLabelId(const std::string &label) const {
		if (label.empty())
			return 0;
		if (!labelRegistry_) {
			throw std::runtime_error("LabelTokenRegistry not initialized. Ensure SystemStateManager is set.");
		}
		return labelRegistry_->getOrCreateLabelId(label);
	}

	std::string DataManager::resolveLabel(int64_t labelId) const {
		if (labelId == 0)
			return "";
		if (!labelRegistry_) {
			return "";
		}

		return labelRegistry_->getLabelString(labelId);
	}

	// --- Transaction State Management ---

	void DataManager::setActiveTransaction(uint64_t txnId) {
		transactionActive_ = true;
		activeTxnId_ = txnId;
		txnOps_.clear();
	}

	void DataManager::clearActiveTransaction() {
		transactionActive_ = false;
		activeTxnId_ = 0;
		txnOps_.clear();
	}

	// --- Node Operations (delegate to NodeManager) ---

	void DataManager::addNode(Node &node) const {
		// Phase 1: Validate
		for (const auto &v : validators_) {
			v->validateNodeInsert(node, node.getProperties());
		}

		// Phase 2: Write
		nodeManager_->add(node);

		if (transactionActive_) {
			txnOps_.push_back({Transaction::TxnOperation::OP_ADD,
							   static_cast<uint8_t>(EntityType::Node), node.getId()});
			if (walManager_) {
				walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::Node),
											   static_cast<uint8_t>(EntityChangeType::CHANGE_ADDED), node.getId(), {});
			}
		}

		notifyNodeAdded(node);
	}

	void DataManager::addNodes(std::vector<Node> &nodes) const {
		if (nodes.empty())
			return;

		// Phase 1: Validate all nodes before any writes (atomicity)
		for (const auto &node : nodes) {
			for (const auto &v : validators_) {
				v->validateNodeInsert(node, node.getProperties());
			}
		}

		// Phase 2: Write
		nodeManager_->addBatch(nodes);
		notifyNodesAdded(nodes);

		if (transactionActive_) {
			for (const auto &node: nodes) {
				txnOps_.push_back({Transaction::TxnOperation::OP_ADD,
								   static_cast<uint8_t>(EntityType::Node), node.getId()});
			}
		}

		for (auto &node: nodes) {
			if (node.getProperties().empty())
				continue;

			propertyManager_->storeProperties(node);

			if (EntityPropertyTraits<Node>::hasPropertyEntity(node)) {
				nodeManager_->update(node);
			}
		}
	}

	void DataManager::updateNode(const Node &node) {
		if (transactionActive_) {
			txnOps_.push_back({Transaction::TxnOperation::OP_UPDATE,
							   static_cast<uint8_t>(EntityType::Node), node.getId()});
		}

		updateEntityImpl<Node>(
				node, [this](int64_t id) { return nodeManager_->get(id); },
				[this](const Node &n) { nodeManager_->update(n); },
				[this](const Node &o, const Node &n) { notifyNodeUpdated(o, n); });
	}

	void DataManager::deleteNode(Node &node) const {
		if (transactionActive_) {
			txnOps_.push_back({Transaction::TxnOperation::OP_DELETE,
							   static_cast<uint8_t>(EntityType::Node), node.getId()});
		}

		nodeManager_->remove(node);
		notifyNodeDeleted(node);
	}

	Node DataManager::getNode(int64_t id) const { return nodeManager_->get(id); }

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) const {
		return nodeManager_->getBatch(ids);
	}

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return nodeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addNodeProperties(int64_t nodeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		// 1. Snapshot OLD state
		Node oldNode = nodeManager_->get(nodeId);
		// Manually fetch and bake the existing properties into the oldNode object.
		// This ensures oldNode.getProperties() returns the state BEFORE the modification.
		auto existingProps = nodeManager_->getProperties(nodeId);
		oldNode.setProperties(existingProps);

		// 1.5. Validate: build merged new props for constraint checking
		auto mergedProps = existingProps;
		for (const auto &[key, val] : properties) {
			mergedProps[key] = val;
		}
		for (const auto &v : validators_) {
			v->validateNodeUpdate(oldNode, existingProps, mergedProps);
		}

		// 2. Perform the modification (suppress intermediate notifications from updateEntity
		//    inside PropertyManager, which would fire with cleared inline properties)
		suppressNotifications_ = true;
		nodeManager_->addProperties(nodeId, properties);
		suppressNotifications_ = false;

		// 3. Snapshot NEW state
		Node newNode = nodeManager_->get(nodeId);
		auto newProps = nodeManager_->getProperties(nodeId);
		newNode.setProperties(newProps);

		// 4. Notify with valid snapshots (full properties, not just inline)
		notifyNodeUpdated(oldNode, newNode);
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) const {
		// 1. Snapshot OLD state
		Node oldNode = nodeManager_->get(nodeId);
		// Freeze old properties
		auto existingProps = nodeManager_->getProperties(nodeId);
		oldNode.setProperties(existingProps);

		// 1.5. Validate: build props with key removed
		auto removedProps = existingProps;
		removedProps.erase(key);
		for (const auto &v : validators_) {
			v->validateNodeUpdate(oldNode, existingProps, removedProps);
		}

		// 2. Perform removal (suppress intermediate notifications)
		suppressNotifications_ = true;
		nodeManager_->removeProperty(nodeId, key);
		suppressNotifications_ = false;

		// 3. Snapshot NEW state
		Node newNode = nodeManager_->get(nodeId);
		auto newProps = nodeManager_->getProperties(nodeId);
		newNode.setProperties(newProps);

		// 4. Notify
		notifyNodeUpdated(oldNode, newNode);
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
					for (const auto &[key, value] : property.getPropertyValues()) {
						allProperties[key] = value;
					}
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				// Blob chain reads are more complex; fall back to regular path
				auto blobProperties = propertyManager_->getPropertiesFromBlob(propertyEntityId);
				for (const auto &[key, value] : blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodePropertiesFromMap(
		const Node &node, const std::unordered_map<int64_t, Property> &propertyMap) {
		if (node.getId() == 0 || !node.isActive())
			return {};

		auto allProperties = EntityPropertyTraits<Node>::getProperties(node);

		if (EntityPropertyTraits<Node>::hasPropertyEntity(node)) {
			auto storageType = EntityPropertyTraits<Node>::getPropertyStorageType(node);
			auto propertyEntityId = EntityPropertyTraits<Node>::getPropertyEntityId(node);

			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				auto it = propertyMap.find(propertyEntityId);
				if (it != propertyMap.end() && it->second.getId() != 0) {
					for (const auto &[key, value] : it->second.getPropertyValues()) {
						allProperties[key] = value;
					}
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				auto blobProperties = propertyManager_->getPropertiesFromBlob(propertyEntityId);
				for (const auto &[key, value] : blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}

	std::unordered_map<int64_t, Property> DataManager::bulkLoadPropertyEntities(
		const std::vector<int64_t> &ids, concurrent::ThreadPool *pool) const {
		std::unordered_map<int64_t, Property> result;
		if (ids.empty() || readFd_ < 0)
			return result;

		// Sort IDs and group by segment for sequential I/O
		std::vector<int64_t> sortedIds(ids);
		std::sort(sortedIds.begin(), sortedIds.end());

		const auto &segIndex = segmentIndexManager_->getPropertySegmentIndex();
		constexpr size_t entitySize = Property::getTotalSize();

		// Find relevant segments and their ID ranges
		struct SegWork {
			size_t segIdx;
			size_t idBegin, idEnd; // indices into sortedIds
		};
		std::vector<SegWork> work;
		for (size_t s = 0; s < segIndex.size(); ++s) {
			auto lo = std::lower_bound(sortedIds.begin(), sortedIds.end(), segIndex[s].startId);
			auto hi = std::upper_bound(lo, sortedIds.end(), segIndex[s].endId);
			if (lo != hi) {
				work.push_back({s,
					static_cast<size_t>(lo - sortedIds.begin()),
					static_cast<size_t>(hi - sortedIds.begin())});
			}
		}

		if (work.empty())
			return result;

		// Parallel path: coalesce consecutive segments into single large I/O calls
		if (pool && !pool->isSingleThreaded() && work.size() > 1) {
			// Build segment index list from work items
			std::vector<size_t> workSegIndices;
			workSegIndices.reserve(work.size());
			for (const auto &w : work)
				workSegIndices.push_back(w.segIdx);

			auto groups = buildCoalescedGroups(workSegIndices, segIndex);
			std::vector<std::vector<std::pair<int64_t, Property>>> perSeg(work.size());

			pool->parallelFor(0, groups.size(), [&](size_t gi) {
				const auto &group = groups[gi];
				// Single pread for the entire coalesced group
				size_t totalBytes = group.segCount * TOTAL_SEGMENT_SIZE;
				std::vector<char> groupBuf(totalBytes);
				auto groupOffset = static_cast<int64_t>(group.startOffset);
				ssize_t n = storage::portable_pread(readFd_, groupBuf.data(), totalBytes, groupOffset);
				if (n < static_cast<ssize_t>(totalBytes))
					return;

				for (size_t mi = 0; mi < group.memberIndices.size(); ++mi) {
					size_t wi = group.memberIndices[mi];
					const auto &w = work[wi];
					size_t bufOffset = mi * TOTAL_SEGMENT_SIZE;

					SegmentHeader header;
					std::memcpy(&header, groupBuf.data() + bufOffset, sizeof(SegmentHeader));
					if (header.used == 0)
						continue;

					const char *dataBuf = groupBuf.data() + bufOffset + sizeof(SegmentHeader);
					auto &local = perSeg[wi];
					for (size_t i = w.idBegin; i < w.idEnd; ++i) {
						int64_t id = sortedIds[i];
						uint32_t slot = static_cast<uint32_t>(id - header.start_id);
						if (slot >= header.used)
							continue;
						Property prop = Property::deserializeFromBuffer(
							dataBuf + slot * entitySize);
						if (prop.isActive())
							local.emplace_back(id, std::move(prop));
					}
				}
			});

			// Merge thread-local results
			size_t totalCount = 0;
			for (const auto &v : perSeg)
				totalCount += v.size();
			result.reserve(totalCount);
			for (auto &v : perSeg) {
				for (auto &[id, prop] : v)
					result[id] = std::move(prop);
			}
		} else {
			// Sequential path
			result.reserve(ids.size());
			for (const auto &w : work) {
				const auto &seg = segIndex[w.segIdx];
				SegmentHeader header = segmentTracker_->getSegmentHeaderCopy(seg.segmentOffset);
				if (header.used == 0)
					continue;

				size_t dataBytes = static_cast<size_t>(header.used) * entitySize;
				std::vector<char> buf(dataBytes);
				auto dataOffset = static_cast<int64_t>(seg.segmentOffset + sizeof(SegmentHeader));
				ssize_t n = storage::portable_pread(readFd_, buf.data(), dataBytes, dataOffset);
				if (n < static_cast<ssize_t>(dataBytes))
					continue;

				for (size_t i = w.idBegin; i < w.idEnd; ++i) {
					int64_t id = sortedIds[i];
					uint32_t slot = static_cast<uint32_t>(id - header.start_id);
					if (slot >= header.used)
						continue;
					Property prop = Property::deserializeFromBuffer(
						buf.data() + slot * entitySize);
					if (prop.isActive())
						result[id] = std::move(prop);
				}
			}
		}

		return result;
	}

	// --- Edge Operations (delegate to EdgeManager) ---

	void DataManager::addEdge(Edge &edge) const {
		// Phase 1: Validate
		for (const auto &v : validators_) {
			v->validateEdgeInsert(edge, edge.getProperties());
		}

		// Phase 2: Write
		edgeManager_->add(edge);

		if (transactionActive_) {
			txnOps_.push_back({Transaction::TxnOperation::OP_ADD,
							   static_cast<uint8_t>(EntityType::Edge), edge.getId()});
			if (walManager_) {
				walManager_->writeEntityChange(activeTxnId_, static_cast<uint8_t>(EntityType::Edge),
											   static_cast<uint8_t>(EntityChangeType::CHANGE_ADDED), edge.getId(), {});
			}
		}

		notifyEdgeAdded(edge);
	}

	void DataManager::addEdges(std::vector<Edge> &edges) const {
		if (edges.empty())
			return;

		// Phase 1: Validate all edges before any writes (atomicity)
		for (const auto &edge : edges) {
			for (const auto &v : validators_) {
				v->validateEdgeInsert(edge, edge.getProperties());
			}
		}

		// 1. Assign IDs and initial persistence
		edgeManager_->addBatch(edges);

		// 2. Indexing (needs IDs + Props)
		notifyEdgesAdded(edges);

		if (transactionActive_) {
			for (const auto &edge: edges) {
				txnOps_.push_back({Transaction::TxnOperation::OP_ADD,
								   static_cast<uint8_t>(EntityType::Edge), edge.getId()});
			}
		}

		// 3. Property Storage Handling
		for (auto &edge: edges) {
			if (edge.getProperties().empty())
				continue;

			// Links external properties using the ID assigned in Step 1
			propertyManager_->storeProperties(edge);

			if (EntityPropertyTraits<Edge>::hasPropertyEntity(edge)) {
				// Persist the modification (link to external prop)
				edgeManager_->update(edge);
			}
		}
	}

	void DataManager::updateEdge(const Edge &edge) {
		if (transactionActive_) {
			txnOps_.push_back({Transaction::TxnOperation::OP_UPDATE,
							   static_cast<uint8_t>(EntityType::Edge), edge.getId()});
		}

		updateEntityImpl<Edge>(
				edge, [this](int64_t id) { return edgeManager_->get(id); },
				[this](const Edge &e) { edgeManager_->update(e); },
				[this](const Edge &o, const Edge &n) { notifyEdgeUpdated(o, n); });
	}

	void DataManager::deleteEdge(Edge &edge) const {
		if (transactionActive_) {
			txnOps_.push_back({Transaction::TxnOperation::OP_DELETE,
							   static_cast<uint8_t>(EntityType::Edge), edge.getId()});
		}

		edgeManager_->remove(edge);
		notifyEdgeDeleted(edge);
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
		// 1. Snapshot OLD state
		Edge oldEdge = edgeManager_->get(edgeId);
		// Freeze old properties
		auto existingProps = edgeManager_->getProperties(edgeId);
		oldEdge.setProperties(existingProps);

		// 1.5. Validate: build merged new props for constraint checking
		auto mergedProps = existingProps;
		for (const auto &[key, val] : properties) {
			mergedProps[key] = val;
		}
		for (const auto &v : validators_) {
			v->validateEdgeUpdate(oldEdge, existingProps, mergedProps);
		}

		// 2. Perform modification (suppress intermediate notifications)
		suppressNotifications_ = true;
		edgeManager_->addProperties(edgeId, properties);
		suppressNotifications_ = false;

		// 3. Snapshot NEW state
		Edge newEdge = edgeManager_->get(edgeId);
		auto newProps = edgeManager_->getProperties(edgeId);
		newEdge.setProperties(newProps);

		notifyEdgeUpdated(oldEdge, newEdge);
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) const {
		// 1. Snapshot OLD state
		Edge oldEdge = edgeManager_->get(edgeId);
		// Freeze old properties
		auto existingProps = edgeManager_->getProperties(edgeId);
		oldEdge.setProperties(existingProps);

		// 1.5. Validate: build props with key removed
		auto removedProps = existingProps;
		removedProps.erase(key);
		for (const auto &v : validators_) {
			v->validateEdgeUpdate(oldEdge, existingProps, removedProps);
		}

		// 2. Perform removal (suppress intermediate notifications)
		suppressNotifications_ = true;
		edgeManager_->removeProperty(edgeId, key);
		suppressNotifications_ = false;

		// 3. Snapshot NEW state
		Edge newEdge = edgeManager_->get(edgeId);
		auto newProps = edgeManager_->getProperties(edgeId);
		newEdge.setProperties(newProps);

		notifyEdgeUpdated(oldEdge, newEdge);
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

	// --- Property Entity Operations ---

	void DataManager::addPropertyEntity(Property &property) const { propertyManager_->add(property); }

	void DataManager::updatePropertyEntity(const Property &property) const { propertyManager_->update(property); }

	void DataManager::deleteProperty(Property &property) const { propertyManager_->remove(property); }

	Property DataManager::getProperty(int64_t id) const { return propertyManager_->get(id); }

	// --- Blob Operations ---

	void DataManager::addBlobEntity(Blob &blob) const { blobManager_->add(blob); }

	void DataManager::updateBlobEntity(const Blob &blob) const { blobManager_->update(blob); }

	void DataManager::deleteBlob(Blob &blob) const { blobManager_->remove(blob); }

	Blob DataManager::getBlob(int64_t id) const { return blobManager_->get(id); }

	// --- Index Operations ---

	void DataManager::addIndexEntity(Index &index) const {
		indexEntityManager_->add(index);
	}

	void DataManager::updateIndexEntity(const Index &index) const {
		indexEntityManager_->update(index);
	}

	void DataManager::deleteIndex(Index &index) const { indexEntityManager_->remove(index); }

	Index DataManager::getIndex(int64_t id) const { return indexEntityManager_->get(id); }

	// --- State Operations ---

	void DataManager::addStateEntity(State &state) const { stateManager_->add(state); }

	void DataManager::updateStateEntity(const State &state) const { stateManager_->update(state); }

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
		notifyStateUpdated(oldState, newState);
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
		result.reserve(std::min(static_cast<size_t>(endId - startId + 1), limit));

		// This set will keep track of IDs that have already been processed (from memory)
		// to avoid adding them again from the disk-based load.
		std::unordered_set<int64_t> processedIds;

		// --- PASS 1: Populate from Memory (PersistenceManager & Cache) ---
		// This pass ensures data consistency by prioritizing in-memory changes over stale disk data.

		// Get reference to cache via EntityTraits (Adapter)
		auto &cache = EntityTraits<EntityType>::getCache(this);

		for (int64_t currentId = startId; currentId <= endId; ++currentId) {
			if (result.size() >= limit) {
				break;
			}

			// 1. Check PersistenceManager first (Highest Priority)
			// It checks both the Active Layer (new writes) and Flushing Layer (currently saving).
			auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(currentId);

			if (dirtyInfo.has_value()) {
				// Mark this ID as processed so we don't fetch it from cache or disk.
				processedIds.insert(currentId);

				// Only add if it's not marked for deletion.
				if (dirtyInfo->changeType != EntityChangeType::CHANGE_DELETED && dirtyInfo->backup.has_value()) {
					result.push_back(*dirtyInfo->backup);
				}
				continue; // Move to the next ID
			}

			// 2. If not dirty, check the LRU Cache.
			if (cache.contains(currentId)) {
				EntityType entity = cache.peek(currentId); // Use peek to avoid changing LRU order
				if (entity.isActive()) {
					result.push_back(entity);
				}
				// Mark as processed so we don't fetch it from disk.
				processedIds.insert(currentId);
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

			int64_t intersectStart = std::max(startId, segmentStartId);
			int64_t intersectEnd = std::min(endId, segmentEndId);

			if (intersectStart > intersectEnd) {
				continue; // No overlap with this segment.
			}

			// Load a batch of entities from this segment within the intersecting range.
			size_t needed = limit - result.size();
			std::vector<EntityType> segmentEntities =
					loadEntitiesFromSegment<EntityType>(header.file_offset, intersectStart, intersectEnd, needed);

			for (const EntityType &entity: segmentEntities) {
				// IMPORTANT: Only add the entity if it was not already processed from memory.
				if (!processedIds.contains(entity.getId())) {
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

		if (readFd_ >= 0) {
			// Thread-safe path: pread() is atomic and needs no synchronization
			constexpr size_t entitySize = EntityType::getTotalSize();
			char buf[entitySize];
			ssize_t n = storage::portable_pread(readFd_, buf, entitySize, fileOffset);
			if (n < static_cast<ssize_t>(entitySize))
				return std::nullopt;
			membuf mb(buf, entitySize);
			std::istream stream(&mb);
			entity = EntityType::deserialize(stream);
		} else {
			// Legacy path: requires external synchronization
			file_->seekg(fileOffset);
			entity = EntityType::deserialize(*file_);
		}

		if (!entity.isActive()) {
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
		if (relativePosition >= header.used) {
			return std::nullopt; // ID is out of range for this segment
		}

		// Calculate file offset for this entity
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														relativePosition * EntityType::getTotalSize());

		if (readFd_ >= 0) {
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
																 size_t limit, bool filterDeleted) const {
		std::vector<EntityType> result;
		if (segmentOffset == 0 || limit == 0) {
			return result;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

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

		// Calculate starting file offset
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														startOffset * EntityType::getTotalSize());

		// Read entities
		for (uint64_t i = 0; i < count; ++i) {
			if (filterDeleted) {
				auto entityOpt = readEntityFromDisk<EntityType>(entityOffset);
				if (entityOpt.has_value()) {
					result.push_back(entityOpt.value());
				}
			} else {
				if (readFd_ >= 0) {
					constexpr size_t entitySize = EntityType::getTotalSize();
					char buf[entitySize];
					ssize_t n = storage::portable_pread(readFd_, buf, entitySize, entityOffset);
					if (n >= static_cast<ssize_t>(entitySize)) {
						membuf mb(buf, entitySize);
						std::istream stream(&mb);
						result.push_back(EntityType::deserialize(stream));
					}
				} else {
					file_->seekg(entityOffset);
					EntityType entity = EntityType::deserialize(*file_);
					result.push_back(entity);
				}
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

	FlushSnapshot DataManager::prepareFlushSnapshot() const { return persistenceManager_->createSnapshot(); }

	void DataManager::commitFlushSnapshot() const { persistenceManager_->commitSnapshot(); }

	void DataManager::setMaxDirtyEntities(size_t max) const { persistenceManager_->setMaxDirtyEntities(max); }

	void DataManager::setAutoFlushCallback(std::function<void()> cb) const {
		persistenceManager_->setAutoFlushCallback(std::move(cb));
	}

	void DataManager::checkAndTriggerAutoFlush() const {
		if (transactionActive_)
			return; // Suppress auto-flush during active transaction
		persistenceManager_->checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>>
	DataManager::getDirtyEntityInfos(const std::vector<EntityChangeType> &types) {
		// 1. Get all dirty entities from PersistenceManager (Active + Flushing)
		auto allInfos = persistenceManager_->getAllDirtyInfos<EntityType>();

		// 2. Filter by requested types
		// Optimization: If types contains ALL types, return directly
		if (types.size() == 3) { // Assuming ADDED, MODIFIED, DELETED are the only main ones
			return allInfos;
		}

		std::vector<DirtyEntityInfo<EntityType>> result;
		result.reserve(allInfos.size());

		for (const auto &info: allInfos) {
			bool typeMatch = false;
			for (auto type: types) {
				if (info.changeType == type) {
					typeMatch = true;
					break;
				}
			}
			if (typeMatch) {
				result.push_back(info);
			}
		}
		return result;
	}

	template<typename T>
	T make_inactive() {
		T entity;
		entity.markInactive();
		return entity;
	}

	template<typename EntityType>
	EntityType DataManager::getEntityFromMemoryOrDisk(int64_t id) {
		// Check if we're in a read-only transaction with a snapshot
		const auto *snapshot = currentSnapshot_;
		if (snapshot != nullptr) {
			// Read-only transaction: check snapshot for dirty state, then go to disk
			return getEntityWithSnapshot<EntityType>(id, snapshot);
		}

		// 1. Check dirty info via PersistenceManager (write transaction or no-txn context)
		{
			auto dirtyInfo = getDirtyInfo<EntityType>(id);

			if (dirtyInfo.has_value()) {
				// CASE A: Entity is marked as DELETED in memory
				if (dirtyInfo->changeType == EntityChangeType::CHANGE_DELETED) {
					return make_inactive<EntityType>();
				}

				// CASE B: Entity is ADDED or MODIFIED
				if (dirtyInfo->backup.has_value()) {
					return *dirtyInfo->backup;
				}
			}
		}

		// 2. Check Cache (use peek() — shared read lock, no LRU mutation)
		// This avoids exclusive lock contention during parallel scans.
		auto &cache = EntityTraits<EntityType>::getCache(this);
		{
			EntityType entity = cache.peek(id);
			if (entity.getId() != 0) {
				if (!entity.isActive())
					return make_inactive<EntityType>();
				return entity;
			}
		}

		// 3. Load from Disk
		// When pread is available, reads are thread-safe without locking.
		// Otherwise falls back to fstream (must be single-threaded).
		EntityType entity = EntityTraits<EntityType>::loadFromDisk(this, id);
		if (entity.getId() != 0 && entity.isActive()) {
			// Use tryPut to avoid blocking on cache lock during parallel scans.
			// If another thread holds the lock, we skip caching — the entity
			// will be re-read from disk next time (still fast via pread).
			cache.tryPut(id, entity);
			return entity;
		}

		return make_inactive<EntityType>();
	}

	// Helper: get the snapshot map for a given entity type
	namespace {
		template<typename EntityType>
		const std::unordered_map<int64_t, DirtyEntityInfo<EntityType>> &
		getSnapshotMap(const CommittedSnapshot &snapshot);

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Node>> &
		getSnapshotMap<Node>(const CommittedSnapshot &snapshot) { return snapshot.nodes; }

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &
		getSnapshotMap<Edge>(const CommittedSnapshot &snapshot) { return snapshot.edges; }

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Property>> &
		getSnapshotMap<Property>(const CommittedSnapshot &snapshot) { return snapshot.properties; }

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &
		getSnapshotMap<Blob>(const CommittedSnapshot &snapshot) { return snapshot.blobs; }

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<Index>> &
		getSnapshotMap<Index>(const CommittedSnapshot &snapshot) { return snapshot.indexes; }

		template<>
		const std::unordered_map<int64_t, DirtyEntityInfo<State>> &
		getSnapshotMap<State>(const CommittedSnapshot &snapshot) { return snapshot.states; }
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

		// 2. Check Cache (use peek() — shared read lock, no LRU mutation)
		auto &cache = EntityTraits<EntityType>::getCache(this);
		{
			EntityType entity = cache.peek(id);
			if (entity.getId() != 0) {
				if (!entity.isActive())
					return make_inactive<EntityType>();
				return entity;
			}
		}

		// 3. Load from Disk via pread (thread-safe)
		EntityType entity = EntityTraits<EntityType>::loadFromDisk(this, id);
		if (entity.getId() != 0 && entity.isActive()) {
			cache.tryPut(id, entity);
			return entity;
		}

		return make_inactive<EntityType>();
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

	template<typename EntityType>
	std::vector<EntityType> DataManager::bulkLoadEntities(int64_t filterStartId, int64_t filterEndId) const {
		if (readFd_ < 0)
			return {}; // Requires pread support

		// Get sorted segment list for this entity type
		const auto &segIndex = EntityTraits<EntityType>::getSegmentIndex(this);
		constexpr size_t entitySize = EntityType::getTotalSize();

		std::vector<EntityType> result;
		result.reserve(segIndex.size() * 4); // Rough estimate

		for (const auto &seg : segIndex) {
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
			ssize_t n = storage::portable_pread(readFd_, buf.data(), dataBytes, dataOffset);
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
		auto nodeOpt = findAndReadEntity<Node>(id);
		if (nodeOpt.has_value())
			return *nodeOpt;
		return make_inactive<Node>();
	}

	Edge DataManager::loadEdgeFromDisk(const int64_t id) const {
		auto edgeOpt = findAndReadEntity<Edge>(id);
		if (edgeOpt.has_value())
			return *edgeOpt;
		return make_inactive<Edge>();
	}

	Property DataManager::loadPropertyFromDisk(const int64_t id) const {
		auto propertyOpt = findAndReadEntity<Property>(id);
		if (propertyOpt.has_value())
			return *propertyOpt;
		return make_inactive<Property>();
	}

	Blob DataManager::loadBlobFromDisk(const int64_t id) const {
		auto blobOpt = findAndReadEntity<Blob>(id);
		if (blobOpt.has_value())
			return *blobOpt;
		return make_inactive<Blob>();
	}

	Index DataManager::loadIndexFromDisk(const int64_t id) const {
		auto indexOpt = findAndReadEntity<Index>(id);
		if (indexOpt.has_value())
			return *indexOpt;
		return make_inactive<Index>();
	}

	State DataManager::loadStateFromDisk(const int64_t id) const {
		auto stateOpt = findAndReadEntity<State>(id);
		if (stateOpt.has_value())
			return *stateOpt;
		return make_inactive<State>();
	}

	// --- Transaction Rollback ---

	void DataManager::rollbackActiveTransaction() {
		// 1. Fire reverse observer notifications to revert index state
		for (auto it = txnOps_.rbegin(); it != txnOps_.rend(); ++it) {
			auto entityType = static_cast<EntityType>(it->entityType);

			switch (it->opType) {
				case Transaction::TxnOperation::OP_ADD: {
					// Undo ADD: notify observers as if entity was deleted
					if (entityType == EntityType::Node) {
						try {
							Node node = nodeManager_->get(it->entityId);
							notifyNodeDeleted(node);
						} catch (...) {
						}
					} else if (entityType == EntityType::Edge) {
						try {
							Edge edge = edgeManager_->get(it->entityId);
							notifyEdgeDeleted(edge);
						} catch (...) {
						}
					}
					break;
				}
				case Transaction::TxnOperation::OP_DELETE: {
					// Undo DELETE: we can't easily restore deleted entities without snapshots.
					// The dirty registry clearing will handle removing the delete markers.
					break;
				}
				case Transaction::TxnOperation::OP_UPDATE: {
					// Undo UPDATE: similar to delete, handled by dirty registry clearing
					break;
				}
			}
		}

		// 2. Clear all dirty registries (revert all entity state changes)
		persistenceManager_->clearAll();

		// 3. Clear all caches (force reload from disk on next access)
		clearCache();
	}

	// --- Cache and Transaction Management ---

	void DataManager::clearCache() const {
		nodeCache_.clear();
		edgeCache_.clear();
		propertyCache_.clear();
		blobCache_.clear();
		indexCache_.clear();
		stateCache_.clear();
	}

	template<typename EntityType>
	void DataManager::markEntityDeleted(EntityType &entity) {
		auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(entity.getId());

		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED) {
			// Revert ADD by overwriting with explicit DELETED (or just removing, but DELETED is safer for log)
			// Actually, if it was just added in memory and never saved, we can technically ignore it,
			// BUT to be safe in the registry logic, we mark it DELETED.
			// Better yet, update registry to remove it?
			// Simpler: Mark DELETED.
			// persistenceManager_->upsert(DirtyEntityInfo<EntityType>(EntityChangeType::CHANGE_DELETED, entity));

			// Remove from persistence manager completely (Undoes the ADD).
			// This prevents an unnecessary DELETE record from being written to the WAL/Disk.
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
	template std::vector<Node> DataManager::readEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t,
																		  bool) const;
	template std::vector<DirtyEntityInfo<Node>>
	DataManager::getDirtyEntityInfos<Node>(const std::vector<EntityChangeType> &);
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
	template std::vector<Edge> DataManager::readEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t,
																		  bool) const;
	template std::vector<DirtyEntityInfo<Edge>>
	DataManager::getDirtyEntityInfos<Edge>(const std::vector<EntityChangeType> &);
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
	template void DataManager::setEntityDirty<Property>(const DirtyEntityInfo<Property> &);

	// Blob-specific instantiations
	template Blob DataManager::getEntityFromMemoryOrDisk<Blob>(int64_t id);
	template std::optional<DirtyEntityInfo<Blob>> DataManager::getDirtyInfo<Blob>(int64_t);
	template std::vector<Blob> DataManager::getEntitiesInRange<Blob>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Blob>(int64_t id) const;
	template void DataManager::markEntityDeleted<Blob>(Blob &entity);
	template std::vector<DirtyEntityInfo<Blob>>
	DataManager::getDirtyEntityInfos<Blob>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Blob>(const DirtyEntityInfo<Blob> &);

	// Index-specific instantiations
	template Index DataManager::getEntityFromMemoryOrDisk<Index>(int64_t id);
	template std::optional<DirtyEntityInfo<Index>> DataManager::getDirtyInfo<Index>(int64_t);
	template std::vector<Index> DataManager::getEntitiesInRange<Index>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Index>(int64_t id) const;
	template void DataManager::markEntityDeleted<Index>(Index &entity);
	template std::vector<DirtyEntityInfo<Index>>
	DataManager::getDirtyEntityInfos<Index>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Index>(const DirtyEntityInfo<Index> &);

	// State-specific instantiations
	template State DataManager::getEntityFromMemoryOrDisk<State>(int64_t id);
	template std::optional<DirtyEntityInfo<State>> DataManager::getDirtyInfo<State>(int64_t);
	template std::vector<State> DataManager::getEntitiesInRange<State>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<State>(int64_t id) const;
	template void DataManager::markEntityDeleted<State>(State &entity);
	template std::vector<DirtyEntityInfo<State>>
	DataManager::getDirtyEntityInfos<State>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<State>(const DirtyEntityInfo<State> &);
} // namespace graph::storage
