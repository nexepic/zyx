/**
 * @file QueryBuilder.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"

namespace graph::query {

	class QueryBuilder {
	public:
		explicit QueryBuilder(std::shared_ptr<QueryPlanner> planner);

		/**
		 * @brief Starts a query by scanning nodes.
		 * @param variable The variable name (e.g. "n").
		 * @param label Optional label filter.
		 * @param key Optional property key for index pushdown.
		 * @param value Optional property value for index pushdown.
		 */
		QueryBuilder& match_(const std::string& variable,
							const std::string& label = "",
							const std::string& key = "",
							const PropertyValue& value = PropertyValue());

		/**
		 * @brief Filters the current result set.
		 * @param variable The variable to check.
		 * @param key The property key.
		 * @param value The expected value.
		 */
		QueryBuilder& where_(const std::string& variable, const std::string& key, const PropertyValue& value);

		// --- Optimized Abstraction ---

		// Create Node: create("n", "User", {{"age", 18}})
		QueryBuilder& create_(const std::string& variable, const std::string& label,
							 const std::unordered_map<std::string, PropertyValue>& props = {});

		// Create Edge: create("e", "KNOWS", {}, "n", "m")
		// Overloading allows the user to just think "I want to create something"
		QueryBuilder& create_(const std::string& variable, const std::string& label,
							 const std::string& sourceVar, const std::string& targetVar,
							 const std::unordered_map<std::string, PropertyValue>& props = {});

		/**
		 * @brief Deletes variables in the current scope.
		 * @param variables List of variables to delete.
		 * @param detach If true, delete attached relationships first.
		 */
		QueryBuilder& delete_(const std::vector<std::string>& variables, bool detach = false);

		/**
		 * @brief Updates a property on a variable.
		 */
		QueryBuilder& set_(const std::string& variable, const std::string& key, const PropertyValue& value);

		// ====================================================================
		// Administration (Index)
		// ====================================================================

		/**
		 * @brief Creates an index.
		 * @param label The label to index.
		 * @param property The property to index.
		 * @param indexName Optional name for the index.
		 */
		QueryBuilder& createIndex_(const std::string& label, const std::string& property, const std::string& indexName = "");

		/**
		 * @brief Drops an index by name.
		 */
		QueryBuilder& dropIndex_(const std::string& indexName);

		/**
		 * @brief Drops an index by definition.
		 */
		QueryBuilder& dropIndex_(const std::string& label, const std::string& property);

		QueryBuilder& showIndexes_();

		// ====================================================================
		// Procedures
		// ====================================================================

		/**
		 * @brief Calls a system procedure or algorithm.
		 */
		QueryBuilder& call_(const std::string& procedure, const std::vector<PropertyValue>& args = {});

		// ====================================================================
		// Finalize
		// ====================================================================

		/**
		 * @brief Projects specific variables to return.
		 */
		QueryBuilder& return_(const std::vector<std::string>& variables);

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> build();

	private:
		std::shared_ptr<QueryPlanner> planner_;
		std::unique_ptr<execution::PhysicalOperator> root_;

		// Helper to append operator to the chain
		void append(std::unique_ptr<execution::PhysicalOperator> op);
	};
}