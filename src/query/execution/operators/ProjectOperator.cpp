/**
 * @file ProjectOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/ProjectOperator.hpp"
#include <sstream>

namespace graph::query::execution::operators {

    ProjectOperator::ProjectOperator(std::unique_ptr<PhysicalOperator> child,
                                     std::vector<std::string> variableNames)
        : child_(std::move(child)), variableNames_(std::move(variableNames)) {}

    void ProjectOperator::open() {
        if (child_) child_->open();
    }

    std::optional<RecordBatch> ProjectOperator::next() {
        auto batchOpt = child_->next();
        if (!batchOpt) return std::nullopt;

        RecordBatch& inputBatch = *batchOpt;
        RecordBatch outputBatch;
        outputBatch.reserve(inputBatch.size());

        for (const auto& record : inputBatch) {
            Record newRecord;

            // Only copy the requested variables into the new record
            for (const auto& var : variableNames_) {
                // Try Node
                if (auto node = record.getNode(var)) {
                    newRecord.setNode(var, *node);
                }
                // Try Edge
                else if (auto edge = record.getEdge(var)) {
                    newRecord.setEdge(var, *edge);
                }
                // Try Property Value (if Record supports getValue)
                else if (auto val = record.getValue(var)) {
                    newRecord.setValue(var, *val);
                }
            }

            outputBatch.push_back(std::move(newRecord));
        }

        return outputBatch;
    }

    void ProjectOperator::close() {
        if (child_) child_->close();
    }

    std::vector<std::string> ProjectOperator::getOutputVariables() const {
        return variableNames_;
    }

    std::string ProjectOperator::toString() const {
        std::ostringstream oss;
        oss << "Project(";
        for (size_t i = 0; i < variableNames_.size(); ++i) {
            oss << variableNames_[i];
            if (i < variableNames_.size() - 1) oss << ", ";
        }
        oss << ")";
        return oss.str();
    }

    std::vector<const PhysicalOperator*> ProjectOperator::getChildren() const {
        if (child_) return {child_.get()};
        return {};
    }

} // namespace graph::query::execution::operators