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
#include "graph/storage/DataManager.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/SegmentTracker.hpp"

namespace graph::query::indexes {

	IndexBuilder::IndexBuilder(std::shared_ptr<IndexManager> indexManager,
							   std::shared_ptr<storage::FileStorage> storage) :
		indexManager_(std::move(indexManager)), storage_(std::move(storage)), dataManager_(storage_->getDataManager()) {
	}

	IndexBuilder::~IndexBuilder() {
		cancel();
		if (buildTask_.valid()) {
			try {
				buildTask_.wait();
			} catch (...) {
				// Ignore exceptions during destruction
			}
		}
	}

	bool IndexBuilder::startBuildAllIndexes() {
		if (isBuilding()) {
			return false; // Already building an index
		}

		cancelRequested_ = false;
		status_ = IndexBuildStatus::IN_PROGRESS;
		progress_ = 0;

		buildTask_ = std::async(std::launch::async, &IndexBuilder::buildAllIndexesWorker, this);
		return true;
	}

	bool IndexBuilder::startBuildLabelIndex() {
		if (isBuilding()) {
			return false; // Already building an index
		}

		cancelRequested_ = false;
		status_ = IndexBuildStatus::IN_PROGRESS;
		progress_ = 0;

		buildTask_ = std::async(std::launch::async, &IndexBuilder::buildLabelIndexWorker, this);
		return true;
	}

	bool IndexBuilder::startBuildPropertyIndex(const std::string &key) {
		if (isBuilding()) {
			return false; // Already building an index
		}

		cancelRequested_ = false;
		status_ = IndexBuildStatus::IN_PROGRESS;
		progress_ = 0;

		buildTask_ = std::async(std::launch::async, &IndexBuilder::buildPropertyIndexWorker, this, key);
		return true;
	}

	bool IndexBuilder::isBuilding() const {
		return status_ == IndexBuildStatus::IN_PROGRESS && buildTask_.valid() &&
			   buildTask_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
	}

	IndexBuildStatus IndexBuilder::getStatus() const { return status_; }

	int IndexBuilder::getProgress() const { return progress_; }

	bool IndexBuilder::waitForCompletion(std::chrono::seconds timeout) const {
		if (!buildTask_.valid()) {
			return true; // No task running
		}

		auto result = buildTask_.wait_for(timeout);
		return result == std::future_status::ready;
	}

	void IndexBuilder::cancel() {
		if (isBuilding()) {
			cancelRequested_ = true;

			if (buildTask_.valid()) {
				try {
					buildTask_.wait();
				} catch ([[maybe_unused]] const std::exception &e) {
					// Log exception if needed
				}
			}

			status_ = IndexBuildStatus::FAILED;
		}
	}

