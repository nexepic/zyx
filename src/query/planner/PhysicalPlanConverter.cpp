/**
 * @file PhysicalPlanConverter.cpp
 * @brief Converts an optimized logical plan tree into a physical operator tree.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/planner/PhysicalPlanConverter.hpp"

#include "graph/query/execution/operators/AggregateOperator.hpp"
#include "graph/query/execution/operators/ExplainOperator.hpp"
#include "graph/query/execution/operators/ProfileOperator.hpp"
#include "graph/query/execution/operators/CartesianProductOperator.hpp"
#include "graph/query/execution/operators/CreateConstraintOperator.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/CreateVectorIndexOperator.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/DropConstraintOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/LimitOperator.hpp"
#include "graph/query/execution/operators/MergeEdgeOperator.hpp"
#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/query/execution/operators/NodeCountScanOperator.hpp"
#include "graph/query/execution/operators/NodeDistinctCountScanOperator.hpp"
#include "graph/query/execution/operators/NodeGroupCountScanOperator.hpp"
#include "graph/query/execution/operators/NodeProjectionScanOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/NodeTopKScanOperator.hpp"
#include "graph/query/execution/operators/OptionalMatchOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/RelationshipCountScanOperator.hpp"
#include "graph/query/execution/operators/RelationshipProjectionScanOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/ShowConstraintsOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
#include "graph/query/execution/operators/SingleRowOperator.hpp"
#include "graph/query/execution/operators/SkipOperator.hpp"
#include "graph/query/execution/operators/SortOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/execution/operators/TransactionControlOperator.hpp"
#include "graph/query/execution/operators/UnionOperator.hpp"
#include "graph/query/execution/operators/UnwindOperator.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"
#include "graph/query/execution/operators/ForeachOperator.hpp"
#include "graph/query/execution/operators/CallSubqueryOperator.hpp"
#include "graph/query/execution/operators/LoadCsvOperator.hpp"
#include "graph/query/execution/operators/RecordInjectorOperator.hpp"
#include "graph/query/execution/operators/NamedPathOperator.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTransactionControl.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalLoadCsv.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/planner/NodeAccessPathPlanner.hpp"
#include "graph/query/planner/PhysicalScanLoweringPlanner.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"
#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <variant>

namespace graph::query {

using namespace logical;
using namespace execution;
using namespace execution::operators;

namespace {

std::vector<SortItem> toPhysicalSortItems(const std::vector<LogicalSortItem> &logicalItems) {
	std::vector<SortItem> items;
	items.reserve(logicalItems.size());
	for (const auto &litem : logicalItems) {
		items.emplace_back(litem.expression, litem.ascending);
	}
	return items;
}

std::vector<ProjectItem> toPhysicalProjectItems(const std::vector<LogicalProjectItem> &logicalItems) {
	std::vector<ProjectItem> items;
	items.reserve(logicalItems.size());
	for (const auto &litem : logicalItems) {
		items.emplace_back(litem.expression, litem.alias);
	}
	return items;
}

const expressions::VariableReferenceExpression *asPropertyAccess(
		const std::shared_ptr<expressions::Expression> &expression) {
	if (!expression || expression->getExpressionType() != expressions::ExpressionType::PROPERTY_ACCESS) { // ZYX_COV_EXCL_LINE
		return nullptr;
	}
	return static_cast<const expressions::VariableReferenceExpression *>(expression.get());
}

	std::vector<PhysicalOperator::ExplainAttribute> accessPathAttributes(
			const planner::AccessPathSummary &summary,
			const std::string &prefix = "access_path") {
		return planner::toAccessPathAttributes(summary, prefix);
	}

	void appendExplainAttributes(std::vector<PhysicalOperator::ExplainAttribute> &target,
	                             std::vector<PhysicalOperator::ExplainAttribute> extra) {
		target.insert(target.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
	}

	std::vector<PhysicalOperator::ExplainAttribute> scanSpecializationAttributes(
			const planner::PhysicalScanLowering &lowering) {
		auto attributes = lowering.explainAttributes;
		if (attributes.empty()) {
			attributes.emplace_back("scan_specialization.rule", lowering.ruleName);
			attributes.emplace_back("scan_specialization.reason", lowering.reason);
			attributes.emplace_back("scan_specialization.estimated_cost",
			                        planner::formatAccessPathCost(lowering.estimatedCost));
		}
		return attributes;
	}

std::vector<PhysicalOperator::ExplainAttribute> relationshipAccessPathAttributes(
		const planner::RelationshipCountScanPlan &plan) {
	auto attributes = planner::toAccessPathAttributes(plan.seedAccessPath, "seed_access_path");
	if (plan.relationshipAccessPath.has_value()) {
		planner::appendAccessPathAttributes(attributes, *plan.relationshipAccessPath, "relationship_access_path");
	}
	return attributes;
}

	std::unique_ptr<PhysicalOperator> createLoweredScanOperator(
			const std::shared_ptr<storage::DataManager> &dm,
			const std::shared_ptr<indexes::IndexManager> &im,
			planner::PhysicalScanLowering lowering) {
		auto specializationAttributes = scanSpecializationAttributes(lowering);
		switch (lowering.kind) {
			case planner::PhysicalScanLoweringKind::PSLK_NODE_PROJECTION_SCAN: {
				auto plan = std::move(std::get<planner::NodeProjectionScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, accessPathAttributes(plan.accessPath));
				return std::make_unique<NodeProjectionScanOperator>(
						dm, im, std::move(plan.config), std::move(plan.requirements),
						std::move(plan.predicates), std::move(plan.projections), plan.limit,
						std::move(attributes));
			}
			case planner::PhysicalScanLoweringKind::PSLK_NODE_TOPK_SCAN: {
				auto plan = std::move(std::get<planner::NodeTopKScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, accessPathAttributes(plan.accessPath));
				return std::make_unique<NodeTopKScanOperator>(
						dm, im, std::move(plan.config), std::move(plan.requirements),
						std::move(plan.predicates), std::move(plan.projections),
						std::move(plan.sortProperty), plan.ascending, plan.limit,
						std::move(attributes));
			}
			case planner::PhysicalScanLoweringKind::PSLK_NODE_COUNT_SCAN: {
				auto plan = std::move(std::get<planner::NodeCountScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, accessPathAttributes(plan.accessPath));
				return std::make_unique<NodeCountScanOperator>(
						dm, im, std::move(plan.config), std::move(plan.requirements),
						std::move(plan.predicates), std::move(plan.outputAlias),
						std::move(attributes));
			}
			case planner::PhysicalScanLoweringKind::PSLK_NODE_DISTINCT_COUNT_SCAN: {
				auto plan = std::move(std::get<planner::NodeDistinctCountScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, accessPathAttributes(plan.accessPath));
				return std::make_unique<NodeDistinctCountScanOperator>(
						dm, im, std::move(plan.config), std::move(plan.requirements),
						std::move(plan.predicates), std::move(plan.distinctProperty),
						std::move(plan.outputAlias), std::move(attributes));
			}
			case planner::PhysicalScanLoweringKind::PSLK_NODE_GROUP_COUNT_SCAN: {
				auto plan = std::move(std::get<planner::NodeGroupCountScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, accessPathAttributes(plan.accessPath));
				return std::make_unique<NodeGroupCountScanOperator>(
						dm, im, std::move(plan.config), std::move(plan.requirements),
						std::move(plan.predicates), std::move(plan.groupProperty),
						std::move(plan.groupAlias), std::move(plan.outputAlias),
						std::move(attributes));
			}
			case planner::PhysicalScanLoweringKind::PSLK_RELATIONSHIP_PROJECTION_SCAN: {
				auto plan = std::move(std::get<planner::RelationshipProjectionScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, accessPathAttributes(plan.relationshipAccessPath, "relationship_access_path"));
				return std::make_unique<RelationshipProjectionScanOperator>(
						dm, im, std::move(plan.config), std::move(plan.targetVariable),
						std::move(plan.targetLabels), std::move(plan.projections), plan.limit,
						std::move(attributes));
			}
			case planner::PhysicalScanLoweringKind::PSLK_RELATIONSHIP_COUNT_SCAN: {
				auto plan = std::move(std::get<planner::RelationshipCountScanPlan>(lowering.plan));
				auto attributes = std::move(specializationAttributes);
				appendExplainAttributes(attributes, relationshipAccessPathAttributes(plan));
				return std::make_unique<RelationshipCountScanOperator>(
						dm, im, std::move(plan.seedConfig), std::move(plan.seedRequirements),
						std::move(plan.seedPredicates), std::move(plan.hops), std::move(plan.directCount),
						std::move(plan.outputAlias), std::move(attributes));
			}
		}
	throw std::logic_error("unhandled physical scan lowering kind");
}

void addRequiredProperty(NodeScanRequirements &requirements, const std::string &property) {
	if (std::find(requirements.requiredProperties.begin(), requirements.requiredProperties.end(), property) ==
	    requirements.requiredProperties.end()) {
		requirements.requiredProperties.push_back(property);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convert(
	const LogicalOperator *logicalOp) const {

	if (!logicalOp) {
		throw std::invalid_argument("PhysicalPlanConverter::convert: null logical operator");
	}

	switch (logicalOp->getType()) {
		case LogicalOpType::LOP_NODE_SCAN:            return convertNodeScan(logicalOp);
		case LogicalOpType::LOP_FILTER:               return convertFilter(logicalOp);
		case LogicalOpType::LOP_PROJECT:              return convertProject(logicalOp);
		case LogicalOpType::LOP_AGGREGATE:            return convertAggregate(logicalOp);
		case LogicalOpType::LOP_SORT:                 return convertSort(logicalOp);
		case LogicalOpType::LOP_LIMIT:                return convertLimit(logicalOp);
		case LogicalOpType::LOP_SKIP:                 return convertSkip(logicalOp);
		case LogicalOpType::LOP_JOIN:                 return convertJoin(logicalOp);
		case LogicalOpType::LOP_OPTIONAL_MATCH:       return convertOptionalMatch(logicalOp);
		case LogicalOpType::LOP_TRAVERSAL:            return convertTraversal(logicalOp);
		case LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL: return convertVarLengthTraversal(logicalOp);
		case LogicalOpType::LOP_UNWIND:               return convertUnwind(logicalOp);
		case LogicalOpType::LOP_UNION:                return convertUnion(logicalOp);
		case LogicalOpType::LOP_SINGLE_ROW:           return convertSingleRow(logicalOp);
		case LogicalOpType::LOP_CREATE_NODE:          return convertCreateNode(logicalOp);
		case LogicalOpType::LOP_CREATE_EDGE:          return convertCreateEdge(logicalOp);
		case LogicalOpType::LOP_SET:                  return convertSet(logicalOp);
		case LogicalOpType::LOP_DELETE:               return convertDelete(logicalOp);
		case LogicalOpType::LOP_REMOVE:               return convertRemove(logicalOp);
		case LogicalOpType::LOP_MERGE_NODE:           return convertMergeNode(logicalOp);
		case LogicalOpType::LOP_MERGE_EDGE:           return convertMergeEdge(logicalOp);
		case LogicalOpType::LOP_CREATE_INDEX:         return convertCreateIndex(logicalOp);
		case LogicalOpType::LOP_DROP_INDEX:           return convertDropIndex(logicalOp);
		case LogicalOpType::LOP_SHOW_INDEXES:         return convertShowIndexes(logicalOp);
		case LogicalOpType::LOP_CREATE_VECTOR_INDEX:  return convertCreateVectorIndex(logicalOp);
		case LogicalOpType::LOP_CREATE_CONSTRAINT:    return convertCreateConstraint(logicalOp);
		case LogicalOpType::LOP_DROP_CONSTRAINT:      return convertDropConstraint(logicalOp);
		case LogicalOpType::LOP_SHOW_CONSTRAINTS:     return convertShowConstraints(logicalOp);
		case LogicalOpType::LOP_TRANSACTION_CONTROL:  return convertTransactionControl(logicalOp);
		case LogicalOpType::LOP_CALL_PROCEDURE:       return convertCallProcedure(logicalOp);
		case LogicalOpType::LOP_EXPLAIN:              return convertExplain(logicalOp);
		case LogicalOpType::LOP_PROFILE:              return convertProfile(logicalOp);
		case LogicalOpType::LOP_FOREACH:              return convertForeach(logicalOp);
		case LogicalOpType::LOP_CALL_SUBQUERY:        return convertCallSubquery(logicalOp);
		case LogicalOpType::LOP_LOAD_CSV:             return convertLoadCsv(logicalOp);
		case LogicalOpType::LOP_NAMED_PATH:           return convertNamedPath(logicalOp);
		default:
			throw std::runtime_error(
				"PhysicalPlanConverter: unsupported logical operator type: " +
				toString(logicalOp->getType()));
	}
}

// ---------------------------------------------------------------------------
// Read operators
// ---------------------------------------------------------------------------

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertNodeScan(
	const LogicalOperator *op) const {

	const auto *scan = static_cast<const LogicalNodeScan *>(op);

	const auto &predicates = scan->getPropertyPredicates();

	// Keep ordinary scans on the same access-path decision layer as specialized plans.
	auto accessPath = planner::chooseNodeAccessPathDecision(*scan, im_);
	NodeScanConfig config = accessPath.config();
	if (accessPath.selectedRequiresConservativeFallback()) {
		planner::fallbackToLabelOrFullScan(config);
	}

	std::unique_ptr<PhysicalOperator> root =
		std::make_unique<NodeScanOperator>(dm_, im_, config);

	// Residual multi-label filter (first label handled by index/scan)
	if (scan->getLabels().size() > 1) {
		std::vector<int64_t> allLabelIds;
		allLabelIds.reserve(scan->getLabels().size());
		for (const auto &lbl : scan->getLabels()) {
			allLabelIds.push_back(dm_->resolveTokenId(lbl));
		}
		std::string variable = scan->getVariable();
		auto predicate = [variable, allLabelIds](const Record &r) -> bool {
			auto n = r.getNode(variable);
			if (!n) return false; // ZYX_COV_EXCL_LINE
			for (int64_t lid : allLabelIds) {
				if (!n->hasLabelId(lid)) return false; // ZYX_COV_EXCL_LINE
			}
			return true;
		};
		std::string desc = "MultiLabel(" + variable + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	// Residual property filters for all predicates not handled by index scan
	for (size_t i = 0; i < predicates.size(); ++i) {
		const auto &[pKey, pVal] = predicates[i];
		// Skip the predicate actually selected for the property scan.
		if (config.type == ScanType::PROPERTY_SCAN && pKey == config.indexKey) continue;
		if (config.type == ScanType::COMPOSITE_SCAN) { // ZYX_COV_EXCL_LINE
			// Skip predicates that are part of the composite index
			bool inComposite = false;
			for (const auto &ck : config.compositeKeys) { // ZYX_COV_EXCL_LINE
				if (ck == pKey) { inComposite = true; break; } // ZYX_COV_EXCL_LINE
			}
			if (inComposite) continue; // ZYX_COV_EXCL_LINE
		}

		std::string variable = scan->getVariable();
		std::string filterKey = pKey;
		PropertyValue filterVal = pVal;
		auto predicate = [variable, filterKey, filterVal](const Record &r) -> bool {
			auto n = r.getNode(variable);
			if (!n) return false; // ZYX_COV_EXCL_LINE
			const auto &props = n->getProperties();
			auto it = props.find(filterKey);
			return it != props.end() && it->second == filterVal;
		};
		std::string desc = variable + "." + filterKey + " == " + filterVal.toString() + " (Residual)";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	// Residual range filters for predicates not handled by range scan
	for (const auto &rp : scan->getRangePredicates()) {
		// Skip the range predicate that was handled by the range scan
		if (config.type == ScanType::RANGE_SCAN && rp.key == config.indexKey) continue; // ZYX_COV_EXCL_LINE

		std::string variable = scan->getVariable();
		std::string filterKey = rp.key;
		PropertyValue filterMin = rp.minValue;
		PropertyValue filterMax = rp.maxValue;
		bool minIncl = rp.minInclusive;
		bool maxIncl = rp.maxInclusive;
		auto predicate = [variable, filterKey, filterMin, filterMax, minIncl, maxIncl](const Record &r) -> bool {
			auto n = r.getNode(variable);
			if (!n) return false; // ZYX_COV_EXCL_LINE
			const auto &props = n->getProperties();
			auto it = props.find(filterKey);
			if (it == props.end()) return false;
			const auto &v = it->second;
			if (filterMin.getType() != PropertyType::NULL_TYPE) {
				if (minIncl ? v < filterMin : v <= filterMin) return false;
			}
			if (filterMax.getType() != PropertyType::NULL_TYPE) {
				if (maxIncl ? v > filterMax : v >= filterMax) return false;
			}
			return true;
		};
		std::string desc = variable + "." + filterKey + " range (Residual)";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	return root;
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertFilter(
	const LogicalOperator *op) const {

	const auto *filter = static_cast<const LogicalFilter *>(op);

	auto childPhys = convert(filter->getChildren()[0]);

	auto astShared = filter->getPredicate();
	auto *dm = dm_.get();

	return std::make_unique<FilterOperator>(
		std::move(childPhys), astShared, dm, filter->toString());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertProject(
	const LogicalOperator *op) const {

	const auto *project = static_cast<const LogicalProject *>(op);

	if (auto lowering = planner::tryLowerProjectToScan(*project, im_)) {
		return createLoweredScanOperator(dm_, im_, std::move(*lowering));
	}

	auto children = project->getChildren();
	if (!project->isDistinct() && !children.empty() && children[0] && // ZYX_COV_EXCL_LINE
	    children[0]->getType() == LogicalOpType::LOP_LIMIT) {
		const auto *limit = static_cast<const LogicalLimit *>(children[0]);
		auto limitChildren = limit->getChildren();
		if (!limitChildren.empty() && limitChildren[0] && // ZYX_COV_EXCL_LINE
		    limitChildren[0]->getType() == LogicalOpType::LOP_SORT) {
			const auto *sort = static_cast<const LogicalSort *>(limitChildren[0]);
			auto sortChildren = sort->getChildren();
			if (!sortChildren.empty() && sortChildren[0] && // ZYX_COV_EXCL_LINE
			    sortChildren[0]->getType() == LogicalOpType::LOP_NODE_SCAN) { // ZYX_COV_EXCL_LINE
				const auto *scan = static_cast<const LogicalNodeScan *>(sortChildren[0]);
				NodeScanRequirements requirements;
				requirements.materialization = NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
				const auto &scanVariable = scan->getVariable();
				bool canUseSelectedScan = scan->getPropertyPredicates().empty() && // ZYX_COV_EXCL_LINE
				                          scan->getRangePredicates().empty() &&
				                          !scan->getCompositeEquality().has_value(); // ZYX_COV_EXCL_LINE

				for (const auto &item : project->getItems()) {
					const auto *property = asPropertyAccess(item.expression);
					if (!property || property->getVariableName() != scanVariable) { // ZYX_COV_EXCL_LINE
						canUseSelectedScan = false;
						break;
					}
					addRequiredProperty(requirements, property->getPropertyName());
				}
				for (const auto &item : sort->getSortItems()) {
					const auto *property = asPropertyAccess(item.expression);
					if (!property || property->getVariableName() != scanVariable) { // ZYX_COV_EXCL_LINE
						canUseSelectedScan = false;
						break;
					}
					addRequiredProperty(requirements, property->getPropertyName());
				}

				if (canUseSelectedScan) {
					NodeScanConfig config = planner::chooseNodeAccessPathDecision(*scan, im_).config();
					auto scanPhys = std::make_unique<NodeScanOperator>(
						dm_, im_, std::move(config), std::move(requirements));
					auto sortPhys = std::make_unique<SortOperator>(
						std::move(scanPhys), toPhysicalSortItems(sort->getSortItems()), limit->getLimit());
					return std::make_unique<ProjectOperator>(
						std::move(sortPhys), toPhysicalProjectItems(project->getItems()), false, dm_.get());
				}
			}
		}
	}

	auto childPhys = convert(children[0]);
	auto items = toPhysicalProjectItems(project->getItems());
	return std::make_unique<ProjectOperator>(std::move(childPhys), std::move(items),
	                                         project->isDistinct(), dm_.get());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertAggregate(
	const LogicalOperator *op) const {

	const auto *agg = static_cast<const LogicalAggregate *>(op);

	if (auto lowering = planner::tryLowerAggregateToScan(*agg, im_)) {
		return createLoweredScanOperator(dm_, im_, std::move(*lowering));
	}

	auto childPhys = convert(agg->getChildren()[0]);

	// Map function name string to AggregateFunctionType enum
	auto mapFn = [](const std::string &name) -> AggregateFunctionType {
		if (name == "count")          return AggregateFunctionType::AGG_COUNT;
		if (name == "sum")            return AggregateFunctionType::AGG_SUM;
		if (name == "avg")            return AggregateFunctionType::AGG_AVG;
		if (name == "min")            return AggregateFunctionType::AGG_MIN;
		if (name == "max")            return AggregateFunctionType::AGG_MAX;
		if (name == "collect")        return AggregateFunctionType::AGG_COLLECT;
		if (name == "stdev")          return AggregateFunctionType::AGG_STDEV;
		if (name == "stdevp")         return AggregateFunctionType::AGG_STDEVP;
		if (name == "percentiledisc") return AggregateFunctionType::AGG_PERCENTILE_DISC;
		if (name == "percentilecont") return AggregateFunctionType::AGG_PERCENTILE_CONT;
		throw std::runtime_error("PhysicalPlanConverter: unknown aggregate function: " + name);
	};

	std::vector<AggregateItem> aggregates;
	aggregates.reserve(agg->getAggregations().size());
	for (const auto &litem : agg->getAggregations()) {
		double percentileArg = 0.5;
		if (litem.extraArg) {
			// Evaluate the percentile literal as a constant
			graph::query::expressions::EvaluationContext dummyCtx(
				graph::query::execution::Record{}, nullptr);
			graph::query::expressions::ExpressionEvaluator eval(dummyCtx);
			auto pval = eval.evaluate(litem.extraArg.get());
			if (pval.getType() == PropertyType::DOUBLE) {
				percentileArg = std::get<double>(pval.getVariant());
			} else if (pval.getType() == PropertyType::INTEGER) { // ZYX_COV_EXCL_LINE
				percentileArg = static_cast<double>(std::get<int64_t>(pval.getVariant()));
			}
		}
		aggregates.emplace_back(mapFn(litem.functionName), litem.argument,
		                        litem.alias, litem.distinct, percentileArg);
	}

	// Build GroupByItems – use stored aliases if available, fall back to expression's toString()
	const auto &aliases = agg->getGroupByAliases();
	std::vector<GroupByItem> groupByItems;
	groupByItems.reserve(agg->getGroupByExprs().size());
	for (size_t i = 0; i < agg->getGroupByExprs().size(); ++i) {
		const auto &expr = agg->getGroupByExprs()[i];
		std::string alias = (i < aliases.size() && !aliases[i].empty())
			? aliases[i]
			: (expr ? expr->toString() : "");
		groupByItems.emplace_back(expr, alias);
	}

	return std::make_unique<AggregateOperator>(std::move(childPhys), std::move(aggregates),
	                                           std::move(groupByItems), dm_.get());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertSort(
	const LogicalOperator *op) const {

	const auto *sort = static_cast<const LogicalSort *>(op);

	auto childPhys = convert(sort->getChildren()[0]);
	return std::make_unique<SortOperator>(
		std::move(childPhys), toPhysicalSortItems(sort->getSortItems()));
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertLimit(
	const LogicalOperator *op) const {

	const auto *limit = static_cast<const LogicalLimit *>(op);
	auto children = limit->getChildren();
	if (!children.empty() && children[0] && children[0]->getType() == LogicalOpType::LOP_SORT) { // ZYX_COV_EXCL_LINE
		const auto *sort = static_cast<const LogicalSort *>(children[0]);
		auto sortChildren = sort->getChildren();
		if (!sortChildren.empty() && sortChildren[0]) { // ZYX_COV_EXCL_LINE
			auto childPhys = convert(sortChildren[0]);
			return std::make_unique<SortOperator>(
				std::move(childPhys), toPhysicalSortItems(sort->getSortItems()), limit->getLimit());
		}
	}

	auto childPhys = convert(children[0]);
	return std::make_unique<LimitOperator>(std::move(childPhys), limit->getLimit());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertSkip(
	const LogicalOperator *op) const {

	const auto *skip = static_cast<const LogicalSkip *>(op);
	auto childPhys = convert(skip->getChildren()[0]);
	return std::make_unique<SkipOperator>(std::move(childPhys), skip->getOffset());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertJoin(
	const LogicalOperator *op) const {

	const auto *join = static_cast<const LogicalJoin *>(op);
	auto leftPhys  = convert(join->getLeft());
	auto rightPhys = convert(join->getRight());
	return std::make_unique<CartesianProductOperator>(std::move(leftPhys), std::move(rightPhys));
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertOptionalMatch(
	const LogicalOperator *op) const {

	const auto *opt = static_cast<const LogicalOptionalMatch *>(op);
	auto inputPhys   = convert(opt->getInput());
	auto patternPhys = convert(opt->getOptionalPattern());
	return std::make_unique<OptionalMatchOperator>(std::move(inputPhys), std::move(patternPhys),
	                                               opt->getRequiredVariables());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertTraversal(
	const LogicalOperator *op) const {

	const auto *trav = static_cast<const LogicalTraversal *>(op);
	auto childPhys = convert(trav->getChildren()[0]);
	std::unique_ptr<PhysicalOperator> root =
		std::make_unique<TraversalOperator>(dm_, std::move(childPhys),
		                                    trav->getSourceVar(), trav->getEdgeVar(),
		                                    trav->getTargetVar(), trav->getEdgeType(),
		                                    trav->getDirection());

	// Add target label filter if specified
	if (!trav->getTargetLabels().empty()) {
		std::vector<int64_t> labelIds;
		labelIds.reserve(trav->getTargetLabels().size());
		for (const auto &lbl : trav->getTargetLabels()) {
			labelIds.push_back(dm_->getOrCreateTokenId(lbl));
		}
		std::string targetVar = trav->getTargetVar();
		auto predicate = [targetVar, labelIds](const Record &r) -> bool {
			auto n = r.getNode(targetVar);
			if (!n) return false; // ZYX_COV_EXCL_LINE
			for (int64_t lid : labelIds) {
				if (!n->hasLabelId(lid)) return false;
			}
			return true;
		};
		std::string desc = "TargetLabel(" + targetVar + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	// Add target property filter if specified
	if (!trav->getTargetProperties().empty()) {
		std::string targetVar = trav->getTargetVar();
		auto props = trav->getTargetProperties();
		auto predicate = [targetVar, props](const Record &r) -> bool {
			auto n = r.getNode(targetVar);
			if (!n) return false; // ZYX_COV_EXCL_LINE
			const auto &nodeProps = n->getProperties();
			for (const auto &[key, val] : props) {
				auto it = nodeProps.find(key);
				if (it == nodeProps.end() || it->second != val) return false; // ZYX_COV_EXCL_LINE
			}
			return true;
		};
		std::string desc = "TargetProps(" + targetVar + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	// Add edge property filter if specified
	if (!trav->getEdgeProperties().empty()) {
		std::string edgeVar = trav->getEdgeVar();
		auto edgeProps = trav->getEdgeProperties();
		auto predicate = [edgeVar, edgeProps](const Record &r) -> bool {
			auto e = r.getEdge(edgeVar);
			if (!e) return false; // ZYX_COV_EXCL_LINE
			const auto &ep = e->getProperties();
			for (const auto &[key, val] : edgeProps) {
				auto it = ep.find(key);
				if (it == ep.end() || it->second != val) return false; // ZYX_COV_EXCL_LINE
			}
			return true;
		};
		std::string desc = "EdgeProps(" + edgeVar + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	return root;
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertVarLengthTraversal(
	const LogicalOperator *op) const {

	const auto *vlt = static_cast<const LogicalVarLengthTraversal *>(op);
	auto childPhys = convert(vlt->getChildren()[0]);
	std::vector<int64_t> targetLabelIds;
	targetLabelIds.reserve(vlt->getTargetLabels().size());
	for (const auto &label : vlt->getTargetLabels()) {
		targetLabelIds.push_back(dm_->getOrCreateTokenId(label));
	}

	std::unique_ptr<PhysicalOperator> root =
		std::make_unique<VarLengthTraversalOperator>(dm_, std::move(childPhys),
		                                             vlt->getSourceVar(), vlt->getTargetVar(),
		                                             vlt->getEdgeType(), vlt->getMinHops(),
		                                             vlt->getMaxHops(), vlt->getDirection(),
		                                             std::move(targetLabelIds), vlt->getTargetProperties(),
		                                             im_, vlt->getTargetLabels());

	return root;
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertUnwind(
	const LogicalOperator *op) const {

	const auto *unwind = static_cast<const LogicalUnwind *>(op);

	auto childPhys = convert(unwind->getChildren()[0]);

	if (unwind->hasLiteralList()) {
		return std::make_unique<UnwindOperator>(std::move(childPhys), unwind->getAlias(),
		                                        unwind->getListValues());
	}
	return std::make_unique<UnwindOperator>(std::move(childPhys), unwind->getAlias(),
	                                        unwind->getListExpr());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertUnion(
	const LogicalOperator *op) const {

	const auto *un = static_cast<const LogicalUnion *>(op);
	auto leftPhys  = convert(un->getLeft());
	auto rightPhys = convert(un->getRight());
	return std::make_unique<UnionOperator>(std::move(leftPhys), std::move(rightPhys), un->isAll());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertSingleRow(
	const LogicalOperator * /*op*/) const {

	if (singleRowOverride_) {
		return std::move(singleRowOverride_);
	}
	return std::make_unique<SingleRowOperator>();
}

