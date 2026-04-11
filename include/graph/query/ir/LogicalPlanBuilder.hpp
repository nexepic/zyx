/**
 * @file LogicalPlanBuilder.hpp
 * @brief Builds LogicalOperator trees from semantically analyzed QueryAST structures.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/ir/QueryAST.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include <memory>

namespace graph::query::ir {

/**
 * @class LogicalPlanBuilder
 * @brief Converts analyzed QueryAST clause structures into LogicalOperator chains.
 *
 * Plan ordering for projections:
 * - Aggregate:     input -> Aggregate -> [Project] -> Sort -> Skip -> Limit
 * - Non-aggregate: input -> Sort -> Project -> Skip -> Limit
 */
class LogicalPlanBuilder {
public:
	// --- Projection Clauses ---
	static std::unique_ptr<query::logical::LogicalOperator> buildReturn(
		const ProjectionBody& body,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildWith(
		const WithClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	// --- Reading Clauses ---
	static std::unique_ptr<query::logical::LogicalOperator> buildMatch(
		const MatchClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildUnwind(
		const UnwindClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildCall(
		const CallClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	// --- Writing Clauses ---
	static std::unique_ptr<query::logical::LogicalOperator> buildCreate(
		const CreateClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildSet(
		const SetClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildDelete(
		const DeleteClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildRemove(
		const RemoveClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	static std::unique_ptr<query::logical::LogicalOperator> buildMerge(
		const MergeClause& clause,
		std::unique_ptr<query::logical::LogicalOperator> input);

	// --- Admin Clauses ---
	static std::unique_ptr<query::logical::LogicalOperator> buildShowIndexes(
		const ShowIndexesClause& clause);

	static std::unique_ptr<query::logical::LogicalOperator> buildCreateIndex(
		const CreateIndexClause& clause);

	static std::unique_ptr<query::logical::LogicalOperator> buildDropIndex(
		const DropIndexClause& clause);

	static std::unique_ptr<query::logical::LogicalOperator> buildCreateVectorIndex(
		const CreateVectorIndexClause& clause);

	static std::unique_ptr<query::logical::LogicalOperator> buildCreateConstraint(
		const CreateConstraintClause& clause);

	static std::unique_ptr<query::logical::LogicalOperator> buildDropConstraint(
		const DropConstraintClause& clause);

	static std::unique_ptr<query::logical::LogicalOperator> buildShowConstraints(
		const ShowConstraintsClause& clause);
};

} // namespace graph::query::ir
