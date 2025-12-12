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

namespace graph::query::execution::operators {

    void NodeScanOperator::open() {
        currentIdx_ = 0;
        candidateIds_.clear();

        // 1. Determine Candidate IDs
        switch (config_.type) {
            case ScanType::PROPERTY_SCAN:
                // Implicit Filtering: The Index returns ONLY IDs that match key=value.
                candidateIds_ = im_->findNodeIdsByProperty(config_.indexKey, config_.indexValue);
                break;

            case ScanType::LABEL_SCAN:
                // Implicit Filtering: The Index returns IDs with this label.
                candidateIds_ = im_->findNodeIdsByLabel(config_.label);
                break;

            case ScanType::FULL_SCAN:
            default:
                // No Filtering: Get all IDs.
                int64_t maxId = dm_->getIdAllocator()->getCurrentMaxNodeId();
                candidateIds_.reserve(maxId);
                for(int64_t i=1; i<=maxId; ++i) candidateIds_.push_back(i);
                break;
        }
    }

    std::optional<RecordBatch> NodeScanOperator::next() {
        if (currentIdx_ >= candidateIds_.size()) return std::nullopt;

        RecordBatch batch;
        batch.reserve(1000);

        while (batch.size() < 1000 && currentIdx_ < candidateIds_.size()) {
            int64_t id = candidateIds_[currentIdx_++];

            // 2. Load Header
            Node node = dm_->getNode(id);
            if (!node.isActive()) continue;

            // 3. Label Double-Check
            // Necessary for FULL_SCAN mode, or if Index is slightly stale.
            if (!config_.label.empty() && node.getLabel() != config_.label) {
                continue;
            }

            // 4. Hydrate Properties
            // MANDATORY: Subsequent FilterOperators depend on this data.
            auto props = dm_->getNodeProperties(id);
            node.setProperties(std::move(props));

            // REMOVED: Property Check Logic (moved to FilterOperator)
            // if (!key.empty() && node.prop[key] != value) continue; <--- DELETED

            Record r;
            r.setNode(config_.variable, node);
            batch.push_back(std::move(r));
        }

        if (batch.empty() && currentIdx_ >= candidateIds_.size()) return std::nullopt;
        return batch;
    }
}