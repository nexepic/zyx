/**
 * @file ExpressionEvaluator.cpp
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

#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include <cmath>

namespace graph::query::expressions {

ExpressionEvaluator::ExpressionEvaluator(const EvaluationContext &context)
	: context_(context), result_() {}

PropertyValue ExpressionEvaluator::evaluate(Expression *expr) {
	if (!expr) {
		return PropertyValue();
	}

	expr->accept(*this);
	return result_;
}

void ExpressionEvaluator::visit(LiteralExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	if (expr->isNull()) {
		result_ = PropertyValue();
	} else if (expr->isBoolean()) {
		result_ = PropertyValue(expr->getBooleanValue());
	} else if (expr->isInteger()) {
		result_ = PropertyValue(expr->getIntegerValue());
	} else if (expr->isDouble()) {
		result_ = PropertyValue(expr->getDoubleValue());
	} else if (expr->isString()) {
		result_ = PropertyValue(expr->getStringValue());
	} else {
		result_ = PropertyValue();
	}
}

void ExpressionEvaluator::visit(VariableReferenceExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	const std::string &varName = expr->getVariableName();

	if (expr->hasProperty()) {
		// Property access: n.prop
		auto value = context_.resolveProperty(varName, expr->getPropertyName());
		if (value) {
			result_ = *value;
		} else {
			// Missing property → NULL
			result_ = PropertyValue();
		}
	} else {
		// Simple variable reference: n
		auto value = context_.resolveVariable(varName);
		if (value) {
			result_ = *value;
		} else {
			throw UndefinedVariableException(varName);
		}
	}
}

void ExpressionEvaluator::visit(BinaryOpExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate left and right operands
	PropertyValue leftVal = evaluate(expr->getLeft());
	PropertyValue rightVal = evaluate(expr->getRight());

	BinaryOperatorType op = expr->getOperator();

	// NULL propagation: if either operand is NULL, result is NULL
	if (propagateNull(leftVal, rightVal)) {
		result_ = PropertyValue();
		return;
	}

	// Dispatch based on operator type
	if (isArithmeticOperator(op)) {
		result_ = evaluateArithmetic(op, leftVal, rightVal);
	} else if (isComparisonOperator(op)) {
		result_ = evaluateComparison(op, leftVal, rightVal);
	} else if (isLogicalOperator(op)) {
		result_ = evaluateLogical(op, leftVal, rightVal);
	} else {
		throw ExpressionEvaluationException("Unknown binary operator");
	}
}

void ExpressionEvaluator::visit(UnaryOpExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	PropertyValue operandVal = evaluate(expr->getOperand());

	// NULL propagation
	if (propagateNull(operandVal)) {
		result_ = PropertyValue();
		return;
	}

	result_ = evaluateUnary(expr->getOperator(), operandVal);
}

void ExpressionEvaluator::visit(FunctionCallExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	const std::string& functionName = expr->getFunctionName();
	auto& registry = FunctionRegistry::getInstance();

	// Look up the function
	const ScalarFunction* function = registry.lookupScalarFunction(functionName);
	if (!function) {
		throw ExpressionEvaluationException(
			"Unknown function: " + functionName
		);
	}

	// Validate argument count
	const auto& argsExpr = expr->getArguments();
	if (!function->validateArgCount(argsExpr.size())) {
		auto sig = function->getSignature();
		throw ExpressionEvaluationException(
			"Invalid argument count for function '" + functionName +
			"': expected " + std::to_string(sig.minArgs) +
			(sig.variadic ? "+" : "-" + std::to_string(sig.maxArgs)) +
			", got " + std::to_string(argsExpr.size())
		);
	}

	// Evaluate arguments
	std::vector<PropertyValue> args;
	args.reserve(argsExpr.size());
	for (const auto& argExpr : argsExpr) {
		PropertyValue argValue = evaluate(argExpr.get());
		args.push_back(std::move(argValue));
	}

	// Call the function
	result_ = function->evaluate(args, context_);
}

void ExpressionEvaluator::visit(InExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the value expression
	PropertyValue value = evaluate(expr->getValue());

	// Check if value is in the list
	const auto &listValues = expr->getListValues();
	bool found = false;
	for (const auto &listItem : listValues) {
		if (value == listItem) {
			found = true;
			break;
		}
	}

	result_ = PropertyValue(found);
}

void ExpressionEvaluator::visit(CaseExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Simple CASE: CASE expr WHEN expr THEN expr ...
	if (expr->isSimpleCase()) {
		PropertyValue comparisonVal = evaluate(expr->getComparisonExpression());

		for (const auto &branch : expr->getBranches()) {
			PropertyValue whenVal = evaluate(branch.whenExpression.get());

			// NULL propagation: if WHEN is NULL, skip this branch
			if (EvaluationContext::isNull(whenVal)) {
				continue;
			}

			// Check equality: comparisonVal == whenVal
			if (comparisonVal == whenVal) {
				result_ = evaluate(branch.thenExpression.get());
				return;
			}
		}
	}
	// Searched CASE: CASE WHEN bool_expr THEN expr ...
	else {
		for (const auto &branch : expr->getBranches()) {
			PropertyValue whenVal = evaluate(branch.whenExpression.get());

			// NULL propagation: NULL is treated as false
			if (EvaluationContext::isNull(whenVal)) {
				continue;
			}

			// Evaluate WHEN condition as boolean
			bool conditionMet = EvaluationContext::toBoolean(whenVal);

			if (conditionMet) {
				result_ = evaluate(branch.thenExpression.get());
				return;
			}
		}
	}

	// No branch matched, use ELSE or default to NULL
	if (expr->getElseExpression()) {
		result_ = evaluate(expr->getElseExpression());
	} else {
		result_ = PropertyValue();
	}
}

// ============================================================================
// Helper Methods
// ============================================================================

PropertyValue ExpressionEvaluator::evaluateArithmetic(BinaryOperatorType op,
                                                       const PropertyValue &left,
                                                       const PropertyValue &right) {
	// Convert to double for arithmetic (handles int-int, int-double, double-double)
	double leftDouble = EvaluationContext::toDouble(left);
	double rightDouble = EvaluationContext::toDouble(right);

	switch (op) {
		case BinaryOperatorType::ADD:
			return PropertyValue(leftDouble + rightDouble);

		case BinaryOperatorType::SUBTRACT:
			return PropertyValue(leftDouble - rightDouble);

		case BinaryOperatorType::MULTIPLY:
			return PropertyValue(leftDouble * rightDouble);

		case BinaryOperatorType::DIVIDE:
			if (rightDouble == 0.0) {
				throw DivisionByZeroException();
			}
			return PropertyValue(leftDouble / rightDouble);

		case BinaryOperatorType::MODULO: {
			// Modulo only makes sense for integers
			int64_t leftInt = EvaluationContext::toInteger(left);
			int64_t rightInt = EvaluationContext::toInteger(right);
			if (rightInt == 0) {
				throw DivisionByZeroException();
			}
			return PropertyValue(leftInt % rightInt);
		}

		case BinaryOperatorType::POWER:
			return PropertyValue(std::pow(leftDouble, rightDouble));

		default:
			throw ExpressionEvaluationException("Invalid arithmetic operator");
	}
}

PropertyValue ExpressionEvaluator::evaluateComparison(BinaryOperatorType op,
                                                       const PropertyValue &left,
                                                       const PropertyValue &right) {
	switch (op) {
		case BinaryOperatorType::EQUAL:
			return PropertyValue(left == right);

		case BinaryOperatorType::NOT_EQUAL:
			return PropertyValue(left != right);

		case BinaryOperatorType::LESS:
			return PropertyValue(left < right);

		case BinaryOperatorType::GREATER:
			return PropertyValue(left > right);

		case BinaryOperatorType::LESS_EQUAL:
			return PropertyValue(left <= right);

		case BinaryOperatorType::GREATER_EQUAL:
			return PropertyValue(left >= right);

		default:
			throw ExpressionEvaluationException("Invalid comparison operator");
	}
}

PropertyValue ExpressionEvaluator::evaluateLogical(BinaryOperatorType op,
                                                    const PropertyValue &left,
                                                    const PropertyValue &right) {
	bool leftBool = EvaluationContext::toBoolean(left);
	bool rightBool = EvaluationContext::toBoolean(right);

	switch (op) {
		case BinaryOperatorType::AND:
			// Short-circuit: if left is false, don't evaluate right
			return PropertyValue(leftBool && rightBool);

		case BinaryOperatorType::OR:
			// Short-circuit: if left is true, don't evaluate right
			return PropertyValue(leftBool || rightBool);

		case BinaryOperatorType::XOR:
			return PropertyValue(leftBool != rightBool);

		default:
			throw ExpressionEvaluationException("Invalid logical operator");
	}
}

PropertyValue ExpressionEvaluator::evaluateUnary(UnaryOperatorType op, const PropertyValue &operand) {
	switch (op) {
		case UnaryOperatorType::MINUS: {
			double val = EvaluationContext::toDouble(operand);
			return PropertyValue(-val);
		}

		case UnaryOperatorType::NOT: {
			bool val = EvaluationContext::toBoolean(operand);
			return PropertyValue(!val);
		}

		default:
			throw ExpressionEvaluationException("Invalid unary operator");
	}
}

bool ExpressionEvaluator::propagateNull(const PropertyValue &left, const PropertyValue &right) {
	return EvaluationContext::isNull(left) || EvaluationContext::isNull(right);
}

bool ExpressionEvaluator::propagateNull(const PropertyValue &value) {
	return EvaluationContext::isNull(value);
}

void ExpressionEvaluator::visit(ListSliceExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the list expression
	PropertyValue listValue = evaluate(const_cast<Expression*>(expr->getList()));

	if (listValue.getType() != PropertyType::LIST) {
		throw ExpressionEvaluationException("Cannot slice non-list value");
	}

	const auto& list = listValue.getList();
	size_t listSize = list.size();

	// Single index: list[0]
	if (!expr->hasRange()) {
		if (!expr->getStart()) {
			throw ExpressionEvaluationException("List slice requires index");
		}

		PropertyValue indexValue = evaluate(const_cast<Expression*>(expr->getStart()));

		if (indexValue.getType() != PropertyType::INTEGER &&
			indexValue.getType() != PropertyType::DOUBLE) {
			throw ExpressionEvaluationException("List index must be integer");
		}

		int64_t index = 0;
		if (indexValue.getType() == PropertyType::INTEGER) {
			index = std::get<int64_t>(indexValue.getVariant());
		} else {
			index = static_cast<int64_t>(std::get<double>(indexValue.getVariant()));
		}

		// Handle negative index
		if (index < 0) {
			index = listSize + index;
		}

		// Check bounds
		if (index < 0 || static_cast<size_t>(index) >= listSize) {
			result_ = PropertyValue(); // Return null for out of bounds
			return;
		}

		result_ = list[index]; // Return the PropertyValue element
		return;
	}

	// Range slice: list[0..2]
	int64_t start = 0;
	int64_t end = static_cast<int64_t>(listSize);

	if (expr->getStart()) {
		PropertyValue startValue = evaluate(const_cast<Expression*>(expr->getStart()));
		if (startValue.getType() == PropertyType::INTEGER) {
			start = std::get<int64_t>(startValue.getVariant());
		} else if (startValue.getType() == PropertyType::DOUBLE) {
			start = static_cast<int64_t>(std::get<double>(startValue.getVariant()));
		} else {
			throw ExpressionEvaluationException("List start index must be integer");
		}

		// Handle negative start
		if (start < 0) {
			start = listSize + start;
		}
	}

	if (expr->getEnd()) {
		PropertyValue endValue = evaluate(const_cast<Expression*>(expr->getEnd()));
		if (endValue.getType() == PropertyType::INTEGER) {
			end = std::get<int64_t>(endValue.getVariant());
		} else if (endValue.getType() == PropertyType::DOUBLE) {
			end = static_cast<int64_t>(std::get<double>(endValue.getVariant()));
		} else {
			throw ExpressionEvaluationException("List end index must be integer");
		}

		// Handle negative end
		if (end < 0) {
			end = listSize + end;
		}
	}

	// Clamp to valid range
	start = std::max<int64_t>(0, std::min<int64_t>(start, static_cast<int64_t>(listSize)));
	end = std::max<int64_t>(0, std::min<int64_t>(end, static_cast<int64_t>(listSize)));

	// Build result list
	std::vector<PropertyValue> resultVec;
	for (int64_t i = start; i < end; ++i) {
		resultVec.push_back(list[i]);
	}

	result_ = PropertyValue(resultVec);
}

void ExpressionEvaluator::visit(ListComprehensionExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the list expression
	PropertyValue listValue = evaluate(const_cast<Expression*>(expr->getListExpr()));

	// Type check: must be a list
	if (listValue.getType() != PropertyType::LIST) {
		throw ExpressionEvaluationException("List comprehension requires a list value");
	}

	const auto& elements = listValue.getList();
	std::vector<PropertyValue> resultList;

	// Iterate over elements
	for (const auto& element : elements) {
		// Set iteration variable in context
		context_.setVariable(expr->getVariable(), element);

		// Evaluate WHERE clause if present
		if (expr->getWhereExpr()) {
			PropertyValue whereResult = evaluate(const_cast<Expression*>(expr->getWhereExpr()));

			// Type check: WHERE must return boolean
			if (whereResult.getType() != PropertyType::BOOLEAN) {
				throw ExpressionEvaluationException("WHERE clause must return a boolean value");
			}

			bool condition = std::get<bool>(whereResult.getVariant());
			if (!condition) {
				continue;  // Skip this element
			}
		}

		// Evaluate MAP expression or preserve element
		if (expr->getMapExpr()) {
			PropertyValue mapResult = evaluate(const_cast<Expression*>(expr->getMapExpr()));
			resultList.push_back(mapResult);
		} else {
			// FILTER mode: preserve original element
			resultList.push_back(element);
		}

		// Clean up temporary variable to prevent memory growth and stale entries
		context_.clearVariable(expr->getVariable());
	}

	result_ = PropertyValue(resultList);
}

void ExpressionEvaluator::visit(ListLiteralExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Simply return the stored list value
	result_ = expr->getValue();
}

void ExpressionEvaluator::visit(IsNullExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the left expression
	PropertyValue leftVal = evaluate(expr->getExpression());

	// IS NULL / IS NOT NULL follows Cypher's three-valued logic:
	// - NULL IS NULL → true
	// - NULL IS NOT NULL → false
	// - 1 IS NULL → false
	// - 1 IS NOT NULL → true
	bool isNull = (leftVal.getType() == PropertyType::NULL_TYPE);
	bool result = expr->isNegated() ? !isNull : isNull;

	result_ = PropertyValue(result);
}

} // namespace graph::query::expressions
