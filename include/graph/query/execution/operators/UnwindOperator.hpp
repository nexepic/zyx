/**
 * @file UnwindOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include <vector>
#include <string>

namespace graph::query::execution::operators {

    class UnwindOperator : public PhysicalOperator {
    public:
        UnwindOperator(std::unique_ptr<PhysicalOperator> child,
                       std::string alias,
                       std::vector<PropertyValue> list)
            : child_(std::move(child)), alias_(std::move(alias)), list_(std::move(list)) {}

        void open() override {
            if (child_) child_->open();

            // Reset state
            currentChildBatch_ = std::nullopt;
            childRecordIndex_ = 0;
            listIndex_ = 0;
        }

        std::optional<RecordBatch> next() override {
            RecordBatch outputBatch;
            outputBatch.reserve(DEFAULT_BATCH_SIZE);

            // ====================================================
            // CASE A: Source Mode (No upstream child)
            // UNWIND [1,2] AS x
            // ====================================================
            if (!child_) {
                if (listIndex_ >= list_.size()) return std::nullopt;

                while (outputBatch.size() < DEFAULT_BATCH_SIZE && listIndex_ < list_.size()) {
                    Record r;
                    r.setValue(alias_, list_[listIndex_++]);
                    outputBatch.push_back(std::move(r));
                }
                return outputBatch;
            }

            // ====================================================
            // CASE B: Pipeline Mode (Has upstream child)
            // MATCH (n) UNWIND [1,2] AS x RETURN n, x
            // Logic: For each Input Record, emit N records (one for each list item)
            // ====================================================
            while (outputBatch.size() < DEFAULT_BATCH_SIZE) {

                // 1. Fetch next input batch if needed
                if (!currentChildBatch_ || childRecordIndex_ >= currentChildBatch_->size()) {
                    currentChildBatch_ = child_->next();

                    if (!currentChildBatch_) {
                        // Upstream exhausted
                        if (!outputBatch.empty()) return outputBatch;
                        return std::nullopt;
                    }

                    // Reset counters for the new batch
                    childRecordIndex_ = 0;
                    listIndex_ = 0;
                }

                // 2. Process current input record
                const auto& inputRecord = (*currentChildBatch_)[childRecordIndex_];

                // 3. Expand list for this record
                while (listIndex_ < list_.size() && outputBatch.size() < DEFAULT_BATCH_SIZE) {
                    // Copy original record (keep 'u')
                    Record expandedRecord = inputRecord;

                    // Add new variable ('tag')
                    expandedRecord.setValue(alias_, list_[listIndex_]);

                    outputBatch.push_back(std::move(expandedRecord));
                    listIndex_++;
                }

                // 4. Check if we finished the list for the current record
                if (listIndex_ >= list_.size()) {
                    childRecordIndex_++; // Move to next input record
                    listIndex_ = 0;      // Reset list for the next record
                }
            }

            return outputBatch;
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
            vars.push_back(alias_);
            return vars;
        }

        [[nodiscard]] std::string toString() const override {
            return "Unwind(" + alias_ + ", size=" + std::to_string(list_.size()) + ")";
        }

        [[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
            if (child_) return {child_.get()};
            return {};
        }

    private:
        std::unique_ptr<PhysicalOperator> child_;
        std::string alias_;
        std::vector<PropertyValue> list_;

        // State Machine Variables
        std::optional<RecordBatch> currentChildBatch_;
        size_t childRecordIndex_ = 0; // Index in the input batch
        size_t listIndex_ = 0;        // Index in the UNWIND list
    };

}