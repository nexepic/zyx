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
#include <memory>
#include <string>
#include <vector>
#include "generated/CypherParser.h"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/Record.hpp"

namespace graph::query::expressions {
class Expression;
}

namespace graph::parser::cypher::helpers {

/**
 * @class ExpressionBuilder
 * @brief Builds predicates and expressions from Cypher AST contexts.
 *
 * This class handles two modes of operation:
 *
 * **LEGACY MODE** (for backward compatibility):
 * - buildWherePredicate(): Generates std::function predicates directly
 * - Used by existing FilterOperator implementation
 *
 * **NEW MODE** (AST-based, recommended):
 * - buildExpression(): Returns Expression* AST
 * - More powerful, supports complex expressions
 * - Used by new ExpressionEvaluator
 *
 * Migration path: Old code continues to work, new code should use buildExpression().
 */
class ExpressionBuilder {
public:
	// =========================================================================
	// NEW API: AST-based expression building (RECOMMENDED)
	// =========================================================================

	/**
	 * @brief Build an Expression AST from a Cypher expression context.
	 *
	 * Supports:
	 * - Literals: 42, "hello", true, null
	 * - Variables: n, n.prop
	 * - Binary ops: arithmetic (+, -, *, /, %), comparison (=, <>, <, >, <=, >=), logical (AND, OR, XOR)
	 * - Unary ops: -, NOT
	 * - Parenthesized expressions
	 * - IN operator
	 * - Function calls (basic support)
	 * - CASE expressions
	 *
	 * @param expr The ANTLR4 expression context
	 * @return Unique pointer to the constructed Expression AST
	 * @throws std::runtime_error if the expression syntax is not supported
	 *
	 * @code
	 * auto expr = ExpressionBuilder::buildExpression(ctx);
	 * EvaluationContext evalCtx(record);
	 * ExpressionEvaluator evaluator(evalCtx);
	 * PropertyValue result = evaluator.evaluate(expr.get());
	 * @endcode
	 */
	static std::unique_ptr<graph::query::expressions::Expression> buildExpression(
		CypherParser::ExpressionContext *expr);

	/**
	 * @brief Evaluate a literal expression at parse time.
	 *
	 * This is a convenience method for evaluating expressions that contain only literals
	 * (numbers, strings, booleans, null, lists of literals). It builds the AST and
	 * evaluates it immediately without requiring a runtime record context.
	 *
	 * **Use case**: Pattern properties in MATCH/CREATE/MERGE clauses where values are
	 * literal constants: `MATCH (n {name: "Alice", age: 30})`
	 *
	 * **Limitation**: Only supports literal expressions. Variables, function calls, and
	 * property references will cause an exception.
	 *
	 * @param expr The ANTLR4 expression context
	 * @return The evaluated PropertyValue
	 * @throws std::runtime_error if the expression contains non-literal elements
	 *
	 * @code
	 * // Extract properties from pattern
	 * auto props = AstExtractor::extractProperties(ctx->properties(),
	 *     [](CypherParser::ExpressionContext *expr) {
	 *         return ExpressionBuilder::evaluateLiteralExpression(expr);
	 *     });
	 * @endcode
	 */
	static PropertyValue evaluateLiteralExpression(CypherParser::ExpressionContext *expr);

	// =========================================================================
	// LEGACY API: Direct predicate generation (BACKWARD COMPATIBLE)
	// =========================================================================

	/**
	 * @brief Build a WHERE predicate function from an expression context.
	 *
	 * **DEPRECATED**: Use buildExpression() + ExpressionEvaluator instead.
	 * This method is kept for backward compatibility with existing FilterOperator code.
	 *
	 * Currently supports binary comparisons: n.prop <op> value
	 * where <op> is one of: =, <>, <, >, <=, >=, IN
	 *
	 * @param expr The expression context
	 * @param outDesc Output parameter for predicate description (e.g., "n.age > 10")
	 * @return A predicate function that evaluates records
	 * @throws std::runtime_error if the expression is not supported
	 *
	 * @deprecated Use buildExpression() for new code
	 */
	[[deprecated("Use buildExpression() + ExpressionEvaluator for new code")]]
	static std::function<bool(const query::execution::Record &)> buildWherePredicate(
		CypherParser::ExpressionContext *expr,
		std::string &outDesc);

	// =========================================================================
	// Utility Methods (shared by both APIs)
	// =========================================================================

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

	/**
	 * @brief Check if a function name is an aggregate function.
	 * @param functionName The function name (case-insensitive)
	 * @return true if the function is an aggregate function (count, sum, avg, min, max, collect)
	 */
	static bool isAggregateFunction(const std::string& functionName);

private:
	// =========================================================================
	// Internal helper methods for AST construction
	// =========================================================================

	// Build specific expression types
	static std::unique_ptr<graph::query::expressions::Expression> buildOrExpression(
		CypherParser::OrExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildXorExpression(
		CypherParser::XorExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildAndExpression(
		CypherParser::AndExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildNotExpression(
		CypherParser::NotExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildComparisonExpression(
		CypherParser::ComparisonExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildArithmeticExpression(
		CypherParser::ArithmeticExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildUnaryExpression(
		CypherParser::UnaryExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildAtomExpression(
		CypherParser::AtomContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildVariableOrProperty(
		CypherParser::UnaryExpressionContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildListLiteral(
		CypherParser::ListLiteralContext *ctx);

	static std::unique_ptr<graph::query::expressions::Expression> buildFunctionCall(
		CypherParser::FunctionInvocationContext *ctx);

	// Helper methods for literal value extraction
	static graph::PropertyValue parseListLiteral(CypherParser::ListLiteralContext *ctx);
};

} // namespace graph::parser::cypher::helpers
