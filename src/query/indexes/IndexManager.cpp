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

namespace graph::query::indexes {

	IndexManager::IndexManager(std::shared_ptr<storage::FileStorage> storage) : storage_(std::move(storage)) {
		StateRegistry::initialize(storage_->getDataManager());

		labelIndex_ = std::make_shared<LabelIndex>(storage_->getDataManager());
		propertyIndex_ = std::make_shared<PropertyIndex>(storage_->getDataManager());
		relationshipIndex_ = std::make_shared<RelationshipIndex>();
		fullTextIndex_ = std::make_shared<FullTextIndex>();
	}

	// Build only the label index
	bool IndexManager::buildLabelIndex() {
		// Clear existing label index
		labelIndex_->clear();

		storage_->save();

		// Rebuild label index from all nodes
		const auto& nodes = storage_->getAllNodes();
		for (const auto& [id, node] : nodes) {
			labelIndex_->addNode(id, node.getLabel());
		}

		// Persist the index state
		// labelIndex_->flush();
		return true;
	}

	// Build index for a specific property key
	bool IndexManager::buildPropertyIndex(const std::string& key) {
		// Clear specific property key index
		propertyIndex_->clearKey(key);

		// Rebuild the property index for the specific key
		const auto& nodes = storage_->getAllNodes();
		for (const auto& [id, node] : nodes) {
			auto properties = storage_->getNodeProperties(id);
			auto it = properties.find(key);
			if (it != properties.end()) {
				propertyIndex_->addProperty(id, key, it->second);
			}
		}

		// Persist the index state
		// propertyIndex_->flush();
		return true;
	}

	// Drop a specific index
	bool IndexManager::dropIndex(const std::string& indexType, const std::string& key) {
		if (indexType == "label") {
			labelIndex_->clear();
			labelIndex_->flush();
			return true;
		}
		else if (indexType == "property") {
			if (key.empty()) {
				// Drop all property indexes
				propertyIndex_->clear();
			} else {
				// Drop specific property key index
				propertyIndex_->clearKey(key);
			}
			propertyIndex_->flush();
			return true;
		}
		else if (indexType == "relationship") {
			relationshipIndex_->clear();
			relationshipIndex_->flush();
			return true;
		}
		else if (indexType == "fulltext") {
			fullTextIndex_->clear();
			fullTextIndex_->flush();
			return true;
		}
		return false;
	}

	// List all active indexes
	std::vector<std::pair<std::string, std::string>> IndexManager::listIndexes() {
		std::vector<std::pair<std::string, std::string>> result;

		// Add label index if active
		if (!labelIndex_->isEmpty()) {
			result.emplace_back("label", "");
		}

		// Add property indexes - first check if any exist at all
		if (!propertyIndex_->isEmpty()) {
			auto propertyKeys = propertyIndex_->getIndexedKeys();
			for (const auto& key : propertyKeys) {
				result.emplace_back("property", key);
			}
		}

		// // Add relationship index if active
		// if (!relationshipIndex_->isEmpty()) {
		// 	result.emplace_back("relationship", "");
		// }
  //
		// // Add fulltext index if active
		// if (!fullTextIndex_->isEmpty()) {
		// 	result.emplace_back("fulltext", "");
		// }

		return result;
	}

