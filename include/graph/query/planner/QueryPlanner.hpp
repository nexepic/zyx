/**
 * @file QueryPlanner.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include "graph/core/PropertyTypes.hpp"

// Forward declarations to reduce compile-time dependencies
namespace graph::storage { class DataManager; }
namespace graph::query::indexes { class IndexManager; }
namespace graph::query::optimizer { class Optimizer; }
namespace graph::query::execution {
    class PhysicalOperator;
    class Record;
}

namespace graph::query {

    class QueryPlanner {
    public:
        /**
         * @brief Constructs the planner and initializes the internal Optimizer.
         */
        QueryPlanner(std::shared_ptr<storage::DataManager> dm,
                     std::shared_ptr<indexes::IndexManager> im);

    	~QueryPlanner();

        // =================================================================
        // Read Operations (Factories)
        // =================================================================

        /**
         * @brief Creates a Scan operator.
         *        The internal Optimizer determines if an IndexScan or FullScan is used.
         *        If IndexScan is not possible, the Planner appends a FilterOperator automatically.
         *
         * @param variable The variable name (e.g., "n").
         * @param label The label filter (e.g., "User").
         * @param key Optional property key for index pushdown.
         * @param value Optional property value for index pushdown.
         */
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> scan(
            const std::string& variable,
            const std::string& label,
            const std::string& key = "",
            const PropertyValue& value = PropertyValue()
        ) const;

        /**
         * @brief Wraps an operator with a generic predicate filter.
         *
         * @param description A string describing the logic (e.g., "n.age > 10") for debugging.
         */
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> filter(
            std::unique_ptr<execution::PhysicalOperator> child,
            std::function<bool(const execution::Record&)> predicate,
            const std::string& description
        ) const;

        /**
         * @brief Expands a node into connected edges and target nodes.
         */
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> traverse(
            std::unique_ptr<execution::PhysicalOperator> source,
            const std::string& sourceVar,
            const std::string& edgeVar,
            const std::string& targetVar,
            const std::string& edgeLabel,
            const std::string& direction
        ) const;

        /**
         * @brief Projects specific variables from the stream (SELECT/RETURN).
         */
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> project(
            std::unique_ptr<execution::PhysicalOperator> child,
            const std::vector<std::string>& variables
        ) const;

        // =================================================================
        // Write Operations (Factories)
        // =================================================================

        // Create Node
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> create(
            const std::string& variable,
            const std::string& label,
            const std::unordered_map<std::string, PropertyValue>& props
        ) const;

        // Create Edge
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> create(
            const std::string& variable,
            const std::string& label,
            const std::unordered_map<std::string, PropertyValue>& props,
            const std::string& sourceVar,
            const std::string& targetVar
        ) const;

        // Create Index (DDL)
        [[nodiscard]] std::unique_ptr<execution::PhysicalOperator> createIndex(
            const std::string& label,
            const std::string& propertyKey
        ) const;

    	[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> callProcedure(
			const std::string& procedure,
			const std::vector<::graph::PropertyValue>& args
		) const;

    	[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> showIndexes() const;

    	[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> dropIndex(
			const std::string& label,
			const std::string& propertyKey
		) const;

    private:
        std::shared_ptr<storage::DataManager> dm_;
        std::shared_ptr<indexes::IndexManager> im_;

        // The Brain: Handles logic for selecting the best scan strategy
        std::unique_ptr<optimizer::Optimizer> optimizer_;
    };

} // namespace graph::query