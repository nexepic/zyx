/**
 * @file CypherToPlanVisitor.cpp
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

#include "CypherToPlanVisitor.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalLoadCsv.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalTransactionControl.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "clauses/AdminClauseHandler.hpp"
#include "clauses/ReadingClauseHandler.hpp"
#include "clauses/ResultClauseHandler.hpp"
#include "clauses/WritingClauseHandler.hpp"
#include "clauses/WithClauseHandler.hpp"
#include "helpers/AstExtractor.hpp"
#include "helpers/ExpressionBuilder.hpp"

namespace graph::parser::cypher {

CypherToPlanVisitor::CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner) :
	planner_(std::move(planner)) {}

std::unique_ptr<query::logical::LogicalOperator> CypherToPlanVisitor::getLogicalPlan() {
	if (!rootOp_) {
		return nullptr;
	}

	// Run the optimizer on read-oriented plans (skip DDL/transaction control)
	auto rootType = rootOp_->getType();
	bool isOptimizable = (rootType != query::logical::LogicalOpType::LOP_TRANSACTION_CONTROL &&
	                      rootType != query::logical::LogicalOpType::LOP_CREATE_INDEX &&
	                      rootType != query::logical::LogicalOpType::LOP_DROP_INDEX &&
	                      rootType != query::logical::LogicalOpType::LOP_SHOW_INDEXES &&
	                      rootType != query::logical::LogicalOpType::LOP_CREATE_VECTOR_INDEX &&
	                      rootType != query::logical::LogicalOpType::LOP_CREATE_CONSTRAINT &&
	                      rootType != query::logical::LogicalOpType::LOP_DROP_CONSTRAINT &&
	                      rootType != query::logical::LogicalOpType::LOP_SHOW_CONSTRAINTS &&
	                      rootType != query::logical::LogicalOpType::LOP_EXPLAIN &&
	                      rootType != query::logical::LogicalOpType::LOP_PROFILE &&
	                      rootType != query::logical::LogicalOpType::LOP_CALL_PROCEDURE);

	if (isOptimizable) {
		auto *opt = planner_->getOptimizer();
		if (opt) {
			rootOp_ = opt->optimize(std::move(rootOp_));
		}
	}

	return std::move(rootOp_);
}

std::unique_ptr<query::execution::PhysicalOperator> CypherToPlanVisitor::getPlan() {
	auto logicalPlan = getLogicalPlan();
	if (!logicalPlan) {
		return nullptr;
	}

	// Convert logical plan to physical plan using managers from the planner
	auto dm = planner_->getDataManager();
	auto im = planner_->getIndexManager();
	auto cm = planner_->getConstraintManager();

	query::PhysicalPlanConverter converter(dm, im, cm);
	return converter.convert(logicalPlan.get());
}

// --- Entry Points ---
std::any CypherToPlanVisitor::visitCypher(CypherParser::CypherContext *ctx) { return visitChildren(ctx); }

std::any CypherToPlanVisitor::visitExplainStatement(CypherParser::ExplainStatementContext *ctx) {
	// Visit inner query/admin statement to build rootOp_
	visitChildren(ctx);
	// Wrap in LogicalExplain
	rootOp_ = std::make_unique<query::logical::LogicalExplain>(std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitProfileStatement(CypherParser::ProfileStatementContext *ctx) {
	// Visit inner query/admin statement to build rootOp_
	visitChildren(ctx);
	// Wrap in LogicalProfile
	rootOp_ = std::make_unique<query::logical::LogicalProfile>(std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitRegularStatement(CypherParser::RegularStatementContext *ctx) {
	return visitChildren(ctx);
}

std::any CypherToPlanVisitor::visitAdminStatement(CypherParser::AdminStatementContext *ctx) {
	return visitChildren(ctx);
}

std::any CypherToPlanVisitor::visitRegularQuery(CypherParser::RegularQueryContext *ctx) {
	// Handle UNION operations
	// Grammar: regularQuery : singleQuery ( K_UNION K_ALL? singleQuery )*

	auto singleQueries = ctx->singleQuery();

	// Visit first single query to get the initial plan
	// Note: singleQueries is guaranteed non-empty by grammar (regularQuery requires at least one singleQuery)
	visitSingleQuery(singleQueries[0]);

	std::unique_ptr<query::logical::LogicalOperator> currentPlan = std::move(rootOp_);

	// Process additional single queries connected by UNION
	auto unionKeywords = ctx->K_UNION();
	auto allKeywords = ctx->K_ALL();

	for (size_t i = 1; i < singleQueries.size(); ++i) {
		// Visit the next single query
		visitSingleQuery(singleQueries[i]);

		// Check if this is UNION ALL (i-1 because there are n-1 UNIONs for n queries)
		bool isAll = false;
		if (i - 1 < allKeywords.size() && allKeywords[i - 1] != nullptr) {
			isAll = true;
		}

		// Create LogicalUnion to combine current plan with the new query
		currentPlan = std::make_unique<query::logical::LogicalUnion>(
			std::move(currentPlan),
			std::move(rootOp_),
			isAll
		);
	}

	// Store the final plan
	rootOp_ = std::move(currentPlan);
	return std::any();
}

std::any CypherToPlanVisitor::visitSingleQuery(CypherParser::SingleQueryContext *ctx) {
	// Automatically chains children (Clauses) in order
	return visitChildren(ctx);
}

// --- Reading Clauses ---
std::any CypherToPlanVisitor::visitMatchStatement(CypherParser::MatchStatementContext *ctx) {
	// Check if this is an OPTIONAL MATCH
	if (ctx->K_OPTIONAL()) {
		rootOp_ = clauses::ReadingClauseHandler::handleOptionalMatch(ctx, std::move(rootOp_), planner_);
	} else {
		rootOp_ = clauses::ReadingClauseHandler::handleMatch(ctx, std::move(rootOp_), planner_);
	}
	return std::any();
}

std::any CypherToPlanVisitor::visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *ctx) {
	rootOp_ = clauses::ReadingClauseHandler::handleStandaloneCall(ctx, std::move(rootOp_), planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitInQueryCallStatement(CypherParser::InQueryCallStatementContext *ctx) {
	rootOp_ = clauses::ReadingClauseHandler::handleInQueryCall(ctx, std::move(rootOp_), planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitUnwindStatement(CypherParser::UnwindStatementContext *ctx) {
	rootOp_ = clauses::ReadingClauseHandler::handleUnwind(ctx, std::move(rootOp_), planner_);
	return std::any();
}

// --- Projection Clauses ---
std::any CypherToPlanVisitor::visitWithClause(CypherParser::WithClauseContext *ctx) {
	rootOp_ = clauses::WithClauseHandler::handleWith(ctx, std::move(rootOp_), planner_);
	return std::any();
}

// --- Writing Clauses ---
std::any CypherToPlanVisitor::visitCreateStatement(CypherParser::CreateStatementContext *ctx) {
	rootOp_ = clauses::WritingClauseHandler::handleCreate(ctx, std::move(rootOp_), planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitSetStatement(CypherParser::SetStatementContext *ctx) {
	rootOp_ = clauses::WritingClauseHandler::handleSet(ctx, std::move(rootOp_), planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitDeleteStatement(CypherParser::DeleteStatementContext *ctx) {
	rootOp_ = clauses::WritingClauseHandler::handleDelete(ctx, std::move(rootOp_), planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitRemoveStatement(CypherParser::RemoveStatementContext *ctx) {
	rootOp_ = clauses::WritingClauseHandler::handleRemove(ctx, std::move(rootOp_), planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitMergeStatement(CypherParser::MergeStatementContext *ctx) {
	rootOp_ = clauses::WritingClauseHandler::handleMerge(ctx, std::move(rootOp_), planner_);
	return std::any();
}

// --- Result Clause ---
std::any CypherToPlanVisitor::visitReturnStatement(CypherParser::ReturnStatementContext *ctx) {
	rootOp_ = clauses::ResultClauseHandler::handleReturn(ctx, std::move(rootOp_), planner_);
	return std::any();
}

// --- Administrative Clauses ---
std::any CypherToPlanVisitor::visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleShowIndexes(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleCreateIndexByPattern(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleCreateIndexByLabel(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateVectorIndex(CypherParser::CreateVectorIndexContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleCreateVectorIndex(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropIndexByName(CypherParser::DropIndexByNameContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleDropIndexByName(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleDropIndexByLabel(ctx, planner_);
	return std::any();
}

// --- Constraint DDL ---
std::any CypherToPlanVisitor::visitCreateNodeConstraint(CypherParser::CreateNodeConstraintContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleCreateNodeConstraint(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateEdgeConstraint(CypherParser::CreateEdgeConstraintContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleCreateEdgeConstraint(ctx, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropConstraintByName(CypherParser::DropConstraintByNameContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleDropConstraint(ctx->symbolicName()->getText(), false, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropConstraintIfExists(CypherParser::DropConstraintIfExistsContext *ctx) {
	rootOp_ = clauses::AdminClauseHandler::handleDropConstraint(ctx->symbolicName()->getText(), true, planner_);
	return std::any();
}

std::any CypherToPlanVisitor::visitShowConstraintsStatement(CypherParser::ShowConstraintsStatementContext *) {
	rootOp_ = clauses::AdminClauseHandler::handleShowConstraints(planner_);
	return std::any();
}

// --- Transaction Control ---
std::any CypherToPlanVisitor::visitTxnBegin(CypherParser::TxnBeginContext *) {
	rootOp_ = std::make_unique<query::logical::LogicalTransactionControl>(
		query::logical::LogicalTxnCommand::LTXN_BEGIN);
	return std::any();
}

std::any CypherToPlanVisitor::visitTxnCommit(CypherParser::TxnCommitContext *) {
	rootOp_ = std::make_unique<query::logical::LogicalTransactionControl>(
		query::logical::LogicalTxnCommand::LTXN_COMMIT);
	return std::any();
}

std::any CypherToPlanVisitor::visitTxnRollback(CypherParser::TxnRollbackContext *) {
	rootOp_ = std::make_unique<query::logical::LogicalTransactionControl>(
		query::logical::LogicalTxnCommand::LTXN_ROLLBACK);
	return std::any();
}

// --- FOREACH ---
std::any CypherToPlanVisitor::visitForeachStatement(CypherParser::ForeachStatementContext *ctx) {
	// Grammar: K_FOREACH LPAREN variable K_IN expression PIPE updatingClause+ RPAREN
	std::string iterVar = helpers::AstExtractor::extractVariable(ctx->variable());
	auto listExpr = std::shared_ptr<query::expressions::Expression>(
		helpers::ExpressionBuilder::buildExpression(ctx->expression()).release());

	// Save current rootOp_ and scope, compile body independently
	auto savedRootOp = std::move(rootOp_);

	// Push scope for FOREACH body (non-isolated: inherits outer vars)
	scope_.pushFrame(false);
	scope_.define(iterVar);

	// Compile body: each updatingClause chains onto rootOp_
	// Use a SingleRow as the seed so clause handlers accept non-null input.
	// The physical plan converter will replace this with a RecordInjector.
	rootOp_ = std::make_unique<query::logical::LogicalSingleRow>();
	for (auto *updClause : ctx->updatingClause()) {
		visitChildren(updClause);
	}
	auto bodyOp = std::move(rootOp_);

	// Restore scope
	scope_.popFrame();

	// Restore rootOp_ and wrap in LogicalForeach
	rootOp_ = std::make_unique<query::logical::LogicalForeach>(
		std::move(savedRootOp), iterVar, listExpr, std::move(bodyOp));
	return std::any();
}

// --- CALL { subquery } ---
std::any CypherToPlanVisitor::visitCallSubquery(CypherParser::CallSubqueryContext *ctx) {
	// Grammar: K_CALL LBRACE singleQuery RBRACE inTransactionsClause?
	auto savedRootOp = std::move(rootOp_);

	// Determine outer scope variables from the preceding logical plan's output.
	// The scope stack is not reliably maintained by clause handlers, so we derive
	// available outer variables directly from the compiled plan.
	std::unordered_set<std::string> outerVars;
	if (savedRootOp) {
		auto vars = savedRootOp->getOutputVariables();
		outerVars.insert(vars.begin(), vars.end());
	}

	// Push isolated scope for subquery
	scope_.pushFrame(true);

	// Compile the subquery with SingleRow seed so WITH import works
	rootOp_ = std::make_unique<query::logical::LogicalSingleRow>();
	visitSingleQuery(ctx->singleQuery());
	auto subqueryOp = std::move(rootOp_);

	// Extract returned vars from the compiled subquery
	std::vector<std::string> returnedVars;
	if (subqueryOp) {
		returnedVars = subqueryOp->getOutputVariables();
	}

	// Pop subquery scope
	scope_.popFrame();

	// Detect imported vars from the WITH clauses at the start of the subquery.
	// In CALL { WITH n RETURN ... }, the WITH clause imports outer variables.
	std::vector<std::string> importedVars;
	auto *sq = ctx->singleQuery();
	for (auto *wc : sq->withClause()) {
		auto *projBody = wc->projectionBody();
		auto *projItems = projBody->projectionItems();
		// WITH * imports all outer vars
		if (projItems->MULTIPLY()) {
			importedVars.assign(outerVars.begin(), outerVars.end());
			break;
		}
		for (auto *item : projItems->projectionItem()) {
			// Use the source expression to detect outer scope references.
			// For WITH n: source is "n". For WITH n AS m: source is "n".
			std::string sourceVar = item->expression()->getText();
			if (outerVars.count(sourceVar)) {
				importedVars.push_back(sourceVar);
			}
		}
	}

	// Define returned vars in outer scope
	for (const auto &var : returnedVars) {
		scope_.define(var);
	}

	// Handle IN TRANSACTIONS
	bool inTransactions = false;
	int64_t batchSize = 0;
	if (ctx->inTransactionsClause()) {
		inTransactions = true;
		auto inTxnCtx = ctx->inTransactionsClause();
		if (inTxnCtx->expression()) {
			auto val = helpers::ExpressionBuilder::evaluateLiteralExpression(inTxnCtx->expression());
			if (val.getType() == graph::PropertyType::INTEGER) {
				batchSize = std::get<int64_t>(val.getVariant());
			}
		}
	}

	rootOp_ = std::make_unique<query::logical::LogicalCallSubquery>(
		std::move(savedRootOp), std::move(subqueryOp),
		std::move(importedVars), std::move(returnedVars),
		inTransactions, batchSize);
	return std::any();
}

// --- LOAD CSV ---
std::any CypherToPlanVisitor::visitLoadCsvStatement(CypherParser::LoadCsvStatementContext *ctx) {
	// Grammar: K_LOAD K_CSV (K_WITH K_HEADERS)? K_FROM expression K_AS variable (K_FIELDTERMINATOR StringLiteral)?
	std::string rowVar = helpers::AstExtractor::extractVariable(ctx->variable());
	auto urlExpr = std::shared_ptr<query::expressions::Expression>(
		helpers::ExpressionBuilder::buildExpression(ctx->expression()).release());

	bool withHeaders = (ctx->K_HEADERS() != nullptr);

	std::string fieldTerminator = ",";
	if (ctx->K_FIELDTERMINATOR()) {
		std::string ft = ctx->StringLiteral()->getText();
		// Strip quotes
		if (ft.size() >= 2) {
			ft = ft.substr(1, ft.size() - 2);
		}
		// Handle escape sequences
		if (ft == "\\t") ft = "\t";
		fieldTerminator = ft;
	}

	scope_.define(rowVar);

	rootOp_ = std::make_unique<query::logical::LogicalLoadCsv>(
		std::move(rootOp_), urlExpr, rowVar, withHeaders, fieldTerminator);
	return std::any();
}

} // namespace graph::parser::cypher
