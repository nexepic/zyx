/**
 * @file NoneFunction.hpp
 * @date 2025/3/9
 *
 * Implementation of none() quantifier function
 */

#pragma once

#include "PredicateFunction.hpp"
#include <cstddef>

namespace graph::query::expressions {

/**
 * @class NoneFunction
 * @brief Implements the none() quantifier function
 *
 * Syntax: none(variable IN list WHERE condition)
 *
 * Returns TRUE if no elements in the list satisfy the WHERE condition.
 * Returns TRUE for empty lists (vacuously true - no elements to not satisfy).
 * Short-circuits on first matching element.
 *
 * Examples:
 * - none(x IN [1, 2, 3] WHERE x > 5) -> true
 * - none(x IN [1, 2, 3] WHERE x > 2) -> false
 * - none(x IN [] WHERE x > 0) -> true (empty list)
 */
class NoneFunction : public PredicateFunction {
public:
	[[nodiscard]] FunctionSignature getSignature() const override {
		// Signature is for validation only; actual parsing is custom
		return FunctionSignature("none", 1, SIZE_MAX, true);
	}

	[[nodiscard]] std::string getName() const override { return "none"; }

	/**
	 * @brief Evaluates the none() function
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
	 * @brief Evaluates the predicate for no elements
	 *
	 * @param list The list of PropertyValue elements
	 * @param variable The iteration variable name
	 * @param whereExpr The WHERE clause expression
	 * @param context The evaluation context
	 * @return true if no elements satisfy the condition, false otherwise
	 */
	[[nodiscard]] bool evaluatePredicate(
		const std::vector<PropertyValue>& list,
		const std::string& variable,
		const std::unique_ptr<Expression>& whereExpr,
		EvaluationContext& context
	) const override;
};

} // namespace graph::query::expressions
