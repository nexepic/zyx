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
#include <algorithm>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include "graph/log/Log.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/storage/indexes/IndexMeta.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include "graph/storage/indexes/ScopedNodePropertyKey.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace graph::query::indexes {
namespace {
	template<typename T>
	bool unorderedValuesEqual(std::vector<T> lhs, std::vector<T> rhs) {
		if (lhs.size() != rhs.size()) {
			return false;
		}
		std::sort(lhs.begin(), lhs.end());
		std::sort(rhs.begin(), rhs.end());
		return lhs == rhs;
	}

	bool nodeIndexRelevantFieldsChanged(const Node &oldNode, const Node &newNode) {
		return oldNode.getId() != newNode.getId() ||
			   oldNode.isActive() != newNode.isActive() ||
			   !unorderedValuesEqual(oldNode.getLabelIds(), newNode.getLabelIds()) ||
			   oldNode.getProperties() != newNode.getProperties();
	}

	bool edgeIndexRelevantFieldsChanged(const Edge &oldEdge, const Edge &newEdge) {
		return oldEdge.getId() != newEdge.getId() ||
			   oldEdge.isActive() != newEdge.isActive() ||
			   oldEdge.getTypeId() != newEdge.getTypeId() ||
			   oldEdge.getProperties() != newEdge.getProperties();
	}

	std::unordered_map<std::string, const storage::BulkPropertyColumn *>
	mapColumnsByKey(const std::vector<storage::BulkPropertyColumn> &columns) {
		std::unordered_map<std::string, const storage::BulkPropertyColumn *> byKey;
		byKey.reserve(columns.size());
		for (const auto &column: columns) {
			byKey.emplace(column.key, &column);
		}
		return byKey;
	}

