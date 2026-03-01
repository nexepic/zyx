/**
 * @file ResultClauseHandler.cpp
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

#include "ResultClauseHandler.hpp"
#include "helpers/AstExtractor.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/execution/operators/AggregateOperator.hpp"
#include <algorithm>
#include <unordered_map>

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::execution::PhysicalOperator> ResultClauseHandler::handleReturn(
	CypherParser::ReturnStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto body = ctx->projectionBody();

	// Check for DISTINCT
	bool distinct = (body->K_DISTINCT() != nullptr);

	// If there is no existing plan (e.g. standalone RETURN 1),
	// inject a SingleRowOperator to provide a valid pipeline source.
	if (!rootOp) {
		rootOp = planner->singleRowOp();
	}

	// Order By (MUST come before Projection so SortOperator has access to full nodes)
	// However, ORDER BY can reference projection aliases, so we need to collect those first
	std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> projectionAliases; // alias -> expression

	auto items = body->projectionItems();
	if (!items->MULTIPLY()) { // If not RETURN *
		for (auto item : items->projectionItem()) {
			// Build Expression AST from the projection expression
			auto exprAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			std::string alias;
			if (item->K_AS()) {
				alias = helpers::AstExtractor::extractVariable(item->variable());
			} else {
				alias = item->expression()->getText();
			}
			projectionAliases[alias] = std::shared_ptr<graph::query::expressions::Expression>(exprAST.release());
		}
	}

	if (body->orderStatement()) {
		std::vector<query::execution::operators::SortItem> sortItems;
		auto sortItemList = body->orderStatement()->sortItem();

		for (auto item : sortItemList) {
			// Build Expression AST from the sort expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Check if this is a simple variable reference to a projection alias
			if (auto varExpr = dynamic_cast<graph::query::expressions::VariableReferenceExpression*>(expressionAST.get())) {
				if (!varExpr->hasProperty()) {
					const std::string& varName = varExpr->getVariableName();
					// Check if this variable name is a projection alias
					auto it = projectionAliases.find(varName);
					if (it != projectionAliases.end()) {
						// Use the original projection expression instead of the alias
						expressionAST = std::unique_ptr<graph::query::expressions::Expression>(it->second->clone());
					}
				}
			}

			// Convert to shared_ptr for storage
			auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

			// Determine Direction
			bool asc = true;  // Default to ascending
			if (item->K_DESC() || item->K_DESCENDING()) {
				asc = false;  // Set to false for DESC/DESCENDING
			}

			sortItems.push_back({expressionShared, asc});
		}

		if (!sortItems.empty()) {
			rootOp = planner->sortOp(std::move(rootOp), sortItems);
		}
	}

	// Handle Projection (MUST come after Order By)
	if (!items->MULTIPLY()) { // If not RETURN *
		// Check for aggregate functions
		bool hasAggregates = false;
		std::vector<query::execution::operators::AggregateItem> aggItems;
		std::vector<query::execution::operators::ProjectItem> projItems;

		for (auto item : items->projectionItem()) {
			// Build Expression AST from the projection expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Check if this is an aggregate function
			bool isAggregate = false;
			query::execution::operators::AggregateFunctionType aggType;

			// Simple check: if the expression is a FunctionCallExpression
			if (auto funcCall = dynamic_cast<graph::query::expressions::FunctionCallExpression*>(expressionAST.get())) {
				std::string funcName = funcCall->getFunctionName();
				if (helpers::ExpressionBuilder::isAggregateFunction(funcName)) {
					isAggregate = true;
					hasAggregates = true;

					// Determine aggregate function type
					std::string nameLower = funcName;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
					               [](unsigned char c) { return std::tolower(c); });

					if (nameLower == "count") aggType = query::execution::operators::AggregateFunctionType::COUNT;
					else if (nameLower == "sum") aggType = query::execution::operators::AggregateFunctionType::SUM;
					else if (nameLower == "avg") aggType = query::execution::operators::AggregateFunctionType::AVG;
					else if (nameLower == "min") aggType = query::execution::operators::AggregateFunctionType::MIN;
					else if (nameLower == "max") aggType = query::execution::operators::AggregateFunctionType::MAX;
					else if (nameLower == "collect") aggType = query::execution::operators::AggregateFunctionType::COLLECT;

					// Get the argument expression (or nullptr for COUNT(*))
					std::shared_ptr<graph::query::expressions::Expression> argExpr(nullptr);
					if (funcCall->getArgumentCount() > 0) {
						// For aggregate functions, extract the first argument
						// Note: The function call may have arguments like n, n.prop, etc.
						// We need to extract the actual argument expression
						const auto& args = funcCall->getArguments();
						if (!args.empty()) {
							argExpr = std::shared_ptr<graph::query::expressions::Expression>(args[0]->clone().release());
						}
					}

					// Determine alias
					std::string alias = item->expression()->getText();
					if (item->K_AS()) {
						alias = helpers::AstExtractor::extractVariable(item->variable());
					}

					aggItems.emplace_back(aggType, argExpr, alias);
				}
			}

			// If not an aggregate, add to projection items
			if (!isAggregate) {
				// Convert to shared_ptr for storage
				auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

				// Alias (Output Name) - use original expression text to preserve formatting
				std::string alias = item->expression()->getText();
				if (item->K_AS()) {
					alias = helpers::AstExtractor::extractVariable(item->variable());
				}

				projItems.emplace_back(expressionShared, alias);
			}
		}

		// Apply appropriate operator
		if (hasAggregates) {
			// Use AggregateOperator for aggregations
			rootOp = std::make_unique<query::execution::operators::AggregateOperator>(
				std::move(rootOp), aggItems);
		} else if (!projItems.empty()) {
			// Use ProjectOperator for regular projections
			rootOp = planner->projectOp(std::move(rootOp), projItems, distinct);
		}
	}

	// Handle SKIP
	if (body->skipStatement()) {
		auto skipExpr = body->skipStatement()->expression();
		int64_t offset = 0;
		try {
			offset = std::stoll(skipExpr->getText());
		} catch (...) {
			throw std::runtime_error("SKIP requires an integer literal.");
		}

		rootOp = planner->skipOp(std::move(rootOp), offset);
	}

	// Handle LIMIT
	if (body->limitStatement()) {
		auto limitExpr = body->limitStatement()->expression();
		int64_t limit = 0;
		try {
			limit = std::stoll(limitExpr->getText());
		} catch (...) {
			throw std::runtime_error("LIMIT requires an integer literal.");
		}

		rootOp = planner->limitOp(std::move(rootOp), limit);
	}

	return rootOp;
}

} // namespace graph::parser::cypher::clauses
