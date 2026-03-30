/**
 * @file WithClauseHandler.cpp
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

#include "WithClauseHandler.hpp"
#include "helpers/AstExtractor.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/QueryErrorMessages.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include <algorithm>

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::logical::LogicalOperator> WithClauseHandler::handleWith(
	CypherParser::WithClauseContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	// Grammar: withClause : K_WITH projectionBody ( K_WHERE where )?
	// Parser guarantees ctx and projectionBody are always present (non-null)

	// Validate pipeline (WITH requires preceding clause)
	if (!rootOp) {
		throw std::runtime_error("WITH clause must follow a MATCH or CREATE clause.");
	}

	auto projectionBody = ctx->projectionBody();

	// Check for DISTINCT
	bool distinct = (projectionBody->K_DISTINCT() != nullptr);

	// Get projection items
	auto items = projectionBody->projectionItems();

	// Process projection
	if (!items->MULTIPLY()) { // If not WITH *
		bool hasAggregates = false;
		std::vector<query::logical::LogicalAggItem> aggItems;
		std::vector<query::logical::LogicalProjectItem> projItems;

		for (auto item : items->projectionItem()) {
			// Build Expression AST from the projection expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Check if this is an aggregate function
			bool isAggregate = false;
			std::string aggFuncName;

			if (auto funcCall = dynamic_cast<graph::query::expressions::FunctionCallExpression*>(expressionAST.get())) {
				std::string funcName = funcCall->getFunctionName();
				if (helpers::ExpressionBuilder::isAggregateFunction(funcName)) {
					isAggregate = true;
					hasAggregates = true;

					aggFuncName = funcName;
					std::transform(aggFuncName.begin(), aggFuncName.end(), aggFuncName.begin(),
					               [](unsigned char c) { return std::tolower(c); });

					std::shared_ptr<graph::query::expressions::Expression> argExpr(nullptr);
					if (funcCall->getArgumentCount() > 0) {
						const auto& args = funcCall->getArguments();
						if (!args.empty()) {
							argExpr = std::shared_ptr<graph::query::expressions::Expression>(args[0]->clone().release());
						}
					}

					std::string alias = item->expression()->getText();
					if (item->K_AS()) {
						alias = helpers::AstExtractor::extractVariable(item->variable());
					}

					aggItems.emplace_back(aggFuncName, argExpr, alias, funcCall->isDistinct());
				}
			}

			if (!isAggregate) {
				auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

				std::string alias = item->expression()->getText();
				if (item->K_AS()) {
					alias = helpers::AstExtractor::extractVariable(item->variable());
				}

				projItems.emplace_back(expressionShared, alias);
			}
		}

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

	// Handle WHERE clause in WITH (WITH ... WHERE condition)
	if (ctx->where()) {
		auto ast = helpers::ExpressionBuilder::buildExpression(ctx->where()->expression());
		auto astShared = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
		rootOp = std::make_unique<query::logical::LogicalFilter>(std::move(rootOp), astShared);
	}

	return rootOp;
}

} // namespace graph::parser::cypher::clauses
