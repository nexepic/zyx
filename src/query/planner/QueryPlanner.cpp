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

#include "graph/query/execution/operators/AlgoShortestPathOperator.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/ListConfigOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/SetConfigOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
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

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::scanOp(
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
                rootOp = filterOp(std::move(rootOp), predicate, desc);
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

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::filterOp(
        std::unique_ptr<execution::PhysicalOperator> child,
        std::function<bool(const execution::Record&)> predicate,
        const std::string& description
    ) {
        return std::make_unique<execution::operators::FilterOperator>(
            std::move(child), std::move(predicate), description
        );
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::traverseOp(
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

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::projectOp(
        std::unique_ptr<execution::PhysicalOperator> child,
        const std::vector<std::string>& variables
    ) {
        return std::make_unique<execution::operators::ProjectOperator>(
            std::move(child), variables
        );
    }

    // --- Write Operations ---

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::createOp(
        const std::string& variable,
        const std::string& label,
        const std::unordered_map<std::string, PropertyValue>& props
    ) const {
        return std::make_unique<execution::operators::CreateNodeOperator>(
            dm_, variable, label, props
        );
    }

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::createOp(
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

    std::unique_ptr<execution::PhysicalOperator> QueryPlanner::createIndexOp(
        const std::string& label,
        const std::string& propertyKey
    ) const {
        return std::make_unique<execution::operators::CreateIndexOperator>(
            im_, label, propertyKey
        );
    }

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::callProcedureOp(
		const std::string& procedure,
		const std::vector<::graph::PropertyValue>& args
	) const {
    	if (procedure == "dbms.setConfig") {
    		if (args.size() != 2) throw std::runtime_error("dbms.setConfig expects (key, value)");
    		// args[0] should be key (string), args[1] is value
    		return std::make_unique<execution::operators::SetConfigOperator>(dm_, args[0].toString(), args[1]);
    	}

    	if (procedure == "dbms.listConfig") {
    		return std::make_unique<execution::operators::ListConfigOperator>(dm_);
    	}

    	if (procedure == "dbms.getConfig") {
    		if (args.size() != 1) throw std::runtime_error("dbms.getConfig expects (key)");
    		return std::make_unique<execution::operators::ListConfigOperator>(dm_, args[0].toString());
    	}

    	if (procedure == "algo.shortestPath") {
    		if (args.size() < 2) {
    			throw std::runtime_error("algo.shortestPath requires at least (startId, endId)");
    		}

    		// Extract IDs (Assuming arguments are passed as Integers)
    		// If user passes '1', it might be parsed as int64.
    		// Using std::visit or simpler helpers if available.
    		// Here assuming simple conversion via PropertyValue helper logic or direct variant access.

    		int64_t startId = 0;
    		int64_t endId = 0;

    		// Simple extraction logic (adjust based on your PropertyValue API)
    		try {
    			startId = std::stoll(args[0].toString());
    			endId = std::stoll(args[1].toString());
    		} catch (...) {
    			throw std::runtime_error("IDs must be numeric");
    		}

    		std::string dir = "both";
    		if (args.size() > 2) dir = args[2].toString();

    		return std::make_unique<execution::operators::AlgoShortestPathOperator>(
				dm_, startId, endId, dir
			);
    	}

    	throw std::runtime_error("Unknown procedure: " + procedure);
    }

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::showIndexesOp() const {
    	return std::make_unique<execution::operators::ShowIndexesOperator>(im_);
    }

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::dropIndexOp(
		const std::string& label,
		const std::string& propertyKey
	) const {
    	return std::make_unique<execution::operators::DropIndexOperator>(
			im_, label, propertyKey
		);
    }

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::deleteOp(
		std::unique_ptr<execution::PhysicalOperator> child,
		const std::vector<std::string>& variables,
		bool detach
	) const {
    	return std::make_unique<execution::operators::DeleteOperator>(
			dm_, std::move(child), variables, detach
		);
    }

	std::unique_ptr<execution::PhysicalOperator> QueryPlanner::setOp(
		std::unique_ptr<execution::PhysicalOperator> child,
		const std::vector<execution::operators::SetItem>& items
	) const {
    	return std::make_unique<execution::operators::SetOperator>(
			dm_, std::move(child), items
		);
    }

} // namespace graph::query