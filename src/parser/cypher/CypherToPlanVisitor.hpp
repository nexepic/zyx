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

#include "CypherBaseVisitor.h" // ANTLR Generated
#include "graph/query/QueryPlanner.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include <memory>
#include <any>

namespace graph::parser::cypher {

	/**
	 * @class CypherToPlanVisitor
	 * @brief Traverses the ANTLR AST to construct the Query Execution Pipeline (Operator Tree).
	 */
	class CypherToPlanVisitor : public CypherBaseVisitor {
	public:
		// Dependency Injection: Needs the Planner Factory
		explicit CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner);

		// Getter to retrieve the built tree
		[[nodiscard]] std::unique_ptr<query::execution::PhysicalOperator> getPlan();

		// --- Visitor Methods ---
		std::any visitQuery(CypherParser::QueryContext *ctx) override;
		std::any visitMatchStatement(CypherParser::MatchStatementContext *ctx) override;
		std::any visitCreateStatement(CypherParser::CreateStatementContext *ctx) override;

		// Helper to parse literals
		std::any visitLiteral(CypherParser::LiteralContext *ctx) override;

	private:
		std::shared_ptr<query::QueryPlanner> planner_;

		// The root of the operator tree being built
		std::unique_ptr<query::execution::PhysicalOperator> rootOp_;

		// Helper to parse ANTLR literals into internal PropertyValue
		PropertyValue parseValue(CypherParser::LiteralContext *ctx);

		// Helper to chain a new operator to the existing pipeline
		void chainOperator(std::unique_ptr<query::execution::PhysicalOperator> newOp);
	};

}