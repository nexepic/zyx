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
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::logical::LogicalOperator> WritingClauseHandler::handleCreate(
	CypherParser::CreateStatementContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Grammar: createStatement : K_CREATE pattern
	// Parser guarantees pattern is always present
	auto pattern = ctx->pattern();
	return helpers::PatternBuilder::buildCreatePattern(pattern, std::move(rootOp), planner);
}

std::unique_ptr<query::logical::LogicalOperator> WritingClauseHandler::handleSet(
	CypherParser::SetStatementContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	// Extract physical SetItems from the SET clause
	auto physicalItems = helpers::PatternBuilder::extractSetItems(ctx);

	// Validate pipeline (SET requires preceding clause)
	if (!rootOp) {
		throw std::runtime_error("SET clause must follow a MATCH or CREATE clause.");
	}

	// Convert physical SetItem to LogicalSetItem
	std::vector<query::logical::LogicalSetItem> logicalItems;
	logicalItems.reserve(physicalItems.size());
	for (const auto &item : physicalItems) {
		query::logical::SetActionType logType;
		switch (item.type) {
			case query::execution::operators::SetActionType::PROPERTY:
				logType = query::logical::SetActionType::LSET_PROPERTY;
				break;
			case query::execution::operators::SetActionType::LABEL:
				logType = query::logical::SetActionType::LSET_LABEL;
				break;
			case query::execution::operators::SetActionType::MAP_MERGE:
				logType = query::logical::SetActionType::LSET_MAP_MERGE;
				break;
			default:
				logType = query::logical::SetActionType::LSET_PROPERTY;
				break;
		}
		logicalItems.push_back({logType, item.variable, item.key, item.expression});
	}

	return std::make_unique<query::logical::LogicalSet>(std::move(logicalItems), std::move(rootOp));
}

std::unique_ptr<query::logical::LogicalOperator> WritingClauseHandler::handleDelete(
	CypherParser::DeleteStatementContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	// Grammar: DETACH? DELETE expression (COMMA expression)*

	bool detach = (ctx->K_DETACH() != nullptr);
	std::vector<std::string> vars;

	for (auto expr : ctx->expression()) {
		// Simplified: Expect expression to be a variable name
		vars.push_back(expr->getText());
	}

	// Validate pipeline (DELETE requires preceding clause)
	if (!rootOp) {
		throw std::runtime_error("DELETE clause must follow a MATCH or CREATE clause.");
	}

	return std::make_unique<query::logical::LogicalDelete>(vars, detach, std::move(rootOp));
}

std::unique_ptr<query::logical::LogicalOperator> WritingClauseHandler::handleRemove(
	CypherParser::RemoveStatementContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	std::vector<query::logical::LogicalRemoveItem> logicalItems;

	for (auto item : ctx->removeItem()) {
		// Case 1: n.prop (Property)
		if (item->propertyExpression()) {
			auto propExpr = item->propertyExpression();
			std::string varName = propExpr->atom()->getText();
			std::string keyName = propExpr->propertyKeyName(0)->getText();
			logicalItems.push_back(
				{query::logical::LogicalRemoveActionType::LREM_PROPERTY, varName, keyName});
		}
		// Case 2: n:Label (Label)
		else if (item->variable() && item->nodeLabels()) {
			std::string varName = helpers::AstExtractor::extractVariable(item->variable());
			std::string labelName = helpers::AstExtractor::extractLabel(item->nodeLabels());
			logicalItems.push_back(
				{query::logical::LogicalRemoveActionType::LREM_LABEL, varName, labelName});
		}
	}

	// Validate pipeline (REMOVE requires preceding clause)
	if (!rootOp) {
		throw std::runtime_error("REMOVE clause must follow a MATCH or CREATE clause.");
	}

	return std::make_unique<query::logical::LogicalRemove>(std::move(logicalItems), std::move(rootOp));
}

std::unique_ptr<query::logical::LogicalOperator> WritingClauseHandler::handleMerge(
	CypherParser::MergeStatementContext *ctx,
	std::unique_ptr<query::logical::LogicalOperator> /*rootOp*/,
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

	// Build Logical Operator
	return helpers::PatternBuilder::buildMergePattern(patternPart, onCreateItems, onMatchItems, planner);
}

} // namespace graph::parser::cypher::clauses
