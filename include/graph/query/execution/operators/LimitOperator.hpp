/**
 * @file LimitOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/22
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"

namespace graph::query::execution::operators {

    class LimitOperator : public PhysicalOperator {
    public:
        LimitOperator(std::unique_ptr<PhysicalOperator> child, int64_t limit)
            : child_(std::move(child)), limit_(limit) {}

        void open() override {
            count_ = 0;
            if(child_) child_->open();
        }

        std::optional<RecordBatch> next() override {
            // 1. Check if limit reached
            if (count_ >= limit_) return std::nullopt;

            // 2. Fetch from child
            auto batchOpt = child_->next();
            if (!batchOpt) return std::nullopt;

            RecordBatch& inputBatch = *batchOpt;

            // 3. Calculate how many to take
            int64_t remaining = limit_ - count_;

            // If the batch fits entirely within the remaining limit
            if (static_cast<int64_t>(inputBatch.size()) <= remaining) {
                count_ += inputBatch.size();
                return inputBatch; // Pass through efficiently
            }

            // Otherwise, slice the batch
            RecordBatch outputBatch;
            outputBatch.reserve(remaining);

            for (size_t i = 0; i < static_cast<size_t>(remaining); ++i) {
                outputBatch.push_back(std::move(inputBatch[i]));
            }

            count_ += remaining;
            return outputBatch;
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return child_->getOutputVariables();
        }

        [[nodiscard]] std::string toString() const override {
            return "Limit(" + std::to_string(limit_) + ")";
        }

        [[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
            return {child_.get()};
        }

    private:
        std::unique_ptr<PhysicalOperator> child_;
        int64_t limit_;
        int64_t count_ = 0;
    };

} // namespace graph::query::execution::operators