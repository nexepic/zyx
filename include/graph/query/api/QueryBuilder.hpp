/**
 * @file QueryBuilder.hpp
 * @author Nexepic
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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

		// ====================================================================
		// Read Operations
		// ====================================================================

		/**
		 * @brief Starts a query by scanning nodes.
		 * @param variable The variable name (e.g. "n").
		 * @param label Optional label filter.
		 * @param key Optional property key for index pushdown.
		 * @param value Optional property value for index pushdown.
		 */
		QueryBuilder &match_(const std::string &variable, const std::string &label = "", const std::string &key = "",
							 const PropertyValue &value = PropertyValue());

		/**
		 * @brief Filters the current result set.
		 */
		QueryBuilder &where_(const std::string &variable, const std::string &key, const PropertyValue &value);

		// ====================================================================
		// Write Operations (Data)
		// ====================================================================

		/**
		 * @brief Create Node: create("n", "User", {{"age", 18}})
		 */
		QueryBuilder &create_(const std::string &variable, const std::string &label,
							  const std::unordered_map<std::string, PropertyValue> &props = {});

		/**
		 * @brief Create Edge: create("e", "KNOWS", "n", "m", {})
		 */
		QueryBuilder &create_(const std::string &variable, const std::string &label, const std::string &sourceVar,
							  const std::string &targetVar,
							  const std::unordered_map<std::string, PropertyValue> &props = {});

		/**
		 * @brief Deletes variables in the current scope.
		 * @param variables List of variables to delete.
		 * @param detach If true, delete attached relationships first.
		 */
		QueryBuilder &delete_(const std::vector<std::string> &variables, bool detach = false);

		/**
		 * @brief Updates a property on a variable.
		 */
		QueryBuilder &set_(const std::string &variable, const std::string &key, const PropertyValue &value);

		/**
		 * @brief Updates a Label on a variable.
		 */
		QueryBuilder &setLabel_(const std::string &variable, const std::string &label);

		/**
		 * @brief Removes a property or label.
		 * @param type "PROPERTY" or "LABEL" (Helper enum logic handled internally).
		 */
		QueryBuilder &remove_(const std::string &variable, const std::string &key, bool isLabel = false);

		// ====================================================================
		// Administration (Index)
		// ====================================================================

		QueryBuilder &createIndex_(const std::string &label, const std::string &property,
								   const std::string &indexName = "");
		QueryBuilder &dropIndex_(const std::string &indexName);
		QueryBuilder &dropIndex_(const std::string &label, const std::string &property);
		QueryBuilder &showIndexes_();

		// ====================================================================
		// Procedures
		// ====================================================================

		QueryBuilder &call_(const std::string &procedure, const std::vector<PropertyValue> &args = {});

		// ====================================================================
		// Finalize
		// ====================================================================

		QueryBuilder &return_(const std::vector<std::string> &variables);

		// Skip & Limit
		QueryBuilder &skip_(int64_t offset);
		QueryBuilder &limit_(int64_t limit);

		// Order By
		// Format: {{var, prop, asc}, ...}
		struct SortOrder {
			std::string variable;
			std::string property;
			bool ascending;
		};
		QueryBuilder &orderBy_(const std::vector<SortOrder> &items);

		QueryBuilder &unwind(const std::vector<PropertyValue> &list, const std::string &alias);

		[[nodiscard]] std::unique_ptr<execution::PhysicalOperator> build();

	private:
		std::shared_ptr<QueryPlanner> planner_;
		std::unique_ptr<execution::PhysicalOperator> root_;

		// Helper to append operator to the chain (handling pipes vs sources)
		void append(std::unique_ptr<execution::PhysicalOperator> op);
	};

} // namespace graph::query
