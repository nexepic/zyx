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
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include <unordered_map>

namespace graph::query::indexes {
namespace {
	std::string scopedPropertyKey(const std::string &label, const std::string &property) {
		return "__zyx_scoped_node_property__" + label + "__" + property;
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
			const std::string scopedKey = label.empty() ? "" : scopedPropertyKey(label, key);

			// CRITICAL CHANGE: Use clearIndexData instead of clearKey.
			// clearKey would remove the definition; clearIndexData only resets the tree/data.
			propertyIndex->clearIndexData(key);
			if (!scopedKey.empty()) {
				propertyIndex->createIndex(scopedKey);
				propertyIndex->clearIndexData(scopedKey);
			}

			std::vector<int64_t> batchIds;
			batchIds.reserve(BATCH_SIZE);
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() == BATCH_SIZE) {
						processNodeBatch(batchIds, nullptr, propertyIndex, key, scopedLabelId, scopedKey);
						batchIds.clear();
					}
				}
			}
			// Process remaining entities
			if (!batchIds.empty()) {
				processNodeBatch(batchIds, nullptr, propertyIndex, key, scopedLabelId, scopedKey);
			}
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
			return true;
		} catch (...) {
			return false;
		}
	}

	void IndexBuilder::processNodeBatch(const std::vector<int64_t> &nodeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey,
										int64_t scopedLabelId,
										const std::string &scopedPropertyKey) const {
		std::unordered_map<int64_t, std::vector<int64_t>> nodeIdsByLabelId;
		std::vector<std::tuple<int64_t, std::string, PropertyValue>> propertyEntries;
		if (propertyIndex) {
			propertyEntries.reserve(nodeIds.size() * (scopedPropertyKey.empty() ? 1 : 2));
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
					propertyEntries.emplace_back(nodeId, propertyKey, it->second);
					if (scopedLabelId != 0 && node.hasLabelId(scopedLabelId) && !scopedPropertyKey.empty()) {
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
