/**
 * @file AllFunction.hpp
 * @date 2025/3/9
 *
 * Implementation of all() quantifier function
 */

#pragma once

#include "PredicateFunction.hpp"
#include <cstddef>

namespace graph::query::expressions {

/**
 * @class AllFunction
 * @brief Implements the all() quantifier function
 *
 * Syntax: all(variable IN list WHERE condition)
 *
 * Returns TRUE if all elements in the list satisfy the WHERE condition.
 * Returns TRUE for empty lists (vacuous truth).
 * Short-circuits on first non-matching element.
 *
 * Examples:
 * - all(x IN [1, 2, 3] WHERE x > 0) -> true
 * - all(x IN [1, -2, 3] WHERE x > 0) -> false
 * - all(x IN [] WHERE x > 0) -> true (vacuous truth)
 */
class AllFunction : public PredicateFunction {
public:
	[[nodiscard]] FunctionSignature getSignature() const override {
		// Signature is for validation only; actual parsing is custom
		return FunctionSignature("all", 1, SIZE_MAX, true);
	}

	[[nodiscard]] std::string getName() const override { return "all"; }

	/**
	 * @brief Evaluates the all() function
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
	 * @brief Evaluates the predicate for all elements
	 *
	 * @param list The list of PropertyValue elements
	 * @param variable The iteration variable name
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return true if all elements satisfy the condition, false otherwise
	 */
	[[nodiscard]] bool evaluatePredicate(
		const std::vector<PropertyValue>& list,
		const std::string& variable,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const override;
};

} // namespace graph::query::expressions