	bool IndexBuilder::buildAllIndexesWorker() {
		try {
			// Clear existing indexes
			auto labelIndex = indexManager_->getLabelIndex();
			auto propertyIndex = indexManager_->getPropertyIndex();
			auto relationshipIndex = indexManager_->getRelationshipIndex();
			auto fullTextIndex = indexManager_->getFullTextIndex();

			labelIndex->clear();
			propertyIndex->clear();
			relationshipIndex->clear();
			fullTextIndex->clear();

			// Process nodes in batches
			auto nodeRanges = getNodeIdRanges();
			size_t totalRanges = nodeRanges.size();

			for (size_t i = 0; i < totalRanges && !cancelRequested_; i++) {
				const auto &[startId, endId] = nodeRanges[i];
				std::vector<int64_t> batchIds;

				// Collect valid IDs in this range
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);

					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, labelIndex, propertyIndex, fullTextIndex);
						batchIds.clear();
					}
				}

				// Process remaining nodes
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, labelIndex, propertyIndex, fullTextIndex);
				}

				// Update progress based on node processing
				updateProgress(static_cast<int>((i + 1) * 50 / totalRanges));

				if (cancelRequested_)
					break;
			}

			// Process edges in batches
			auto edgeRanges = getEdgeIdRanges();
			totalRanges = edgeRanges.size();

			for (size_t i = 0; i < totalRanges && !cancelRequested_; i++) {
				const auto &[startId, endId] = edgeRanges[i];
				std::vector<int64_t> batchIds;

				// Collect valid IDs in this range
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);

					if (batchIds.size() >= BATCH_SIZE) {
						processEdgeBatch(batchIds, relationshipIndex);
						batchIds.clear();
					}
				}

				// Process remaining edges
				if (!batchIds.empty()) {
					processEdgeBatch(batchIds, relationshipIndex);
				}

				// Update progress based on edge processing (50% to 100%)
				updateProgress(50 + static_cast<int>((i + 1) * 50 / totalRanges));

				if (cancelRequested_)
					break;
			}

			// Persist the indexes
			indexManager_->persistState();

			if (cancelRequested_) {
				status_ = IndexBuildStatus::FAILED;
				return false;
			} else {
				status_ = IndexBuildStatus::COMPLETED;
				progress_ = 100;
				return true;
			}
		} catch ([[maybe_unused]] const std::exception &e) {
			// Log exception if needed
			status_ = IndexBuildStatus::FAILED;
			return false;
		}
	}

	bool IndexBuilder::buildLabelIndexWorker() {
		try {
			// Clear existing label index
			auto labelIndex = indexManager_->getLabelIndex();
			labelIndex->clear();

			// Process nodes in batches
			auto nodeRanges = getNodeIdRanges();
			size_t totalRanges = nodeRanges.size();

			for (size_t i = 0; i < totalRanges && !cancelRequested_; i++) {
				const auto &[startId, endId] = nodeRanges[i];
				std::vector<int64_t> batchIds;

				// Collect valid IDs in this range
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);

					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, labelIndex, nullptr, nullptr);
						batchIds.clear();
					}
				}

				// Process remaining nodes
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, labelIndex, nullptr, nullptr);
				}

				// Update progress
				updateProgress(static_cast<int>((i + 1) * 100 / totalRanges));

				if (cancelRequested_)
					break;
			}

			if (cancelRequested_) {
				status_ = IndexBuildStatus::FAILED;
				return false;
			} else {
				status_ = IndexBuildStatus::COMPLETED;
				progress_ = 100;
				return true;
			}
		} catch ([[maybe_unused]] const std::exception &e) {
			// Log exception if needed
			status_ = IndexBuildStatus::FAILED;
			return false;
		}
	}

	bool IndexBuilder::buildPropertyIndexWorker(const std::string &key) {
		try {
			// Clear existing property index for this key
			auto propertyIndex = indexManager_->getPropertyIndex();
			propertyIndex->clearKey(key);

			// Process nodes in batches
			auto nodeRanges = getNodeIdRanges();
			size_t totalRanges = nodeRanges.size();

			for (size_t i = 0; i < totalRanges && !cancelRequested_; i++) {
				const auto &[startId, endId] = nodeRanges[i];
				std::vector<int64_t> batchIds;

				// Collect valid IDs in this range
				for (int64_t id = startId; id <= endId; id++) {
					batchIds.push_back(id);

					if (batchIds.size() >= BATCH_SIZE) {
						processNodeBatch(batchIds, nullptr, propertyIndex, nullptr, key);
						batchIds.clear();
					}
				}

				// Process remaining nodes
				if (!batchIds.empty()) {
					processNodeBatch(batchIds, nullptr, propertyIndex, nullptr, key);
				}

				// Update progress
				updateProgress(static_cast<int>((i + 1) * 100 / totalRanges));

				if (cancelRequested_)
					break;
			}

			if (cancelRequested_) {
				status_ = IndexBuildStatus::FAILED;
				return false;
			} else {
				status_ = IndexBuildStatus::COMPLETED;
				progress_ = 100;
				return true;
			}
		} catch ([[maybe_unused]] const std::exception &e) {
			// Log exception if needed
			status_ = IndexBuildStatus::FAILED;
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

	void IndexBuilder::updateProgress(int newProgress) {
		if (newProgress > progress_) {
			progress_ = newProgress;
		}
	}

} // namespace graph::query::indexes
