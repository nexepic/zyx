/**
 * @file CypherToPlanVisitor.hpp
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

#pragma once

#include <memory>
#include <string>
#include "generated/CypherParserBaseVisitor.h"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/planner/ScopeStack.hpp"

namespace graph::parser::cypher {

/**
 * @class CypherToPlanVisitor
 * @brief ANTLR4 visitor that converts Cypher parse trees into logical execution plans,
 *        then optimizes and converts to physical plans in getPlan().
 *
 * This visitor delegates to specialized helper and handler classes:
 * - Helpers: AstExtractor, ExpressionBuilder, PatternBuilder, OperatorChain
 * - Clauses: ReadingClauseHandler, WritingClauseHandler, ResultClauseHandler, AdminClauseHandler
 *
 * The visitor builds a logical IR (LogicalOperator tree) during traversal.
 * getPlan() runs the optimizer and PhysicalPlanConverter to produce the
 * final PhysicalOperator tree for execution.
 */
class CypherToPlanVisitor : public CypherParserBaseVisitor {
public:
	explicit CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner);

	std::unique_ptr<query::execution::PhysicalOperator> getPlan();

	/**
	 * @brief Returns the optimized logical plan without physical conversion.
	 * Used by PlanCache to store reusable logical plans.
	 */
	std::unique_ptr<query::logical::LogicalOperator> getLogicalPlan();

	// --- Entry Points ---
	std::any visitCypher(CypherParser::CypherContext *ctx) override;
	std::any visitExplainStatement(CypherParser::ExplainStatementContext *ctx) override;
	std::any visitProfileStatement(CypherParser::ProfileStatementContext *ctx) override;
	std::any visitRegularStatement(CypherParser::RegularStatementContext *ctx) override;
	std::any visitAdminStatement(CypherParser::AdminStatementContext *ctx) override;
	std::any visitRegularQuery(CypherParser::RegularQueryContext *ctx) override;
	std::any visitSingleQuery(CypherParser::SingleQueryContext *ctx) override;

	// --- Reading Clauses ---
	std::any visitMatchStatement(CypherParser::MatchStatementContext *ctx) override;
	std::any visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *ctx) override;
	std::any visitInQueryCallStatement(CypherParser::InQueryCallStatementContext *ctx) override;
	std::any visitUnwindStatement(CypherParser::UnwindStatementContext *ctx) override;
	std::any visitCallSubquery(CypherParser::CallSubqueryContext *ctx) override;
	std::any visitLoadCsvStatement(CypherParser::LoadCsvStatementContext *ctx) override;

	// --- Projection Clauses ---
	std::any visitWithClause(CypherParser::WithClauseContext *ctx) override;

	// --- Writing Clauses ---
	std::any visitCreateStatement(CypherParser::CreateStatementContext *ctx) override;
	std::any visitSetStatement(CypherParser::SetStatementContext *ctx) override;
	std::any visitDeleteStatement(CypherParser::DeleteStatementContext *ctx) override;
	std::any visitRemoveStatement(CypherParser::RemoveStatementContext *ctx) override;
	std::any visitMergeStatement(CypherParser::MergeStatementContext *ctx) override;
	std::any visitForeachStatement(CypherParser::ForeachStatementContext *ctx) override;

	// --- Result Clause ---
	std::any visitReturnStatement(CypherParser::ReturnStatementContext *ctx) override;

	// --- Administrative Clauses ---
	std::any visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) override;
	std::any visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *ctx) override;
	std::any visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *ctx) override;
	std::any visitCreateVectorIndex(CypherParser::CreateVectorIndexContext *ctx) override;
	std::any visitDropIndexByName(CypherParser::DropIndexByNameContext *ctx) override;
	std::any visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *ctx) override;

	// --- Constraint DDL ---
	std::any visitCreateNodeConstraint(CypherParser::CreateNodeConstraintContext *ctx) override;
	std::any visitCreateEdgeConstraint(CypherParser::CreateEdgeConstraintContext *ctx) override;
	std::any visitDropConstraintByName(CypherParser::DropConstraintByNameContext *ctx) override;
	std::any visitDropConstraintIfExists(CypherParser::DropConstraintIfExistsContext *ctx) override;
	std::any visitShowConstraintsStatement(CypherParser::ShowConstraintsStatementContext *ctx) override;

	// --- Transaction Control ---
	std::any visitTxnBegin(CypherParser::TxnBeginContext *ctx) override;
	std::any visitTxnCommit(CypherParser::TxnCommitContext *ctx) override;
	std::any visitTxnRollback(CypherParser::TxnRollbackContext *ctx) override;

private:
	std::shared_ptr<query::QueryPlanner> planner_;
	std::unique_ptr<query::logical::LogicalOperator> rootOp_;
	query::planner::ScopeStack scope_;
};

} // namespace graph::parser::cypher
