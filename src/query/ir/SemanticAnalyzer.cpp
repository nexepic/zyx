/**
 * @file SemanticAnalyzer.cpp
 * @brief Implementation of semantic analysis for ProjectionBody.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/ir/SemanticAnalyzer.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"

namespace graph::query::ir {

void SemanticAnalyzer::analyzeProjection(ProjectionBody& body) {
	using namespace graph::query::expressions;

	// =========================================================================
	// Step 1: Classify each projection item
	// =========================================================================
	body.hasAggregates = false;

	for (auto& item : body.items) {
		item.containsAggregate = ExpressionUtils::containsAggregate(item.expression.get());

		if (item.containsAggregate) {
			body.hasAggregates = true;

			// Check if the top-level expression itself is an aggregate function call
			if (item.expression->getExpressionType() == ExpressionType::FUNCTION_CALL) {
				auto* funcCall = static_cast<const FunctionCallExpression*>(item.expression.get());
				item.isTopLevelAggregate = ExpressionUtils::isAggregateFunction(funcCall->getFunctionName());
			} else {
				item.isTopLevelAggregate = false;
			}
		}
	}

	// =========================================================================
	// Step 2: Resolve ORDER BY aliases
	// =========================================================================
	if (!body.hasAggregates) {
		// Build alias map: alias -> expression
		std::unordered_map<std::string, std::shared_ptr<Expression>> aliasMap;
		for (const auto& item : body.items) {
			aliasMap[item.alias] = item.expression;
		}

		// For non-aggregate queries, expand aliases to original expressions
		// (Sort needs access to full record before projection strips columns)
		for (auto& sortItem : body.orderBy) {
			auto* expr = sortItem.expression.get();
			if (!expr) continue;

			if (auto* varExpr = dynamic_cast<VariableReferenceExpression*>(expr)) {
				if (!varExpr->hasProperty()) {
					auto it = aliasMap.find(varExpr->getVariableName());
					if (it != aliasMap.end()) {
						sortItem.expression = std::shared_ptr<Expression>(it->second->clone());
					}
				}
			} else if (auto* sliceExpr = dynamic_cast<ListSliceExpression*>(expr)) {
				if (auto* baseVarExpr = dynamic_cast<const VariableReferenceExpression*>(sliceExpr->getList())) {
					if (!baseVarExpr->hasProperty()) {
						auto it = aliasMap.find(baseVarExpr->getVariableName());
						if (it != aliasMap.end()) {
							sortItem.expression = std::make_shared<ListSliceExpression>(
								std::unique_ptr<Expression>(it->second->clone()),
								sliceExpr->getStart() ? std::unique_ptr<Expression>(sliceExpr->getStart()->clone()) : nullptr,
								sliceExpr->getEnd() ? std::unique_ptr<Expression>(sliceExpr->getEnd()->clone()) : nullptr,
								sliceExpr->hasRange()
							);
						}
					}
				}
			}
		}
	}
	// For aggregate queries: DON'T expand aliases.
	// Sort will be placed after Aggregate, referencing output aliases directly.
}

} // namespace graph::query::ir
