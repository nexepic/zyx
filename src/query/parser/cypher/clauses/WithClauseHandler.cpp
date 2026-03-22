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
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/execution/operators/AggregateOperator.hpp"
#include <algorithm>

namespace graph::parser::cypher::clauses {

std::unique_ptr<graph::query::execution::PhysicalOperator> WithClauseHandler::handleWith(
	CypherParser::WithClauseContext *ctx,
	std::unique_ptr<graph::query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: withClause : K_WITH projectionBody ( K_WHERE where )?
	// Parser guarantees ctx and projectionBody are always present (non-null)
	// Following project pattern: trust grammar guarantees like ResultClauseHandler

	// Ensure valid pipeline (WITH requires preceding clause)
	rootOp = graph::query::PipelineValidator::ensureValidPipeline(
	    std::move(rootOp), planner, "WITH",
	    graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING
	);

	auto projectionBody = ctx->projectionBody();

	// Check for DISTINCT
	bool distinct = (projectionBody->K_DISTINCT() != nullptr);

	// Get projection items
	auto items = projectionBody->projectionItems();

	// Process projection
	if (!items->MULTIPLY()) { // If not WITH *
		bool hasAggregates = false;
		std::vector<query::execution::operators::AggregateItem> aggItems;
		std::vector<graph::query::execution::operators::ProjectItem> projItems;

		for (auto item : items->projectionItem()) {
			// Build Expression AST from the projection expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Check if this is an aggregate function
			bool isAggregate = false;
			query::execution::operators::AggregateFunctionType aggType{};

			if (auto funcCall = dynamic_cast<graph::query::expressions::FunctionCallExpression*>(expressionAST.get())) {
				std::string funcName = funcCall->getFunctionName();
				if (helpers::ExpressionBuilder::isAggregateFunction(funcName)) {
					isAggregate = true;
					hasAggregates = true;

					std::string nameLower = funcName;
					std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
					               [](unsigned char c) { return std::tolower(c); });

					if (nameLower == "count") aggType = query::execution::operators::AggregateFunctionType::AGG_COUNT;
					else if (nameLower == "sum") aggType = query::execution::operators::AggregateFunctionType::AGG_SUM;
					else if (nameLower == "avg") aggType = query::execution::operators::AggregateFunctionType::AGG_AVG;
					else if (nameLower == "min") aggType = query::execution::operators::AggregateFunctionType::AGG_MIN;
					else if (nameLower == "max") aggType = query::execution::operators::AggregateFunctionType::AGG_MAX;
					else if (nameLower == "collect") aggType = query::execution::operators::AggregateFunctionType::AGG_COLLECT;

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

					aggItems.emplace_back(aggType, argExpr, alias, funcCall->isDistinct());
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
			// Convert non-aggregate projection items to group-by items
			std::vector<query::execution::operators::GroupByItem> groupByItems;
			for (const auto& pi : projItems) {
				groupByItems.emplace_back(pi.expression, pi.alias);
			}
			rootOp = std::make_unique<query::execution::operators::AggregateOperator>(
				std::move(rootOp), aggItems, std::move(groupByItems), planner->getDataManager().get());
		} else if (!projItems.empty()) {
			rootOp = planner->projectOp(std::move(rootOp), projItems, distinct);
		}
	}

	// Handle WHERE clause in WITH (WITH ... WHERE condition)
	if (ctx->where()) {
		auto ast = helpers::ExpressionBuilder::buildExpression(ctx->where()->expression());
		auto astShared = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
		std::string desc = astShared->toString();
		auto dm = planner->getDataManager().get();
		auto predicate = [astShared, dm](const query::execution::Record &r) -> bool {
			return graph::query::expressions::ExpressionEvaluationHelper::evaluateBool(astShared.get(), r, dm);
		};
		rootOp = planner->filterOp(std::move(rootOp), predicate, desc);
	}

	return rootOp;
}

} // namespace graph::parser::cypher::clauses
