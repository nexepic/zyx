/**
 * @file PropertyValueEvaluator.hpp
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

#include "generated/CypherParser.h"
#include "graph/core/PropertyTypes.hpp"

namespace graph::parser::cypher::helpers {

/**
 * @class PropertyValueEvaluator
 * @brief Evaluates Cypher expression contexts into PropertyValue objects.
 *
 * This class handles the evaluation of various expression types from the Cypher
 * AST into PropertyValue objects. It supports:
 * - Literals (strings, numbers, booleans, null)
 * - List literals (for vector indexes, UNWIND, etc.)
 *
 * Complex expressions (arithmetic, function calls, etc.) are not yet supported.
 */
class PropertyValueEvaluator {
public:
	/**
	 * @brief Evaluate an expression context into a PropertyValue.
	 *
	 * This navigates the AST from ExpressionContext down to AtomContext
	 * and extracts the value. Supports literals and list literals.
	 *
	 * @param ctx The expression context (may be null)
	 * @return The evaluated PropertyValue, or empty if evaluation fails
	 */
	static PropertyValue evaluate(CypherParser::ExpressionContext *ctx);

	/**
	 * @brief Navigate the AST chain to get the AtomContext.
	 *
	 * The Cypher grammar has a deep chain: Expression -> Or -> Xor -> And ->
	 * Not -> Comparison -> Arithmetic -> Unary -> Atom
	 *
	 * @param e The expression context
	 * @return The atom context, or null if navigation fails
	 */
	static CypherParser::AtomContext *getAtomFromExpression(CypherParser::ExpressionContext *e);

	/**
	 * @brief Parse a literal context into a PropertyValue.
	 * Delegates to AstExtractor::parseValue.
	 */
	static PropertyValue parseLiteral(CypherParser::LiteralContext *ctx);

	/**
	 * @brief Parse a list literal into a PropertyValue (vector of floats).
	 * Used for vector indexes.
	 */
	static PropertyValue parseListLiteral(CypherParser::ListLiteralContext *ctx);
};

} // namespace graph::parser::cypher::helpers
