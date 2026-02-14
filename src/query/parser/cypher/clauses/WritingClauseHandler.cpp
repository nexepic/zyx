/**
 * @file WritingClauseHandler.cpp
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

#include "WritingClauseHandler.hpp"
#include "helpers/AstExtractor.hpp"
#include "helpers/PatternBuilder.hpp"
#include "graph/query/planner/QueryPlanner.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleCreate(
	CypherParser::CreateStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto pattern = ctx->pattern();
	if (!pattern)
		return rootOp;

	return helpers::PatternBuilder::buildCreatePattern(pattern, std::move(rootOp), planner);
}

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleSet(
	CypherParser::SetStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Prepare a list of items to update
	std::vector<query::execution::operators::SetItem> items;

	// Use the helper to extract SET items
	items = helpers::PatternBuilder::extractSetItems(ctx);

	// Ensure we have a valid pipeline to operate on
	if (!rootOp) {
		throw std::runtime_error("SET clause must follow a MATCH or CREATE clause.");
	}

	// Create and chain the SetOperator
	return planner->setOp(std::move(rootOp), items);
}

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleDelete(
	CypherParser::DeleteStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: DETACH? DELETE expression (COMMA expression)*

	bool detach = (ctx->K_DETACH() != nullptr);
	std::vector<std::string> vars;

	for (auto expr : ctx->expression()) {
		// Simplified: Expect expression to be a variable name
		vars.push_back(expr->getText());
	}

	if (!rootOp) {
		throw std::runtime_error("DELETE cannot be the start of a query. Use MATCH first.");
	}

	return planner->deleteOp(std::move(rootOp), vars, detach);
}

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleRemove(
	CypherParser::RemoveStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	std::vector<query::execution::operators::RemoveItem> items;

	for (auto item : ctx->removeItem()) {
		// Case 1: n.prop (Property)
		if (item->propertyExpression()) {
			auto propExpr = item->propertyExpression();
			std::string varName = propExpr->atom()->getText();
			std::string keyName = propExpr->propertyKeyName(0)->getText();
			items.push_back({query::execution::operators::RemoveActionType::PROPERTY, varName, keyName});
		}
		// Case 2: n:Label (Label)
		else if (item->variable() && item->nodeLabels()) {
			std::string varName = helpers::AstExtractor::extractVariable(item->variable());
			std::string labelName = helpers::AstExtractor::extractLabel(item->nodeLabels());
			items.push_back({query::execution::operators::RemoveActionType::LABEL, varName, labelName});
		}
	}

	if (!rootOp)
		throw std::runtime_error("REMOVE must follow a MATCH or CREATE");

	return planner->removeOp(std::move(rootOp), items);
}

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleMerge(
	CypherParser::MergeStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: MERGE patternPart ( ON (MATCH|CREATE) setClause )*

	auto patternPart = ctx->patternPart();

	// Parse Actions
	std::vector<query::execution::operators::SetItem> onCreateItems;
	std::vector<query::execution::operators::SetItem> onMatchItems;

	// Iterate children to find ON MATCH / ON CREATE blocks
	for (size_t i = 0; i < ctx->children.size(); ++i) {
		if (ctx->children[i]->getText() == "ON") {
			// Next is MATCH or CREATE
			std::string type = ctx->children[i + 1]->getText(); // MATCH / CREATE
			// Next is SET clause (Parser rule context)
			auto setCtx = dynamic_cast<CypherParser::SetStatementContext *>(ctx->children[i + 2]);

			auto items = helpers::PatternBuilder::extractSetItems(setCtx);
			if (type == "MATCH") {
				onMatchItems.insert(onMatchItems.end(), items.begin(), items.end());
			} else if (type == "CREATE") {
				onCreateItems.insert(onCreateItems.end(), items.begin(), items.end());
			}
		}
	}

	// Build Operator
	return helpers::PatternBuilder::buildMergePattern(patternPart, onCreateItems, onMatchItems, planner);
}

} // namespace graph::parser::cypher::clauses
