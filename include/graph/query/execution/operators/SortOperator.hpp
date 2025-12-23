/**
 * @file SortOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/22
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include <algorithm>
#include <vector>
#include <string>

namespace graph::query::execution::operators {

    struct SortItem {
        std::string variable;      // e.g., "n"
        std::string property;      // e.g., "age" (Empty if sorting by the element itself/ID)
        bool ascending = true;
    };

    class SortOperator : public PhysicalOperator {
    public:
        SortOperator(std::unique_ptr<PhysicalOperator> child,
                     std::vector<SortItem> sortItems)
            : child_(std::move(child)), sortItems_(std::move(sortItems)) {}

        void open() override {
            if(child_) child_->open();
            sortedRecords_.clear();
            currentOutputIndex_ = 0;
            isSorted_ = false;
        }

        std::optional<RecordBatch> next() override {
            // 1. Materialize Phase (Blocking)
            if (!isSorted_) {
                while (true) {
                    auto batchOpt = child_->next();
                    if (!batchOpt) break;

                    // Accumulate all records
                    auto& batch = *batchOpt;
                    sortedRecords_.insert(sortedRecords_.end(),
                                          std::make_move_iterator(batch.begin()),
                                          std::make_move_iterator(batch.end()));
                }

                // 2. Sort Phase
                performSort();
                isSorted_ = true;
            }

            // 3. Output Phase (Stream buffered results)
            if (currentOutputIndex_ >= sortedRecords_.size()) {
                return std::nullopt;
            }

            RecordBatch batch;
            batch.reserve(BATCH_SIZE);

            while (batch.size() < BATCH_SIZE && currentOutputIndex_ < sortedRecords_.size()) {
                batch.push_back(std::move(sortedRecords_[currentOutputIndex_++]));
            }

            return batch;
        }

        void close() override {
            if(child_) child_->close();
            sortedRecords_.clear();
        }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return child_->getOutputVariables();
        }

    	[[nodiscard]] std::string toString() const override {
        	std::string s = "Sort(";
        	for (size_t i = 0; i < sortItems_.size(); ++i) {
        		const auto& item = sortItems_[i];
        		s += item.variable + (item.property.empty() ? "" : "." + item.property);

        		s += (item.ascending ? " ASC" : " DESC");

        		if (i < sortItems_.size() - 1) {
        			s += ", ";
        		}
        	}
        	s += ")";
        	return s;
        }

        [[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
            return {child_.get()};
        }

    private:
        std::unique_ptr<PhysicalOperator> child_;
        std::vector<SortItem> sortItems_;

        // Buffer
        std::vector<Record> sortedRecords_;
        size_t currentOutputIndex_ = 0;
        bool isSorted_ = false;
        static constexpr size_t BATCH_SIZE = 1000;

        void performSort() {
            std::sort(sortedRecords_.begin(), sortedRecords_.end(),
                [this](const Record& a, const Record& b) -> bool {
                    for (const auto& item : sortItems_) {
                        // Extract values
                        PropertyValue valA, valB;

                        // Helper to extract value from Record based on var.prop
                        auto extract = [&](const Record& r, PropertyValue& out) {
                            if (auto n = r.getNode(item.variable)) {
                                if (item.property.empty()) out = PropertyValue(n->getId()); // Default sort by ID
                                else {
                                    const auto& p = n->getProperties();
                                    if (p.count(item.property)) out = p.at(item.property);
                                    else out = PropertyValue(); // Null
                                }
                            }
                            // Add Edge support if needed...
                        };

                        extract(a, valA);
                        extract(b, valB);

                        if (valA != valB) {
                            if (item.ascending) return valA < valB;
                            else return valA > valB;
                        }
                        // If equal, continue to next sort key
                    }
                    return false; // Strictly equal
                });
        }
    };

} // namespace graph::query::execution::operators