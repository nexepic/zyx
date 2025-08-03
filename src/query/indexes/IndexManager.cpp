/**
 * @file IndexManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/indexes/IndexManager.hpp"
#include <algorithm>
#include <ranges>
#include "graph/query/indexes/IndexBuilder.hpp"

namespace graph::query::indexes {

	IndexManager::IndexManager(std::shared_ptr<storage::FileStorage> storage) :
		storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {

		StateRegistry::initialize(dataManager_);

		labelIndex_ = std::make_shared<LabelIndex>(dataManager_);
		propertyIndex_ = std::make_shared<PropertyIndex>(dataManager_);
		relationshipIndex_ = std::make_shared<RelationshipIndex>();
		fullTextIndex_ = std::make_shared<FullTextIndex>();
	}

	IndexManager::~IndexManager() = default;

	void IndexManager::initialize() {
		// Create index builder
		indexBuilder_ = std::make_unique<IndexBuilder>(shared_from_this(), storage_);
	}

	bool IndexManager::executeBuildTask(const std::function<bool()> &buildFunc) const {
		// Commit any pending changes before indexing. This is crucial for ensuring data consistency.
		storage_->flush();

		// Execute the specific build task.
		return buildFunc();
	}

	bool IndexManager::buildLabelIndex() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		bool result = executeBuildTask([&]() { return indexBuilder_->buildLabelIndex(); });

		// Enable automatic updates for this index upon successful start.
		if (result) {
			enableLabelIndex(true);
		}

		return result;
	}

	bool IndexManager::buildPropertyIndex(const std::string &key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		bool result = executeBuildTask([&]() { return indexBuilder_->buildPropertyIndex(key); });

		// Enable automatic updates for this index upon successful start.
		if (result) {
			enablePropertyIndex(key, true);
		}

		return result;
	}

	bool IndexManager::buildIndexes() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		bool result = executeBuildTask([&]() { return indexBuilder_->buildAllIndexes(); });

		// Enable automatic updates for all relevant indexes upon successful start.
		if (result) {
			enableLabelIndex(true);
			enableRelationshipIndex(true);
			enableFullTextIndex(true);

			// Get all property keys that were indexed.
			auto propertyKeys = propertyIndex_->getIndexedKeys();
			for (const auto &key: propertyKeys) {
				enablePropertyIndex(key, true);
			}
		}

		return result;
	}

	bool IndexManager::dropIndex(const std::string &indexType, const std::string &key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (indexType == "label") {
			labelIndex_->drop();
			indexConfig_.labelIndexEnabled = false;
			return true;
		} else if (indexType == "property") {
			if (key.empty()) {
				// Drop all property indexes
				propertyIndex_->drop();
				indexConfig_.propertyIndexKeys.clear();
			} else {
				// Drop specific property key index
				propertyIndex_->dropKey(key);
				indexConfig_.propertyIndexKeys.erase(key);
			}
			return true;
		} else if (indexType == "relationship") {
			relationshipIndex_->clear();
			relationshipIndex_->flush();
			indexConfig_.relationshipIndexEnabled = false;
			return true;
		} else if (indexType == "fulltext") {
			fullTextIndex_->clear();
			fullTextIndex_->flush();
			indexConfig_.fullTextIndexEnabled = false;
			return true;
		}
		return false;
	}

	std::vector<std::pair<std::string, std::string>> IndexManager::listIndexes() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		std::vector<std::pair<std::string, std::string>> result;

		// Add label index if active
		if (!labelIndex_->isEmpty()) {
			result.emplace_back("label", "");
		}

		// Add property indexes - first check if any exist at all
		if (!propertyIndex_->isEmpty()) {
			auto propertyKeys = propertyIndex_->getIndexedKeys();
			for (const auto &key: propertyKeys) {
				result.emplace_back("property", key);
			}
		}

		// // Add relationship index if active
		// if (!relationshipIndex_->isEmpty()) {
		//     result.emplace_back("relationship", "");
		// }
		//
		// // Add fulltext index if active
		// if (!fullTextIndex_->isEmpty()) {
		//     result.emplace_back("fulltext", "");
		// }

		return result;
	}

	void IndexManager::persistState() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Persist all indexes' state
		if (labelIndex_)
			labelIndex_->flush();
		if (propertyIndex_)
			propertyIndex_->flush();
		if (relationshipIndex_)
			relationshipIndex_->flush();
		if (fullTextIndex_)
			fullTextIndex_->flush();
	}

	void IndexManager::enableLabelIndex(bool enable) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		indexConfig_.labelIndexEnabled = enable;
	}

	void IndexManager::enablePropertyIndex(const std::string &key, bool enable) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (enable) {
			indexConfig_.propertyIndexKeys.insert(key);
		} else {
			indexConfig_.propertyIndexKeys.erase(key);
		}
	}

	void IndexManager::enableRelationshipIndex(bool enable) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		indexConfig_.relationshipIndexEnabled = enable;
	}

	void IndexManager::enableFullTextIndex(bool enable) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		indexConfig_.fullTextIndexEnabled = enable;
	}

	void IndexManager::updateNodeLabelIndex(const Node &node, const std::string &oldLabel, bool isDeleted) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (!indexConfig_.labelIndexEnabled)
			return;
		if (node.getId() == 0)
			return;

		const int64_t nodeId = node.getId();
		const std::string &newLabel = node.getLabel();

		if (isDeleted) {
			// On deletion, remove the node from its last known label index.
			// Note: The passed `node` object should represent the state just before deletion.
			labelIndex_->removeNode(nodeId, newLabel);
		} else {
			// Handle Add or Update
			if (!oldLabel.empty() && oldLabel != newLabel) {
				// The label was changed, so remove it from the old index.
				labelIndex_->removeNode(nodeId, oldLabel);
			}

			if (!newLabel.empty()) {
				// Add the node to the new/current label index.
				labelIndex_->addNode(nodeId, newLabel);
			}
		}
	}

	void IndexManager::updateEdgeIndexes(const Edge &edge, bool isNew, bool isDeleted) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Skip if relationship indexing is not enabled
		if (!indexConfig_.relationshipIndexEnabled)
			return;

		const int64_t edgeId = edge.getId();

		if (edgeId == 0)
			return;

		if (isDeleted) {
			// Remove from relationship index
			relationshipIndex_->removeEdge(edgeId);
		} else {
			// Add to relationship index (handles both new and updated)
			relationshipIndex_->addEdge(edgeId, edge.getSourceNodeId(), edge.getTargetNodeId(), edge.getLabel());
		}
	}

	void IndexManager::onNodeAdded(const Node &node) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// 1. Update label index
		// For a new node, the oldLabel is empty, and it's not a deletion.
		updateNodeLabelIndex(node, "", false);

		// 2. Update property indexes
		// For a new node, the oldProps map is empty.
		updatePropertyIndexes(node.getId(), {}, node.getProperties());

		persistState();
	}

	/**
	 * @brief Must be called AFTER a node has been updated in storage.
	 * @param oldNode The state of the node BEFORE the update.
	 * @param newNode The state of the node AFTER the update.
	 */
	void IndexManager::onNodeUpdated(const Node &oldNode, const Node &newNode) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// 1. Update label index
		// We provide both old and new labels.
		updateNodeLabelIndex(newNode, oldNode.getLabel(), false);

		// 2. Update property indexes
		// We provide both old and new properties.
		updatePropertyIndexes(newNode.getId(), oldNode.getProperties(), newNode.getProperties());

		persistState();
	}

	void IndexManager::onNodeDeleted(const Node &node) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// 1. Update label index for deletion
		updateNodeLabelIndex(node, node.getLabel(), true);

		// 2. Update property indexes for deletion
		// The newProps map is empty because the node will cease to exist.
		updatePropertyIndexes(node.getId(), node.getProperties(), {});

		persistState();
	}


	// --- You would implement similar onEdgeAdded, onEdgeUpdated, onEdgeDeleted methods here ---
	void IndexManager::onEdgeAdded(const Edge &edge) const {
		// Conceptual: update property indexes for the new edge
		updatePropertyIndexes(edge.getId(), {}, edge.getProperties());
		persistState();
	}

	void IndexManager::onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) const {
		// Conceptual: update property indexes for the updated edge
		updatePropertyIndexes(newEdge.getId(), oldEdge.getProperties(), newEdge.getProperties());
		persistState();
	}

	void IndexManager::onEdgeDeleted(const Edge &edge) const {
		// Conceptual: update property indexes for the deleted edge
		updatePropertyIndexes(edge.getId(), edge.getProperties(), {});
		persistState();
	}

	// Helper method to update property indexes
	void IndexManager::updatePropertyIndexes(int64_t entityId,
											 const std::unordered_map<std::string, PropertyValue> &oldProps,
											 const std::unordered_map<std::string, PropertyValue> &newProps) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Get all unique property keys from both old and new maps that are indexed.
		std::unordered_set<std::string> keys;
		for (const auto &key: oldProps | std::views::keys) {
			if (indexConfig_.propertyIndexKeys.contains(key)) {
				keys.insert(key);
			}
		}
		for (const auto &key: newProps | std::views::keys) {
			if (indexConfig_.propertyIndexKeys.contains(key)) {
				keys.insert(key);
			}
		}

		// Process each affected key
		for (const auto &key: keys) {
			auto oldIt = oldProps.find(key);
			auto newIt = newProps.find(key);

			bool hadOldValue = (oldIt != oldProps.end());
			bool hasNewValue = (newIt != newProps.end());

			if (hadOldValue && !hasNewValue) {
				// --- Case 1: Property was REMOVED ---
				// We have the old value, so we can remove it correctly.
				propertyIndex_->removeProperty(entityId, key, oldIt->second);

				// Handle full-text index as well
				if (indexConfig_.fullTextIndexEnabled && std::holds_alternative<std::string>(oldIt->second)) {
					fullTextIndex_->removeTextProperty(entityId, key);
				}

			} else if (!hadOldValue && hasNewValue) {
				// --- Case 2: Property was ADDED ---
				propertyIndex_->addProperty(entityId, key, newIt->second);

				if (indexConfig_.fullTextIndexEnabled && std::holds_alternative<std::string>(newIt->second)) {
					fullTextIndex_->addTextProperty(entityId, key, std::get<std::string>(newIt->second));
				}

			} else if (hadOldValue && hasNewValue) {
				// --- Case 3: Property was UPDATED ---
				if (oldIt->second != newIt->second) {
					// The value changed, so we must REMOVE the old and ADD the new.

					// Remove old property from index
					propertyIndex_->removeProperty(entityId, key, oldIt->second);

					// Add new property to index
					propertyIndex_->addProperty(entityId, key, newIt->second);

					// Handle full-text index
					if (indexConfig_.fullTextIndexEnabled) {
						if (std::holds_alternative<std::string>(oldIt->second)) {
							fullTextIndex_->removeTextProperty(entityId, key);
						}
						if (std::holds_alternative<std::string>(newIt->second)) {
							fullTextIndex_->addTextProperty(entityId, key, std::get<std::string>(newIt->second));
						}
					}
				}
				// If oldIt->second == newIt->second, the indexed property value didn't change,
				// so we do nothing.
			}
		}
	}

	std::shared_ptr<LabelIndex> IndexManager::getLabelIndex() const { return labelIndex_; }

	std::shared_ptr<PropertyIndex> IndexManager::getPropertyIndex() const { return propertyIndex_; }

	std::shared_ptr<RelationshipIndex> IndexManager::getRelationshipIndex() const { return relationshipIndex_; }

	std::shared_ptr<FullTextIndex> IndexManager::getFullTextIndex() const { return fullTextIndex_; }

	std::vector<int64_t> IndexManager::findNodeIdsByLabel(const std::string &label) const {
		return labelIndex_->findNodes(label);
	}

	std::vector<int64_t> IndexManager::findNodeIdsByProperty(const std::string &key, const std::string &value) const {
		PropertyValue propValue = value;
		return propertyIndex_->findExactMatch(key, propValue);
	}

	std::vector<int64_t> IndexManager::findNodeIdsByPropertyRange(const std::string &key, double minValue,
																  double maxValue) const {
		return propertyIndex_->findRange(key, minValue, maxValue);
	}

	std::vector<int64_t> IndexManager::findNodeIdsByTextSearch(const std::string &key,
															   const std::string &searchText) const {
		return fullTextIndex_->search(key, searchText);
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByRelationship(const std::string &label) const {
		return relationshipIndex_->findEdgesByLabel(label);
	}

	std::vector<int64_t> IndexManager::findEdgeIdsByNodes(int64_t fromNodeId, int64_t toNodeId) const {
		return relationshipIndex_->findEdgesBetweenNodes(fromNodeId, toNodeId);
	}

	std::vector<int64_t> IndexManager::findOutgoingEdgeIds(int64_t nodeId, const std::string &label) const {
		return relationshipIndex_->findOutgoingEdges(nodeId, label);
	}

	std::vector<int64_t> IndexManager::findIncomingEdgeIds(int64_t nodeId, const std::string &label) const {
		return relationshipIndex_->findIncomingEdges(nodeId, label);
	}

} // namespace graph::query::indexes
