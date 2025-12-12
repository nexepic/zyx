/**
 * @file QueryPlanner.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/optimizer/Optimizer.hpp"

namespace graph::query {

    QueryPlanner::QueryPlanner(std::shared_ptr<storage::DataManager> dm,
                               std::shared_ptr<indexes::IndexManager> im)
        : dm_(std::move(dm)), im_(im) {

        // Initialize the Optimizer
        optimizer_ = std::make_unique<optimizer::Optimizer>(im_);
    }

	QueryPlanner::~QueryPlanner() = default;

    // --- Read Operations ---

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::scan(
        const std::string& variable,
        const std::string& label,
        const std::string& key,
        const PropertyValue& value
    ) const {

        // 1. Consult the Optimizer
        // The optimizer decides if we can use an IndexScan or must fallback to FullScan.
        auto config = optimizer_->optimizeNodeScan(variable, label, key, value);

        // 2. Build the Scan Operator
        // The Scan Operator purely executes the config (fetching IDs).
        std::unique_ptr<execution::PhysicalOperator> rootOp =
            std::make_unique<execution::operators::NodeScanOperator>(dm_, im_, config);

        // 3. Handle Residual Predicates (The "Purity" Logic)
        // We requested a scan with a property filter (key, value).
        // - If Optimizer chose PROPERTY_SCAN: The Scan result is already filtered by the Index.
        // - If Optimizer chose LABEL/FULL_SCAN: The Scan result contains ALL nodes.
        //   We MUST add a FilterOperator to check the property.

        if (!key.empty()) {
            if (config.type != execution::ScanType::PROPERTY_SCAN) {
                // Case: Index NOT used. Add explicit Filter.
                auto predicate = [variable, key, value](const execution::Record& r) {
                    auto n = r.getNode(variable);
                    if (!n) return false;
                    const auto& props = n->getProperties();
                    auto it = props.find(key);
                    return it != props.end() && it->second == value;
                };

                std::string desc = variable + "." + key + " == " + value.toString() + " (Residual)";
                rootOp = filter(std::move(rootOp), predicate, desc);
            }
            else {
                // Case: Index USED.
                // Technically, we could skip the filter.
                // However, for 100% robustness against Hash Collisions in the index,
                // some engines add a "Verify" filter here too.
                // For your current stage, we can trust the index and skip the extra filter.
            }
        }

        return rootOp;
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::filter(
        std::unique_ptr<execution::PhysicalOperator> child,
        std::function<bool(const execution::Record&)> predicate,
        const std::string& description
    ) const {
        return std::make_unique<execution::operators::FilterOperator>(
            std::move(child), std::move(predicate), description
        );
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::traverse(
        std::unique_ptr<execution::PhysicalOperator> source,
        const std::string& sourceVar,
        const std::string& edgeVar,
        const std::string& targetVar,
        const std::string& edgeLabel,
        const std::string& direction
    ) const {
        return std::make_unique<execution::operators::TraversalOperator>(
            dm_, std::move(source), sourceVar, edgeVar, targetVar, edgeLabel, direction
        );
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::project(
        std::unique_ptr<execution::PhysicalOperator> child,
        const std::vector<std::string>& variables
    ) const {
        return std::make_unique<execution::operators::ProjectOperator>(
            std::move(child), variables
        );
    }

    // --- Write Operations ---

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::create(
        const std::string& variable,
        const std::string& label,
        const std::unordered_map<std::string, PropertyValue>& props
    ) const {
        return std::make_unique<execution::operators::CreateNodeOperator>(
            dm_, variable, label, props
        );
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::create(
        const std::string& variable,
        const std::string& label,
        const std::unordered_map<std::string, PropertyValue>& props,
        const std::string& sourceVar,
        const std::string& targetVar
    ) const {
        return std::make_unique<execution::operators::CreateEdgeOperator>(
            dm_, variable, label, props, sourceVar, targetVar
        );
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::createIndex(
        const std::string& label,
        const std::string& propertyKey
    ) const {
        return std::make_unique<execution::operators::CreateIndexOperator>(
            im_, label, propertyKey
        );
    }

} // namespace graph::query