// ---------------------------------------------------------------------------
// Write operators
// ---------------------------------------------------------------------------

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCreateNode(
	const LogicalOperator *op) const {

	const auto *cn = static_cast<const LogicalCreateNode *>(op);

	auto physOp = std::make_unique<CreateNodeOperator>(dm_, cn->getVariable(), cn->getLabels(),
	                                                   cn->getProperties(),
	                                                   cn->getPropertyExprs());

	const auto children = cn->getChildren();
	if (!children.empty()) {
		physOp->setChild(convert(children[0]));
	}

	return physOp;
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCreateEdge(
	const LogicalOperator *op) const {

	const auto *ce = static_cast<const LogicalCreateEdge *>(op);

	auto physOp = std::make_unique<CreateEdgeOperator>(dm_, ce->getVariable(), ce->getEdgeType(),
	                                                   ce->getProperties(), ce->getSourceVar(),
	                                                   ce->getTargetVar());

	const auto children = ce->getChildren();
	if (!children.empty()) {
		physOp->setChild(convert(children[0]));
	}

	return physOp;
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertSet(
	const LogicalOperator *op) const {

	const auto *lset = static_cast<const LogicalSet *>(op);

	std::vector<SetItem> items;
	items.reserve(lset->getItems().size());
	for (const auto &litem : lset->getItems()) {
		execution::operators::SetActionType actionType;
		switch (litem.type) {
			case logical::SetActionType::LSET_PROPERTY:
				actionType = execution::operators::SetActionType::PROPERTY;  break;
			case logical::SetActionType::LSET_LABEL:
				actionType = execution::operators::SetActionType::LABEL;     break;
			case logical::SetActionType::LSET_MAP_MERGE:
				actionType = execution::operators::SetActionType::MAP_MERGE; break;
			default:
				actionType = execution::operators::SetActionType::PROPERTY;  break;
		}
		items.emplace_back(actionType, litem.variable, litem.key, litem.expression);
	}

	const auto children = lset->getChildren();
	auto childPhys = children.empty() ? nullptr : convert(children[0]);

	return std::make_unique<SetOperator>(dm_, std::move(childPhys), std::move(items));
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertDelete(
	const LogicalOperator *op) const {

	const auto *del = static_cast<const LogicalDelete *>(op);

	const auto children = del->getChildren();
	auto childPhys = children.empty() ? nullptr : convert(children[0]);

	return std::make_unique<DeleteOperator>(dm_, std::move(childPhys), del->getVariables(),
	                                        del->isDetach());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertRemove(
	const LogicalOperator *op) const {

	const auto *rem = static_cast<const LogicalRemove *>(op);

	std::vector<RemoveItem> items;
	items.reserve(rem->getItems().size());
	for (const auto &litem : rem->getItems()) {
		RemoveActionType actionType;
		switch (litem.type) {
			case LogicalRemoveActionType::LREM_PROPERTY: actionType = RemoveActionType::PROPERTY; break;
			case LogicalRemoveActionType::LREM_LABEL:    actionType = RemoveActionType::LABEL;    break;
			default:                                     actionType = RemoveActionType::PROPERTY; break;
		}
		items.push_back({actionType, litem.variable, litem.key});
	}

	const auto children = rem->getChildren();
	auto childPhys = children.empty() ? nullptr : convert(children[0]);

	return std::make_unique<RemoveOperator>(dm_, std::move(childPhys), std::move(items));
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertMergeNode(
	const LogicalOperator *op) const {

	const auto *mn = static_cast<const LogicalMergeNode *>(op);

	// Convert MergeSetActions to SetItems
	auto convertActions = [](const std::vector<MergeSetAction> &actions) {
		std::vector<SetItem> items;
		items.reserve(actions.size());
		for (const auto &a : actions) {
			items.emplace_back(execution::operators::SetActionType::PROPERTY,
			                   a.variable, a.key, a.expression);
		}
		return items;
	};

	auto physOp = std::make_unique<MergeNodeOperator>(dm_, im_, mn->getVariable(), mn->getLabels(),
	                                                  mn->getMatchProps(),
	                                                  convertActions(mn->getOnCreateItems()),
	                                                  convertActions(mn->getOnMatchItems()));

	const auto children = mn->getChildren();
	if (!children.empty()) {
		physOp->setChild(convert(children[0]));
	}

	return physOp;
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertMergeEdge(
	const LogicalOperator *op) const {

	const auto *me = static_cast<const LogicalMergeEdge *>(op);

	auto convertActions = [](const std::vector<MergeSetAction> &actions) {
		std::vector<SetItem> items;
		items.reserve(actions.size());
		for (const auto &a : actions) {
			items.emplace_back(execution::operators::SetActionType::PROPERTY,
			                   a.variable, a.key, a.expression);
		}
		return items;
	};

	auto mergeEdge = std::make_unique<MergeEdgeOperator>(dm_, im_, me->getSourceVar(), me->getEdgeVar(),
	                                                     me->getTargetVar(), me->getEdgeType(),
	                                                     me->getMatchProps(), me->getDirection(),
	                                                     convertActions(me->getOnCreateActions()),
	                                                     convertActions(me->getOnMatchActions()));

	// If the logical merge edge has a child (node resolution chain), convert it
	auto children = me->getChildren();
	if (!children.empty() && children[0]) { // ZYX_COV_EXCL_LINE
		mergeEdge->setChild(convert(children[0]));
	}

	return mergeEdge;
}

// ---------------------------------------------------------------------------
// Admin / DDL operators
// ---------------------------------------------------------------------------

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCreateIndex(
	const LogicalOperator *op) const {

	const auto *ci = static_cast<const LogicalCreateIndex *>(op);
	if (ci->isComposite()) {
		return std::make_unique<CreateIndexOperator>(im_, ci->getIndexName(), ci->getLabel(),
		                                             ci->getPropertyKeys());
	}
	return std::make_unique<CreateIndexOperator>(im_, ci->getIndexName(), ci->getLabel(),
	                                             ci->getPropertyKey());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertDropIndex(
	const LogicalOperator *op) const {

	const auto *di = static_cast<const LogicalDropIndex *>(op);

	if (!di->getIndexName().empty()) {
		return std::make_unique<DropIndexOperator>(im_, di->getIndexName());
	}
	return std::make_unique<DropIndexOperator>(im_, di->getLabel(), di->getPropertyKey());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertShowIndexes(
	const LogicalOperator * /*op*/) const {

	return std::make_unique<ShowIndexesOperator>(im_);
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCreateVectorIndex(
	const LogicalOperator *op) const {

	const auto *cvi = static_cast<const LogicalCreateVectorIndex *>(op);
	return std::make_unique<CreateVectorIndexOperator>(im_, cvi->getIndexName(), cvi->getLabel(),
	                                                   cvi->getProperty(), cvi->getDimension(),
	                                                   cvi->getMetric());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCreateConstraint(
	const LogicalOperator *op) const {

	const auto *cc = static_cast<const LogicalCreateConstraint *>(op);
	return std::make_unique<CreateConstraintOperator>(cm_, cc->getName(), cc->getEntityType(),
	                                                  cc->getConstraintType(), cc->getLabel(),
	                                                  cc->getProperties(), cc->getOptions());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertDropConstraint(
	const LogicalOperator *op) const {

	const auto *dc = static_cast<const LogicalDropConstraint *>(op);
	return std::make_unique<DropConstraintOperator>(cm_, dc->getName(), dc->isIfExists());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertShowConstraints(
	const LogicalOperator * /*op*/) const {

	return std::make_unique<ShowConstraintsOperator>(cm_);
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertTransactionControl(
	const LogicalOperator *op) const {

	const auto *tc = static_cast<const LogicalTransactionControl *>(op);

	TransactionCommand cmd;
	switch (tc->getCommand()) {
		case LogicalTxnCommand::LTXN_BEGIN:    cmd = TransactionCommand::TXN_CTL_BEGIN;    break;
		case LogicalTxnCommand::LTXN_COMMIT:   cmd = TransactionCommand::TXN_CTL_COMMIT;   break;
		case LogicalTxnCommand::LTXN_ROLLBACK: cmd = TransactionCommand::TXN_CTL_ROLLBACK; break;
		default:
			throw std::runtime_error("PhysicalPlanConverter: unknown transaction command");
	}

	return std::make_unique<TransactionControlOperator>(cmd);
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCallProcedure(
	const LogicalOperator *op) const {

	const auto *cp = static_cast<const LogicalCallProcedure *>(op);

	const planner::ProcedureContext ctx{dm_, im_, pm_, planCacheHits_, planCacheMisses_};
	if (const auto factory = planner::ProcedureRegistry::instance().get(cp->getProcedureName())) {
		return factory(ctx, cp->getArgs());
	}

	throw std::runtime_error("PhysicalPlanConverter: unknown procedure: " + cp->getProcedureName());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertExplain(
	const LogicalOperator *op) const {

	const auto *explain = static_cast<const LogicalExplain *>(op);
	// Pass the inner logical plan to ExplainOperator — it prints the plan without executing
	return std::make_unique<ExplainOperator>(explain->getInnerPlan());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertProfile(
	const LogicalOperator *op) const {

	const auto *profile = static_cast<const LogicalProfile *>(op);
	// Convert the inner logical plan to physical, then wrap in ProfileOperator
	auto innerPhys = convert(profile->getInnerPlan());
	return std::make_unique<ProfileOperator>(std::move(innerPhys));
}

// ---------------------------------------------------------------------------
// Subquery & Advanced operators
// ---------------------------------------------------------------------------

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertForeach(
	const LogicalOperator *op) const {

	const auto *foreach = static_cast<const LogicalForeach *>(op);

	std::unique_ptr<PhysicalOperator> childPhys;
	if (foreach->getInput()) {
		childPhys = convert(foreach->getInput());
	}

	// Create a RecordInjector and install it as the SingleRow replacement.
	// When convert() encounters LogicalSingleRow in the body, convertSingleRow()
	// returns this injector instead of a plain SingleRowOperator.
	auto injector = std::make_unique<RecordInjectorOperator>();
	RecordInjectorOperator *injectorPtr = injector.get();
	singleRowOverride_ = std::move(injector);

	std::unique_ptr<PhysicalOperator> bodyPhys;
	if (foreach->getBody()) {
		bodyPhys = convert(foreach->getBody());
	}

	// Clear any unconsumed override (e.g., if body had no LogicalSingleRow)
	singleRowOverride_.reset();

	return std::make_unique<ForeachOperator>(
		std::move(childPhys), foreach->getIterVar(),
		foreach->getListExpr(), std::move(bodyPhys), injectorPtr);
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertCallSubquery(
	const LogicalOperator *op) const {

	const auto *callSub = static_cast<const LogicalCallSubquery *>(op);

	std::unique_ptr<PhysicalOperator> inputPhys;
	if (callSub->getInput()) {
		inputPhys = convert(callSub->getInput());
	}

	// Create a RecordInjector for imported variable injection.
	// Install it as the SingleRow replacement before converting the subquery.
	RecordInjectorOperator *injectorPtr = nullptr;
	if (!callSub->getImportedVars().empty()) {
		auto injector = std::make_unique<RecordInjectorOperator>();
		injectorPtr = injector.get();
		singleRowOverride_ = std::move(injector);
	}

	std::unique_ptr<PhysicalOperator> subqueryPhys;
	if (callSub->getSubquery()) {
		subqueryPhys = convert(callSub->getSubquery());
	}

	// Clear any unconsumed override
	singleRowOverride_.reset();

	return std::make_unique<CallSubqueryOperator>(
		std::move(inputPhys), std::move(subqueryPhys),
		callSub->getImportedVars(), callSub->getReturnedVars(),
		injectorPtr,
		callSub->isInTransactions(), callSub->getBatchSize());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertLoadCsv(
	const LogicalOperator *op) const {

	const auto *loadCsv = static_cast<const LogicalLoadCsv *>(op);

	std::unique_ptr<PhysicalOperator> childPhys;
	if (!loadCsv->getChildren().empty() && loadCsv->getChildren()[0]) { // ZYX_COV_EXCL_LINE
		childPhys = convert(loadCsv->getChildren()[0]);
	}

	return std::make_unique<LoadCsvOperator>(
		std::move(childPhys), loadCsv->getUrlExpr(),
		loadCsv->getRowVariable(), loadCsv->isWithHeaders(),
		loadCsv->getFieldTerminator());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertNamedPath(
	const LogicalOperator *op) const {

	const auto *namedPath = static_cast<const LogicalNamedPath *>(op);

	auto children = namedPath->getChildren();
	std::unique_ptr<PhysicalOperator> childPhys;
	if (!children.empty() && children[0]) { // ZYX_COV_EXCL_LINE
		childPhys = convert(children[0]);
	}

	return std::make_unique<NamedPathOperator>(
		std::move(childPhys), dm_,
		namedPath->getPathVariable(),
		namedPath->getNodeVariables(),
		namedPath->getEdgeVariables());
}

} // namespace graph::query
