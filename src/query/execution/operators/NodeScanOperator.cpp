/**
 * @file NodeScanOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include <algorithm>

namespace graph::query::execution::operators {

    NodeScanOperator::NodeScanOperator(std::shared_ptr<storage::DataManager> dataManager,
                                       std::shared_ptr<indexes::IndexManager> indexManager,
                                       std::string variableName,
                                       std::string label)
        : dataManager_(std::move(dataManager)),
          indexManager_(std::move(indexManager)),
          variableName_(std::move(variableName)),
          label_(std::move(label)),
          currentIndex_(0),
          useIndex_(false) {}

    void NodeScanOperator::open() {
        currentIndex_ = 0;
        candidateIds_.clear();

        // Optimizer Logic: Determine access path at runtime (Open phase)
        bool hasLabelIndex = !label_.empty() && indexManager_->hasLabelIndex("node");

        if (hasLabelIndex) {
            prepareIndexScan();
        } else {
            prepareFullScan();
        }
    }

    void NodeScanOperator::prepareIndexScan() {
        useIndex_ = true;
        candidateIds_ = indexManager_->findNodeIdsByLabel(label_);
    }

    void NodeScanOperator::prepareFullScan() {
        useIndex_ = false;
        // In a real scenario, you might not load ALL IDs into memory here.
        // You would use an iterator from DataManager/SegmentTracker.
        // For this refactoring, we simulate it by fetching the ID range.
        int64_t maxId = dataManager_->getIdAllocator()->getCurrentMaxNodeId();
        // Pre-allocate logic or iterator setup goes here
        // Simplified:
        for (int64_t i = 1; i <= maxId; ++i) {
            candidateIds_.push_back(i);
        }
    }

    std::optional<RecordBatch> NodeScanOperator::next() {
        if (currentIndex_ >= candidateIds_.size()) {
            return std::nullopt; // End of stream
        }

        RecordBatch batch;
        batch.reserve(BATCH_SIZE);

        // Fetch loop
        while (batch.size() < BATCH_SIZE && currentIndex_ < candidateIds_.size()) {
            // Determine range for bulk fetch
            size_t end = std::min(currentIndex_ + BATCH_SIZE, candidateIds_.size());
            std::vector<int64_t> idsToFetch;
            idsToFetch.reserve(end - currentIndex_);

            for (size_t i = currentIndex_; i < end; ++i) {
                idsToFetch.push_back(candidateIds_[i]);
            }

            // Bulk fetch from DataManager
            auto nodes = dataManager_->getNodeBatch(idsToFetch);
            currentIndex_ = end;

            for (const auto& node : nodes) {
                if (!node.isActive()) continue;

                // If doing full scan, we must filter by label manually
                if (!useIndex_ && !label_.empty() && node.getLabel() != label_) {
                    continue;
                }

                // Create Record
                Record record;
                record.setNode(variableName_, node);
                batch.push_back(std::move(record));
            }
        }

        // If we exhausted the candidates but produced no results (all filtered),
        // we might need to return an empty batch or std::nullopt.
        // Convention: Return empty batch if stream isn't done but this chunk was empty,
        // or recursively fetch. Here we return what we have.
        if (batch.empty() && currentIndex_ >= candidateIds_.size()) {
            return std::nullopt;
        }

        return batch;
    }

    void NodeScanOperator::close() {
        candidateIds_.clear();
    }

    std::vector<std::string> NodeScanOperator::getOutputVariables() const {
        return {variableName_};
    }

} // namespace graph::query::execution::operators