/**
 * @file CypherToPlanVisitor.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <any>
#include <memory>
#include <string>
#include <unordered_map>
#include "generated/CypherParserBaseVisitor.h"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"

namespace graph::parser::cypher {

	class CypherToPlanVisitor : public CypherParserBaseVisitor {
	public:
		explicit CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner);

		std::unique_ptr<query::execution::PhysicalOperator> getPlan();

		// --- Entry Points ---
		std::any visitCypher(CypherParser::CypherContext *ctx) override;
		std::any visitRegularQuery(CypherParser::RegularQueryContext *ctx) override;
		std::any visitSingleQuery(CypherParser::SingleQueryContext *ctx) override;

		// --- Clauses (Using *Statement naming) ---

		// Reading
		std::any visitMatchStatement(CypherParser::MatchStatementContext *ctx) override;
		std::any visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *ctx) override;

		// Updating
		std::any visitCreateStatement(CypherParser::CreateStatementContext *ctx) override;

		// Return
		std::any visitReturnStatement(CypherParser::ReturnStatementContext *ctx) override;

		// --- Administration ---
		std::any visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) override;

		// Drop
		std::any visitDropIndexByName(CypherParser::DropIndexByNameContext *ctx) override;
		std::any visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *ctx) override;

		// Create
		std::any visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *ctx) override;
		std::any visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *ctx) override;

		std::any visitDeleteStatement(CypherParser::DeleteStatementContext *ctx) override;
		std::any visitSetStatement(CypherParser::SetStatementContext *ctx) override;

		std::any visitRemoveStatement(CypherParser::RemoveStatementContext *ctx) override;

		std::any visitMergeStatement(CypherParser::MergeStatementContext *ctx) override;

		static std::vector<query::execution::operators::SetItem> extractSetItems(CypherParser::SetStatementContext* ctx);

	private:
		std::shared_ptr<query::QueryPlanner> planner_;
		std::unique_ptr<query::execution::PhysicalOperator> rootOp_;

		// --- Helpers ---
		void chainOperator(std::unique_ptr<query::execution::PhysicalOperator> newOp);

		static std::string extractPropertyKeyFromExpr(CypherParser::PropertyExpressionContext* ctx);

		static std::string extractVariable(CypherParser::VariableContext *ctx);
		static std::string extractLabel(CypherParser::NodeLabelsContext *ctx);
		static std::string extractLabelFromNodeLabel(CypherParser::NodeLabelContext *ctx);
		static std::string extractRelType(CypherParser::RelationshipTypesContext *ctx);

		static PropertyValue parseValue(CypherParser::LiteralContext *ctx);
		static std::unordered_map<std::string, PropertyValue> extractProperties(CypherParser::PropertiesContext *ctx);

		std::function<bool(const query::execution::Record &)> buildWherePredicate(CypherParser::ExpressionContext *expr,
																				  std::string &outDesc);
	};

} // namespace graph::parser::cypher
