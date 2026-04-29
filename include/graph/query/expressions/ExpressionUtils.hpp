/**
 * @file ExpressionUtils.hpp
 * @brief Shared utilities for expression analysis: aggregate detection,
 *        variable collection, and aggregate extraction/replacement.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace graph::query::expressions {
class Expression;
class FunctionCallExpression;
} // namespace graph::query::expressions

namespace graph::query::logical {
struct LogicalAggItem;
} // namespace graph::query::logical

namespace graph::query::expressions {

/**
 * @class ExpressionUtils
 * @brief Central utilities for expression tree analysis.
 *
 * Replaces duplicated logic scattered across ExpressionBuilder,
 * ResultClauseHandler, FilterPushdownRule, and ProjectionPushdownRule.
 */
class ExpressionUtils {
public:
	/**
	 * @brief Returns all aggregate function names (lowercase).
	 */
	static const std::vector<std::string>& getAggregateFunctionNames();

	/**
	 * @brief Check if a function name is an aggregate function.
	 * @param functionName The function name (case-insensitive)
	 * @return true if count, sum, avg, min, max, or collect
	 */
	static bool isAggregateFunction(const std::string& functionName);

	/**
	 * @brief Recursively check if an expression tree contains any aggregate function calls.
	 *
	 * Complete: covers ALL expression types including InExpression, IsNullExpression,
	 * ListSliceExpression, CaseExpression, ListComprehensionExpression, etc.
	 *
	 * @param expr The expression to check
	 * @return true if the expression contains an aggregate function
	 */
	static bool containsAggregate(const Expression* expr);

	/**
	 * @brief Collect all variable names referenced inside an expression.
	 *
	 * Complete: recurses into ALL expression types. Only the base variable name
	 * (e.g., "n" from "n.age") is recorded.
	 *
	 * @param expr The expression to analyze
	 * @param out Set to insert variable names into
	 */
	static void collectVariables(const Expression* expr, std::set<std::string>& out);

	/**
	 * @brief Overload for unordered_set (used by ProjectionPushdownRule).
	 */
	static void collectVariables(const Expression* expr, std::unordered_set<std::string>& out);

	/**
	 * @brief Replace aggregate function calls in an expression with variable references
	 *        to temporary aliases. Collects extracted aggregates.
	 *
	 * Complete: recurses into ALL expression types (not just BINARY_OP/UNARY_OP).
	 *
	 * @param expr The expression to process (read-only, clones as needed)
	 * @param aggItems Output: extracted aggregate items
	 * @param counter Counter for generating unique temporary aliases
	 * @return A new expression with aggregates replaced by variable references
	 */
	static std::unique_ptr<Expression> extractAndReplaceAggregates(
		const Expression* expr,
		std::vector<logical::LogicalAggItem>& aggItems,
		size_t& counter);
};

} // namespace graph::query::expressions
