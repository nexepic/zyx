/**
 * @file ExpressionUtils.cpp
 * @brief Implementation of shared expression analysis utilities.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include <algorithm>

namespace graph::query::expressions {

// =============================================================================
// isAggregateFunction
// =============================================================================

bool ExpressionUtils::isAggregateFunction(const std::string& functionName) {
	std::string nameLower = functionName;
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
	               [](unsigned char c) { return std::tolower(c); });

	return nameLower == "count" || nameLower == "sum" || nameLower == "avg" ||
	       nameLower == "min" || nameLower == "max" || nameLower == "collect" ||
	       nameLower == "stdev" || nameLower == "stdevp" ||
	       nameLower == "percentiledisc" || nameLower == "percentilecont";
}

// =============================================================================
// containsAggregate — complete recursive check over ALL expression types
// =============================================================================

bool ExpressionUtils::containsAggregate(const Expression* expr) {
	if (!expr) return false;

	switch (expr->getExpressionType()) {
		case ExpressionType::FUNCTION_CALL: {
			auto* funcCall = static_cast<const FunctionCallExpression*>(expr);
			if (isAggregateFunction(funcCall->getFunctionName())) return true;
			for (const auto& arg : funcCall->getArguments()) {
				if (containsAggregate(arg.get())) return true;
			}
			return false;
		}
		case ExpressionType::BINARY_OP: {
			auto* binOp = static_cast<const BinaryOpExpression*>(expr);
			return containsAggregate(binOp->getLeft()) ||
			       containsAggregate(binOp->getRight());
		}
		case ExpressionType::UNARY_OP: {
			auto* unaryOp = static_cast<const UnaryOpExpression*>(expr);
			return containsAggregate(unaryOp->getOperand());
		}
		case ExpressionType::CASE_EXPRESSION: {
			auto* caseExpr = static_cast<const CaseExpression*>(expr);
			if (caseExpr->getComparisonExpression() &&
			    containsAggregate(caseExpr->getComparisonExpression())) {
				return true;
			}
			for (const auto& branch : caseExpr->getBranches()) {
				if (containsAggregate(branch.whenExpression.get()) ||
				    containsAggregate(branch.thenExpression.get())) {
					return true;
				}
			}
			if (caseExpr->getElseExpression()) {
				return containsAggregate(caseExpr->getElseExpression());
			}
			return false;
		}
		case ExpressionType::IS_NULL: {
			auto* isNull = static_cast<const IsNullExpression*>(expr);
			return containsAggregate(isNull->getExpression());
		}
		case ExpressionType::IN_LIST: {
			auto* inExpr = static_cast<const InExpression*>(expr);
			if (containsAggregate(inExpr->getValue())) return true;
			if (inExpr->hasDynamicList()) {
				return containsAggregate(inExpr->getListExpression());
			}
			return false;
		}
		case ExpressionType::LIST_SLICE: {
			auto* slice = static_cast<const ListSliceExpression*>(expr);
			return containsAggregate(slice->getList()) ||
			       containsAggregate(slice->getStart()) ||
			       containsAggregate(slice->getEnd());
		}
		case ExpressionType::LIST_COMPREHENSION: {
			auto* comp = static_cast<const ListComprehensionExpression*>(expr);
			return containsAggregate(comp->getListExpr()) ||
			       containsAggregate(comp->getWhereExpr()) ||
			       containsAggregate(comp->getMapExpr());
		}
		case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
			auto* quant = static_cast<const QuantifierFunctionExpression*>(expr);
			return containsAggregate(quant->getListExpression()) ||
			       containsAggregate(quant->getWhereExpression());
		}
		case ExpressionType::EXPR_REDUCE: {
			auto* reduce = static_cast<const ReduceExpression*>(expr);
			return containsAggregate(reduce->getInitialExpr()) ||
			       containsAggregate(reduce->getListExpr()) ||
			       containsAggregate(reduce->getBodyExpr());
		}
		case ExpressionType::EXPR_EXISTS: {
			auto* exists = static_cast<const ExistsExpression*>(expr);
			if (exists->hasWhereClause()) {
				return containsAggregate(exists->getWhereExpression());
			}
			return false;
		}
		case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
			auto* patComp = static_cast<const PatternComprehensionExpression*>(expr);
			if (containsAggregate(patComp->getMapExpression())) return true;
			if (patComp->hasWhereClause()) {
				return containsAggregate(patComp->getWhereExpression());
			}
			return false;
		}
		case ExpressionType::MAP_PROJECTION: {
			auto* mapProj = static_cast<const MapProjectionExpression*>(expr);
			for (const auto& item : mapProj->getItems()) {
				if (item.expression && containsAggregate(item.expression.get())) {
					return true;
				}
			}
			return false;
		}
		case ExpressionType::LITERAL:
		case ExpressionType::VARIABLE_REFERENCE:
		case ExpressionType::PROPERTY_ACCESS:
		case ExpressionType::PARAMETER:
		case ExpressionType::SHORTEST_PATH:
		case ExpressionType::EXPR_LIST_LITERAL:
			return false;
	}
	return false; // unreachable, but satisfies compiler
}

// =============================================================================
// collectVariables — complete recursive variable collection
// =============================================================================

namespace {

template<typename SetType>
void collectVarsImpl(const Expression* expr, SetType& out) {
	if (!expr) return;

	switch (expr->getExpressionType()) {
		case ExpressionType::VARIABLE_REFERENCE:
		case ExpressionType::PROPERTY_ACCESS: {
			auto* varExpr = static_cast<const VariableReferenceExpression*>(expr);
			out.insert(varExpr->getVariableName());
			break;
		}
		case ExpressionType::BINARY_OP: {
			auto* binExpr = static_cast<const BinaryOpExpression*>(expr);
			collectVarsImpl(binExpr->getLeft(), out);
			collectVarsImpl(binExpr->getRight(), out);
			break;
		}
		case ExpressionType::UNARY_OP: {
			auto* unExpr = static_cast<const UnaryOpExpression*>(expr);
			collectVarsImpl(unExpr->getOperand(), out);
			break;
		}
		case ExpressionType::IS_NULL: {
			auto* isNull = static_cast<const IsNullExpression*>(expr);
			collectVarsImpl(isNull->getExpression(), out);
			break;
		}
		case ExpressionType::IN_LIST: {
			auto* inExpr = static_cast<const InExpression*>(expr);
			collectVarsImpl(inExpr->getValue(), out);
			if (inExpr->hasDynamicList()) {
				collectVarsImpl(inExpr->getListExpression(), out);
			}
			break;
		}
		case ExpressionType::FUNCTION_CALL: {
			auto* funcCall = static_cast<const FunctionCallExpression*>(expr);
			for (const auto& arg : funcCall->getArguments()) {
				collectVarsImpl(arg.get(), out);
			}
			break;
		}
		case ExpressionType::CASE_EXPRESSION: {
			auto* caseExpr = static_cast<const CaseExpression*>(expr);
			if (caseExpr->getComparisonExpression()) {
				collectVarsImpl(caseExpr->getComparisonExpression(), out);
			}
			for (const auto& branch : caseExpr->getBranches()) {
				collectVarsImpl(branch.whenExpression.get(), out);
				collectVarsImpl(branch.thenExpression.get(), out);
			}
			if (caseExpr->getElseExpression()) {
				collectVarsImpl(caseExpr->getElseExpression(), out);
			}
			break;
		}
		case ExpressionType::LIST_SLICE: {
			auto* slice = static_cast<const ListSliceExpression*>(expr);
			collectVarsImpl(slice->getList(), out);
			collectVarsImpl(slice->getStart(), out);
			collectVarsImpl(slice->getEnd(), out);
			break;
		}
		case ExpressionType::LIST_COMPREHENSION: {
			auto* comp = static_cast<const ListComprehensionExpression*>(expr);
			collectVarsImpl(comp->getListExpr(), out);
			collectVarsImpl(comp->getWhereExpr(), out);
			collectVarsImpl(comp->getMapExpr(), out);
			break;
		}
		case ExpressionType::EXPR_QUANTIFIER_FUNCTION: {
			auto* quant = static_cast<const QuantifierFunctionExpression*>(expr);
			collectVarsImpl(quant->getListExpression(), out);
			collectVarsImpl(quant->getWhereExpression(), out);
			break;
		}
		case ExpressionType::EXPR_REDUCE: {
			auto* reduce = static_cast<const ReduceExpression*>(expr);
			collectVarsImpl(reduce->getInitialExpr(), out);
			collectVarsImpl(reduce->getListExpr(), out);
			collectVarsImpl(reduce->getBodyExpr(), out);
			break;
		}
		case ExpressionType::EXPR_EXISTS: {
			auto* exists = static_cast<const ExistsExpression*>(expr);
			if (!exists->getSourceVar().empty()) {
				out.insert(exists->getSourceVar());
			}
			if (exists->hasWhereClause()) {
				collectVarsImpl(exists->getWhereExpression(), out);
			}
			break;
		}
		case ExpressionType::EXPR_PATTERN_COMPREHENSION: {
			auto* patComp = static_cast<const PatternComprehensionExpression*>(expr);
			if (!patComp->getSourceVar().empty()) {
				out.insert(patComp->getSourceVar());
			}
			collectVarsImpl(patComp->getMapExpression(), out);
			if (patComp->hasWhereClause()) {
				collectVarsImpl(patComp->getWhereExpression(), out);
			}
			break;
		}
		case ExpressionType::MAP_PROJECTION: {
			auto* mapProj = static_cast<const MapProjectionExpression*>(expr);
			out.insert(mapProj->getVariable());
			for (const auto& item : mapProj->getItems()) {
				if (item.expression) {
					collectVarsImpl(item.expression.get(), out);
				}
			}
			break;
		}
		case ExpressionType::SHORTEST_PATH: {
			auto* sp = static_cast<const ShortestPathExpression*>(expr);
			out.insert(sp->getStartVar());
			out.insert(sp->getEndVar());
			break;
		}
		case ExpressionType::LITERAL:
		case ExpressionType::PARAMETER:
		case ExpressionType::EXPR_LIST_LITERAL:
			break;
	}
}

} // anonymous namespace

void ExpressionUtils::collectVariables(const Expression* expr, std::set<std::string>& out) {
	collectVarsImpl(expr, out);
}

void ExpressionUtils::collectVariables(const Expression* expr, std::unordered_set<std::string>& out) {
	collectVarsImpl(expr, out);
}

// =============================================================================
// extractAndReplaceAggregates — complete recursive aggregate extraction
// =============================================================================

std::unique_ptr<Expression> ExpressionUtils::extractAndReplaceAggregates(
	const Expression* expr,
	std::vector<logical::LogicalAggItem>& aggItems,
	size_t& counter)
{
	if (!expr) return nullptr;

	// If this IS a top-level aggregate function call, extract it.
	if (expr->getExpressionType() == ExpressionType::FUNCTION_CALL) {
		auto* funcCall = static_cast<const FunctionCallExpression*>(expr);
		std::string funcName = funcCall->getFunctionName();
		std::string funcNameLower = funcName;
		std::transform(funcNameLower.begin(), funcNameLower.end(), funcNameLower.begin(),
		               [](unsigned char c) { return std::tolower(c); });

		if (isAggregateFunction(funcNameLower)) {
			std::string tempAlias = "__agg_" + std::to_string(counter++);

			std::shared_ptr<Expression> argExpr = nullptr;
			if (funcCall->getArgumentCount() > 0) {
				const auto& args = funcCall->getArguments();
				if (!args.empty()) {
					argExpr = std::shared_ptr<Expression>(args[0]->clone().release());
				}
			}

			// For percentile functions, capture the second argument (percentile value)
			std::shared_ptr<Expression> extraArg = nullptr;
			if ((funcNameLower == "percentiledisc" || funcNameLower == "percentilecont") &&
			    funcCall->getArgumentCount() > 1) {
				const auto& args = funcCall->getArguments();
				extraArg = std::shared_ptr<Expression>(args[1]->clone().release());
			}

			aggItems.emplace_back(funcNameLower, argExpr, tempAlias, funcCall->isDistinct(), extraArg);
			return std::make_unique<VariableReferenceExpression>(tempAlias);
		}

		// Non-aggregate function: recurse into arguments
		std::vector<std::unique_ptr<Expression>> newArgs;
		for (const auto& arg : funcCall->getArguments()) {
			newArgs.push_back(extractAndReplaceAggregates(arg.get(), aggItems, counter));
		}
		return std::make_unique<FunctionCallExpression>(
			funcCall->getFunctionName(), std::move(newArgs), funcCall->isDistinct());
	}

	switch (expr->getExpressionType()) {
		case ExpressionType::BINARY_OP: {
			auto* binOp = static_cast<const BinaryOpExpression*>(expr);
			auto newLeft = extractAndReplaceAggregates(binOp->getLeft(), aggItems, counter);
			auto newRight = extractAndReplaceAggregates(binOp->getRight(), aggItems, counter);
			return std::make_unique<BinaryOpExpression>(
				std::move(newLeft), binOp->getOperator(), std::move(newRight));
		}
		case ExpressionType::UNARY_OP: {
			auto* unaryOp = static_cast<const UnaryOpExpression*>(expr);
			auto newOperand = extractAndReplaceAggregates(unaryOp->getOperand(), aggItems, counter);
			return std::make_unique<UnaryOpExpression>(
				unaryOp->getOperator(), std::move(newOperand));
		}
		case ExpressionType::IS_NULL: {
			auto* isNull = static_cast<const IsNullExpression*>(expr);
			auto newInner = extractAndReplaceAggregates(isNull->getExpression(), aggItems, counter);
			return std::make_unique<IsNullExpression>(std::move(newInner), isNull->isNegated());
		}
		case ExpressionType::IN_LIST: {
			auto* inExpr = static_cast<const InExpression*>(expr);
			auto newValue = extractAndReplaceAggregates(inExpr->getValue(), aggItems, counter);
			if (inExpr->hasDynamicList()) {
				auto newList = extractAndReplaceAggregates(inExpr->getListExpression(), aggItems, counter);
				return std::make_unique<InExpression>(std::move(newValue), std::move(newList));
			}
			return std::make_unique<InExpression>(std::move(newValue), inExpr->getListValues());
		}
		case ExpressionType::CASE_EXPRESSION: {
			auto* caseExpr = static_cast<const CaseExpression*>(expr);
			std::unique_ptr<CaseExpression> newCase;
			if (caseExpr->isSimpleCase()) {
				newCase = std::make_unique<CaseExpression>(
					extractAndReplaceAggregates(caseExpr->getComparisonExpression(), aggItems, counter));
			} else {
				newCase = std::make_unique<CaseExpression>();
			}
			for (const auto& branch : caseExpr->getBranches()) {
				newCase->addBranch(
					extractAndReplaceAggregates(branch.whenExpression.get(), aggItems, counter),
					extractAndReplaceAggregates(branch.thenExpression.get(), aggItems, counter));
			}
			if (caseExpr->getElseExpression()) {
				newCase->setElseExpression(
					extractAndReplaceAggregates(caseExpr->getElseExpression(), aggItems, counter));
			}
			return newCase;
		}
		default:
			return expr->clone();
	}
}

} // namespace graph::query::expressions
