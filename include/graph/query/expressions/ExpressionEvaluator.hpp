/**
 * @file ExpressionEvaluator.hpp
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

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/core/PropertyTypes.hpp"
#include <stack>

namespace graph::query::expressions {

/**
 * @class ExpressionEvaluator
 * @brief Evaluates Expression AST nodes against an EvaluationContext.
 *
 * This visitor walks the Expression AST and computes PropertyValue results.
 * It implements proper type coercion, NULL propagation, and short-circuit
 * evaluation for logical operators.
 *
 * Usage:
 * @code
 *   Record record;
 *   record.setNode("n", node);
 *   EvaluationContext context(record);
 *   ExpressionEvaluator evaluator(context);
 *   PropertyValue result = evaluator.evaluate(expression);
 * @endcode
 */
class ExpressionEvaluator : public ExpressionVisitor {
public:
	/**
	 * @brief Constructs an evaluator with the given context.
	 * @param context The evaluation context (record and type coercion utilities)
	 */
	explicit ExpressionEvaluator(const EvaluationContext &context);

	/**
	 * @brief Evaluates an expression and returns the result.
	 * @param expr The expression to evaluate
	 * @return The computed PropertyValue
	 * @throws ExpressionEvaluationException on errors
	 */
	[[nodiscard]] PropertyValue evaluate(Expression *expr);

	// Visitor interface implementation
	void visit(LiteralExpression *expr) override;
	void visit(VariableReferenceExpression *expr) override;
	void visit(BinaryOpExpression *expr) override;
	void visit(UnaryOpExpression *expr) override;
	void visit(FunctionCallExpression *expr) override;
	void visit(CaseExpression *expr) override;
	void visit(InExpression *expr) override;

	/**
	 * @brief Gets the result of the most recent evaluation.
	 * @return The computed value (may be NULL)
	 */
	[[nodiscard]] const PropertyValue &getResult() const { return result_; }

private:
	const EvaluationContext &context_;
	PropertyValue result_;

	// Helper methods for binary operations
	[[nodiscard]] PropertyValue evaluateArithmetic(BinaryOperatorType op,
	                                               const PropertyValue &left,
	                                               const PropertyValue &right);

	[[nodiscard]] PropertyValue evaluateComparison(BinaryOperatorType op,
	                                               const PropertyValue &left,
	                                               const PropertyValue &right);

	[[nodiscard]] PropertyValue evaluateLogical(BinaryOperatorType op,
	                                            const PropertyValue &left,
	                                            const PropertyValue &right);

	// Helper for unary operations
	[[nodiscard]] PropertyValue evaluateUnary(UnaryOperatorType op, const PropertyValue &operand);

	// NULL propagation check
	[[nodiscard]] static bool propagateNull(const PropertyValue &left, const PropertyValue &right);
	[[nodiscard]] static bool propagateNull(const PropertyValue &value);
};

} // namespace graph::query::expressions
