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

namespace graph::query::indexes {

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

			for (const auto &[startId, endId]: getNodeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						// Pass nullptr for propertyIndex to only process labels
						processNodeBatch(batchIds, labelIndex, nullptr, "");
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, labelIndex, nullptr, "");
				}
			}
			labelIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildEdgeLabelIndex() const {
		try {
			auto labelIndex = indexManager_->getEdgeIndexManager()->getLabelIndex();
			// Clear existing data to rebuild
			labelIndex->clear();

			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						// Pass nullptr for propertyIndex to only process labels
						processEdgeBatch(batchIds, labelIndex, nullptr, "");
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processEdgeBatch(batchIds, labelIndex, nullptr, "");
				}
			}
			labelIndex->flush();
			return true;
		} catch (...) {
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndex(const std::string &key) const {
		try {
			auto propertyIndex = indexManager_->getNodeIndexManager()->getPropertyIndex();

			// CRITICAL CHANGE: Use clearIndexData instead of clearKey.
			// clearKey would remove the definition; clearIndexData only resets the tree/data.
			propertyIndex->clearIndexData(key);

			for (const auto &[startId, endId]: getNodeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, nullptr, propertyIndex, key);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, nullptr, propertyIndex, key);
				}
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

			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processEdgeBatch(batchIds, nullptr, propertyIndex, key);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processEdgeBatch(batchIds, nullptr, propertyIndex, key);
				}
			}
			return true;
		} catch (...) {
			return false;
		}
	}

	void IndexBuilder::processNodeBatch(const std::vector<int64_t> &nodeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey) const {
		auto nodeBatch = dataManager_->getNodeBatch(nodeIds);
		for (const auto &node: nodeBatch) {
			if (node.getId() == 0 || !node.isActive())
				continue;

			int64_t nodeId = node.getId();

			if (labelIndex) {
				if (node.getLabelId() != 0) {
					std::string labelStr = dataManager_->resolveLabel(node.getLabelId());
					if (!labelStr.empty()) {
						labelIndex->addNode(nodeId, labelStr);
					}
				}
			}

			if (propertyIndex) {
				auto properties = dataManager_->getNodeProperties(nodeId);
				if (propertyKey.empty()) { // Index all properties
					for (const auto &[key, value]: properties) {
						propertyIndex->addProperty(nodeId, key, value);
					}
				} else { // Index specific property
					if (auto it = properties.find(propertyKey); it != properties.end()) {
						propertyIndex->addProperty(nodeId, propertyKey, it->second);
					}
				}
			}
		}
	}

	void IndexBuilder::processEdgeBatch(const std::vector<int64_t> &edgeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::string &propertyKey) const {
		auto edgeBatch = dataManager_->getEdgeBatch(edgeIds);
		for (const auto &edge: edgeBatch) {
			if (edge.getId() == 0 || !edge.isActive())
				continue;

			int64_t edgeId = edge.getId();

			if (labelIndex) {
				if (edge.getLabelId() != 0) {
					std::string labelStr = dataManager_->resolveLabel(edge.getLabelId());
					if (!labelStr.empty()) {
						labelIndex->addNode(edgeId, labelStr);
					}
				}
			}

			if (propertyIndex) {
				auto properties = dataManager_->getEdgeProperties(edgeId);
				if (propertyKey.empty()) { // Index all properties
					for (const auto &[key, value]: properties) {
						propertyIndex->addProperty(edgeId, key, value);
					}
				} else { // Index specific property
					if (auto it = properties.find(propertyKey); it != properties.end()) {
						propertyIndex->addProperty(edgeId, propertyKey, it->second);
					}
				}
			}
		}
	}

	// TODO: Move these to a more appropriate place, like SegmentTracker or DataManager
	std::vector<std::pair<int64_t, int64_t>> IndexBuilder::getNodeIdRanges() const {
		std::vector<std::pair<int64_t, int64_t>> ranges;

		// Get all node segments
		auto nodeSegments = dataManager_->getSegmentTracker()->getSegmentsByType(Node::typeId);

		for (const auto &segment: nodeSegments) {
			if (segment.used > 0) {
				// Create a range from start_id to (start_id + used - 1)
				ranges.emplace_back(segment.start_id, segment.start_id + segment.used - 1);
			}
		}

		return ranges;
	}

	std::vector<std::pair<int64_t, int64_t>> IndexBuilder::getEdgeIdRanges() const {
		std::vector<std::pair<int64_t, int64_t>> ranges;

		// Get all edge segments
		auto edgeSegments = dataManager_->getSegmentTracker()->getSegmentsByType(Edge::typeId);

		for (const auto &segment: edgeSegments) {
			if (segment.used > 0) {
				// Create a range from start_id to (start_id + used - 1)
				ranges.emplace_back(segment.start_id, segment.start_id + segment.used - 1);
			}
		}

		return ranges;
	}

} // namespace graph::query::indexes
