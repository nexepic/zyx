/**
 * @file ReadingClauseHandler.cpp
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

#include "ReadingClauseHandler.hpp"
#include "helpers/AstExtractor.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "helpers/PatternBuilder.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::execution::PhysicalOperator> ReadingClauseHandler::handleMatch(
	CypherParser::MatchStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto pattern = ctx->pattern();
	auto where = ctx->where();

	return helpers::PatternBuilder::buildMatchPattern(pattern, std::move(rootOp), planner, where);
}

std::unique_ptr<query::execution::PhysicalOperator> ReadingClauseHandler::handleStandaloneCall(
	CypherParser::StandaloneCallStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar guarantees explicitProcedureInvocation is always present
	auto invoc = ctx->explicitProcedureInvocation();
	std::string procName = invoc->procedureName()->getText();
	std::vector<PropertyValue> args;

	if (!invoc->expression().empty()) {
		for (auto expr : invoc->expression()) {
				args.push_back(helpers::ExpressionBuilder::evaluateLiteralExpression(expr));
		}
	}

	return planner->callProcedureOp(procName, args);
}

std::unique_ptr<query::execution::PhysicalOperator> ReadingClauseHandler::handleInQueryCall(
	CypherParser::InQueryCallStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto invoc = ctx->explicitProcedureInvocation();
	std::string procName = invoc->procedureName()->getText();
	std::vector<graph::PropertyValue> args;

	// Parse Arguments
	if (!invoc->expression().empty()) {
		for (auto expr : invoc->expression()) {
				args.push_back(helpers::ExpressionBuilder::evaluateLiteralExpression(expr));
		}
	}

	// Create Procedure Operator
	rootOp = planner->callProcedureOp(procName, args);

	// Handle YIELD
	if (ctx->K_YIELD() && ctx->yieldItems()) {
		std::vector<query::execution::operators::ProjectItem> projItems;
		auto items = ctx->yieldItems();

		for (auto item : items->yieldItem()) {
			std::string originalField;
			std::string outputVar = helpers::AstExtractor::extractVariable(item->variable());

			if (item->procedureResultField()) {
				originalField = item->procedureResultField()->getText();
			} else {
				// If not renamed (YIELD node), implies field name is same as variable name
				originalField = outputVar;
			}

			auto expr = std::make_shared<graph::query::expressions::VariableReferenceExpression>(originalField);
			projItems.emplace_back(expr, outputVar);
		}

		// When yieldItems exists, projItems is guaranteed to have at least one element
		rootOp = planner->projectOp(std::move(rootOp), projItems);
	}

	return rootOp;
}

std::unique_ptr<query::execution::PhysicalOperator> ReadingClauseHandler::handleUnwind(
	CypherParser::UnwindStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: unwindStatement : K_UNWIND expression K_AS variable
	// Parser guarantees variable is always present and non-empty

	// 1. Extract Alias
	std::string alias = helpers::AstExtractor::extractVariable(ctx->variable());

	// 2. Try literal list first for backward compatibility, fall back to expression-based
	auto expr = ctx->expression();
	std::vector<PropertyValue> listValues = helpers::ExpressionBuilder::extractListFromExpression(expr);

	// Ensure valid pipeline (auto-inject singleRowOp if empty)
	rootOp = graph::query::PipelineValidator::ensureValidPipeline(
	    std::move(rootOp), planner, "UNWIND",
	    graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY
	);

	// 3. Build Operator
	if (!listValues.empty()) {
		// Literal list: use compile-time list constructor
		rootOp = planner->unwindOp(std::move(rootOp), alias, listValues);
	} else {
		// Expression-based: build AST and evaluate at runtime
		auto ast = helpers::ExpressionBuilder::buildExpression(expr);
		auto astShared = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
		rootOp = planner->unwindOp(std::move(rootOp), alias, astShared);
	}

	return rootOp;
}

std::unique_ptr<query::execution::PhysicalOperator> ReadingClauseHandler::handleOptionalMatch(
	CypherParser::MatchStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto pattern = ctx->pattern();
	auto where = ctx->where();

	// OPTIONAL MATCH needs to use OptionalMatchOperator for left outer join semantics
	// For now, we'll use the regular match pattern building but mark it as optional
	// TODO: Implement full OptionalMatchOperator with proper left outer join semantics

	return helpers::PatternBuilder::buildMatchPattern(pattern, std::move(rootOp), planner, where, true);
}

} // namespace graph::parser::cypher::clauses
