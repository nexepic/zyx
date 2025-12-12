/**
 * @file FilterOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/FilterOperator.hpp"

namespace graph::query::execution::operators {

    FilterOperator::FilterOperator(std::unique_ptr<PhysicalOperator> child,
                                   Predicate predicate,
                                   std::string predicateStr)
        : child_(std::move(child)),
          predicate_(std::move(predicate)),
          predicateStr_(std::move(predicateStr)) {}

    void FilterOperator::open() {
        if (child_) child_->open();
    }

    std::optional<RecordBatch> FilterOperator::next() {
        // Standard Filter Logic:
        // Keep pulling batches from child until we find matching records or child is exhausted.
        while (true) {
            auto batchOpt = child_->next();
            if (!batchOpt) return std::nullopt; // End of stream

            RecordBatch& inputBatch = *batchOpt;
            RecordBatch outputBatch;

            // Optimization: Reserve memory to avoid reallocations
            outputBatch.reserve(inputBatch.size());

            for (auto& record : inputBatch) {
                if (predicate_(record)) {
                    outputBatch.push_back(std::move(record));
                }
            }

            // If we found valid records, return them.
            if (!outputBatch.empty()) {
                return outputBatch;
            }

            // If outputBatch is empty, it means the entire input batch was filtered out.
            // We loop again immediately to fetch the next batch.
        }
    }

    void FilterOperator::close() {
        if (child_) child_->close();
    }

    std::vector<std::string> FilterOperator::getOutputVariables() const {
        return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
    }

    std::string FilterOperator::toString() const {
        return "Filter(" + predicateStr_ + ")";
    }

    std::vector<const PhysicalOperator*> FilterOperator::getChildren() const {
        if (child_) return {child_.get()};
        return {};
    }

} // namespace graph::query::execution::operators