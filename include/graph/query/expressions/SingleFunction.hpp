/**
 * @file SingleFunction.hpp
 * @date 2025/3/9
 *
 * Implementation of single() quantifier function
 */

#pragma once

#include "PredicateFunction.hpp"
#include <cstddef>

namespace graph::query::expressions {

/**
 * @class SingleFunction
 * @brief Implements the single() quantifier function
 *
 * Syntax: single(variable IN list WHERE condition)
 *
 * Returns TRUE if exactly one element in the list satisfies the WHERE condition.
 * Returns FALSE for empty lists (no elements to satisfy).
 * Does NOT short-circuit - must count all matches (unless > 1 found).
 *
 * Examples:
 * - single(x IN [1, 2, 3] WHERE x > 2) -> true (only 3)
 * - single(x IN [1, 2, 3] WHERE x > 0) -> false (3 matches)
 * - single(x IN [1, 2, 3] WHERE x > 5) -> false (0 matches)
 * - single(x IN [] WHERE x > 0) -> false (empty list)
 */
class SingleFunction : public PredicateFunction {
public:
	[[nodiscard]] FunctionSignature getSignature() const override {
		// Signature is for validation only; actual parsing is custom
		return FunctionSignature("single", 1, SIZE_MAX, true);
	}

	[[nodiscard]] std::string getName() const override { return "single"; }

	/**
	 * @brief Evaluates the single() function
	 *
	 * @param variable The iteration variable name
	 * @param listExpr Expression that evaluates to a list
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return Boolean result as PropertyValue
	 */
	[[nodiscard]] PropertyValue evaluate(
		const std::string& variable,
		const std::unique_ptr<Expression>& listExpr,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const override;

protected:
	/**
	 * @brief Evaluates the predicate for exactly one element
	 *
	 * @param list The list of PropertyValue elements
	 * @param variable The iteration variable name
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return true if exactly one element satisfies the condition, false otherwise
	 */
	[[nodiscard]] bool evaluatePredicate(
		const std::vector<PropertyValue>& list,
		const std::string& variable,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const override;
};

} // namespace graph::query::expressions
