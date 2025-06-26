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

	IndexManager::~IndexManager() {
		// Ensure any in-progress index building is cancelled
		if (indexBuilder_) {
			indexBuilder_->cancel();
		}
	}

	void IndexManager::initialize() {
		// Create index builder
		indexBuilder_ = std::make_unique<IndexBuilder>(shared_from_this(), storage_);
	}

	bool IndexManager::buildLabelIndex() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Check if an async build is in progress
		if (isIndexBuilding()) {
			return false;
		}

		// Clear existing label index
		labelIndex_->clear();

		// Commit any pending changes before indexing
		storage_->save();

		bool result = indexBuilder_->buildLabelIndexWorker();

		// Enable automatic updates for this index
		if (result) {
			enableLabelIndex(true);
		}

		return result;
	}

	bool IndexManager::buildPropertyIndex(const std::string &key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Check if an async build is in progress
		if (isIndexBuilding()) {
			return false;
		}

		// Clear specific property key index
		propertyIndex_->clearKey(key);

		// Commit any pending changes before indexing
		storage_->save();

		bool result = indexBuilder_->buildPropertyIndexWorker(key);

		// Enable automatic updates for this index
		if (result) {
			enablePropertyIndex(key, true);
		}

		return result;
	}

	bool IndexManager::buildIndexes() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Check if an async build is in progress
		if (isIndexBuilding()) {
			return false;
		}

		// Commit any pending changes before indexing
		storage_->save();

		bool result = indexBuilder_->buildAllIndexesWorker();

		// Enable automatic updates for all indexes
		if (result) {
			enableLabelIndex(true);
			enableRelationshipIndex(true);
			enableFullTextIndex(true);

			// Get all property keys that were indexed
			auto propertyKeys = propertyIndex_->getIndexedKeys();
			for (const auto &key: propertyKeys) {
				enablePropertyIndex(key, true);
			}
		}

		return result;
	}

	bool IndexManager::startBuildLabelIndex() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (isIndexBuilding()) {
			return false;
		}

		// Commit any pending changes before indexing
		storage_->save();

		return indexBuilder_->startBuildLabelIndex();
	}

	bool IndexManager::startBuildPropertyIndex(const std::string &key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (isIndexBuilding()) {
			return false;
		}

		// Commit any pending changes before indexing
		storage_->save();

		return indexBuilder_->startBuildPropertyIndex(key);
	}

	bool IndexManager::startBuildAllIndexes() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (isIndexBuilding()) {
			return false;
		}

		// Commit any pending changes before indexing
		storage_->save();

		return indexBuilder_->startBuildAllIndexes();
	}

	bool IndexManager::isIndexBuilding() const { return indexBuilder_ && indexBuilder_->isBuilding(); }

	int IndexManager::getIndexBuildProgress() const { return indexBuilder_ ? indexBuilder_->getProgress() : 0; }

	bool IndexManager::waitForIndexCompletion(int timeoutSeconds) {
		if (!indexBuilder_) {
			return true; // No builder, so considered complete
		}

		return indexBuilder_->waitForCompletion(std::chrono::seconds(timeoutSeconds));
	}

	void IndexManager::cancelIndexBuild() {
		if (indexBuilder_) {
			indexBuilder_->cancel();
		}
	}

	bool IndexManager::dropIndex(const std::string &indexType, const std::string &key) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (indexType == "label") {
			labelIndex_->clear();
			labelIndex_->flush();
			indexConfig_.labelIndexEnabled = false;
			return true;
		} else if (indexType == "property") {
			if (key.empty()) {
				// Drop all property indexes
				propertyIndex_->clear();
				indexConfig_.propertyIndexKeys.clear();
			} else {
				// Drop specific property key index
				propertyIndex_->clearKey(key);
				indexConfig_.propertyIndexKeys.erase(key);
			}
			propertyIndex_->flush();
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

	std::vector<std::pair<std::string, std::string>> IndexManager::listIndexes() {
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

	void IndexManager::updateNodeIndexes(const Node &node, bool isNew, bool isDeleted) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		const int64_t nodeId = node.getId();

		// Skip temporary nodes
		if (nodeId <= 0)
			return;

		if (isDeleted) {
			// Remove from all indexes
			if (indexConfig_.labelIndexEnabled) {
				labelIndex_->removeNode(nodeId, node.getLabel());
			}

			if (!indexConfig_.propertyIndexKeys.empty()) {
				auto properties = dataManager_->getNodeProperties(nodeId);
				for (const auto &[key, _]: properties) {
					if (indexConfig_.propertyIndexKeys.find(key) != indexConfig_.propertyIndexKeys.end()) {
						propertyIndex_->removeProperty(nodeId, key);
					}

					if (indexConfig_.fullTextIndexEnabled && std::holds_alternative<std::string>(_)) {
						fullTextIndex_->removeTextProperty(nodeId, key);
					}
				}
			}
		} else {
			// Add or update indexes
			if (indexConfig_.labelIndexEnabled) {
				if (!isNew) {
					// For updates, first remove old label
					labelIndex_->removeNode(nodeId, node.getLabel());
				}

				// Add current label
				labelIndex_->addNode(nodeId, node.getLabel());
			}

			if (!indexConfig_.propertyIndexKeys.empty()) {
				auto properties = dataManager_->getNodeProperties(nodeId);

				// Only process properties that have indexes enabled
				for (const auto &[key, value]: properties) {
					if (indexConfig_.propertyIndexKeys.find(key) != indexConfig_.propertyIndexKeys.end()) {
						propertyIndex_->addProperty(nodeId, key, value);

						// Also handle full-text index for string properties
						if (indexConfig_.fullTextIndexEnabled && std::holds_alternative<std::string>(value)) {
							fullTextIndex_->addTextProperty(nodeId, key, std::get<std::string>(value));
						}
					}
				}
			}
		}
	}

	void IndexManager::updateEdgeIndexes(const Edge &edge, bool isNew, bool isDeleted) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Skip if relationship indexing is not enabled
		if (!indexConfig_.relationshipIndexEnabled)
			return;

		const int64_t edgeId = edge.getId();

		// Skip temporary edges
		if (edgeId <= 0)
			return;

		if (isDeleted) {
			// Remove from relationship index
			relationshipIndex_->removeEdge(edgeId);
		} else {
			// Add to relationship index (handles both new and updated)
			relationshipIndex_->addEdge(edgeId, edge.getFromNodeId(), edge.getToNodeId(), edge.getLabel());
		}
	}

	void IndexManager::updateIndexes(const std::vector<Node> &addedNodes, const std::vector<Node> &updatedNodes,
									 const std::vector<uint64_t> &removedNodeIds, const std::vector<Edge> &addedEdges,
									 const std::vector<Edge> &updatedEdges,
									 const std::vector<uint64_t> &removedEdgeIds) {
		// Process nodes
		for (const auto &nodeId: removedNodeIds) {
			// Find node in storage
			const auto &node = dataManager_->getNode(nodeId);
			if (node.getId() != 0) {
				updateNodeIndexes(node, false, true);
			}
		}

		for (const auto &node: addedNodes) {
			updateNodeIndexes(node, true, false);
		}

		for (const auto &node: updatedNodes) {
			updateNodeIndexes(node, false, false);
		}

		// Process edges
		for (const auto &edgeId: removedEdgeIds) {
			// Find edge in storage
			const auto &edge = dataManager_->getEdge(edgeId);
			if (edge.getId() != 0) {
				updateEdgeIndexes(edge, false, true);
			}
		}

		for (const auto &edge: addedEdges) {
			updateEdgeIndexes(edge, true, false);
		}

		for (const auto &edge: updatedEdges) {
			updateEdgeIndexes(edge, false, false);
		}

		// Persist changes if needed
		persistState();
	}

	// Helper method to update property indexes
	void IndexManager::updatePropertyIndexes(int64_t entityId,
											 const std::unordered_map<std::string, PropertyValue> &oldProps,
											 const std::unordered_map<std::string, PropertyValue> &newProps) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Get all keys that need updating
		std::unordered_set<std::string> keys;
		for (const auto &[key, _]: oldProps) {
			if (indexConfig_.propertyIndexKeys.find(key) != indexConfig_.propertyIndexKeys.end()) {
				keys.insert(key);
			}
		}

		for (const auto &[key, _]: newProps) {
			if (indexConfig_.propertyIndexKeys.find(key) != indexConfig_.propertyIndexKeys.end()) {
				keys.insert(key);
			}
		}

		// Process each key
		for (const auto &key: keys) {
			auto oldIt = oldProps.find(key);
			auto newIt = newProps.find(key);

			if (oldIt != oldProps.end() && newIt == newProps.end()) {
				// Property removed
				propertyIndex_->removeProperty(entityId, key);

				if (indexConfig_.fullTextIndexEnabled && std::holds_alternative<std::string>(oldIt->second)) {
					fullTextIndex_->removeTextProperty(entityId, key);
				}
			} else if (newIt != newProps.end()) {
				// Property added or updated
				propertyIndex_->addProperty(entityId, key, newIt->second);

				if (indexConfig_.fullTextIndexEnabled && std::holds_alternative<std::string>(newIt->second)) {
					// For updates, first remove old text
					if (oldIt != oldProps.end() && std::holds_alternative<std::string>(oldIt->second)) {
						fullTextIndex_->removeTextProperty(entityId, key);
					}

					// Add new text
					fullTextIndex_->addTextProperty(entityId, key, std::get<std::string>(newIt->second));
				}
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
