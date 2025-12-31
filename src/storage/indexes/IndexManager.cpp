/**
 * @file IndexManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/log/Log.hpp"
#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/storage/indexes/IndexMeta.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace graph::query::indexes {

	IndexManager::IndexManager(std::shared_ptr<storage::FileStorage> storage) :
		storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {

		// Instantiate the Node index manager without the redundant config keys.
		// The existence of the index data itself determines if it's "enabled".
		nodeIndexManager_ = std::make_shared<EntityTypeIndexManager>(
				dataManager_, storage_->getSystemStateManager(), IndexTypes::NODE_LABEL_TYPE,
				storage::state::keys::Node::LABEL_ROOT, IndexTypes::NODE_PROPERTY_TYPE,
				storage::state::keys::Node::PROPERTY_PREFIX);

		// Instantiate the Edge index manager similarly.
		edgeIndexManager_ = std::make_shared<EntityTypeIndexManager>(
				dataManager_, storage_->getSystemStateManager(), IndexTypes::EDGE_LABEL_TYPE,
				storage::state::keys::Edge::LABEL_ROOT, IndexTypes::EDGE_PROPERTY_TYPE,
				storage::state::keys::Edge::PROPERTY_PREFIX);
	}

	IndexManager::~IndexManager() = default;

	void IndexManager::initialize() {
        // 1. Initialize the builder
        indexBuilder_ = std::make_unique<IndexBuilder>(shared_from_this(), storage_);

        // 2. Register listeners
        storage_->registerEventListener(weak_from_this());
        dataManager_->registerObserver(shared_from_this());

        // ====================================================================
        // Bootstrap: Auto-build Label Indexes IF enabled in config
        // ====================================================================

        auto sysState = storage_->getSystemStateManager();

        auto ensureMetadata = [&](const std::string& name, const std::string& type) {
            auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);
            if (!allIndexes.contains(name)) {
                IndexMetadata meta{name, type, "label", "", ""};
                sysState->set(storage::state::keys::SYS_INDEXES, name, meta.toString());
                log::Log::info("Registered system index metadata: {}", name);
            }
        };

        // --- Node Label Index ---
        auto nodeLabelIdx = nodeIndexManager_->getLabelIndex();

        // [CHECK] Only proceed if enabled (reads from config)
        if (nodeLabelIdx->isEnabled()) {
            // Build if missing data
            if (!nodeLabelIdx->hasPhysicalData()) {
                if (dataManager_->getIdAllocator()->getCurrentMaxNodeId() > 0) {
                    log::Log::info("Bootstrapping Node Label Index...");
                    executeBuildTask([&]() { return indexBuilder_->buildNodeLabelIndex(); });
                    nodeLabelIdx->saveState();
                }
            }
            // Ensure metadata exists for SHOW INDEXES
            ensureMetadata("node_label_idx", "node");
        }

        // --- Edge Label Index ---
        auto edgeLabelIdx = edgeIndexManager_->getLabelIndex();

        // [CHECK] Only proceed if enabled
        if (edgeLabelIdx->isEnabled()) {
            if (!edgeLabelIdx->hasPhysicalData()) {
                if (dataManager_->getIdAllocator()->getCurrentMaxEdgeId() > 0) {
                    log::Log::info("Bootstrapping Edge Label Index...");
                    executeBuildTask([&]() { return indexBuilder_->buildEdgeLabelIndex(); });
                    edgeLabelIdx->saveState();
                }
            }
            ensureMetadata("edge_label_idx", "edge");
        }
    }

	void IndexManager::onStorageFlush() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		// When FileStorage flushes, we must persist our index states.
		persistState();
	}

	bool IndexManager::hasLabelIndex(const std::string &entityType) const {
		if (entityType == "node")
			return nodeIndexManager_->hasLabelIndex();
		if (entityType == "edge")
			return edgeIndexManager_->hasLabelIndex();
		return false;
	}

	bool IndexManager::hasPropertyIndex(const std::string &entityType, const std::string &key) const {
		if (entityType == "node")
			return nodeIndexManager_->hasPropertyIndex(key);
		if (entityType == "edge")
			return edgeIndexManager_->hasPropertyIndex(key);
		return false;
	}

	bool IndexManager::executeBuildTask(const std::function<bool()> &buildFunc) const {
		storage_->flush();
		return buildFunc();
	}

	bool IndexManager::createIndex(const std::string &indexName, const std::string &entityType,
								   const std::string &label, const std::string &property) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// 1. Generate Name if empty
		// Format: index_node_User_name or index_node_User_LABEL
		std::string name = indexName;
		if (name.empty()) {
			name = "index_" + entityType + "_" + label + "_" + (property.empty() ? "LABEL" : property);
		}

		// 2. Check Metadata (Prevent duplicate names)
		// We use SystemStateManager to load the "sys.indexes" map to check existence.
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		if (allIndexes.contains(name)) {
			// Index name already exists, return false to indicate failure/no-op
			return false;
		}

		// 3. Determine Index Type & Execute Physical Build
		bool success = false;
		std::string indexType;

		if (property.empty()) {
			// --- Label Index ---
			indexType = "label";
			if (entityType == "node") {
				success = nodeIndexManager_->createLabelIndex(
						[&]() { return executeBuildTask([&]() { return indexBuilder_->buildNodeLabelIndex(); }); });
			} else if (entityType == "edge") {
				// Edge label index
				success = edgeIndexManager_->createLabelIndex(
						[&]() { return executeBuildTask([&]() { return indexBuilder_->buildEdgeLabelIndex(); }); });
			}
		} else {
			// --- Property Index ---
			indexType = "property";
			if (entityType == "node") {
				// Create property index for nodes
				// Note: We use the new createPropertyIndex method which handles 'hasKeyIndexed' check internally.
				success = nodeIndexManager_->createPropertyIndex(property, [&]() {
					return executeBuildTask([&]() { return indexBuilder_->buildNodePropertyIndex(property); });
				});
			} else if (entityType == "edge") {
				// Create property index for edges
				success = edgeIndexManager_->createPropertyIndex(property, [&]() {
					return executeBuildTask([&]() { return indexBuilder_->buildEdgePropertyIndex(property); });
				});
			}
		}

		// 4. Persist Metadata if physical build succeeded
		// We store the definition string so we can look it up later by name or definition.
		if (success) {
			IndexMetadata meta{name, entityType, indexType, label, property};

			// Use 'set' (which performs a Merge in SystemStateManager) to save this new index metadata
			sysState->set(storage::state::keys::SYS_INDEXES, name, meta.toString());
		}

		return success;
	}

	// ------------------------------------------------------------------------
	// Drop Index By Name
	// ------------------------------------------------------------------------
	bool IndexManager::dropIndexByName(const std::string &indexName) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		// 1. Lookup Metadata
		auto it = allIndexes.find(indexName);
		if (it == allIndexes.end()) {
			return false; // Index not found
		}

		IndexMetadata meta = IndexMetadata::fromString(indexName, it->second);

		// 2. Drop Physical Index
		bool physicalDropSuccess = false;
		if (meta.entityType == "node") {
			physicalDropSuccess = nodeIndexManager_->dropIndex(meta.indexType, meta.property);
		} else {
			physicalDropSuccess = edgeIndexManager_->dropIndex(meta.indexType, meta.property);
		}

		// 3. Remove Metadata
		// We must remove this key from the map and save it back
		if (physicalDropSuccess) {
			allIndexes.erase(it);
			// Replace the whole map to persist deletion
			sysState->setMap(storage::state::keys::SYS_INDEXES, allIndexes, storage::state::UpdateMode::REPLACE);
		}

		return physicalDropSuccess;
	}

	bool IndexManager::dropIndexByDefinition(const std::string &label, const std::string &property) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		// Iterate metadata to find the matching definition
		for (const auto &[name, rawMeta]: allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);

			if (meta.label == label && meta.property == property) {
				return dropIndexByName(name);
			}
		}

		return false; // Not found
	}

	// ------------------------------------------------------------------------
	// List Indexes (Returning Metadata)
	// ------------------------------------------------------------------------
	std::vector<std::tuple<std::string, std::string, std::string, std::string>>
	IndexManager::listIndexesDetailed() const {
		// Returns: {Name, Type, Label, Property}
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		std::vector<std::tuple<std::string, std::string, std::string, std::string>> result;
		for (const auto &[name, rawMeta]: allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			result.emplace_back(meta.name, meta.entityType, meta.label, meta.property);
		}
		return result;
	}

	void IndexManager::persistState() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		nodeIndexManager_->persistState();
		edgeIndexManager_->persistState();
	}

	// --- Event Handlers simply delegate to the appropriate manager ---
	void IndexManager::onNodeAdded(const Node &node) { nodeIndexManager_->onEntityAdded(node); }

	void IndexManager::onNodesAdded(const std::vector<Node>& nodes) {
		nodeIndexManager_->onEntitiesAdded(nodes);
	}

	void IndexManager::onNodeUpdated(const Node &oldNode, const Node &newNode) {
		nodeIndexManager_->onEntityUpdated(oldNode, newNode);
	}

	void IndexManager::onNodeDeleted(const Node &node) { nodeIndexManager_->onEntityDeleted(node); }

	void IndexManager::onEdgeAdded(const Edge &edge) { edgeIndexManager_->onEntityAdded(edge); }

	void IndexManager::onEdgesAdded(const std::vector<Edge>& edges) {
		edgeIndexManager_->onEntitiesAdded(edges);
	}

	void IndexManager::onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) {
		edgeIndexManager_->onEntityUpdated(oldEdge, newEdge);
	}

	void IndexManager::onEdgeDeleted(const Edge &edge) { edgeIndexManager_->onEntityDeleted(edge); }

	// --- Query methods delegate to the correct index ---
	std::vector<int64_t> IndexManager::findNodeIdsByLabel(const std::string &label) const {
		log::Log::debug("IndexManager::findNodeIdsByLabel - label: {}", label);
		return nodeIndexManager_->getLabelIndex()->findNodes(label);
	}

	std::vector<int64_t> IndexManager::findNodeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		log::Log::debug("IndexManager::findNodeIdsByProperty - key: {}", key);
		return nodeIndexManager_->getPropertyIndex()->findExactMatch(key, value);
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByLabel(const std::string &label) const {
		log::Log::debug("IndexManager::findEdgeIdsByLabel - label: {}", label);
		return edgeIndexManager_->getLabelIndex()->findNodes(label);
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		log::Log::debug("IndexManager::findEdgeIdsByProperty - key: {}", key);
		return edgeIndexManager_->getPropertyIndex()->findExactMatch(key, value);
	}

} // namespace graph::query::indexes
