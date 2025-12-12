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

namespace graph::query::indexes {

	IndexManager::IndexManager(std::shared_ptr<storage::FileStorage> storage) :
		storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {

		// Instantiate the Node index manager without the redundant config keys.
		// The existence of the index data itself determines if it's "enabled".
		nodeIndexManager_ = std::make_shared<EntityTypeIndexManager>(
				dataManager_, IndexTypes::NODE_LABEL_TYPE, StateKeys::NODE_LABEL_ROOT, IndexTypes::NODE_PROPERTY_TYPE,
				StateKeys::NODE_PROPERTY_PREFIX);

		// Instantiate the Edge index manager similarly.
		edgeIndexManager_ = std::make_shared<EntityTypeIndexManager>(
				dataManager_, IndexTypes::EDGE_LABEL_TYPE, StateKeys::EDGE_LABEL_ROOT, IndexTypes::EDGE_PROPERTY_TYPE,
				StateKeys::EDGE_PROPERTY_PREFIX);
	}

	IndexManager::~IndexManager() = default;

	void IndexManager::initialize() {
		// 1. Initialize the builder
		indexBuilder_ = std::make_unique<IndexBuilder>(shared_from_this(), storage_);

		// 2. Register self as listener
		// This connects IndexManager to FileStorage without FileStorage knowing about "Indexes"
		storage_->registerEventListener(weak_from_this());
	}

	void IndexManager::onStorageFlush() {
		// When FileStorage flushes, we must persist our index states.
		persistState();
	}

	bool IndexManager::hasLabelIndex(const std::string& entityType) const {
		if (entityType == "node") return nodeIndexManager_->hasLabelIndex();
		if (entityType == "edge") return edgeIndexManager_->hasLabelIndex();
		return false;
	}

	bool IndexManager::hasPropertyIndex(const std::string& entityType, const std::string& key) const {
		if (entityType == "node") return nodeIndexManager_->hasPropertyIndex(key);
		if (entityType == "edge") return edgeIndexManager_->hasPropertyIndex(key);
		return false;
	}

	bool IndexManager::executeBuildTask(const std::function<bool()> &buildFunc) const {
		storage_->flush();
		return buildFunc();
	}

	bool IndexManager::buildIndexes(const std::string &entityType) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (entityType == "node") {
			return nodeIndexManager_->buildAllIndexes(
					[&]() { return executeBuildTask([&]() { return indexBuilder_->buildAllNodeIndexes(); }); });
		}
		if (entityType == "edge") {
			return edgeIndexManager_->buildAllIndexes(
					[&]() { return executeBuildTask([&]() { return indexBuilder_->buildAllEdgeIndexes(); }); });
		}
		return false;
	}

	bool IndexManager::buildPropertyIndex(const std::string &entityType, const std::string &key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (entityType == "node") {
			return nodeIndexManager_->buildPropertyIndex(key, [&]() {
				return executeBuildTask([&]() { return indexBuilder_->buildNodePropertyIndex(key); });
			});
		}
		if (entityType == "edge") {
			return edgeIndexManager_->buildPropertyIndex(key, [&]() {
				return executeBuildTask([&]() { return indexBuilder_->buildEdgePropertyIndex(key); });
			});
		}
		return false;
	}

	bool IndexManager::dropIndex(const std::string &entityType, const std::string &indexType, const std::string &key) {
		if (entityType == "node") {
			return nodeIndexManager_->dropIndex(indexType, key);
		}
		if (entityType == "edge") {
			return edgeIndexManager_->dropIndex(indexType, key);
		}
		return false;
	}

	std::vector<std::pair<std::string, std::string>> IndexManager::listIndexes(const std::string &entityType) const {
		if (entityType == "node") {
			return nodeIndexManager_->listIndexes();
		}
		if (entityType == "edge") {
			return edgeIndexManager_->listIndexes();
		}
		return {};
	}

	void IndexManager::persistState() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		nodeIndexManager_->persistState();
		edgeIndexManager_->persistState();
	}

	// --- Event Handlers simply delegate to the appropriate manager ---
	void IndexManager::onNodeAdded(const Node &node) { nodeIndexManager_->onEntityAdded(node); }

	void IndexManager::onNodeUpdated(const Node &oldNode, const Node &newNode) {
		nodeIndexManager_->onEntityUpdated(oldNode, newNode);
	}

	void IndexManager::onNodeDeleted(const Node &node) { nodeIndexManager_->onEntityDeleted(node); }

	void IndexManager::onEdgeAdded(const Edge &edge) { edgeIndexManager_->onEntityAdded(edge); }

	void IndexManager::onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) {
		edgeIndexManager_->onEntityUpdated(oldEdge, newEdge);
	}

	void IndexManager::onEdgeDeleted(const Edge &edge) { edgeIndexManager_->onEntityDeleted(edge); }

	// --- Query methods delegate to the correct index ---
	std::vector<int64_t> IndexManager::findNodeIdsByLabel(const std::string &label) const {
		return nodeIndexManager_->getLabelIndex()->findNodes(label);
	}

	std::vector<int64_t> IndexManager::findNodeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		log::Log::debug("IndexManager::findNodeIdsByProperty - key: {}", key);
		return nodeIndexManager_->getPropertyIndex()->findExactMatch(key, value);
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByLabel(const std::string &label) const {
		return edgeIndexManager_->getLabelIndex()->findNodes(label);
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		return edgeIndexManager_->getPropertyIndex()->findExactMatch(key, value);
	}

} // namespace graph::query::indexes
