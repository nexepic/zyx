/**
 * @file LogicalPlanBuilder.hpp
 * @brief Builds LogicalOperator trees from semantically analyzed QueryAST structures.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/ir/QueryAST.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/QueryPlan.hpp"
#include <memory>

namespace graph::query::planner {
	class ProcedureRegistry;
}

namespace graph::query::ir {

/**
 * @class LogicalPlanBuilder
 * @brief Stateful builder that converts analyzed QueryAST clause structures into
 *        LogicalOperator chains and accumulates mutation flags into a QueryPlan.
 *
 * Plan ordering for projections:
 * - Aggregate:     input -> Aggregate -> [Project] -> Sort -> Skip -> Limit
 * - Non-aggregate: input -> Sort -> Project -> Skip -> Limit
 */
class LogicalPlanBuilder {
public:
	explicit LogicalPlanBuilder(const planner::ProcedureRegistry *registry = nullptr);

	// --- Projection Clauses ---
	std::unique_ptr<query::logical::LogicalOperator> buildReturn(
		const ProjectionBody& body,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildWith(
		const WithClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	// --- Reading Clauses ---
	std::unique_ptr<query::logical::LogicalOperator> buildMatch(
		const MatchClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildUnwind(
		const UnwindClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildCall(
		const CallClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	// --- Writing Clauses ---
	std::unique_ptr<query::logical::LogicalOperator> buildCreate(
		const CreateClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildSet(
		const SetClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildDelete(
		const DeleteClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildRemove(
		const RemoveClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	std::unique_ptr<query::logical::LogicalOperator> buildMerge(
		const MergeClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	// --- Admin Clauses ---
	std::unique_ptr<query::logical::LogicalOperator> buildShowIndexes(
		const ShowIndexesClause& clause);

	std::unique_ptr<query::logical::LogicalOperator> buildCreateIndex(
		const CreateIndexClause& clause);

	std::unique_ptr<query::logical::LogicalOperator> buildDropIndex(
		const DropIndexClause& clause);

	std::unique_ptr<query::logical::LogicalOperator> buildCreateVectorIndex(
		const CreateVectorIndexClause& clause);

	std::unique_ptr<query::logical::LogicalOperator> buildCreateConstraint(
		const CreateConstraintClause& clause);

	std::unique_ptr<query::logical::LogicalOperator> buildDropConstraint(
		const DropConstraintClause& clause);

	std::unique_ptr<query::logical::LogicalOperator> buildShowConstraints(
		const ShowConstraintsClause& clause);

	// --- Flag accessors ---
	[[nodiscard]] bool mutatesData() const { return mutatesData_; }
	[[nodiscard]] bool mutatesSchema() const { return mutatesSchema_; }

	/** @brief Mark data mutation (for operators built outside this class, e.g. FOREACH). */
	void markDataMutation() { mutatesData_ = true; }
	/** @brief Mark schema mutation. */
	void markSchemaMutation() { mutatesSchema_ = true; }

private:
	const planner::ProcedureRegistry *procedureRegistry_ = nullptr;
	bool mutatesData_ = false;
	bool mutatesSchema_ = false;
};

} // namespace graph::query::ir
