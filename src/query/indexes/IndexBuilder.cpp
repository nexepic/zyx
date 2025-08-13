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
#include <algorithm>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/query/indexes/LabelIndex.hpp"
#include "graph/query/indexes/PropertyIndex.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::indexes {

	IndexBuilder::IndexBuilder(std::shared_ptr<IndexManager> indexManager,
							   std::shared_ptr<storage::FileStorage> storage) :
		indexManager_(std::move(indexManager)), storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {
	}

	IndexBuilder::~IndexBuilder() = default;

	bool IndexBuilder::buildAllNodeIndexes() const {
		try {
			auto nodeIndexManager = indexManager_->getNodeIndexManager();
			auto labelIndex = nodeIndexManager->getLabelIndex();
			auto propertyIndex = nodeIndexManager->getPropertyIndex();

			labelIndex->clear();
			propertyIndex->clear();

			for (const auto &[startId, endId]: getNodeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, labelIndex, propertyIndex);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, labelIndex, propertyIndex);
				}
			}

			labelIndex->flush();
			propertyIndex->flush();

			return true;
		} catch (const std::exception &e) {
			std::cerr << "Error in buildAllNodeIndexes: " << e.what() << std::endl;
			return false;
		}
	}

	bool IndexBuilder::buildAllEdgeIndexes() const {
		try {
			auto edgeIndexManager = indexManager_->getEdgeIndexManager();
			auto labelIndex = edgeIndexManager->getLabelIndex();
			auto propertyIndex = edgeIndexManager->getPropertyIndex();

			labelIndex->clear();
			propertyIndex->clear();

			for (const auto &[startId, endId]: getEdgeIdRanges()) {
				std::vector<int64_t> batchIds;
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);
					if (batchIds.size() >= BATCH_SIZE) {
						processEdgeBatch(batchIds, labelIndex, propertyIndex);
						batchIds.clear();
					}
				}
				if (!batchIds.empty()) {
					processEdgeBatch(batchIds, labelIndex, propertyIndex);
				}
			}

			labelIndex->flush();
			propertyIndex->flush();

			return true;
		} catch (const std::exception &e) {
			std::cerr << "Error in buildAllEdgeIndexes: " << e.what() << std::endl;
			return false;
		}
	}

	bool IndexBuilder::buildNodePropertyIndex(const std::string &key) const {
		try {
			auto propertyIndex = indexManager_->getNodeIndexManager()->getPropertyIndex();
			propertyIndex->clearKey(key);

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
			propertyIndex->clearKey(key);

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
				labelIndex->addNode(nodeId, node.getLabel());
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
				labelIndex->addNode(edgeId, edge.getLabel()); // Use addNode for generality
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

		std::ranges::sort(ranges, [](const auto &a, const auto &b) { return a.first < b.first; });

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

		std::ranges::sort(ranges, [](const auto &a, const auto &b) { return a.first < b.first; });

		return ranges;
	}

} // namespace graph::query::indexes