	void IndexManager::persistState() const {
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

	bool IndexManager::buildIndexes() {
		// Clear existing indexes
		labelIndex_->clear();
		propertyIndex_->clear();
		relationshipIndex_->clear();
		fullTextIndex_->clear();

		// Index all nodes
		const auto &nodes = storage_->getAllNodes();
		for (const auto &[nodeId, node]: nodes) {
			// Index by label
			const auto &label = node.getLabel();
			labelIndex_->addNode(nodeId, label);

			// Index by properties
			const auto &properties = node.getProperties();
			for (const auto &[key, value]: properties) {
				// Add to property index
				if (std::holds_alternative<std::string>(value)) {
					std::string strValue = std::get<std::string>(value);
					propertyIndex_->addProperty(nodeId, key, strValue);

					// Also add to full-text index for string properties
					fullTextIndex_->addTextProperty(nodeId, key, strValue);
				} else if (std::holds_alternative<int64_t>(value)) {
					int64_t intValue = std::get<int64_t>(value);
					propertyIndex_->addProperty(nodeId, key, intValue);
				} else if (std::holds_alternative<double>(value)) {
					double doubleValue = std::get<double>(value);
					propertyIndex_->addProperty(nodeId, key, doubleValue);
				} else if (std::holds_alternative<bool>(value)) {
					bool boolValue = std::get<bool>(value);
					propertyIndex_->addProperty(nodeId, key, boolValue);
				}
			}
		}

		// Index all edges
		const auto &edges = storage_->getAllEdges();
		for (const auto &[edgeId, edge]: edges) {
			relationshipIndex_->addEdge(edgeId, edge.getFromNodeId(), edge.getToNodeId(), edge.getLabel());
		}

		return true;
	}

	void IndexManager::updateIndexes(const std::vector<Node> &addedNodes, const std::vector<Node> &updatedNodes,
									 const std::vector<uint64_t> &removedNodeIds, const std::vector<Edge> &addedEdges,
									 const std::vector<Edge> &updatedEdges,
									 const std::vector<uint64_t> &removedEdgeIds) {
		// Handle removed nodes
		for (uint64_t nodeId: removedNodeIds) {
			// Find node in storage to get its label
			const auto &nodes = storage_->getAllNodes();
			auto nodeIt = nodes.find(nodeId);
			if (nodeIt != nodes.end()) {
				const auto &node = nodeIt->second;

				// Remove from label index
				labelIndex_->removeNode(nodeId, node.getLabel());

				// Remove from property indexes
				const auto &properties = node.getProperties();
				for (const auto &[key, _]: properties) {
					propertyIndex_->removeProperty(nodeId, key);
					fullTextIndex_->removeTextProperty(nodeId, key);
				}
			}
		}

		// Handle removed edges
		for (uint64_t edgeId: removedEdgeIds) {
			relationshipIndex_->removeEdge(edgeId);
		}

		// Handle added nodes
		for (const auto &node: addedNodes) {
			uint64_t nodeId = node.getId();

			// Add to label index
			labelIndex_->addNode(nodeId, node.getLabel());

			// Add to property indexes
			const auto &properties = node.getProperties();
			for (const auto &[key, value]: properties) {
				// Add to property index
				if (std::holds_alternative<std::string>(value)) {
					std::string strValue = std::get<std::string>(value);
					propertyIndex_->addProperty(nodeId, key, strValue);

					// Also add to full-text index for string properties
					fullTextIndex_->addTextProperty(nodeId, key, strValue);
				} else if (std::holds_alternative<int64_t>(value)) {
					int64_t intValue = std::get<int64_t>(value);
					propertyIndex_->addProperty(nodeId, key, intValue);
				} else if (std::holds_alternative<double>(value)) {
					double doubleValue = std::get<double>(value);
					propertyIndex_->addProperty(nodeId, key, doubleValue);
				} else if (std::holds_alternative<bool>(value)) {
					bool boolValue = std::get<bool>(value);
					propertyIndex_->addProperty(nodeId, key, boolValue);
				}
			}
		}

		// Handle updated nodes - treat as remove + add
		for (const auto &node: updatedNodes) {
			uint64_t nodeId = node.getId();

			// First remove old index entries
			const auto &nodes = storage_->getAllNodes();
			auto nodeIt = nodes.find(nodeId);
			if (nodeIt != nodes.end()) {
				const auto &oldNode = nodeIt->second;

				// Remove from label index
				labelIndex_->removeNode(nodeId, oldNode.getLabel());

				// Remove from property indexes
				const auto &properties = oldNode.getProperties();
				for (const auto &[key, _]: properties) {
					propertyIndex_->removeProperty(nodeId, key);
					fullTextIndex_->removeTextProperty(nodeId, key);
				}
			}

			// Then add new index entries
			labelIndex_->addNode(nodeId, node.getLabel());

			const auto &properties = node.getProperties();
			for (const auto &[key, value]: properties) {
				// Add to property index
				if (std::holds_alternative<std::string>(value)) {
					std::string strValue = std::get<std::string>(value);
					propertyIndex_->addProperty(nodeId, key, strValue);
					fullTextIndex_->addTextProperty(nodeId, key, strValue);
				} else if (std::holds_alternative<int64_t>(value)) {
					int64_t intValue = std::get<int64_t>(value);
					propertyIndex_->addProperty(nodeId, key, intValue);
				} else if (std::holds_alternative<double>(value)) {
					double doubleValue = std::get<double>(value);
					propertyIndex_->addProperty(nodeId, key, doubleValue);
				} else if (std::holds_alternative<bool>(value)) {
					bool boolValue = std::get<bool>(value);
					propertyIndex_->addProperty(nodeId, key, boolValue);
				}
			}
		}

		// Handle added edges
		for (const auto &edge: addedEdges) {
			relationshipIndex_->addEdge(edge.getId(), edge.getFromNodeId(), edge.getToNodeId(), edge.getLabel());
		}

		// Handle updated edges - treat as remove + add
		for (const auto &edge: updatedEdges) {
			relationshipIndex_->removeEdge(edge.getId());
			relationshipIndex_->addEdge(edge.getId(), edge.getFromNodeId(), edge.getToNodeId(), edge.getLabel());
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
