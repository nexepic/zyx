/**
 * @file IndexPushdownRule.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include "graph/query/indexes/IndexManager.hpp"

namespace graph::query::optimizer::rules {

    class IndexPushdownRule {
    public:
        explicit IndexPushdownRule(std::shared_ptr<indexes::IndexManager> indexManager)
            : indexManager_(std::move(indexManager)) {}

        /**
         * @brief Decides the optimal scan configuration based on available indexes.
         *
         * @param variable The query variable (e.g., "n").
         * @param label The label filter (e.g., "User").
         * @param key The property key (e.g., "age").
         * @param value The property value (e.g., 25).
         * @return execution::NodeScanConfig The optimized configuration.
         */
        [[nodiscard]] execution::NodeScanConfig apply(
            const std::string& variable,
            const std::string& label,
            const std::string& key,
            const PropertyValue& value
        ) const {
            execution::NodeScanConfig config;
            config.variable = variable;
            config.label = label;

            // --- Optimization Logic (Moved from QueryPlanner) ---

            bool hasPropIndex = !key.empty() && indexManager_->hasPropertyIndex("node", key);
            bool hasLabelIndex = !label.empty() && indexManager_->hasLabelIndex("node");

            // Heuristic 1: Property Index is usually most selective (O(log N))
            if (hasPropIndex) {
                config.type = execution::ScanType::PROPERTY_SCAN;
                config.indexKey = key;
                config.indexValue = value;
            }
            // Heuristic 2: Label Index reduces search space (O(N_label))
            else if (hasLabelIndex) {
                config.type = execution::ScanType::LABEL_SCAN;
            }
            // Fallback: Full Scan (O(N_total))
            else {
                config.type = execution::ScanType::FULL_SCAN;
            }

            return config;
        }

    private:
        std::shared_ptr<indexes::IndexManager> indexManager_;
    };

} // namespace graph::query::optimizer::rules