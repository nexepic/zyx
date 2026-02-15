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
#include "graph/query/planner/QueryPlanner.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::execution::PhysicalOperator> ResultClauseHandler::handleReturn(
	CypherParser::ReturnStatementContext *ctx,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto body = ctx->projectionBody();

	// If there is no existing plan (e.g. standalone RETURN 1),
	// inject a SingleRowOperator to provide a valid pipeline source.
	if (!rootOp) {
		rootOp = planner->singleRowOp();
	}

	// Order By (MUST come before Projection so SortOperator has access to full nodes)
	if (body->orderStatement()) {
		std::vector<query::execution::operators::SortItem> sortItems;
		auto sortItemList = body->orderStatement()->sortItem();

		for (auto item : sortItemList) {
			// Parse Expression (n.age)
			std::string varName;
			std::string propName;

			// Hacky parse for "n.prop" string
			std::string text = item->expression()->getText();
			size_t dotPos = text.find('.');
			if (dotPos != std::string::npos) {
				varName = text.substr(0, dotPos);
				propName = text.substr(dotPos + 1);
			} else {
				varName = text; // Just "n" (Sort by ID)
			}

			// Determine Direction
			bool asc = true;  // Default to ascending
			if (item->K_DESC() || item->K_DESCENDING()) {
				asc = false;  // Set to false for DESC/DESCENDING
			}

			sortItems.push_back({varName, propName, asc});
		}

		if (!sortItems.empty()) {
			rootOp = planner->sortOp(std::move(rootOp), sortItems);
		}
	}

	// Handle Projection (MUST come after Order By)
	auto items = body->projectionItems();
	if (!items->MULTIPLY()) { // If not RETURN *
		std::vector<query::execution::operators::ProjectItem> projItems;

		for (auto item : items->projectionItem()) {
			// 1. Expression Text (Source)
			std::string exprText = item->expression()->getText();

			// 2. Alias (Output Name)
			// If 'AS alias' is present, use it. Otherwise, use expression text.
			std::string alias = exprText;
			if (item->K_AS()) {
				alias = helpers::AstExtractor::extractVariable(item->variable());
			}

			projItems.push_back({exprText, alias});
		}

		if (!projItems.empty()) {
			rootOp = planner->projectOp(std::move(rootOp), projItems);
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
