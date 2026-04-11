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
#include "graph/query/ir/CypherASTBuilder.hpp"
#include "graph/query/ir/SemanticAnalyzer.hpp"
#include "graph/query/ir/LogicalPlanBuilder.hpp"
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
	visitChildren(ctx);
	rootOp_ = std::make_unique<query::logical::LogicalExplain>(std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitProfileStatement(CypherParser::ProfileStatementContext *ctx) {
	visitChildren(ctx);
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
	auto singleQueries = ctx->singleQuery();

	visitSingleQuery(singleQueries[0]);
	std::unique_ptr<query::logical::LogicalOperator> currentPlan = std::move(rootOp_);

	auto unionKeywords = ctx->K_UNION();
	auto allKeywords = ctx->K_ALL();

	for (size_t i = 1; i < singleQueries.size(); ++i) {
		visitSingleQuery(singleQueries[i]);

		bool isAll = false;
		if (i - 1 < allKeywords.size() && allKeywords[i - 1] != nullptr) {
			isAll = true;
		}

		currentPlan = std::make_unique<query::logical::LogicalUnion>(
			std::move(currentPlan),
			std::move(rootOp_),
			isAll
		);
	}

	rootOp_ = std::move(currentPlan);
	return std::any();
}

std::any CypherToPlanVisitor::visitSingleQuery(CypherParser::SingleQueryContext *ctx) {
	return visitChildren(ctx);
}

// --- Reading Clauses ---
std::any CypherToPlanVisitor::visitMatchStatement(CypherParser::MatchStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildMatchClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildMatch(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildStandaloneCallClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCall(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitInQueryCallStatement(CypherParser::InQueryCallStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildInQueryCallClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCall(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitUnwindStatement(CypherParser::UnwindStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildUnwindClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildUnwind(clause, std::move(rootOp_));
	return std::any();
}

// --- Projection Clauses ---
std::any CypherToPlanVisitor::visitWithClause(CypherParser::WithClauseContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildWithClause(ctx);
	query::ir::SemanticAnalyzer::analyzeProjection(clause.body);
	rootOp_ = query::ir::LogicalPlanBuilder::buildWith(clause, std::move(rootOp_));
	return std::any();
}

// --- Writing Clauses ---
std::any CypherToPlanVisitor::visitCreateStatement(CypherParser::CreateStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildCreateClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCreate(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitSetStatement(CypherParser::SetStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildSetClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildSet(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitDeleteStatement(CypherParser::DeleteStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildDeleteClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildDelete(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitRemoveStatement(CypherParser::RemoveStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildRemoveClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildRemove(clause, std::move(rootOp_));
	return std::any();
}

std::any CypherToPlanVisitor::visitMergeStatement(CypherParser::MergeStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildMergeClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildMerge(clause, std::move(rootOp_));
	return std::any();
}

// --- Result Clause ---
std::any CypherToPlanVisitor::visitReturnStatement(CypherParser::ReturnStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildReturnClause(ctx);
	query::ir::SemanticAnalyzer::analyzeProjection(clause.body);
	rootOp_ = query::ir::LogicalPlanBuilder::buildReturn(clause.body, std::move(rootOp_));
	return std::any();
}

// --- Administrative Clauses ---
std::any CypherToPlanVisitor::visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildShowIndexesClause();
	rootOp_ = query::ir::LogicalPlanBuilder::buildShowIndexes(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildCreateIndexByPatternClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCreateIndex(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildCreateIndexByLabelClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCreateIndex(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateVectorIndex(CypherParser::CreateVectorIndexContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildCreateVectorIndexClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCreateVectorIndex(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropIndexByName(CypherParser::DropIndexByNameContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildDropIndexByNameClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildDropIndex(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildDropIndexByLabelClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildDropIndex(clause);
	return std::any();
}

// --- Constraint DDL ---
std::any CypherToPlanVisitor::visitCreateNodeConstraint(CypherParser::CreateNodeConstraintContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildCreateNodeConstraintClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCreateConstraint(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitCreateEdgeConstraint(CypherParser::CreateEdgeConstraintContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildCreateEdgeConstraintClause(ctx);
	rootOp_ = query::ir::LogicalPlanBuilder::buildCreateConstraint(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropConstraintByName(CypherParser::DropConstraintByNameContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildDropConstraintClause(ctx->symbolicName()->getText(), false);
	rootOp_ = query::ir::LogicalPlanBuilder::buildDropConstraint(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitDropConstraintIfExists(CypherParser::DropConstraintIfExistsContext *ctx) {
	auto clause = query::ir::CypherASTBuilder::buildDropConstraintClause(ctx->symbolicName()->getText(), true);
	rootOp_ = query::ir::LogicalPlanBuilder::buildDropConstraint(clause);
	return std::any();
}

std::any CypherToPlanVisitor::visitShowConstraintsStatement(CypherParser::ShowConstraintsStatementContext *) {
	auto clause = query::ir::CypherASTBuilder::buildShowConstraintsClause();
	rootOp_ = query::ir::LogicalPlanBuilder::buildShowConstraints(clause);
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
	std::string iterVar = helpers::AstExtractor::extractVariable(ctx->variable());
	auto listExpr = std::shared_ptr<query::expressions::Expression>(
		helpers::ExpressionBuilder::buildExpression(ctx->expression()).release());

	auto savedRootOp = std::move(rootOp_);

	scope_.pushFrame(false);
	scope_.define(iterVar);

	rootOp_ = std::make_unique<query::logical::LogicalSingleRow>();
	for (auto *updClause : ctx->updatingClause()) {
		visitChildren(updClause);
	}
	auto bodyOp = std::move(rootOp_);

	scope_.popFrame();

	rootOp_ = std::make_unique<query::logical::LogicalForeach>(
		std::move(savedRootOp), iterVar, listExpr, std::move(bodyOp));
	return std::any();
}

// --- CALL { subquery } ---
std::any CypherToPlanVisitor::visitCallSubquery(CypherParser::CallSubqueryContext *ctx) {
	auto savedRootOp = std::move(rootOp_);

	std::unordered_set<std::string> outerVars;
	if (savedRootOp) {
		auto vars = savedRootOp->getOutputVariables();
		outerVars.insert(vars.begin(), vars.end());
	}

	scope_.pushFrame(true);

	rootOp_ = std::make_unique<query::logical::LogicalSingleRow>();
	visitSingleQuery(ctx->singleQuery());
	auto subqueryOp = std::move(rootOp_);

	std::vector<std::string> returnedVars;
	if (subqueryOp) {
		returnedVars = subqueryOp->getOutputVariables();
	}

	scope_.popFrame();

	std::vector<std::string> importedVars;
	auto *sq = ctx->singleQuery();
	for (auto *wc : sq->withClause()) {
		auto *projBody = wc->projectionBody();
		auto *projItems = projBody->projectionItems();
		if (projItems->MULTIPLY()) {
			importedVars.assign(outerVars.begin(), outerVars.end());
			break;
		}
		for (auto *item : projItems->projectionItem()) {
			std::string sourceVar = item->expression()->getText();
			if (outerVars.count(sourceVar)) {
				importedVars.push_back(sourceVar);
			}
		}
	}

	for (const auto &var : returnedVars) {
		scope_.define(var);
	}

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
	std::string rowVar = helpers::AstExtractor::extractVariable(ctx->variable());
	auto urlExpr = std::shared_ptr<query::expressions::Expression>(
		helpers::ExpressionBuilder::buildExpression(ctx->expression()).release());

	bool withHeaders = (ctx->K_HEADERS() != nullptr);

	std::string fieldTerminator = ",";
	if (ctx->K_FIELDTERMINATOR()) {
		std::string ft = ctx->StringLiteral()->getText();
		if (ft.size() >= 2) {
			ft = ft.substr(1, ft.size() - 2);
		}
		if (ft == "\\t") ft = "\t";
		fieldTerminator = ft;
	}

	scope_.define(rowVar);

	rootOp_ = std::make_unique<query::logical::LogicalLoadCsv>(
		std::move(rootOp_), urlExpr, rowVar, withHeaders, fieldTerminator);
	return std::any();
}

} // namespace graph::parser::cypher
