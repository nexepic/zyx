/**
 * @file IndexBuilder.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/indexes/IndexBuilder.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/query/indexes/FullTextIndex.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/query/indexes/LabelIndex.hpp"
#include "graph/query/indexes/PropertyIndex.hpp"
#include "graph/query/indexes/RelationshipIndex.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::indexes {

	IndexBuilder::IndexBuilder(std::shared_ptr<IndexManager> indexManager,
							   std::shared_ptr<storage::FileStorage> storage) :
		indexManager_(std::move(indexManager)), storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {
	}

	IndexBuilder::~IndexBuilder() = default;

	bool IndexBuilder::buildAllIndexes() const {
		try {
			// Get index pointers
			auto labelIndex = indexManager_->getLabelIndex();
			auto propertyIndex = indexManager_->getPropertyIndex();
			auto relationshipIndex = indexManager_->getRelationshipIndex();
			auto fullTextIndex = indexManager_->getFullTextIndex();

			// Clear existing indexes
			labelIndex->clear();
			propertyIndex->clear();
			relationshipIndex->clear();
			fullTextIndex->clear();

			// Process all nodes
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, labelIndex, propertyIndex, fullTextIndex);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, labelIndex, propertyIndex, fullTextIndex);
				}
			}

			// Process all edges
			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processEdgeBatch(batchIds, relationshipIndex);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processEdgeBatch(batchIds, relationshipIndex);
				}
			}

			// Persist the indexes' state to disk
			indexManager_->persistState();
			return true;
		} catch ([[maybe_unused]] const std::exception &e) {
			// Log exception if needed
			return false;
		}
	}

	bool IndexBuilder::buildLabelIndex() const {
		try {
			// Clear existing label index
			auto labelIndex = indexManager_->getLabelIndex();
			labelIndex->clear();

			// Process nodes in batches
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, labelIndex, nullptr, nullptr);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, labelIndex, nullptr, nullptr);
				}
			}

			return true;
		} catch ([[maybe_unused]] const std::exception &e) {
			// Log exception if needed
			return false;
		}
	}

	bool IndexBuilder::buildPropertyIndex(const std::string &key) const {
		try {
			// Clear existing property index for this specific key
			auto propertyIndex = indexManager_->getPropertyIndex();
			propertyIndex->clearKey(key);

			// Process nodes in batches
			for (const auto &[startId, endId]: getNodeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, nullptr, propertyIndex, nullptr, key);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, nullptr, propertyIndex, nullptr, key);
				}
			}

			return true;
		} catch ([[maybe_unused]] const std::exception &e) {
			// Log exception if needed
			return false;
		}
	}

	void IndexBuilder::processNodeBatch(const std::vector<int64_t> &nodeIds,
										const std::shared_ptr<LabelIndex> &labelIndex,
										const std::shared_ptr<PropertyIndex> &propertyIndex,
										const std::shared_ptr<FullTextIndex> &fullTextIndex,
										const std::string &propertyKey) const {
		// Get nodes in batch to minimize memory usage
		auto nodeBatch = dataManager_->getNodeBatch(nodeIds);

		for (const auto &node: nodeBatch) {
			// Skip invalid or inactive nodes
			if (node.getId() == 0 || !node.isActive()) {
				continue;
			}

			int64_t nodeId = node.getId();

			// Add to label index if requested
			if (labelIndex) {
				const auto &label = node.getLabel();
				labelIndex->addNode(nodeId, label);
			}

			// Add to property index if requested
			if (propertyIndex) {
				auto properties = dataManager_->getNodeProperties(nodeId);

				if (propertyKey.empty()) {
					// Index all properties
					for (const auto &[key, value]: properties) {
						propertyIndex->addProperty(nodeId, key, value);

						// Also add to full-text index for string properties if requested
						if (fullTextIndex && std::holds_alternative<std::string>(value)) {
							std::string strValue = std::get<std::string>(value);
							fullTextIndex->addTextProperty(nodeId, key, strValue);
						}
					}
				} else {
					// Index specific property key
					auto it = properties.find(propertyKey);
					if (it != properties.end()) {
						propertyIndex->addProperty(nodeId, propertyKey, it->second);

						// Also add to full-text index for string properties if requested
						if (fullTextIndex && std::holds_alternative<std::string>(it->second)) {
							std::string strValue = std::get<std::string>(it->second);
							fullTextIndex->addTextProperty(nodeId, propertyKey, strValue);
						}
					}
				}
			}
		}
	}

	void IndexBuilder::processEdgeBatch(const std::vector<int64_t> &edgeIds,
										const std::shared_ptr<RelationshipIndex> &relationshipIndex) const {
		// Get edges in batch to minimize memory usage
		auto edgeBatch = dataManager_->getEdgeBatch(edgeIds);

		for (const auto &edge: edgeBatch) {
			// Skip invalid or inactive edges
			if (edge.getId() == 0 || !edge.isActive()) {
				continue;
			}

			// Add to relationship index
			relationshipIndex->addEdge(edge.getId(), edge.getSourceNodeId(), edge.getTargetNodeId(), edge.getLabel());
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
