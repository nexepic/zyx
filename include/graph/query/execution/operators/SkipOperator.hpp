/**
 * @file SkipOperator.hpp
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

    class SkipOperator : public PhysicalOperator {
    public:
        SkipOperator(std::unique_ptr<PhysicalOperator> child, int64_t offset)
            : child_(std::move(child)), offset_(offset) {}

        void open() override {
            skipped_ = 0;
            if(child_) child_->open();
        }

        std::optional<RecordBatch> next() override {
            while (true) {
                auto batchOpt = child_->next();
                if (!batchOpt) return std::nullopt; // End of stream

                RecordBatch& inputBatch = *batchOpt;

                // If we haven't skipped enough yet
                if (skipped_ < offset_) {
                    int64_t neededToSkip = offset_ - skipped_;

                    // Case 1: Entire batch needs to be skipped
                    if (static_cast<int64_t>(inputBatch.size()) <= neededToSkip) {
                        skipped_ += inputBatch.size();
                        continue; // Loop to fetch next batch
                    }

                    // Case 2: Partial skip (we need data from this batch)
                    RecordBatch outputBatch;
                    // Start from the first item after the skipped ones
                    for (size_t i = neededToSkip; i < inputBatch.size(); ++i) {
                        outputBatch.push_back(std::move(inputBatch[i]));
                    }
                    skipped_ += neededToSkip; // Now skipped_ == offset_
                    return outputBatch;
                }

                // If we are done skipping, just pass through
                return inputBatch;
            }
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return child_->getOutputVariables();
        }

        [[nodiscard]] std::string toString() const override {
            return "Skip(" + std::to_string(offset_) + ")";
        }

        [[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
            return {child_.get()};
        }

    private:
        std::unique_ptr<PhysicalOperator> child_;
        int64_t offset_;
        int64_t skipped_ = 0;
    };

} // namespace graph::query::execution::operators