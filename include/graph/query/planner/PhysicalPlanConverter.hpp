/**
 * @file PhysicalPlanConverter.hpp
 * @brief Converts an optimized logical plan tree into a physical operator tree.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <string>
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::storage {
class DataManager;
}

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::storage::constraints {
class ConstraintManager;
}

namespace graph::query::algorithm {
class GraphProjectionManager;
}

namespace graph::query {

class QueryPlanner;

/**
 * @brief Converts a LogicalOperator tree into a PhysicalOperator tree.
 *
 * Each logical operator maps to one or more physical operators.
 * The physical execution layer remains completely unchanged.
 */
class PhysicalPlanConverter {
public:
	PhysicalPlanConverter(std::shared_ptr<storage::DataManager> dm,
	                      std::shared_ptr<indexes::IndexManager> im,
	                      std::shared_ptr<storage::constraints::ConstraintManager> cm = nullptr,
	                      std::shared_ptr<algorithm::GraphProjectionManager> pm = nullptr,
	                      uint64_t planCacheHits = 0, uint64_t planCacheMisses = 0)
		: dm_(std::move(dm)), im_(std::move(im)), cm_(std::move(cm)), pm_(std::move(pm)),
		  planCacheHits_(planCacheHits), planCacheMisses_(planCacheMisses) {}

	/**
	 * @brief Converts the root of a logical plan tree into a physical operator tree.
	 */
	std::unique_ptr<execution::PhysicalOperator> convert(
		const logical::LogicalOperator *logicalOp) const;

private:
	std::shared_ptr<storage::DataManager> dm_;
	std::shared_ptr<indexes::IndexManager> im_;
	std::shared_ptr<storage::constraints::ConstraintManager> cm_;
	std::shared_ptr<algorithm::GraphProjectionManager> pm_;
	uint64_t planCacheHits_ = 0;
	uint64_t planCacheMisses_ = 0;

	/// When set, convertSingleRow returns this instead of a new SingleRowOperator.
	/// Used by convertForeach/convertCallSubquery to inject a RecordInjector at
	/// the leaf of the body/subquery plan without fragile tree-walking.
	mutable std::unique_ptr<execution::PhysicalOperator> singleRowOverride_;

	// Conversion methods for each logical operator type
	std::unique_ptr<execution::PhysicalOperator> convertNodeScan(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertFilter(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertProject(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertAggregate(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertSort(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertLimit(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertSkip(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertJoin(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertOptionalMatch(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertTraversal(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertVarLengthTraversal(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertUnwind(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertUnion(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertSingleRow(
		const logical::LogicalOperator *op) const;

	// Write operators
	std::unique_ptr<execution::PhysicalOperator> convertCreateNode(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertCreateEdge(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertSet(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertDelete(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertRemove(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertMergeNode(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertMergeEdge(
		const logical::LogicalOperator *op) const;

	// Admin/DDL operators
	std::unique_ptr<execution::PhysicalOperator> convertCreateIndex(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertDropIndex(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertShowIndexes(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertCreateVectorIndex(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertCreateConstraint(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertDropConstraint(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertShowConstraints(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertTransactionControl(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertCallProcedure(
		const logical::LogicalOperator *op) const;

	// Observability operators
	std::unique_ptr<execution::PhysicalOperator> convertExplain(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertProfile(
		const logical::LogicalOperator *op) const;

	// Subquery & Advanced operators
	std::unique_ptr<execution::PhysicalOperator> convertForeach(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertCallSubquery(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertLoadCsv(
		const logical::LogicalOperator *op) const;
	std::unique_ptr<execution::PhysicalOperator> convertNamedPath(
		const logical::LogicalOperator *op) const;
};

} // namespace graph::query
