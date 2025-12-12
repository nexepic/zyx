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

		QueryBuilder& match(const std::string& variable, const std::string& label = "");
		QueryBuilder& where(const std::string& variable, const std::string& key, const PropertyValue& value);

		// --- Optimized Abstraction ---

		// Create Node: create("n", "User", {{"age", 18}})
		QueryBuilder& create(const std::string& variable, const std::string& label,
							 const std::unordered_map<std::string, PropertyValue>& props = {});

		// Create Edge: create("e", "KNOWS", {}, "n", "m")
		// Overloading allows the user to just think "I want to create something"
		QueryBuilder& create(const std::string& variable, const std::string& label,
							 const std::string& sourceVar, const std::string& targetVar,
							 const std::unordered_map<std::string, PropertyValue>& props = {});

		QueryBuilder& createIndex(const std::string& label, const std::string& property);

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> build();

	private:
		std::shared_ptr<QueryPlanner> planner_;
		std::unique_ptr<execution::PhysicalOperator> root_;

		// Helper to append operator to the chain
		void append(std::unique_ptr<execution::PhysicalOperator> op);
	};
}