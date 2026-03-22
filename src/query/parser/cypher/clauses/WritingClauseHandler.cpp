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
#include "generated/CypherLexer.h"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/planner/PipelineValidator.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleCreate(
	CypherParser::CreateStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: createStatement : K_CREATE pattern
	// Parser guarantees pattern is always present
	auto pattern = ctx->pattern();
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

	// Ensure valid pipeline (SET requires preceding clause)
	rootOp = graph::query::PipelineValidator::ensureValidPipeline(
	    std::move(rootOp), planner, "SET",
	    graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING
	);

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

	// Ensure valid pipeline (DELETE requires preceding clause)
	rootOp = graph::query::PipelineValidator::ensureValidPipeline(
	    std::move(rootOp), planner, "DELETE",
	    graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING
	);

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

	// Ensure valid pipeline (REMOVE requires preceding clause)
	rootOp = graph::query::PipelineValidator::ensureValidPipeline(
	    std::move(rootOp), planner, "REMOVE",
	    graph::query::PipelineValidator::ValidationMode::REQUIRE_PRECEDING
	);

	return planner->removeOp(std::move(rootOp), items);
}

std::unique_ptr<query::execution::PhysicalOperator> WritingClauseHandler::handleMerge(
	CypherParser::MergeStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> /*rootOp*/,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: MERGE patternPart ( ON (MATCH|CREATE) setClause )*

	auto patternPart = ctx->patternPart();

	// Parse Actions
	std::vector<query::execution::operators::SetItem> onCreateItems;
	std::vector<query::execution::operators::SetItem> onMatchItems;

	// Walk children in order using type-safe token matching
	// Grammar: K_MERGE patternPart ( K_ON ( K_MATCH | K_CREATE ) setStatement )*
	bool sawOn = false;
	bool isOnMatch = false;

	for (auto* child : ctx->children) {
		auto* termNode = dynamic_cast<antlr4::tree::TerminalNode*>(child);
		if (termNode) {
			auto tokenType = termNode->getSymbol()->getType();
			if (tokenType == CypherLexer::K_ON) {
				sawOn = true;
				continue;
			}
			if (sawOn) {
				if (tokenType == CypherLexer::K_MATCH) {
					isOnMatch = true;
				} else if (tokenType == CypherLexer::K_CREATE) {
					isOnMatch = false;
				}
				sawOn = false;
				continue;
			}
		} else {
			// Rule context - check if it's a setStatement
			auto* setCtx = dynamic_cast<CypherParser::SetStatementContext*>(child);
			if (setCtx) {
				auto items = helpers::PatternBuilder::extractSetItems(setCtx);
				if (isOnMatch) {
					onMatchItems.insert(onMatchItems.end(), items.begin(), items.end());
				} else {
					onCreateItems.insert(onCreateItems.end(), items.begin(), items.end());
				}
			}
		}
	}

	// Build Operator
	return helpers::PatternBuilder::buildMergePattern(patternPart, onCreateItems, onMatchItems, planner);
}

} // namespace graph::parser::cypher::clauses
