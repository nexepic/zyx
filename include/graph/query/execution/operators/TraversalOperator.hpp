/**
 * @file TraversalOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

    class TraversalOperator : public PhysicalOperator {
    public:
        /**
         * @brief Constructs a traversal operator.
         *
         * @param dm DataManager for retrieving edges and nodes.
         * @param child Upstream operator providing source nodes.
         * @param sourceVar Variable name of the start node (already in record).
         * @param edgeVar Variable name for the edge to be found.
         * @param targetVar Variable name for the target node to be found.
         * @param edgeLabel Filter by edge label (empty means all labels).
         * @param direction "out", "in", or "both".
         */
        TraversalOperator(std::shared_ptr<storage::DataManager> dm,
                          std::unique_ptr<PhysicalOperator> child,
                          std::string sourceVar,
                          std::string edgeVar,
                          std::string targetVar,
                          std::string edgeLabel,
                          std::string direction)
            : dm_(std::move(dm)), child_(std::move(child)),
              sourceVar_(std::move(sourceVar)), edgeVar_(std::move(edgeVar)),
              targetVar_(std::move(targetVar)), edgeLabel_(std::move(edgeLabel)),
              direction_(std::move(direction)) {}

        void open() override { if(child_) child_->open(); }
        std::optional<RecordBatch> next() override;
        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            auto vars = child_->getOutputVariables();
            vars.push_back(edgeVar_);
            vars.push_back(targetVar_);
            return vars;
        }

    	std::string toString() const override {
        	return "Traversal(" + sourceVar_ + " - [" + edgeVar_ + ":" + edgeLabel_ + "] -> " + targetVar_ + ")";
        }

    	std::vector<const PhysicalOperator*> getChildren() const override {
        	if (child_) return {child_.get()};
        	return {};
        }

    private:
        std::shared_ptr<storage::DataManager> dm_;
        std::unique_ptr<PhysicalOperator> child_;

        std::string sourceVar_;
        std::string edgeVar_;
        std::string targetVar_;
        std::string edgeLabel_;
        std::string direction_;
    };

} // namespace graph::query::execution::operators