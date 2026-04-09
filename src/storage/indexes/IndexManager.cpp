/**
 * @file IndexManager.cpp
 * @author Nexepic
 * @date 2025/3/21
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

#include "graph/storage/indexes/IndexManager.hpp"
#include <sstream>
#include "graph/log/Log.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/storage/indexes/IndexMeta.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
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
				dataManager_, storage_->getSystemStateManager(), IndexTypes::EDGE_TYPE_INDEX,
				storage::state::keys::Edge::LABEL_ROOT, IndexTypes::EDGE_PROPERTY_TYPE,
				storage::state::keys::Edge::PROPERTY_PREFIX);

		vectorIndexManager_ = std::make_shared<VectorIndexManager>(dataManager_, storage_->getSystemStateManager());
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

		auto ensureMetadata = [&](const std::string &name, const std::string &type) {
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

		// --- Edge Type Index ---
		auto edgeTypeIdx = edgeIndexManager_->getLabelIndex();

		// [CHECK] Only proceed if enabled
		if (edgeTypeIdx->isEnabled()) {
			if (!edgeTypeIdx->hasPhysicalData()) {
				if (dataManager_->getIdAllocator()->getCurrentMaxEdgeId() > 0) {
					log::Log::info("Bootstrapping Edge Type Index...");
					executeBuildTask([&]() { return indexBuilder_->buildEdgeTypeIndex(); });
					edgeTypeIdx->saveState();
				}
			}
			ensureMetadata("edge_type_idx", "edge");
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
				// Edge type index
				success = edgeIndexManager_->createLabelIndex(
						[&]() { return executeBuildTask([&]() { return indexBuilder_->buildEdgeTypeIndex(); }); });
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
			result.emplace_back(meta.name, meta.indexType, meta.label, meta.property);
		}
		return result;
	}

	bool IndexManager::createVectorIndex(const std::string &indexName, const std::string &label,
										 const std::string &property, int dimension, const std::string &metric) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// 1. Generate default name if empty
		std::string name = indexName;
		if (name.empty()) {
			name = "vec_" + label + "_" + property;
		}

		// 2. Check Metadata (Prevent duplicate names)
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		if (allIndexes.contains(name)) {
			log::Log::warn("Vector index '{}' already exists.", name);
			return false;
		}

		// 3. Initialize Physical Index via VectorIndexManager
		// This ensures the configuration is saved to disk
		vectorIndexManager_->createIndex(name, dimension, metric);

		// 4. Persist Metadata
		// We set indexType="vector" to distinguish from B-Tree indexes
		IndexMetadata meta{name, "node", "vector", label, property};
		sysState->set(storage::state::keys::SYS_INDEXES, name, meta.toString());

		log::Log::info("Created vector index: {} (Label: {}, Prop: {}, Dim: {})", name, label, property, dimension);
		return true;
	}

	std::string IndexManager::getVectorIndexName(const std::string &label, const std::string &property) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		for (const auto &[name, rawMeta]: allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			if (meta.indexType == "vector" && meta.label == label && meta.property == property) {
				return name;
			}
		}
		return "";
	}

	void IndexManager::persistState() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		nodeIndexManager_->persistState();
		edgeIndexManager_->persistState();
	}

	// --- Event Handlers simply delegate to the appropriate manager ---
	void IndexManager::onNodeAdded(const Node &node) {
		nodeIndexManager_->onEntityAdded(node);

		// Update composite indexes
		updateCompositeIndexForNode(node);

		if (vectorIndexManager_) { // Safety check
			std::string labelStr;
			if (node.getLabelId() != 0) {
				labelStr = dataManager_->resolveTokenName(node.getLabelId());
			}
			// Pass the node properties directly.
			// DataManager has already resolved inline properties into the Node object passed here.
			vectorIndexManager_->updateIndex(node, labelStr, node.getProperties());
		}
	}

	void IndexManager::onNodesAdded(const std::vector<Node> &nodes) {
		// 1. Standard Indexes
		nodeIndexManager_->onEntitiesAdded(nodes);

		// 2. Vector Indexes (Batch) — use batch API for graph construction efficiency
		if (vectorIndexManager_ && !nodes.empty()) {
			std::unordered_map<std::string,
							   std::vector<std::pair<Node, std::unordered_map<std::string, PropertyValue>>>>
					byLabel;

			for (const auto &node: nodes) {
				if (node.getProperties().empty())
					continue;
				std::string labelStr;
				if (node.getLabelId() != 0)
					labelStr = dataManager_->resolveTokenName(node.getLabelId());
				if (!labelStr.empty())
					byLabel[labelStr].push_back({node, node.getProperties()});
			}

			for (auto &[label, batch]: byLabel)
				vectorIndexManager_->updateIndexBatch(batch, label);
		}
	}

	void IndexManager::onNodeUpdated(const Node &oldNode, const Node &newNode) {
		nodeIndexManager_->onEntityUpdated(oldNode, newNode);

		// Update composite indexes
		removeCompositeIndexForNode(oldNode);
		updateCompositeIndexForNode(newNode);

		// Update Vector Index
		if (vectorIndexManager_) {
			std::string labelStr;
			if (newNode.getLabelId() != 0) {
				labelStr = dataManager_->resolveTokenName(newNode.getLabelId());
			}
			vectorIndexManager_->updateIndex(newNode, labelStr, newNode.getProperties());
		}
	}

	void IndexManager::onNodeDeleted(const Node &node) {
		// 1. Update Standard Indexes
		nodeIndexManager_->onEntityDeleted(node);

		// 2. Update composite indexes
		removeCompositeIndexForNode(node);

		// 3. Update Vector Indexes (Removal)
		if (vectorIndexManager_) {
			std::string labelStr;
			if (node.getLabelId() != 0) {
				labelStr = dataManager_->resolveTokenName(node.getLabelId());
			}

			// We only need NodeID and Label to find the index and remove the mapping
			vectorIndexManager_->removeIndex(node.getId(), labelStr);
		}
	}

	void IndexManager::onEdgeAdded(const Edge &edge) { edgeIndexManager_->onEntityAdded(edge); }

	void IndexManager::onEdgesAdded(const std::vector<Edge> &edges) { edgeIndexManager_->onEntitiesAdded(edges); }

	void IndexManager::onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) {
		edgeIndexManager_->onEntityUpdated(oldEdge, newEdge);
	}

	void IndexManager::onEdgeDeleted(const Edge &edge) { edgeIndexManager_->onEntityDeleted(edge); }

	// --- Query methods delegate to the correct index ---
	std::vector<int64_t> IndexManager::findNodeIdsByLabel(const std::string &label) const {
		log::Log::debug("IndexManager::findNodeIdsByLabel - label: {}", label);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getLabelIndex()->findNodes(label);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	std::vector<int64_t> IndexManager::findNodeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		log::Log::debug("IndexManager::findNodeIdsByProperty - key: {}", key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getPropertyIndex()->findExactMatch(key, value);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	std::vector<int64_t> IndexManager::findNodeIdsByPropertyRange(const std::string &key,
	                                                               const PropertyValue &minValue,
	                                                               const PropertyValue &maxValue) const {
		log::Log::debug("IndexManager::findNodeIdsByPropertyRange - key: {}", key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getPropertyIndex()->findRange(key, minValue, maxValue);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	// --- Composite Index API ---

	bool IndexManager::createCompositeIndex(const std::string &indexName, const std::string &entityType,
	                                         const std::string &label, const std::vector<std::string> &properties) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Generate name if empty
		std::string name = indexName;
		if (name.empty()) {
			name = "index_" + entityType + "_" + label + "_composite_";
			for (size_t i = 0; i < properties.size(); ++i) {
				if (i > 0) name += "_";
				name += properties[i];
			}
		}

		// Check duplicate
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);
		if (allIndexes.contains(name))
			return false;

		// Create physical composite index
		if (entityType == "node") {
			nodeIndexManager_->getPropertyIndex()->createCompositeIndex(properties);
		} else {
			return false; // Composite index only supported for nodes
		}

		// Persist metadata
		std::string propStr;
		for (size_t i = 0; i < properties.size(); ++i) {
			if (i > 0) propStr += ",";
			propStr += properties[i];
		}
		IndexMetadata meta{name, entityType, "composite", label, propStr};
		sysState->set(storage::state::keys::SYS_INDEXES, name, meta.toString());

		log::Log::info("Created composite index: {} (Label: {}, Props: {})", name, label, propStr);
		return true;
	}

	bool IndexManager::hasCompositeIndex(const std::string &entityType,
	                                      const std::vector<std::string> &keys) const {
		if (entityType == "node")
			return nodeIndexManager_->getPropertyIndex()->hasCompositeIndex(keys);
		return false;
	}

	std::vector<int64_t> IndexManager::findNodeIdsByCompositeIndex(
	    const std::vector<std::string> &keys,
	    const std::vector<PropertyValue> &values) const {
		log::Log::debug("IndexManager::findNodeIdsByCompositeIndex");
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getPropertyIndex()->findCompositeExact(keys, values);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByType(const std::string &type) const {
		log::Log::debug("IndexManager::findEdgeIdsByType - type: {}", type);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = edgeIndexManager_->getLabelIndex()->findNodes(type);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		log::Log::debug("IndexManager::findEdgeIdsByProperty - key: {}", key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = edgeIndexManager_->getPropertyIndex()->findExactMatch(key, value);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	// --- Composite Index Maintenance Helpers ---

	void IndexManager::updateCompositeIndexForNode(const Node &node) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex) return;

		const auto &props = node.getProperties();
		if (props.empty()) return;

		// For each composite index definition, check if this node has all required properties
		// Access composite definitions through PropertyIndex's public API
		// We iterate through all known composite indexes (stored in metadata)
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		for (const auto &[name, rawMeta] : allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			if (meta.indexType != "composite") continue;

			// Parse properties from comma-separated string
			std::vector<std::string> keys;
			std::stringstream ss(meta.property);
			std::string segment;
			while (std::getline(ss, segment, ',')) {
				keys.push_back(segment);
			}

			// Check if node has all required properties
			std::vector<PropertyValue> values;
			bool allPresent = true;
			for (const auto &key : keys) {
				auto it = props.find(key);
				if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
					allPresent = false;
					break;
				}
				values.push_back(it->second);
			}

			if (allPresent) {
				propIndex->addCompositeEntry(node.getId(), keys, values);
			}
		}
	}

	void IndexManager::removeCompositeIndexForNode(const Node &node) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex) return;

		const auto &props = node.getProperties();
		if (props.empty()) return;

		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		for (const auto &[name, rawMeta] : allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			if (meta.indexType != "composite") continue;

			std::vector<std::string> keys;
			std::stringstream ss(meta.property);
			std::string segment;
			while (std::getline(ss, segment, ',')) {
				keys.push_back(segment);
			}

			std::vector<PropertyValue> values;
			bool allPresent = true;
			for (const auto &key : keys) {
				auto it = props.find(key);
				if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
					allPresent = false;
					break;
				}
				values.push_back(it->second);
			}

			if (allPresent) {
				propIndex->removeCompositeEntry(node.getId(), keys, values);
			}
		}
	}

} // namespace graph::query::indexes
