/**
 * @file AdminClauseHandler.cpp
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

#include "AdminClauseHandler.hpp"
#include "helpers/AstExtractor.hpp"
#include "graph/query/planner/QueryPlanner.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::execution::PhysicalOperator> AdminClauseHandler::handleShowIndexes(
	CypherParser::ShowIndexesStatementContext * /*ctx*/,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	return planner->showIndexesOp();
}

std::unique_ptr<query::execution::PhysicalOperator> AdminClauseHandler::handleCreateIndexByPattern(
	CypherParser::CreateIndexByPatternContext *ctx,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Syntax: CREATE INDEX [name] FOR (n:Label) ON (n.prop)

	// 1. Name (Optional)
	std::string name = "";
	if (ctx->symbolicName()) {
		name = ctx->symbolicName()->getText();
	}

	// 2. Label (From Pattern)
	std::string label;
	auto pat = ctx->nodePattern();
	if (pat->nodeLabels() && !pat->nodeLabels()->nodeLabel().empty()) {
		label = helpers::AstExtractor::extractLabelFromNodeLabel(pat->nodeLabels()->nodeLabel(0));
	} else {
		throw std::runtime_error("CREATE INDEX pattern must specify a Label (e.g. :User)");
	}

	// 3. Property
	std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(ctx->propertyExpression());

	return planner->createIndexOp(name, label, prop);
}

std::unique_ptr<query::execution::PhysicalOperator> AdminClauseHandler::handleCreateIndexByLabel(
	CypherParser::CreateIndexByLabelContext *ctx,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Syntax: CREATE INDEX [name] ON :Label(prop)
	std::string name = "";
	if (ctx->symbolicName()) {
		name = ctx->symbolicName()->getText();
	}

	std::string label = helpers::AstExtractor::extractLabelFromNodeLabel(ctx->nodeLabel());
	std::string prop = ctx->propertyKeyName()->getText();

	return planner->createIndexOp(name, label, prop);
}

std::unique_ptr<query::execution::PhysicalOperator> AdminClauseHandler::handleCreateVectorIndex(
	CypherParser::CreateVectorIndexContext *ctx,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	std::string name = ctx->symbolicName() ? ctx->symbolicName()->getText() : "";
	std::string label = helpers::AstExtractor::extractLabelFromNodeLabel(ctx->nodeLabel());
	std::string prop = ctx->propertyKeyName()->getText();

	int dim = 0;
	std::string metric = "L2";

	// Parse OPTIONS
	if (ctx->K_OPTIONS() && ctx->mapLiteral()) {
		auto mapLit = ctx->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();
		for (size_t i = 0; i < keys.size(); ++i) {
			std::string k = keys[i]->getText();
			std::string v = exprs[i]->getText();
			if (k == "dimension" || k == "dim")
				dim = std::stoi(v);
			else if (k == "metric")
				metric = v.substr(1, v.size() - 2); // remove quotes
		}
	}

	if (dim == 0)
		throw std::runtime_error("Vector Index requires 'dimension'");

	return planner->createVectorIndexOp(name, label, prop, dim, metric);
}

std::unique_ptr<query::execution::PhysicalOperator> AdminClauseHandler::handleDropIndexByName(
	CypherParser::DropIndexByNameContext *ctx,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Syntax: DROP INDEX name
	std::string name = ctx->symbolicName()->getText();
	return planner->dropIndexOp(name);
}

std::unique_ptr<query::execution::PhysicalOperator> AdminClauseHandler::handleDropIndexByLabel(
	CypherParser::DropIndexByLabelContext *ctx,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// Syntax: DROP INDEX ON :Label(prop)
	std::string label = helpers::AstExtractor::extractLabelFromNodeLabel(ctx->nodeLabel());
	std::string prop = ctx->propertyKeyName()->getText();

	return planner->dropIndexOp(label, prop);
}

} // namespace graph::parser::cypher::clauses