	std::vector<Node> materializeNodesForVectorIndexes(
			const std::vector<Node> &nodes,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		if (nodes.empty() || columns.empty()) {
			return nodes;
		}
		std::vector<Node> materialized = nodes;
		for (size_t row = 0; row < materialized.size(); ++row) {
			std::unordered_map<std::string, PropertyValue> props;
			props.reserve(columns.size());
			for (const auto &column: columns) {
				if (row < column.values.size()) {
					props.emplace(column.key, column.values[row]);
				}
			}
			materialized[row].setProperties(std::move(props));
		}
		return materialized;
	}
} // namespace

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
				if (dataManager_->getIdAllocator(EntityType::Node)->getCurrentMaxId() > 0) {
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
				if (dataManager_->getIdAllocator(EntityType::Edge)->getCurrentMaxId() > 0) {
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
		// Build against the logical storage view. Public schema APIs may choose
		// to checkpoint before starting a schema transaction for performance, but
		// the low-level builder must not hide synchronous I/O in every build task.
		return buildFunc();
	}

	bool IndexManager::createIndex(const std::string &indexName, const std::string &entityType,
								   const std::string &label, const std::string &property) const {
		auto results = createIndexes({IndexCreateRequest{indexName, entityType, label, property}});
		return !results.empty() && results.front().success;
	}

	std::vector<IndexManager::IndexCreateResult>
	IndexManager::createIndexes(const std::vector<IndexCreateRequest> &requests) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		std::vector<IndexCreateResult> results;
		results.reserve(requests.size());
		if (requests.empty()) {
			return results;
		}

		struct PreparedRequest {
			size_t resultIndex = 0;
			std::string name;
			std::string entityType;
			std::string indexType;
			std::string label;
			std::string property;
			std::string physicalProperty;
		};

		auto defaultIndexName = [](const IndexCreateRequest &request) {
			return "index_" + request.entityType + "_" + request.label + "_" +
				   (request.property.empty() ? "LABEL" : request.property);
		};

		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);
		std::unordered_set<std::string> plannedNames;
		std::unordered_set<std::string> plannedPhysicalProperties;
		std::vector<PreparedRequest> nodeLabelRequests;
		std::vector<PreparedRequest> edgeLabelRequests;
		std::vector<PreparedRequest> nodePropertyRequests;
		std::vector<PreparedRequest> edgePropertyRequests;

		for (const auto &request: requests) {
			IndexCreateResult result{request, false, ""};
			const size_t resultIndex = results.size();
			std::string name = request.indexName.empty() ? defaultIndexName(request) : request.indexName;

			if (allIndexes.contains(name) || plannedNames.contains(name)) {
				result.reason = "index name already exists";
				results.push_back(std::move(result));
				continue;
			}

			PreparedRequest prepared{resultIndex, name, request.entityType,
									request.property.empty() ? "label" : "property",
									request.label, request.property, ""};

			if (request.property.empty()) {
				if (request.entityType == "node") {
					nodeLabelRequests.push_back(prepared);
				} else if (request.entityType == "edge") {
					edgeLabelRequests.push_back(prepared);
				} else {
					result.reason = "unsupported entity type";
					results.push_back(std::move(result));
					continue;
				}
			} else if (request.entityType == "node") {
				prepared.physicalProperty =
						request.label.empty() ? request.property : makeScopedNodePropertyKey(request.label, request.property);
				const std::string physicalKey = "node:" + prepared.physicalProperty;
				if (nodeIndexManager_->getPropertyIndex()->hasKeyIndexed(prepared.physicalProperty) ||
					plannedPhysicalProperties.contains(physicalKey)) {
					result.reason = "property index already exists";
					results.push_back(std::move(result));
					continue;
				}
				plannedPhysicalProperties.insert(physicalKey);
				nodePropertyRequests.push_back(std::move(prepared));
			} else if (request.entityType == "edge") {
				prepared.physicalProperty = request.property;
				const std::string physicalKey = "edge:" + prepared.physicalProperty;
				if (edgeIndexManager_->getPropertyIndex()->hasKeyIndexed(prepared.physicalProperty) ||
					plannedPhysicalProperties.contains(physicalKey)) {
					result.reason = "property index already exists";
					results.push_back(std::move(result));
					continue;
				}
				plannedPhysicalProperties.insert(physicalKey);
				edgePropertyRequests.push_back(std::move(prepared));
			} else {
				result.reason = "unsupported entity type";
				results.push_back(std::move(result));
				continue;
			}

			plannedNames.insert(name);
			results.push_back(std::move(result));
		}

		auto persistSuccessfulMetadata = [&](const std::vector<PreparedRequest> &preparedRequests) {
			for (const auto &prepared: preparedRequests) {
				IndexMetadata meta{prepared.name, prepared.entityType, prepared.indexType,
								   prepared.label, prepared.property};
				sysState->set(storage::state::keys::SYS_INDEXES, prepared.name, meta.toString());
				results[prepared.resultIndex].success = true;
				results[prepared.resultIndex].reason = "created";
			}
		};

		if (!nodeLabelRequests.empty()) {
			const bool success = nodeIndexManager_->createLabelIndex(
					[&]() { return executeBuildTask([&]() { return indexBuilder_->buildNodeLabelIndex(); }); });
			if (success) {
				persistSuccessfulMetadata(nodeLabelRequests);
			}
		}

		if (!edgeLabelRequests.empty()) {
			const bool success = edgeIndexManager_->createLabelIndex(
					[&]() { return executeBuildTask([&]() { return indexBuilder_->buildEdgeTypeIndex(); }); });
			if (success) {
				persistSuccessfulMetadata(edgeLabelRequests);
			}
		}

		if (!nodePropertyRequests.empty()) {
			std::vector<IndexBuilder::NodePropertyIndexBuildSpec> specs;
			specs.reserve(nodePropertyRequests.size());
			for (const auto &request: nodePropertyRequests) {
				specs.push_back({request.property, request.label});
			}
			const bool success = executeBuildTask([&]() { return indexBuilder_->buildNodePropertyIndexes(specs); });
			if (success) {
				persistSuccessfulMetadata(nodePropertyRequests);
			}
		}

		if (!edgePropertyRequests.empty()) {
			std::vector<std::string> keys;
			keys.reserve(edgePropertyRequests.size());
			for (const auto &request: edgePropertyRequests) {
				keys.push_back(request.property);
			}
			const bool success = executeBuildTask([&]() { return indexBuilder_->buildEdgePropertyIndexes(keys); });
			if (success) {
				persistSuccessfulMetadata(edgePropertyRequests);
			}
		}

		for (auto &result: results) {
			if (result.reason.empty()) {
				result.reason = result.success ? "created" : "build failed";
			}
		}
		return results;
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
			const std::string physicalProperty =
					meta.indexType == "property" && !meta.label.empty() && !meta.property.empty()
							? makeScopedNodePropertyKey(meta.label, meta.property)
							: meta.property;
			physicalDropSuccess = nodeIndexManager_->dropIndex(meta.indexType, physicalProperty);
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
	std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string>>
	IndexManager::listIndexesDetailed() const {
		// Returns: {Name, EntityType, IndexType, Label, Property}
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);

		std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string>> result;
		for (const auto &[name, rawMeta]: allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			result.emplace_back(meta.name, meta.entityType, meta.indexType, meta.label, meta.property);
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
		updateScopedPropertyIndexesForNode(Node{}, node);

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
		updateScopedPropertyIndexesForNodes(nodes);
		updateCompositeIndexesForNodes(nodes);

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

	void IndexManager::onNodesAddedColumnar(
			const std::vector<Node> &nodes,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		nodeIndexManager_->onEntitiesAddedColumnar(nodes, columns);
		updateScopedPropertyIndexesForNodesColumnar(nodes, columns);
		updateCompositeIndexesForNodesColumnar(nodes, columns);

		if (vectorIndexManager_ && !nodes.empty() && !columns.empty()) {
			const auto materialized = materializeNodesForVectorIndexes(nodes, columns);
			std::unordered_map<std::string,
							   std::vector<std::pair<Node, std::unordered_map<std::string, PropertyValue>>>>
					byLabel;
			for (const auto &node: materialized) {
				if (node.getProperties().empty()) {
					continue;
				}
				std::string labelStr;
				if (node.getLabelId() != 0) {
					labelStr = dataManager_->resolveTokenName(node.getLabelId());
				}
				if (!labelStr.empty()) {
					byLabel[labelStr].push_back({node, node.getProperties()});
				}
			}
			for (auto &[label, batch]: byLabel) {
				vectorIndexManager_->updateIndexBatch(batch, label);
			}
		}
	}

	void IndexManager::onNodeUpdated(const Node &oldNode, const Node &newNode) {
		if (!nodeIndexRelevantFieldsChanged(oldNode, newNode)) {
			return;
		}
		nodeIndexManager_->onEntityUpdated(oldNode, newNode);
		updateScopedPropertyIndexesForNode(oldNode, newNode);

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
		removeScopedPropertyIndexesForNode(node);

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

	void IndexManager::onEdgesAddedColumnar(
			const std::vector<Edge> &edges,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		edgeIndexManager_->onEntitiesAddedColumnar(edges, columns);
	}

	void IndexManager::onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) {
		if (!edgeIndexRelevantFieldsChanged(oldEdge, newEdge)) {
			return;
		}
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

	size_t IndexManager::estimateNodeIdsByLabel(const std::string &label) const {
		return nodeIndexManager_->getLabelIndex()->countNodes(label);
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
	                                                               const PropertyValue &maxValue,
	                                                               bool minInclusive,
	                                                               bool maxInclusive) const {
		log::Log::debug("IndexManager::findNodeIdsByPropertyRange - key: {}", key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getPropertyIndex()->findRange(key, minValue, maxValue, minInclusive, maxInclusive);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	bool IndexManager::hasNodePropertyIndexForLabel(const std::string &label, const std::string &key) const {
		if (label.empty() || key.empty()) {
			return false;
		}
		return nodeIndexManager_->getPropertyIndex()->hasKeyIndexed(makeScopedNodePropertyKey(label, key));
	}

	std::vector<int64_t> IndexManager::findNodeIdsByLabelAndProperty(
	    const std::string &label,
	    const std::string &key,
	    const PropertyValue &value) const {
		log::Log::debug("IndexManager::findNodeIdsByLabelAndProperty - label: {}, key: {}", label, key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getPropertyIndex()->findExactMatch(makeScopedNodePropertyKey(label, key), value);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	std::vector<int64_t> IndexManager::findNodeIdsByLabelAndPropertyRange(
	    const std::string &label,
	    const std::string &key,
	    const PropertyValue &minValue,
	    const PropertyValue &maxValue,
	    bool minInclusive,
	    bool maxInclusive) const {
		log::Log::debug("IndexManager::findNodeIdsByLabelAndPropertyRange - label: {}, key: {}", label, key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = nodeIndexManager_->getPropertyIndex()->findRange(
			makeScopedNodePropertyKey(label, key), minValue, maxValue, minInclusive, maxInclusive);
		if (!result.empty()) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	size_t IndexManager::countNodeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		log::Log::debug("IndexManager::countNodeIdsByProperty - key: {}", key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = estimateNodeIdsByProperty(key, value);
		if (result != 0) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	size_t IndexManager::estimateNodeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		return nodeIndexManager_->getPropertyIndex()->countExactMatch(key, value);
	}

	size_t IndexManager::countNodeIdsByPropertyRange(const std::string &key,
	                                                 const PropertyValue &minValue,
	                                                 const PropertyValue &maxValue,
	                                                 bool minInclusive,
	                                                 bool maxInclusive) const {
		log::Log::debug("IndexManager::countNodeIdsByPropertyRange - key: {}", key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = estimateNodeIdsByPropertyRange(key, minValue, maxValue, minInclusive, maxInclusive);
		if (result != 0) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	size_t IndexManager::estimateNodeIdsByPropertyRange(const std::string &key,
	                                                    const PropertyValue &minValue,
	                                                    const PropertyValue &maxValue,
	                                                    bool minInclusive,
	                                                    bool maxInclusive) const {
		return nodeIndexManager_->getPropertyIndex()->countRange(key, minValue, maxValue, minInclusive, maxInclusive);
	}

	size_t IndexManager::countNodeIdsByLabelAndProperty(
	    const std::string &label,
	    const std::string &key,
	    const PropertyValue &value) const {
		log::Log::debug("IndexManager::countNodeIdsByLabelAndProperty - label: {}, key: {}", label, key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = estimateNodeIdsByLabelAndProperty(label, key, value);
		if (result != 0) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	size_t IndexManager::estimateNodeIdsByLabelAndProperty(
	    const std::string &label,
	    const std::string &key,
	    const PropertyValue &value) const {
		return nodeIndexManager_->getPropertyIndex()->countExactMatch(makeScopedNodePropertyKey(label, key), value);
	}

	size_t IndexManager::countNodeIdsByLabelAndPropertyRange(
	    const std::string &label,
	    const std::string &key,
	    const PropertyValue &minValue,
	    const PropertyValue &maxValue,
	    bool minInclusive,
	    bool maxInclusive) const {
		log::Log::debug("IndexManager::countNodeIdsByLabelAndPropertyRange - label: {}, key: {}", label, key);
		lookups_.fetch_add(1, std::memory_order_relaxed);
		auto result = estimateNodeIdsByLabelAndPropertyRange(
			label, key, minValue, maxValue, minInclusive, maxInclusive);
		if (result != 0) indexHits_.fetch_add(1, std::memory_order_relaxed);
		return result;
	}

	size_t IndexManager::estimateNodeIdsByLabelAndPropertyRange(
	    const std::string &label,
	    const std::string &key,
	    const PropertyValue &minValue,
	    const PropertyValue &maxValue,
	    bool minInclusive,
	    bool maxInclusive) const {
		return nodeIndexManager_->getPropertyIndex()->countRange(
			makeScopedNodePropertyKey(label, key), minValue, maxValue, minInclusive, maxInclusive);
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

	size_t IndexManager::estimateNodeIdsByCompositeIndex(
	    const std::vector<std::string> &keys,
	    const std::vector<PropertyValue> &values) const {
		return nodeIndexManager_->getPropertyIndex()->countCompositeExact(keys, values);
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

	size_t IndexManager::estimateEdgeIdsByType(const std::string &type) const {
		return edgeIndexManager_->getLabelIndex()->countNodes(type);
	}

	size_t IndexManager::estimateEdgeIdsByProperty(const std::string &key, const PropertyValue &value) const {
		return edgeIndexManager_->getPropertyIndex()->countExactMatch(key, value);
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

	void IndexManager::updateCompositeIndexesForNodes(const std::vector<Node> &nodes) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex || nodes.empty()) return;

		struct CompositeIndexSpec {
			std::vector<std::string> keys;
		};

		std::vector<CompositeIndexSpec> specs;
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);
		for (const auto &[name, rawMeta] : allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			if (meta.indexType != "composite") continue;

			CompositeIndexSpec spec;
			std::stringstream ss(meta.property);
			std::string segment;
			while (std::getline(ss, segment, ',')) {
				if (!segment.empty()) {
					spec.keys.push_back(segment);
				}
			}
			if (!spec.keys.empty()) {
				specs.push_back(std::move(spec));
			}
		}
		if (specs.empty()) return;

		std::vector<PropertyIndex::CompositeEntry> entries;
		entries.reserve(nodes.size() * specs.size());
		for (const auto &node : nodes) {
			const auto &props = node.getProperties();
			if (node.getId() == 0 || !node.isActive() || props.empty()) continue;

			for (const auto &spec : specs) {
				std::vector<PropertyValue> values;
				values.reserve(spec.keys.size());
				bool allPresent = true;
				for (const auto &key : spec.keys) {
					auto it = props.find(key);
					if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
						allPresent = false;
						break;
					}
					values.push_back(it->second);
				}

				if (allPresent) {
					entries.push_back(PropertyIndex::CompositeEntry{node.getId(), spec.keys, std::move(values)});
				}
			}
		}
		propIndex->addCompositeEntriesBatch(entries);
	}

	void IndexManager::updateCompositeIndexesForNodesColumnar(
			const std::vector<Node> &nodes,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex || nodes.empty() || columns.empty()) {
			return;
		}

		struct CompositeIndexSpec {
			std::vector<std::string> keys;
		};

		std::vector<CompositeIndexSpec> specs;
		auto sysState = storage_->getSystemStateManager();
		auto allIndexes = sysState->getMap<std::string>(storage::state::keys::SYS_INDEXES);
		for (const auto &[name, rawMeta]: allIndexes) {
			IndexMetadata meta = IndexMetadata::fromString(name, rawMeta);
			if (meta.indexType != "composite") {
				continue;
			}
			CompositeIndexSpec spec;
			std::stringstream ss(meta.property);
			std::string segment;
			while (std::getline(ss, segment, ',')) {
				if (!segment.empty()) {
					spec.keys.push_back(segment);
				}
			}
			if (!spec.keys.empty()) {
				specs.push_back(std::move(spec));
			}
		}
		if (specs.empty()) {
			return;
		}

		const auto columnsByKey = mapColumnsByKey(columns);
		std::vector<PropertyIndex::CompositeEntry> entries;
		entries.reserve(nodes.size() * specs.size());
		for (size_t row = 0; row < nodes.size(); ++row) {
			const auto &node = nodes[row];
			if (node.getId() == 0 || !node.isActive()) {
				continue;
			}
			for (const auto &spec: specs) {
				std::vector<PropertyValue> values;
				values.reserve(spec.keys.size());
				bool allPresent = true;
				for (const auto &key: spec.keys) {
					const auto columnIt = columnsByKey.find(key);
					if (columnIt == columnsByKey.end() || row >= columnIt->second->values.size() ||
						columnIt->second->values[row].getType() == PropertyType::NULL_TYPE) {
						allPresent = false;
						break;
					}
					values.push_back(columnIt->second->values[row]);
				}
				if (allPresent) {
					entries.push_back(PropertyIndex::CompositeEntry{node.getId(), spec.keys, std::move(values)});
				}
			}
		}
		propIndex->addCompositeEntriesBatch(entries);
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

	void IndexManager::updateScopedPropertyIndexesForNode(const Node &oldNode, const Node &newNode) {
		removeScopedPropertyIndexesForNode(oldNode);

		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex || newNode.getId() == 0 || !newNode.isActive()) return;

		const auto &props = newNode.getProperties();
		if (props.empty()) return;

		std::vector<std::tuple<int64_t, std::string, PropertyValue>> scopedEntries;
		const auto indexedKeys = propIndex->getIndexedKeysSnapshot();
		for (const auto &physicalKey : indexedKeys) {
			const auto scoped = decodeScopedNodePropertyKey(physicalKey);
			if (!scoped.has_value()) continue;
			const auto &[label, property] = *scoped;
			const int64_t labelId = dataManager_->resolveTokenId(label);
			if (labelId == 0 || !newNode.hasLabelId(labelId)) {
				continue;
			}
			auto propIt = props.find(property);
			if (propIt == props.end() || propIt->second.getType() == PropertyType::NULL_TYPE) {
				continue;
			}
			scopedEntries.emplace_back(newNode.getId(), physicalKey, propIt->second);
		}
		if (!scopedEntries.empty()) {
			propIndex->addPropertiesBatch(scopedEntries);
		}
	}

	void IndexManager::updateScopedPropertyIndexesForNodes(const std::vector<Node> &nodes) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex || nodes.empty()) return;

		struct ScopedIndexSpec {
			std::string physicalKey;
			int64_t labelId = 0;
			std::string property;
		};

		std::vector<ScopedIndexSpec> specs;
		const auto indexedKeys = propIndex->getIndexedKeysSnapshot();
		specs.reserve(indexedKeys.size());
		for (const auto &physicalKey : indexedKeys) {
			const auto scoped = decodeScopedNodePropertyKey(physicalKey);
			if (!scoped.has_value()) continue;
			const auto &[label, property] = *scoped;
			const int64_t labelId = dataManager_->resolveTokenId(label);
			if (labelId == 0) continue;
			specs.push_back(ScopedIndexSpec{physicalKey, labelId, property});
		}
		if (specs.empty()) return;

		std::vector<std::tuple<int64_t, std::string, PropertyValue>> scopedEntries;
		scopedEntries.reserve(nodes.size());
		for (const auto &node : nodes) {
			if (node.getId() == 0 || !node.isActive()) continue;
			const auto &props = node.getProperties();
			if (props.empty()) continue;

			for (const auto &spec : specs) {
				if (!node.hasLabelId(spec.labelId)) {
					continue;
				}
				auto propIt = props.find(spec.property);
				if (propIt == props.end() || propIt->second.getType() == PropertyType::NULL_TYPE) {
					continue;
				}
				scopedEntries.emplace_back(node.getId(), spec.physicalKey, propIt->second);
			}
		}
		if (!scopedEntries.empty()) {
			propIndex->addPropertiesBatch(scopedEntries);
		}
	}

	void IndexManager::updateScopedPropertyIndexesForNodesColumnar(
			const std::vector<Node> &nodes,
			const std::vector<storage::BulkPropertyColumn> &columns) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex || nodes.empty() || columns.empty()) {
			return;
		}

		struct ScopedIndexSpec {
			std::string physicalKey;
			int64_t labelId = 0;
			std::string property;
		};

		std::vector<ScopedIndexSpec> specs;
		const auto indexedKeys = propIndex->getIndexedKeysSnapshot();
		specs.reserve(indexedKeys.size());
		for (const auto &physicalKey: indexedKeys) {
			const auto scoped = decodeScopedNodePropertyKey(physicalKey);
			if (!scoped.has_value()) {
				continue;
			}
			const auto &[label, property] = *scoped;
			const int64_t labelId = dataManager_->resolveTokenId(label);
			if (labelId == 0) {
				continue;
			}
			specs.push_back(ScopedIndexSpec{physicalKey, labelId, property});
		}
		if (specs.empty()) {
			return;
		}

		const auto columnsByKey = mapColumnsByKey(columns);
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> scopedEntries;
		scopedEntries.reserve(nodes.size());
		for (size_t row = 0; row < nodes.size(); ++row) {
			const auto &node = nodes[row];
			if (node.getId() == 0 || !node.isActive()) {
				continue;
			}
			for (const auto &spec: specs) {
				if (!node.hasLabelId(spec.labelId)) {
					continue;
				}
				const auto columnIt = columnsByKey.find(spec.property);
				if (columnIt == columnsByKey.end() || row >= columnIt->second->values.size()) {
					continue;
				}
				const auto &value = columnIt->second->values[row];
				if (value.getType() == PropertyType::NULL_TYPE) {
					continue;
				}
				scopedEntries.emplace_back(node.getId(), spec.physicalKey, value);
			}
		}
		if (!scopedEntries.empty()) {
			propIndex->addPropertiesBatch(scopedEntries);
		}
	}

	void IndexManager::removeScopedPropertyIndexesForNode(const Node &node) {
		auto *propIndex = nodeIndexManager_->getPropertyIndex().get();
		if (!propIndex || node.getId() == 0) return;

		const auto &props = node.getProperties();
		if (props.empty()) return;

		const auto indexedKeys = propIndex->getIndexedKeysSnapshot();
		for (const auto &physicalKey : indexedKeys) {
			const auto scoped = decodeScopedNodePropertyKey(physicalKey);
			if (!scoped.has_value()) continue;
			const auto &[label, property] = *scoped;
			const int64_t labelId = dataManager_->resolveTokenId(label);
			if (labelId == 0 || !node.hasLabelId(labelId)) {
				continue;
			}
			auto propIt = props.find(property);
			if (propIt == props.end() || propIt->second.getType() == PropertyType::NULL_TYPE) {
				continue;
			}
			propIndex->removeProperty(node.getId(), physicalKey, propIt->second);
		}
	}

} // namespace graph::query::indexes
