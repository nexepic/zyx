/**
 * @file QueryErrorMessages.hpp
 * @author Metrix Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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

#include <string>

namespace graph::query {

/**
 * @class QueryErrorMessages
 * @brief Standardized error messages for Cypher query errors.
 *
 * This class provides centralized error message generation to ensure
 * consistency across all clause handlers and improve maintainability.
 *
 * **Benefits:**
 * - Consistent error message format
 * - Easy to update error messages globally
 * - Reduces string duplication in code
 * - Makes error messages more user-friendly
 *
 * **Usage:**
 * @code
 * throw std::runtime_error(QueryErrorMessages::mustFollowClause("WITH"));
 * @endcode
 */
class QueryErrorMessages {
public:
	/**
	 * @brief Error message for clauses that must follow specific preceding clauses.
	 *
	 * @param clauseName The clause that has the requirement (e.g., "WITH", "SET")
	 * @param validPreceding Valid preceding clauses (default: "MATCH or CREATE")
	 * @return Formatted error message
	 *
	 * @code
	 * // "WITH clause must follow a MATCH or CREATE clause."
	 * QueryErrorMessages::mustFollowClause("WITH");
	 *
	 * // "DELETE clause must follow a MATCH clause."
	 * QueryErrorMessages::mustFollowClause("DELETE", "MATCH");
	 * @endcode
	 */
	static std::string mustFollowClause(const std::string &clauseName,
	                                     const std::string &validPreceding = "MATCH or CREATE");

	/**
	 * @brief Error message for clauses that require literal values.
	 *
	 * @param clause The clause name
	 * @param expected Description of expected literal type
	 * @return Formatted error message
	 */
	static std::string requiresLiteral(const std::string &clause,
	                                   const std::string &expected);

	/**
	 * @brief Error message for operations used in invalid context.
	 *
	 * @param operation The operation being performed
	 * @param requirement What's required for the operation
	 * @return Formatted error message
	 */
	static std::string invalidContext(const std::string &operation,
	                                  const std::string &requirement);

	/**
	 * @brief Error message for undefined variable references.
	 *
	 * @param varName The undefined variable name
	 * @return Formatted error message
	 */
	static std::string undefinedVariable(const std::string &varName);

	/**
	 * @brief Error message for invalid syntax or usage.
	 *
	 * @param context Where the error occurred
	 * @param details What's wrong
	 * @return Formatted error message
	 */
	static std::string invalidSyntax(const std::string &context,
	                                 const std::string &details);
};

} // namespace graph::query
