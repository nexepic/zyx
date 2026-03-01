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
#include "clauses/AdminClauseHandler.hpp"
#include "clauses/ReadingClauseHandler.hpp"
#include "clauses/ResultClauseHandler.hpp"
#include "clauses/WritingClauseHandler.hpp"
#include "clauses/WithClauseHandler.hpp"

namespace graph::parser::cypher {

CypherToPlanVisitor::CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner) :
	planner_(std::move(planner)) {}

std::unique_ptr<query::execution::PhysicalOperator> CypherToPlanVisitor::getPlan() { return std::move(rootOp_); }

// --- Entry Points ---
std::any CypherToPlanVisitor::visitCypher(CypherParser::CypherContext *ctx) { return visitChildren(ctx); }

std::any CypherToPlanVisitor::visitRegularQuery(CypherParser::RegularQueryContext *ctx) {
	return visitChildren(ctx);
}

std::any CypherToPlanVisitor::visitSingleQuery(CypherParser::SingleQueryContext *ctx) {
	// Automatically chains children (Clauses) in order
	return visitChildren(ctx);
}

// --- Reading Clauses ---
std::any CypherToPlanVisitor::visitMatchStatement(CypherParser::MatchStatementContext *ctx) {
	rootOp_ = clauses::ReadingClauseHandler::handleMatch(ctx, std::move(rootOp_), planner_);
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

std::any CypherToPlanVisitor::visitWithStatement(CypherParser::WithStatementContext *ctx) {
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

} // namespace graph::parser::cypher
