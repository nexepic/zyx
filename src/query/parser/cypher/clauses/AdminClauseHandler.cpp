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
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"

namespace graph::parser::cypher::clauses {

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleShowIndexes(
	CypherParser::ShowIndexesStatementContext * /*ctx*/,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	return std::make_unique<query::logical::LogicalShowIndexes>();
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleCreateIndexByPattern(
	CypherParser::CreateIndexByPatternContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

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

	// 3. Properties (single or multiple for composite index)
	auto propExprs = ctx->propertyExpression();
	if (propExprs.size() == 1) {
		// Single property index
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(propExprs[0]);
		return std::make_unique<query::logical::LogicalCreateIndex>(name, label, prop);
	} else {
		// Composite index: multiple properties
		std::vector<std::string> props;
		for (auto *propExpr : propExprs) {
			props.push_back(helpers::AstExtractor::extractPropertyKeyFromExpr(propExpr));
		}
		return std::make_unique<query::logical::LogicalCreateIndex>(name, label, std::move(props));
	}
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleCreateIndexByLabel(
	CypherParser::CreateIndexByLabelContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	// Syntax: CREATE INDEX [name] ON :Label(prop)
	std::string name = "";
	if (ctx->symbolicName()) {
		name = ctx->symbolicName()->getText();
	}

	std::string label = helpers::AstExtractor::extractLabelFromNodeLabel(ctx->nodeLabel());
	std::string prop = ctx->propertyKeyName()->getText();

	return std::make_unique<query::logical::LogicalCreateIndex>(name, label, prop);
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleCreateVectorIndex(
	CypherParser::CreateVectorIndexContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

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

	return std::make_unique<query::logical::LogicalCreateVectorIndex>(name, label, prop, dim, metric);
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleDropIndexByName(
	CypherParser::DropIndexByNameContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	// Syntax: DROP INDEX name
	std::string name = ctx->symbolicName()->getText();
	return std::make_unique<query::logical::LogicalDropIndex>(name, "", "");
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleDropIndexByLabel(
	CypherParser::DropIndexByLabelContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	// Syntax: DROP INDEX ON :Label(prop)
	std::string label = helpers::AstExtractor::extractLabelFromNodeLabel(ctx->nodeLabel());
	std::string prop = ctx->propertyKeyName()->getText();

	return std::make_unique<query::logical::LogicalDropIndex>("", label, prop);
}

// --- Constraint DDL ---

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleCreateNodeConstraint(
	CypherParser::CreateNodeConstraintContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	std::string name = ctx->symbolicName()->getText();

	// Extract label from constraintNodePattern: (variable:label)
	auto pattern = ctx->constraintNodePattern();
	std::string label = pattern->labelName()->getText();

	// Parse the constraint body
	auto body = ctx->constraintBody();

	if (auto *unique = dynamic_cast<CypherParser::UniqueConstraintBodyContext *>(body)) {
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(unique->propertyExpression());
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "node", "unique", label, std::vector<std::string>{prop}, "");
	}

	if (auto *notNull = dynamic_cast<CypherParser::NotNullConstraintBodyContext *>(body)) {
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(notNull->propertyExpression());
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "node", "not_null", label, std::vector<std::string>{prop}, "");
	}

	if (auto *propType = dynamic_cast<CypherParser::PropertyTypeConstraintBodyContext *>(body)) {
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(propType->propertyExpression());
		std::string typeName = propType->constraintTypeName()->getText();
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "node", "property_type", label, std::vector<std::string>{prop}, typeName);
	}

	if (auto *nodeKey = dynamic_cast<CypherParser::NodeKeyConstraintBodyContext *>(body)) {
		std::vector<std::string> props;
		for (auto *propExpr : nodeKey->propertyExpression()) {
			props.push_back(helpers::AstExtractor::extractPropertyKeyFromExpr(propExpr));
		}
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "node", "node_key", label, props, "");
	}

	throw std::runtime_error("Unknown constraint body type");
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleCreateEdgeConstraint(
	CypherParser::CreateEdgeConstraintContext *ctx,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	std::string name = ctx->symbolicName()->getText();

	// Extract label from constraintEdgePattern: ()-[variable:label]-()
	auto pattern = ctx->constraintEdgePattern();
	std::string label = pattern->labelName()->getText();

	// Parse the constraint body
	auto body = ctx->constraintBody();

	if (auto *unique = dynamic_cast<CypherParser::UniqueConstraintBodyContext *>(body)) {
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(unique->propertyExpression());
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "edge", "unique", label, std::vector<std::string>{prop}, "");
	}

	if (auto *notNull = dynamic_cast<CypherParser::NotNullConstraintBodyContext *>(body)) {
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(notNull->propertyExpression());
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "edge", "not_null", label, std::vector<std::string>{prop}, "");
	}

	if (auto *propType = dynamic_cast<CypherParser::PropertyTypeConstraintBodyContext *>(body)) {
		std::string prop = helpers::AstExtractor::extractPropertyKeyFromExpr(propType->propertyExpression());
		std::string typeName = propType->constraintTypeName()->getText();
		return std::make_unique<query::logical::LogicalCreateConstraint>(
			name, "edge", "property_type", label, std::vector<std::string>{prop}, typeName);
	}

	throw std::runtime_error("Unknown constraint body type for edge constraint");
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleDropConstraint(
	const std::string &name, bool ifExists,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {
	return std::make_unique<query::logical::LogicalDropConstraint>(name, ifExists);
}

std::unique_ptr<query::logical::LogicalOperator> AdminClauseHandler::handleShowConstraints(
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {
	return std::make_unique<query::logical::LogicalShowConstraints>();
}

} // namespace graph::parser::cypher::clauses
