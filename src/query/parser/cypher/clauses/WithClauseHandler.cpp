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
		std::vector<graph::query::execution::operators::ProjectItem> projItems;

		for (auto item : items->projectionItem()) {
			// Build Expression AST from the projection expression
			auto expressionAST = helpers::ExpressionBuilder::buildExpression(item->expression());

			// Convert to shared_ptr for storage
			auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

			// Alias (Output Name) - use original expression text to preserve formatting
			std::string alias = item->expression()->getText();
			if (item->K_AS()) {
				alias = helpers::AstExtractor::extractVariable(item->variable());
			}

			projItems.emplace_back(expressionShared, alias);
		}

		if (!projItems.empty()) {
			// Use ProjectOperator for WITH clause (same as RETURN but doesn't terminate)
			rootOp = planner->projectOp(std::move(rootOp), projItems, distinct);
		}
	}

	// Handle WHERE clause in WITH (WITH ... WHERE condition)
	if (ctx->where()) {
		auto ast = helpers::ExpressionBuilder::buildExpression(ctx->where()->expression());
		auto astShared = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
		std::string desc = astShared->toString();
		auto predicate = [astShared](const query::execution::Record &r) -> bool {
			return graph::query::expressions::ExpressionEvaluationHelper::evaluateBool(astShared.get(), r);
		};
		rootOp = planner->filterOp(std::move(rootOp), predicate, desc);
	}

	return rootOp;
}

} // namespace graph::parser::cypher::clauses
