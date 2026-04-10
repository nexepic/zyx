/**
 * @file ExpressionEvaluator.cpp
 * @author ZYX Contributors
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
#include "graph/storage/data/DataManager.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"
#include <cmath>
#include <regex>
#include <queue>
#include <unordered_set>

namespace graph::query::expressions {

ExpressionEvaluator::ExpressionEvaluator(const EvaluationContext &context)
	: context_(context), result_() {}

PropertyValue ExpressionEvaluator::evaluate(const Expression *expr) {
	if (!expr) {
		return PropertyValue();
	}

	expr->accept(*this);
	return result_;
}

void ExpressionEvaluator::visit(const LiteralExpression *expr) {
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
	}
}

void ExpressionEvaluator::visit(const VariableReferenceExpression *expr) {
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

void ExpressionEvaluator::visit(const BinaryOpExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate left and right operands
	PropertyValue leftVal = evaluate(expr->getLeft());
	PropertyValue rightVal = evaluate(expr->getRight());

	BinaryOperatorType op = expr->getOperator();

	// For logical operators, handle NULL with three-valued logic (don't propagate blindly)
	if (isLogicalOperator(op)) {
		result_ = evaluateLogical(op, leftVal, rightVal);
		return;
	}

	// For non-logical operators, propagate NULL: if either operand is NULL, result is NULL
	if (propagateNull(leftVal, rightVal)) {
		result_ = PropertyValue();
		return;
	}

	// Dispatch based on operator type
	if (isArithmeticOperator(op)) {
		result_ = evaluateArithmetic(op, leftVal, rightVal);
	} else if (isComparisonOperator(op)) {
		result_ = evaluateComparison(op, leftVal, rightVal);
	} else {
		throw ExpressionEvaluationException("Unknown binary operator");
	}
}

void ExpressionEvaluator::visit(const UnaryOpExpression *expr) {
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

void ExpressionEvaluator::visit(const FunctionCallExpression *expr) {
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

	// Entity introspection functions receive variable name as string, not evaluated value
	if (dynamic_cast<const EntityIntrospectionFunction*>(function)) {
		// Note: validateArgCount above already ensures arg count is valid for the function's signature.
		// All built-in entity introspection functions require exactly 1 argument, enforced by their signatures.
		auto* varRef = (argsExpr.size() == 1)
			? dynamic_cast<const VariableReferenceExpression*>(argsExpr[0].get())
			: nullptr;
		if (!varRef || varRef->hasProperty()) {
			throw ExpressionEvaluationException(
				functionName + "() requires a variable reference argument (e.g., " + functionName + "(n))");
		}
		std::vector<PropertyValue> args = { PropertyValue(varRef->getVariableName()) };
		result_ = function->evaluate(args, context_);
		return;
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

void ExpressionEvaluator::visit(const InExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the value expression
	PropertyValue value = evaluate(expr->getValue());

	// Dynamic list: evaluate the list expression at runtime
	if (expr->hasDynamicList()) {
		PropertyValue listVal = evaluate(expr->getListExpression());

		// NULL propagation
		if (EvaluationContext::isNull(listVal)) {
			result_ = PropertyValue();
			return;
		}

		if (listVal.getType() != PropertyType::LIST) {
			throw ExpressionEvaluationException("IN requires a list value on the right-hand side");
		}

		const auto& list = listVal.getList();
		bool found = false;
		for (const auto& item : list) {
			if (value == item) {
				found = true;
				break;
			}
		}
		result_ = PropertyValue(found);
		return;
	}

	// Static list: check against literal values
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

void ExpressionEvaluator::visit(const CaseExpression *expr) {
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
	// String concatenation: if either operand is a string and op is ADD, concatenate
	if (op == BinaryOperatorType::BOP_ADD &&
	    (left.getType() == PropertyType::STRING || right.getType() == PropertyType::STRING)) {
		return PropertyValue(EvaluationContext::toString(left) + EvaluationContext::toString(right));
	}

	// Integer-preserving arithmetic: if both operands are integers, keep integer type
	bool bothIntegers = (left.getType() == PropertyType::INTEGER &&
	                     right.getType() == PropertyType::INTEGER);

	if (bothIntegers) {
		int64_t l = std::get<int64_t>(left.getVariant());
		int64_t r = std::get<int64_t>(right.getVariant());

		switch (op) {
			case BinaryOperatorType::BOP_ADD:
				return PropertyValue(l + r);
			case BinaryOperatorType::BOP_SUBTRACT:
				return PropertyValue(l - r);
			case BinaryOperatorType::BOP_MULTIPLY:
				return PropertyValue(l * r);
			case BinaryOperatorType::BOP_DIVIDE:
				if (r == 0) throw DivisionByZeroException();
				// Integer division in Cypher produces double if not evenly divisible
				if (l % r == 0) return PropertyValue(l / r);
				return PropertyValue(static_cast<double>(l) / static_cast<double>(r));
			case BinaryOperatorType::BOP_MODULO:
				if (r == 0) throw DivisionByZeroException();
				return PropertyValue(l % r);
			case BinaryOperatorType::BOP_POWER:
				return PropertyValue(std::pow(static_cast<double>(l), static_cast<double>(r)));
			default:
				break;
		}
		// All arithmetic operators are handled above; this is unreachable but satisfies the compiler.
		throw ExpressionEvaluationException("Invalid arithmetic operator");
	}

	// Floating-point arithmetic for mixed types or double operands
	double leftDouble = EvaluationContext::toDouble(left);
	double rightDouble = EvaluationContext::toDouble(right);

	switch (op) {
		case BinaryOperatorType::BOP_ADD:
			return PropertyValue(leftDouble + rightDouble);

		case BinaryOperatorType::BOP_SUBTRACT:
			return PropertyValue(leftDouble - rightDouble);

		case BinaryOperatorType::BOP_MULTIPLY:
			return PropertyValue(leftDouble * rightDouble);

		case BinaryOperatorType::BOP_DIVIDE:
			if (rightDouble == 0.0) {
				throw DivisionByZeroException();
			}
			return PropertyValue(leftDouble / rightDouble);

		case BinaryOperatorType::BOP_MODULO: {
			int64_t leftInt = EvaluationContext::toInteger(left);
			int64_t rightInt = EvaluationContext::toInteger(right);
			if (rightInt == 0) {
				throw DivisionByZeroException();
			}
			return PropertyValue(leftInt % rightInt);
		}

		case BinaryOperatorType::BOP_POWER:
			return PropertyValue(std::pow(leftDouble, rightDouble));
		default:
			break;
	}
	// All arithmetic operators are handled above; this is unreachable but satisfies the compiler.
	throw ExpressionEvaluationException("Invalid arithmetic operator");
}

PropertyValue ExpressionEvaluator::evaluateComparison(BinaryOperatorType op,
                                                       const PropertyValue &left,
                                                       const PropertyValue &right) {
	switch (op) {
		case BinaryOperatorType::BOP_EQUAL:
			return PropertyValue(left == right);

		case BinaryOperatorType::BOP_NOT_EQUAL:
			return PropertyValue(left != right);

		case BinaryOperatorType::BOP_LESS:
			return PropertyValue(left < right);

		case BinaryOperatorType::BOP_GREATER:
			return PropertyValue(left > right);

		case BinaryOperatorType::BOP_LESS_EQUAL:
			return PropertyValue(left <= right);

		case BinaryOperatorType::BOP_GREATER_EQUAL:
			return PropertyValue(left >= right);

		case BinaryOperatorType::BOP_STARTS_WITH: {
			std::string l = EvaluationContext::toString(left);
			std::string r = EvaluationContext::toString(right);
			return PropertyValue(l.size() >= r.size() && l.substr(0, r.size()) == r);
		}

		case BinaryOperatorType::BOP_ENDS_WITH: {
			std::string l = EvaluationContext::toString(left);
			std::string r = EvaluationContext::toString(right);
			return PropertyValue(l.size() >= r.size() && l.substr(l.size() - r.size()) == r);
		}

		case BinaryOperatorType::BOP_CONTAINS: {
			std::string l = EvaluationContext::toString(left);
			std::string r = EvaluationContext::toString(right);
			return PropertyValue(l.find(r) != std::string::npos);
		}

		case BinaryOperatorType::BOP_REGEX_MATCH: {
			std::string str = EvaluationContext::toString(left);
			std::string pattern = EvaluationContext::toString(right);
			try {
				std::regex re(pattern, std::regex_constants::ECMAScript);
				return PropertyValue(std::regex_match(str, re));
			} catch (const std::regex_error&) {
				throw ExpressionEvaluationException("Invalid regular expression: " + pattern);
			}
		}
		default:
			break;
	}
	// All comparison operators are handled above; unreachable but satisfies the compiler.
	throw ExpressionEvaluationException("Invalid comparison operator");
}

PropertyValue ExpressionEvaluator::evaluateLogical(BinaryOperatorType op,
                                                    const PropertyValue &left,
                                                    const PropertyValue &right) {
	bool leftNull = EvaluationContext::isNull(left);
	bool rightNull = EvaluationContext::isNull(right);

	switch (op) {
		case BinaryOperatorType::BOP_AND: {
			// Three-valued logic for AND:
			// false AND NULL → false
			// NULL AND false → false
			// true AND NULL → NULL
			// NULL AND true → NULL
			// NULL AND NULL → NULL
			if (!leftNull && !EvaluationContext::toBoolean(left)) return PropertyValue(false);
			if (!rightNull && !EvaluationContext::toBoolean(right)) return PropertyValue(false);
			if (leftNull || rightNull) return PropertyValue(); // NULL
			// Both non-null and both truthy (left not false, right not false) → true
			return PropertyValue(true);
		}

		case BinaryOperatorType::BOP_OR: {
			// Three-valued logic for OR:
			// true OR NULL → true
			// NULL OR true → true
			// false OR NULL → NULL
			// NULL OR false → NULL
			// NULL OR NULL → NULL
			if (!leftNull && EvaluationContext::toBoolean(left)) return PropertyValue(true);
			if (!rightNull && EvaluationContext::toBoolean(right)) return PropertyValue(true);
			if (leftNull || rightNull) return PropertyValue(); // NULL
			// Both non-null and both falsy (left not true, right not true) → false
			return PropertyValue(false);
		}

		case BinaryOperatorType::BOP_XOR: {
			// Three-valued logic for XOR:
			// Any NULL operand → NULL
			if (leftNull || rightNull) return PropertyValue();
			return PropertyValue(EvaluationContext::toBoolean(left) != EvaluationContext::toBoolean(right));
		}
		default:
			break;
	}
	// All logical operators are handled above; unreachable but satisfies the compiler.
	throw ExpressionEvaluationException("Invalid logical operator");
}

PropertyValue ExpressionEvaluator::evaluateUnary(UnaryOperatorType op, const PropertyValue &operand) {
	switch (op) {
		case UnaryOperatorType::UOP_MINUS: {
			if (operand.getType() == PropertyType::INTEGER) {
				int64_t val = std::get<int64_t>(operand.getVariant());
				return PropertyValue(-val);
			}
			double val = EvaluationContext::toDouble(operand);
			return PropertyValue(-val);
		}

		case UnaryOperatorType::UOP_NOT: {
			bool val = EvaluationContext::toBoolean(operand);
			return PropertyValue(!val);
		}
	}
	// All unary operators are handled above; unreachable but satisfies the compiler.
	return PropertyValue();
}

bool ExpressionEvaluator::propagateNull(const PropertyValue &left, const PropertyValue &right) {
	return EvaluationContext::isNull(left) || EvaluationContext::isNull(right);
}

bool ExpressionEvaluator::propagateNull(const PropertyValue &value) {
	return EvaluationContext::isNull(value);
}

void ExpressionEvaluator::visit(const ListSliceExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the list expression
	PropertyValue listValue = evaluate(expr->getList());

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

		PropertyValue indexValue = evaluate(expr->getStart());

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
		PropertyValue startValue = evaluate(expr->getStart());
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
		PropertyValue endValue = evaluate(expr->getEnd());
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

void ExpressionEvaluator::visit(const ListComprehensionExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Evaluate the list expression
	PropertyValue listValue = evaluate(expr->getListExpr());

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
			PropertyValue whereResult = evaluate(expr->getWhereExpr());

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
			PropertyValue mapResult = evaluate(expr->getMapExpr());
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

void ExpressionEvaluator::visit(const ListLiteralExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Simply return the stored list value
	result_ = expr->getValue();
}

void ExpressionEvaluator::visit(const IsNullExpression *expr) {
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

void ExpressionEvaluator::visit(const QuantifierFunctionExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Look up the quantifier function from FunctionRegistry
	const auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction(expr->getFunctionName());

	if (!func) {
		throw ExpressionEvaluationException(
			"Unknown quantifier function: " + expr->getFunctionName()
		);
	}

	// Try to cast to PredicateFunction
	const PredicateFunction* predFunc = dynamic_cast<const PredicateFunction*>(func);
	if (!predFunc) {
		throw ExpressionEvaluationException(
			"Function " + expr->getFunctionName() + " is not a quantifier function"
		);
	}

	// Clone the list and WHERE expressions for evaluation
	auto listClone = expr->getListExpression()->clone();
	auto whereClone = expr->getWhereExpression()->clone();

	// Create a mutable copy of the context for variable binding during iteration
	EvaluationContext mutableContext(context_);

	// Call the quantifier function's evaluate method
	result_ = predFunc->evaluate(
		expr->getVariable(),
		std::move(listClone),
		std::move(whereClone),
		mutableContext
	);
}

void ExpressionEvaluator::visit(const ExistsExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// Get DataManager from context for traversal
	auto* dataManager = context_.getDataManager();
	if (!dataManager) {
		throw ExpressionEvaluationException(
			"EXISTS pattern expressions require a DataManager in the evaluation context");
	}

	// Resolve source node from context
	const std::string& sourceVar = expr->getSourceVar();
	auto sourceNode = context_.getRecord().getNode(sourceVar);
	if (!sourceNode) {
		// Variable might be in values (e.g. from projection)
		result_ = PropertyValue(false);
		return;
	}

	int64_t sourceNodeId = sourceNode->getId();

	// Create traversal
	auto dmShared = dataManager->shared_from_this();
	graph::traversal::RelationshipTraversal traversal(dmShared);

	// Get edges based on direction
	std::vector<Edge> edges;
	auto direction = expr->getDirection();
	if (direction == PatternDirection::PAT_OUTGOING) {
		edges = traversal.getOutgoingEdges(sourceNodeId);
	} else if (direction == PatternDirection::PAT_INCOMING) {
		edges = traversal.getIncomingEdges(sourceNodeId);
	} else {
		edges = traversal.getAllConnectedEdges(sourceNodeId);
	}

	// Filter by relationship type if specified
	const std::string& relType = expr->getRelType();
	const std::string& targetLabel = expr->getTargetLabel();

	for (const auto& edge : edges) {
		// Filter by relationship type
		if (!relType.empty()) {
			std::string edgeType = dataManager->resolveTokenName(edge.getTypeId());
			if (edgeType != relType) {
				continue;
			}
		}

		// Filter by target label
		if (!targetLabel.empty()) {
			int64_t targetId = (direction == PatternDirection::PAT_INCOMING)
				? edge.getSourceNodeId() : edge.getTargetNodeId();
			Node targetNode = dataManager->getNode(targetId);
			std::string nodeLabel = dataManager->resolveTokenName(targetNode.getLabelId());
			if (nodeLabel != targetLabel) {
				continue;
			}
		}

		// Found a match
		result_ = PropertyValue(true);
		return;
	}

	result_ = PropertyValue(false);
}

void ExpressionEvaluator::visit(const PatternComprehensionExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	auto* dm = context_.getDataManager();
	if (!dm) {
		throw ExpressionEvaluationException(
			"Pattern comprehensions require DataManager access for graph traversal");
	}

	// Resolve source node from context
	const std::string& sourceVar = expr->getSourceVar();
	auto sourceNode = context_.getRecord().getNode(sourceVar);
	if (!sourceNode) {
		result_ = PropertyValue(std::vector<PropertyValue>{});
		return;
	}

	int64_t nodeId = sourceNode->getId();
	auto dmShared = dm->shared_from_this();
	graph::traversal::RelationshipTraversal traversal(dmShared);

	// Get edges based on direction
	std::vector<Edge> edges;
	auto dir = expr->getDirection();
	if (dir == PatternDirection::PAT_OUTGOING) {
		edges = traversal.getOutgoingEdges(nodeId);
	} else if (dir == PatternDirection::PAT_INCOMING) {
		edges = traversal.getIncomingEdges(nodeId);
	} else {
		edges = traversal.getAllConnectedEdges(nodeId);
	}

	// Filter by relationship type if specified
	const std::string& relType = expr->getRelType();
	std::vector<PropertyValue> resultList;

	for (const auto& edge : edges) {
		// Filter by relationship type
		if (!relType.empty()) {
			std::string edgeType = dm->resolveTokenName(edge.getTypeId());
			if (edgeType != relType) continue;
		}

		// Get the target node
		int64_t targetId = (dir == PatternDirection::PAT_INCOMING)
			? edge.getSourceNodeId() : edge.getTargetNodeId();

		Node targetNode = dm->getNode(targetId);

		// Filter by target label if specified
		const std::string& targetLabel = expr->getTargetLabel();
		if (!targetLabel.empty()) {
			std::string nodeLabel = dm->resolveTokenName(targetNode.getLabelId());
			if (nodeLabel != targetLabel) continue;
		}

		// Bind target variable in context
		const std::string& targetVar = expr->getTargetVar();
		if (!targetVar.empty()) {
			// Set the target node as a temporary variable with its properties
			auto props = dm->getNodeProperties(targetId);
			for (const auto& [key, val] : props) {
				context_.setVariable(targetVar + "." + key, val);
			}
			context_.setVariable(targetVar, PropertyValue(targetId));
		}

		// Evaluate WHERE clause if present
		if (expr->getWhereExpression()) {
			PropertyValue whereResult = evaluate(expr->getWhereExpression());
			if (whereResult.getType() != PropertyType::BOOLEAN ||
				!std::get<bool>(whereResult.getVariant())) {
				if (!targetVar.empty()) context_.clearVariable(targetVar);
				continue;
			}
		}

		// Evaluate map expression
		if (expr->getMapExpression()) {
			PropertyValue mapResult = evaluate(expr->getMapExpression());
			resultList.push_back(std::move(mapResult));
		} else {
			resultList.push_back(PropertyValue(targetId));
		}

		if (!targetVar.empty()) context_.clearVariable(targetVar);
	}

	result_ = PropertyValue(std::move(resultList));
}

void ExpressionEvaluator::visit(const ReduceExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	// 1. Evaluate initial value and set accumulator
	PropertyValue accumValue = evaluate(expr->getInitialExpr());
	context_.setVariable(expr->getAccumulator(), accumValue);

	// 2. Evaluate list expression
	PropertyValue listValue = evaluate(expr->getListExpr());
	if (listValue.getType() != PropertyType::LIST) {
		throw ExpressionEvaluationException("REDUCE requires a list expression");
	}

	// 3. Iterate over list, updating accumulator
	const auto& elements = listValue.getList();
	for (const auto& element : elements) {
		context_.setVariable(expr->getVariable(), element);
		accumValue = evaluate(expr->getBodyExpr());
		context_.setVariable(expr->getAccumulator(), accumValue);
	}

	// 4. Clean up temporary variables
	context_.clearVariable(expr->getAccumulator());
	context_.clearVariable(expr->getVariable());

	result_ = accumValue;
}

void ExpressionEvaluator::visit(const ParameterExpression *expr) {
	auto value = context_.resolveParameter(expr->getParameterName());
	if (!value.has_value()) {
		throw ExpressionEvaluationException(
			"Missing query parameter: $" + expr->getParameterName());
	}
	result_ = *value;
}

void ExpressionEvaluator::visit(const ShortestPathExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	auto* dm = context_.getDataManager();
	if (!dm) {
		throw ExpressionEvaluationException(
			"shortestPath requires a DataManager in the evaluation context");
	}

	// Resolve start and end nodes
	auto startNode = context_.getRecord().getNode(expr->getStartVar());
	auto endNode = context_.getRecord().getNode(expr->getEndVar());
	if (!startNode || !endNode) {
		result_ = PropertyValue();
		return;
	}

	int64_t startId = startNode->getId();
	int64_t endId = endNode->getId();

	auto dmShared = dm->shared_from_this();
	graph::traversal::RelationshipTraversal traversal(dmShared);

	// BFS to find shortest path(s)
	std::vector<std::vector<int64_t>> allPaths;
	std::queue<std::vector<int64_t>> queue;
	std::unordered_set<int64_t> visited;
	queue.push({startId});
	int shortestLen = -1;

	while (!queue.empty()) {
		auto path = queue.front();
		queue.pop();

		int64_t current = path.back();

		if (shortestLen >= 0 && static_cast<int>(path.size()) > shortestLen) {
			break; // All shortest paths found
		}

		if (current == endId) {
			shortestLen = static_cast<int>(path.size());
			allPaths.push_back(path);
			if (!expr->isAll()) break; // Only need one
			continue;
		}

		visited.insert(current);

		// Get edges
		auto dir = expr->getDirection();
		std::vector<Edge> edges;
		if (dir == PatternDirection::PAT_OUTGOING) {
			edges = traversal.getOutgoingEdges(current);
		} else if (dir == PatternDirection::PAT_INCOMING) {
			edges = traversal.getIncomingEdges(current);
		} else {
			edges = traversal.getAllConnectedEdges(current);
		}

		for (const auto& edge : edges) {
			// Filter by relationship type
			if (!expr->getRelType().empty()) {
				std::string edgeType = dm->resolveTokenName(edge.getTypeId());
				if (edgeType != expr->getRelType()) continue;
			}

			int64_t nextId = (edge.getSourceNodeId() == current)
				? edge.getTargetNodeId() : edge.getSourceNodeId();

			if (visited.find(nextId) == visited.end()) {
				auto newPath = path;
				newPath.push_back(nextId);
				queue.push(std::move(newPath));
			}
		}
	}

	if (allPaths.empty()) {
		result_ = PropertyValue();
		return;
	}

	// Build path as LIST of alternating node/relationship MAPs
	auto buildPathValue = [&](const std::vector<int64_t>& nodePath) -> PropertyValue {
		std::vector<PropertyValue> pathList;
		for (size_t i = 0; i < nodePath.size(); ++i) {
			// Add node
			Node node = dm->getNode(nodePath[i]);
			std::unordered_map<std::string, PropertyValue> nodeMap;
			nodeMap["_type"] = PropertyValue(std::string("node"));
			nodeMap["_id"] = PropertyValue(nodePath[i]);
			auto props = dm->getNodeProperties(nodePath[i]);
			for (const auto& [k, v] : props) {
				nodeMap[k] = v;
			}
			pathList.push_back(PropertyValue(std::move(nodeMap)));

			// Add edge between this node and next (if not last)
			if (i + 1 < nodePath.size()) {
				// Find the edge between nodePath[i] and nodePath[i+1]
				auto outEdges = traversal.getOutgoingEdges(nodePath[i]);
				for (const auto& edge : outEdges) {
					if (edge.getTargetNodeId() == nodePath[i + 1]) {
						std::unordered_map<std::string, PropertyValue> edgeMap;
						edgeMap["_type"] = PropertyValue(std::string("relationship"));
						edgeMap["_id"] = PropertyValue(edge.getId());
						auto eprops = dm->getEdgeProperties(edge.getId());
						for (const auto& [k, v] : eprops) {
							edgeMap[k] = v;
						}
						pathList.push_back(PropertyValue(std::move(edgeMap)));
						break;
					}
				}
				// Try incoming direction too
				if (pathList.size() % 2 == 1) {
					auto inEdges = traversal.getIncomingEdges(nodePath[i]);
					for (const auto& edge : inEdges) {
						if (edge.getSourceNodeId() == nodePath[i + 1]) {
							std::unordered_map<std::string, PropertyValue> edgeMap;
							edgeMap["_type"] = PropertyValue(std::string("relationship"));
							edgeMap["_id"] = PropertyValue(edge.getId());
							auto eprops = dm->getEdgeProperties(edge.getId());
							for (const auto& [k, v] : eprops) {
								edgeMap[k] = v;
							}
							pathList.push_back(PropertyValue(std::move(edgeMap)));
							break;
						}
					}
				}
			}
		}
		return PropertyValue(std::move(pathList));
	};

	if (expr->isAll()) {
		std::vector<PropertyValue> pathsList;
		for (const auto& p : allPaths) {
			pathsList.push_back(buildPathValue(p));
		}
		result_ = PropertyValue(std::move(pathsList));
	} else {
		result_ = buildPathValue(allPaths[0]);
	}
}

void ExpressionEvaluator::visit(const MapProjectionExpression *expr) {
	if (!expr) {
		result_ = PropertyValue();
		return;
	}

	const std::string& varName = expr->getVariable();
	std::unordered_map<std::string, PropertyValue> resultMap;

	for (const auto& item : expr->getItems()) {
		switch (item.type) {
			case MapProjectionItemType::MPROP_PROPERTY: {
				auto val = context_.resolveProperty(varName, item.key);
				resultMap[item.key] = val ? *val : PropertyValue();
				break;
			}
			case MapProjectionItemType::MPROP_LITERAL: {
				if (item.expression) {
					resultMap[item.key] = evaluate(item.expression.get());
				} else {
					resultMap[item.key] = PropertyValue();
				}
				break;
			}
			case MapProjectionItemType::MPROP_ALL_PROPERTIES: {
				// Get all properties of the entity
				auto* dm = context_.getDataManager();
				if (dm) {
					auto nodeOpt = context_.getRecord().getNode(varName);
					if (nodeOpt) {
						auto props = dm->getNodeProperties(nodeOpt->getId());
						for (const auto& [k, v] : props) {
							resultMap[k] = v;
						}
					}
				}
				break;
			}
		}
	}

	result_ = PropertyValue(std::move(resultMap));
}

} // namespace graph::query::expressions
