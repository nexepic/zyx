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
#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include <algorithm>
#include <unordered_map>

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::logical::LogicalOperator> ResultClauseHandler::handleReturn(
	CypherParser::ReturnStatementContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	auto body = ctx->projectionBody();

	// Check for DISTINCT
	bool distinct = (body->K_DISTINCT() != nullptr);

	// Ensure valid logical pipeline (auto-inject LogicalSingleRow for standalone RETURN)
	rootOp = graph::query::PipelineValidator::ensureValidLogicalPipeline(
	    std::move(rootOp), "RETURN",
	    graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY
	);

	// Order By (MUST come before Projection so SortOperator has access to full nodes)
	// However, ORDER BY can reference projection aliases, so we need to collect those first
	std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> projectionAliases;

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
		std::vector<query::logical::LogicalSortItem> sortItems;
		auto sortItemList = body->orderStatement()->sortItem();

		for (auto item : sortItemList) {
			// Build Expression AST from the sort expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Check if this references a projection alias
			// Case 1: Simple variable reference: vals
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
			// Case 2: List slicing on alias: vals[0], vals[0..2]
			else if (auto sliceExpr = dynamic_cast<graph::query::expressions::ListSliceExpression*>(expressionAST.get())) {
				// Check if the base expression is a variable reference to an alias
				if (auto baseVarExpr = dynamic_cast<const graph::query::expressions::VariableReferenceExpression*>(sliceExpr->getList())) {
					if (!baseVarExpr->hasProperty()) {
						const std::string& varName = baseVarExpr->getVariableName();
						auto it = projectionAliases.find(varName);
						if (it != projectionAliases.end()) {
							// Replace the base expression with the aliased expression
							auto newSliceExpr = std::make_unique<graph::query::expressions::ListSliceExpression>(
								std::unique_ptr<graph::query::expressions::Expression>(it->second->clone()),
								sliceExpr->getStart() ? std::unique_ptr<graph::query::expressions::Expression>(sliceExpr->getStart()->clone()) : nullptr,
								sliceExpr->getEnd() ? std::unique_ptr<graph::query::expressions::Expression>(sliceExpr->getEnd()->clone()) : nullptr,
								sliceExpr->hasRange()
							);
							expressionAST = std::move(newSliceExpr);
						}
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

			sortItems.emplace_back(expressionShared, asc);
		}

		if (!sortItems.empty()) {
			rootOp = std::make_unique<query::logical::LogicalSort>(
				std::move(rootOp), std::move(sortItems));
		}
	}

	// Handle Projection (MUST come after Order By)
	if (!items->MULTIPLY()) { // If not RETURN *
		// Check for aggregate functions
		bool hasAggregates = false;
		std::vector<query::logical::LogicalAggItem> aggItems;
		std::vector<query::logical::LogicalProjectItem> projItems;

		for (auto item : items->projectionItem()) {
			// Build Expression AST from the projection expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Check if this is an aggregate function
			bool isAggregate = false;
			std::string aggFuncName;

			// Simple check: if the expression is a FunctionCallExpression
			if (auto funcCall = dynamic_cast<graph::query::expressions::FunctionCallExpression*>(expressionAST.get())) {
				std::string funcName = funcCall->getFunctionName();
				if (helpers::ExpressionBuilder::isAggregateFunction(funcName)) {
					isAggregate = true;
					hasAggregates = true;

					// Store the function name in lowercase for LogicalAggregate
					aggFuncName = funcName;
					std::transform(aggFuncName.begin(), aggFuncName.end(), aggFuncName.begin(),
					               [](unsigned char c) { return std::tolower(c); });

					// Get the argument expression (or nullptr for COUNT(*))
					std::shared_ptr<graph::query::expressions::Expression> argExpr(nullptr);
					if (funcCall->getArgumentCount() > 0) {
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

					aggItems.emplace_back(aggFuncName, argExpr, alias, funcCall->isDistinct());
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

		// Apply appropriate logical operator
		if (hasAggregates) {
			// Convert non-aggregate projection items to group-by expressions with aliases
			std::vector<std::shared_ptr<graph::query::expressions::Expression>> groupByExprs;
			std::vector<std::string> groupByAliases;
			for (const auto& pi : projItems) {
				groupByExprs.push_back(pi.expression);
				groupByAliases.push_back(pi.alias);
			}
			rootOp = std::make_unique<query::logical::LogicalAggregate>(
				std::move(rootOp), std::move(groupByExprs), std::move(aggItems),
				std::move(groupByAliases));
		} else if (!projItems.empty()) {
			rootOp = std::make_unique<query::logical::LogicalProject>(
				std::move(rootOp), std::move(projItems), distinct);
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

		rootOp = std::make_unique<query::logical::LogicalSkip>(std::move(rootOp), offset);
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

		rootOp = std::make_unique<query::logical::LogicalLimit>(std::move(rootOp), limit);
	}

	return rootOp;
}

} // namespace graph::parser::cypher::clauses
