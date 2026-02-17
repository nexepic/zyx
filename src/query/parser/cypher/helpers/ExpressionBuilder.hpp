/**
 * @file ExpressionBuilder.hpp
 * @author Nexepic
 * @date 2025/12/9
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

#include <functional>
#include <string>
#include <vector>
#include "generated/CypherParser.h"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/Record.hpp"

namespace graph::parser::cypher::helpers {

/**
 * @class ExpressionBuilder
 * @brief Builds predicates and expressions from Cypher AST contexts.
 *
 * This class handles the construction of:
 * - WHERE clause predicates (filter functions)
 * - List literals from expressions (for UNWIND, vector indexes)
 */
class ExpressionBuilder {
public:
	/**
	 * @brief Build a WHERE predicate function from an expression context.
	 *
	 * Currently supports binary comparisons: n.prop <op> value
	 * where <op> is one of: =, <>, <, >, <=, >=
	 *
	 * @param expr The expression context
	 * @param outDesc Output parameter for predicate description (e.g., "n.age > 10")
	 * @return A predicate function that evaluates records
	 * @throws std::runtime_error if the expression is not supported
	 */
	static std::function<bool(const query::execution::Record &)> buildWherePredicate(
		CypherParser::ExpressionContext *expr,
		std::string &outDesc);

	/**
	 * @brief Extract a list literal from an expression context.
	 *
	 * Used by UNWIND to get the list values to iterate over.
	 * Supports only list literals with literal items (not expressions).
	 *
	 * @param ctx The expression context
	 * @return Vector of PropertyValues from the list
	 */
	static std::vector<PropertyValue> extractListFromExpression(CypherParser::ExpressionContext *ctx);

	/**
	 * @brief Navigate the AST chain to get the AtomContext.
	 */
	static CypherParser::AtomContext *getAtomFromExpression(CypherParser::ExpressionContext *e);

	/**
	 * @brief Parse a literal context into a PropertyValue.
	 */
	static PropertyValue parseValue(CypherParser::LiteralContext *ctx);
};

} // namespace graph::parser::cypher::helpers
