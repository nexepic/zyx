/**
 * @file IndexBuilder.cpp
 * @author Nexepic
 * @date 2025/6/24
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

#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/PropertyIndexBuildScanner.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include "graph/storage/indexes/ScopedNodePropertyKey.hpp"
#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace graph::query::indexes {
namespace {

	struct PreparedNodePropertyIndexSpec {
		std::string propertyKey;
		std::string label;
		std::string physicalKey;
		int64_t scopedLabelId = 0;
		bool buildGlobalProperty = true;
	};

	PropertyIndex::TypedPropertyEntry makeTypedPropertyEntry(
			const storage::PropertyEntityOwnerScalarKeyValue &ownerValue,
			const std::string &physicalKey) {
		PropertyIndex::TypedPropertyEntry entry;
		entry.entityId = ownerValue.ownerId;
		entry.key = physicalKey;
		entry.type = ownerValue.type;
		entry.boolValue = ownerValue.boolValue;
		entry.intValue = ownerValue.intValue;
		entry.doubleValue = ownerValue.doubleValue;
		entry.stringValue = ownerValue.stringValue;
		return entry;
	}

	bool nodeValueMatchesSpec(
			const storage::PropertyEntityOwnerScalarKeyValue &ownerValue,
			const PreparedNodePropertyIndexSpec &spec,
			const std::unordered_map<int64_t, std::unordered_set<int64_t>> &scopedNodeIdsByLabel) {
		if (ownerValue.key != spec.propertyKey) {
			return false;
		}
		if (spec.buildGlobalProperty) {
			return true;
		}
		if (spec.scopedLabelId == 0) {
			return false;
		}
		const auto scopedIt = scopedNodeIdsByLabel.find(spec.scopedLabelId);
		return scopedIt != scopedNodeIdsByLabel.end() && scopedIt->second.contains(ownerValue.ownerId);
	}

	std::vector<int64_t> collectActiveNodeIds(
			const storage::DataManager &dataManager,
			const std::vector<std::pair<int64_t, int64_t>> &ranges) {
		std::vector<int64_t> ids;
		for (const auto &[startId, endId]: ranges) {
			if (endId < startId) {
				continue;
			}
			ids.reserve(ids.size() + static_cast<size_t>(endId - startId + 1));
			for (int64_t id = startId; id <= endId; ++id) {
				Node node = dataManager.getNode(id);
				if (node.getId() != 0 && node.isActive()) {
					ids.push_back(node.getId());
				}
			}
		}
		std::ranges::sort(ids);
		ids.erase(std::ranges::unique(ids).begin(), ids.end());
		return ids;
	}

	std::vector<int64_t> collectActiveEdgeIds(
			const storage::DataManager &dataManager,
			const std::vector<std::pair<int64_t, int64_t>> &ranges) {
		std::vector<int64_t> ids;
		for (const auto &[startId, endId]: ranges) {
			if (endId < startId) {
				continue;
			}
			ids.reserve(ids.size() + static_cast<size_t>(endId - startId + 1));
			for (int64_t id = startId; id <= endId; ++id) {
				Edge edge = dataManager.getEdge(id);
				if (edge.getId() != 0 && edge.isActive()) {
					ids.push_back(edge.getId());
				}
			}
		}
		std::ranges::sort(ids);
		ids.erase(std::ranges::unique(ids).begin(), ids.end());
		return ids;
	}

} // namespace

	IndexBuilder::IndexBuilder(std::shared_ptr<IndexManager> indexManager,
							   std::shared_ptr<storage::FileStorage> storage) :
		indexManager_(std::move(indexManager)), storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {
	}

	IndexBuilder::~IndexBuilder() = default;

	bool IndexBuilder::buildNodeLabelIndex() const {
		try {
			auto labelIndex = indexManager_->getNodeIndexManager()->getLabelIndex();
			// Clear existing data to rebuild
			labelIndex->clear();

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						// Pass nullptr for propertyIndex to only process labels
						processNodeBatch(batchIds, labelIndex, nullptr, "");
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processNodeBatch(batchIds, labelIndex, nullptr, "");
			}
			labelIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildEdgeTypeIndex() const {
		try {
			auto labelIndex = indexManager_->getEdgeIndexManager()->getLabelIndex();
			// Clear existing data to rebuild
			labelIndex->clear();

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						// Pass nullptr for propertyIndex to only process labels
						processEdgeBatch(batchIds, labelIndex, nullptr, "");
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processEdgeBatch(batchIds, labelIndex, nullptr, "");
			}
			labelIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndex(const std::string &key) const {
		return buildNodePropertyIndex(key, "");
	}

	bool IndexBuilder::buildNodePropertyIndex(const std::string &key, const std::string &label) const {
		try {
			auto propertyIndex = indexManager_->getNodeIndexManager()->getPropertyIndex();
			const int64_t scopedLabelId = label.empty() ? 0 : dataManager_->resolveTokenId(label);
			const std::string scopedKey = label.empty() ? "" : makeScopedNodePropertyKey(label, key);
			const bool buildGlobalProperty = label.empty();

			// CRITICAL CHANGE: Use clearIndexData instead of clearKey.
			// clearKey would remove the definition; clearIndexData only resets the tree/data.
			if (buildGlobalProperty) {
				propertyIndex->clearIndexData(key);
			}
			if (!scopedKey.empty()) {
				propertyIndex->createIndex(scopedKey);
				propertyIndex->clearIndexData(scopedKey);
			}

			if (buildNodePropertyIndexFromOwnerScan(propertyIndex, key, scopedLabelId, scopedKey, buildGlobalProperty)) {
				propertyIndex->flush();
				return true;
			}

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						processNodeBatch(batchIds, nullptr, propertyIndex, key, scopedLabelId, scopedKey,
										 buildGlobalProperty);
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processNodeBatch(batchIds, nullptr, propertyIndex, key, scopedLabelId, scopedKey, buildGlobalProperty);
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndexes(const std::vector<NodePropertyIndexBuildSpec> &specs) const {
		try {
			auto propertyIndex = indexManager_->getNodeIndexManager()->getPropertyIndex();
			std::vector<PreparedNodePropertyIndexSpec> prepared;
			prepared.reserve(specs.size());
			std::vector<std::string> requestedKeys;
			requestedKeys.reserve(specs.size());

			for (const auto &spec: specs) {
				if (spec.propertyKey.empty()) {
					continue;
				}

				PreparedNodePropertyIndexSpec preparedSpec;
				preparedSpec.propertyKey = spec.propertyKey;
				preparedSpec.label = spec.label;
				preparedSpec.scopedLabelId = spec.label.empty() ? 0 : dataManager_->resolveTokenId(spec.label);
				preparedSpec.buildGlobalProperty = spec.label.empty();
				preparedSpec.physicalKey = preparedSpec.buildGlobalProperty
												   ? preparedSpec.propertyKey
												   : makeScopedNodePropertyKey(spec.label, spec.propertyKey);

				propertyIndex->createIndex(preparedSpec.physicalKey);
				propertyIndex->clearIndexData(preparedSpec.physicalKey);
				if (std::find(requestedKeys.begin(), requestedKeys.end(), preparedSpec.propertyKey) ==
					requestedKeys.end()) {
					requestedKeys.push_back(preparedSpec.propertyKey);
				}
				prepared.push_back(std::move(preparedSpec));
			}

			if (prepared.empty()) {
				return true;
			}

			if (!dataManager_->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node)) {
				for (const auto &spec: prepared) {
					if (!buildNodePropertyIndex(spec.propertyKey, spec.label)) {
						return false;
					}
				}
				return true;
			}

			const bool typedScanCoversAllProperties =
					dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Node);

			const auto activeNodeIds = collectActiveNodeIds(*dataManager_, getNodeIdRanges());
			if (activeNodeIds.empty()) {
				propertyIndex->flush();
				return true;
			}

			std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
			{
				debug::ScopedPerfTimer timer("index_build.node_property.typed_scan");
				storage::PropertyIndexBuildScanner scanner(*dataManager_);
				values = scanner.collect(
						EntityType::Node,
						requestedKeys,
						std::span<const int64_t>(activeNodeIds.data(), activeNodeIds.size()),
						storage_->getThreadPool());
			}

			std::unordered_map<int64_t, std::unordered_set<int64_t>> scopedNodeIdsByLabel;
			if (!values.empty()) {
				debug::ScopedPerfTimer timer("index_build.node_property.typed_label_filter");
				for (const auto &spec: prepared) {
					if (!spec.buildGlobalProperty && spec.scopedLabelId != 0 &&
						!scopedNodeIdsByLabel.contains(spec.scopedLabelId)) {
						scopedNodeIdsByLabel.emplace(spec.scopedLabelId, collectNodeIdsWithLabel(spec.scopedLabelId));
					}
				}
			}

			std::vector<PropertyIndex::TypedPropertyEntry> propertyEntries;
			propertyEntries.reserve(values.size());
			for (const auto &ownerValue: values) {
				for (const auto &spec: prepared) {
					if (nodeValueMatchesSpec(ownerValue, spec, scopedNodeIdsByLabel)) {
						propertyEntries.push_back(makeTypedPropertyEntry(ownerValue, spec.physicalKey));
					}
				}
			}

			if (!propertyEntries.empty()) {
				debug::ScopedPerfTimer timer("index_build.node_property.typed_insert");
				propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
			}
			if (!typedScanCoversAllProperties) {
				debug::ScopedPerfTimer timer("index_build.node_property.fallback_blob_or_inline");
				std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
				fallbackEntries.reserve(prepared.size());
				for (const auto &[startId, endId]: getNodeIdRanges()) {
					for (int64_t id = startId; id <= endId; ++id) {
						Node node = dataManager_->getNode(id);
						if (node.getId() == 0 || !node.isActive() ||
							node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
							continue;
						}
						const auto properties = dataManager_->getNodeProperties(node.getId());
						if (properties.empty()) {
							continue;
						}
						for (const auto &spec: prepared) {
							auto propIt = properties.find(spec.propertyKey);
							if (propIt == properties.end()) {
								continue;
							}
							if (spec.buildGlobalProperty) {
								fallbackEntries.emplace_back(node.getId(), spec.physicalKey, propIt->second);
							} else if (spec.scopedLabelId != 0 && node.hasLabelId(spec.scopedLabelId)) {
								fallbackEntries.emplace_back(node.getId(), spec.physicalKey, propIt->second);
							}
						}
					}
				}
				if (!fallbackEntries.empty()) {
					propertyIndex->addPropertiesBatch(fallbackEntries);
				}
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	// Specialized builder for a single edge property
	bool IndexBuilder::buildEdgePropertyIndex(const std::string &key) const {
		try {
			auto propertyIndex = indexManager_->getEdgeIndexManager()->getPropertyIndex();

			// CRITICAL CHANGE: Use clearIndexData instead of clearKey.
			propertyIndex->clearIndexData(key);

			if (buildEdgePropertyIndexFromOwnerScan(propertyIndex, key)) {
				propertyIndex->flush();
				return true;
			}

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						processEdgeBatch(batchIds, nullptr, propertyIndex, key);
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processEdgeBatch(batchIds, nullptr, propertyIndex, key);
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildEdgePropertyIndexes(const std::vector<std::string> &keys) const {
		try {
			auto propertyIndex = indexManager_->getEdgeIndexManager()->getPropertyIndex();
			std::vector<std::string> requestedKeys;
			requestedKeys.reserve(keys.size());
			for (const auto &key: keys) {
				if (key.empty() || std::find(requestedKeys.begin(), requestedKeys.end(), key) != requestedKeys.end()) {
					continue;
				}
				propertyIndex->createIndex(key);
				propertyIndex->clearIndexData(key);
				requestedKeys.push_back(key);
			}

			if (requestedKeys.empty()) {
				return true;
			}

			if (!dataManager_->canCountPropertyEntityPredicatesByOwnerType(EntityType::Edge)) {
				for (const auto &key: requestedKeys) {
					if (!buildEdgePropertyIndex(key)) {
						return false;
					}
				}
				return true;
			}

			const bool typedScanCoversAllProperties =
					dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Edge);

			const auto activeEdgeIds = collectActiveEdgeIds(*dataManager_, getEdgeIdRanges());
			if (activeEdgeIds.empty()) {
				propertyIndex->flush();
				return true;
			}

			std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
			{
				debug::ScopedPerfTimer timer("index_build.edge_property.typed_scan");
				storage::PropertyIndexBuildScanner scanner(*dataManager_);
				values = scanner.collect(
						EntityType::Edge,
						requestedKeys,
						std::span<const int64_t>(activeEdgeIds.data(), activeEdgeIds.size()),
						storage_->getThreadPool());
			}
			std::vector<PropertyIndex::TypedPropertyEntry> propertyEntries;
			propertyEntries.reserve(values.size());
			for (const auto &ownerValue: values) {
				propertyEntries.push_back(makeTypedPropertyEntry(ownerValue, ownerValue.key));
			}
			if (!propertyEntries.empty()) {
				debug::ScopedPerfTimer timer("index_build.edge_property.typed_insert");
				propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
			}
			if (!typedScanCoversAllProperties) {
				debug::ScopedPerfTimer timer("index_build.edge_property.fallback_blob_or_inline");
				std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
				fallbackEntries.reserve(requestedKeys.size());
				for (const auto &[startId, endId]: getEdgeIdRanges()) {
					for (int64_t id = startId; id <= endId; ++id) {
						Edge edge = dataManager_->getEdge(id);
						if (edge.getId() == 0 || !edge.isActive() ||
							edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
							continue;
						}
						const auto properties = dataManager_->getEdgeProperties(edge.getId());
						if (properties.empty()) {
							continue;
						}
						for (const auto &key: requestedKeys) {
							auto propIt = properties.find(key);
							if (propIt != properties.end()) {
								fallbackEntries.emplace_back(edge.getId(), key, propIt->second);
							}
						}
					}
				}
				if (!fallbackEntries.empty()) {
					propertyIndex->addPropertiesBatch(fallbackEntries);
				}
			}
			propertyIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndexFromOwnerScan(
			const std::shared_ptr<PropertyIndex> &propertyIndex,
			const std::string &propertyKey,
			int64_t scopedLabelId,
			const std::string &scopedPropertyKey,
			bool buildGlobalProperty) const {
		if (!propertyIndex || propertyKey.empty() ||
			!dataManager_->canCountPropertyEntityPredicatesByOwnerType(EntityType::Node)) {
			return false;
		}

		const bool buildScopedEntries = scopedLabelId != 0 && !scopedPropertyKey.empty();
		if (!buildGlobalProperty && !buildScopedEntries) {
			return true;
		}
		const bool typedScanCoversAllProperties =
				dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Node);

		const auto activeNodeIds = collectActiveNodeIds(*dataManager_, getNodeIdRanges());
		if (activeNodeIds.empty()) {
			return true;
		}

		std::unordered_set<int64_t> scopedNodeIds;
		if (buildScopedEntries) {
			scopedNodeIds = collectNodeIdsWithLabel(scopedLabelId);
		}

		std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
		{
			debug::ScopedPerfTimer timer("index_build.node_property.typed_scan");
			storage::PropertyIndexBuildScanner scanner(*dataManager_);
			values = scanner.collect(
					EntityType::Node,
					std::vector<std::string>{propertyKey},
					std::span<const int64_t>(activeNodeIds.data(), activeNodeIds.size()),
					storage_->getThreadPool());
		}

		std::vector<PropertyIndex::TypedPropertyEntry> propertyEntries;
		propertyEntries.reserve(values.size());
		const std::string &physicalKey = buildGlobalProperty ? propertyKey : scopedPropertyKey;
		for (const auto &ownerValue: values) {
			if (!buildGlobalProperty && (!buildScopedEntries || !scopedNodeIds.contains(ownerValue.ownerId))) {
				continue;
			}
			propertyEntries.push_back(makeTypedPropertyEntry(ownerValue, physicalKey));
		}
		if (!propertyEntries.empty()) {
			debug::ScopedPerfTimer timer("index_build.node_property.typed_insert");
			propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
		}

		if (!typedScanCoversAllProperties) {
			debug::ScopedPerfTimer timer("index_build.node_property.fallback_blob_or_inline");
			std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
			fallbackEntries.reserve(values.size());
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				for (int64_t id = startId; id <= endId; ++id) {
					Node node = dataManager_->getNode(id);
					if (node.getId() == 0 || !node.isActive() ||
						node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
						continue;
					}
					if (!buildGlobalProperty && !node.hasLabelId(scopedLabelId)) {
						continue;
					}
					const auto properties = dataManager_->getNodeProperties(node.getId());
					if (auto propIt = properties.find(propertyKey); propIt != properties.end()) {
						fallbackEntries.emplace_back(node.getId(), physicalKey, propIt->second);
					}
				}
			}
			if (!fallbackEntries.empty()) {
				propertyIndex->addPropertiesBatch(fallbackEntries);
			}
		}
		return true;
	}

	bool IndexBuilder::buildEdgePropertyIndexFromOwnerScan(
			const std::shared_ptr<PropertyIndex> &propertyIndex,
			const std::string &propertyKey) const {
		if (!propertyIndex || propertyKey.empty() ||
			!dataManager_->canCountPropertyEntityPredicatesByOwnerType(EntityType::Edge)) {
			return false;
		}

		const bool typedScanCoversAllProperties =
				dataManager_->canCountAllPropertyPredicatesByOwnerType(EntityType::Edge);

		const auto activeEdgeIds = collectActiveEdgeIds(*dataManager_, getEdgeIdRanges());
		if (activeEdgeIds.empty()) {
			return true;
		}

		std::vector<storage::PropertyEntityOwnerScalarKeyValue> values;
		{
			debug::ScopedPerfTimer timer("index_build.edge_property.typed_scan");
			storage::PropertyIndexBuildScanner scanner(*dataManager_);
			values = scanner.collect(
					EntityType::Edge,
					std::vector<std::string>{propertyKey},
					std::span<const int64_t>(activeEdgeIds.data(), activeEdgeIds.size()),
					storage_->getThreadPool());
		}

		std::vector<PropertyIndex::TypedPropertyEntry> propertyEntries;
		propertyEntries.reserve(values.size());
		for (const auto &ownerValue: values) {
			propertyEntries.push_back(makeTypedPropertyEntry(ownerValue, propertyKey));
		}
		if (!propertyEntries.empty()) {
			debug::ScopedPerfTimer timer("index_build.edge_property.typed_insert");
			propertyIndex->addTypedPropertiesBatch(std::move(propertyEntries));
		}

		if (!typedScanCoversAllProperties) {
			debug::ScopedPerfTimer timer("index_build.edge_property.fallback_blob_or_inline");
			std::vector<std::tuple<int64_t, std::string, PropertyValue>> fallbackEntries;
			fallbackEntries.reserve(values.size());
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				for (int64_t id = startId; id <= endId; ++id) {
					Edge edge = dataManager_->getEdge(id);
					if (edge.getId() == 0 || !edge.isActive() ||
						edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
						continue;
					}
					const auto properties = dataManager_->getEdgeProperties(edge.getId());
					if (auto propIt = properties.find(propertyKey); propIt != properties.end()) {
						fallbackEntries.emplace_back(edge.getId(), propertyKey, propIt->second);
					}
				}
			}
			if (!fallbackEntries.empty()) {
				propertyIndex->addPropertiesBatch(fallbackEntries);
			}
		}
		return true;
	}

	std::unordered_set<int64_t> IndexBuilder::collectNodeIdsWithLabel(int64_t labelId) const {
		std::unordered_set<int64_t> nodeIds;
		if (labelId == 0) {
			return nodeIds;
		}
		const auto label = dataManager_->resolveTokenName(labelId);
		auto labelIndex = indexManager_->getNodeIndexManager()->getLabelIndex();
		if (!label.empty() && labelIndex && labelIndex->hasPhysicalData()) {
			const auto indexedNodeIds = labelIndex->findNodes(label);
			nodeIds.reserve(indexedNodeIds.size());
			nodeIds.insert(indexedNodeIds.begin(), indexedNodeIds.end());
			return nodeIds;
		}

		for (const auto &[startId, endId]: getNodeIdRanges()) {
			for (int64_t id = startId; id <= endId; ++id) {
				Node node = dataManager_->getNode(id);
				if (node.getId() != 0 && node.isActive() && node.hasLabelId(labelId)) {
					nodeIds.insert(node.getId());
				}
			}
		}
		return nodeIds;
	}

	void IndexBuilder::processNodeBatch(const std::vector<int64_t> &nodeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey,
										int64_t scopedLabelId,
										const std::string &scopedPropertyKey,
										bool buildGlobalProperty) const {
		std::unordered_map<int64_t, std::vector<int64_t>> nodeIdsByLabelId;
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propertyEntries;
		if (propertyIndex) {
			propertyEntries.reserve(nodeIds.size());
		}

		for (int64_t id: nodeIds) {
			Node node = dataManager_->getNode(id);
			if (node.getId() == 0 || !node.isActive())
				continue;

			int64_t nodeId = node.getId();

			if (labelIndex) {
				for (const int64_t labelId : node.getLabelIds()) {
					if (labelId != 0) {
						nodeIdsByLabelId[labelId].push_back(nodeId);
					}
				}
			}

			if (propertyIndex) {
				auto properties = dataManager_->getNodeProperties(nodeId);
				// Index specific property
				if (auto it = properties.find(propertyKey); it != properties.end()) {
					if (buildGlobalProperty) {
						propertyEntries.emplace_back(nodeId, propertyKey, it->second);
					} else if (scopedLabelId != 0 && node.hasLabelId(scopedLabelId) && !scopedPropertyKey.empty()) {
						propertyEntries.emplace_back(nodeId, scopedPropertyKey, it->second);
					}
				}
			}
		}

		if (propertyIndex && !propertyEntries.empty()) {
			propertyIndex->addPropertiesBatch(propertyEntries);
		}

		if (labelIndex && !nodeIdsByLabelId.empty()) {
			std::unordered_map<std::string, std::vector<int64_t>> nodeIdsByLabel;
			nodeIdsByLabel.reserve(nodeIdsByLabelId.size());
			for (auto &[labelId, ids] : nodeIdsByLabelId) {
				auto label = dataManager_->resolveTokenName(labelId);
				if (!label.empty()) {
					nodeIdsByLabel.emplace(std::move(label), std::move(ids));
				}
			}
			if (!nodeIdsByLabel.empty()) {
				labelIndex->addNodesBatch(nodeIdsByLabel);
			}
		}
	}

	void IndexBuilder::processEdgeBatch(const std::vector<int64_t> &edgeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey) const {
		std::unordered_map<int64_t, std::vector<int64_t>> edgeIdsByTypeId;
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propertyEntries;
		if (propertyIndex) {
			propertyEntries.reserve(edgeIds.size());
		}

		for (int64_t id: edgeIds) {
			Edge edge = dataManager_->getEdge(id);
			if (edge.getId() == 0 || !edge.isActive())
				continue;

			int64_t edgeId = edge.getId();

			if (labelIndex) {
				if (edge.getTypeId() != 0) {
					edgeIdsByTypeId[edge.getTypeId()].push_back(edgeId);
				}
			}

			if (propertyIndex) {
				auto properties = dataManager_->getEdgeProperties(edgeId);
				// Index specific property
				if (auto it = properties.find(propertyKey); it != properties.end()) {
					propertyEntries.emplace_back(edgeId, propertyKey, it->second);
				}
			}
		}

		if (propertyIndex && !propertyEntries.empty()) {
			propertyIndex->addPropertiesBatch(propertyEntries);
		}

		if (labelIndex && !edgeIdsByTypeId.empty()) {
			std::unordered_map<std::string, std::vector<int64_t>> edgeIdsByType;
			edgeIdsByType.reserve(edgeIdsByTypeId.size());
			for (auto &[typeId, ids] : edgeIdsByTypeId) {
				auto type = dataManager_->resolveTokenName(typeId);
				if (!type.empty()) {
					edgeIdsByType.emplace(std::move(type), std::move(ids));
				}
			}
			if (!edgeIdsByType.empty()) {
				labelIndex->addNodesBatch(edgeIdsByType);
			}
		}
	}

	// TODO: Move these to a more appropriate place, like SegmentTracker or DataManager
	std::vector<std::pair<int64_t, int64_t>> IndexBuilder::getNodeIdRanges() const {
		std::vector<std::pair<int64_t, int64_t>> ranges;

		// Get all node segments
		auto nodeSegments = dataManager_->getSegmentTracker()->getSegmentsByType(Node::typeId);

		for (const auto &segment: nodeSegments) {
			// Create a range from start_id to (start_id + used - 1)
			ranges.emplace_back(segment.start_id, segment.start_id + segment.used - 1);
		}

		return ranges;
	}

	std::vector<std::pair<int64_t, int64_t>> IndexBuilder::getEdgeIdRanges() const {
		std::vector<std::pair<int64_t, int64_t>> ranges;

		// Get all edge segments
		auto edgeSegments = dataManager_->getSegmentTracker()->getSegmentsByType(Edge::typeId);

		for (const auto &segment: edgeSegments) {
			// Create a range from start_id to (start_id + used - 1)
			ranges.emplace_back(segment.start_id, segment.start_id + segment.used - 1);
		}

		return ranges;
	}

} // namespace graph::query::indexes
