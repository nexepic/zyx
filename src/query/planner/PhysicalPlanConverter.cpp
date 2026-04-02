/**
 * @file PhysicalPlanConverter.cpp
 * @brief Converts an optimized logical plan tree into a physical operator tree.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/planner/PhysicalPlanConverter.hpp"

#include "graph/query/execution/operators/AggregateOperator.hpp"
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
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/OptionalMatchOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
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
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
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
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
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
#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"
#include "graph/storage/data/DataManager.hpp"

#include <stdexcept>

namespace graph::query {

using namespace logical;
using namespace execution;
using namespace execution::operators;

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
	std::string key   = !predicates.empty() ? predicates[0].first  : "";
	PropertyValue val = !predicates.empty() ? predicates[0].second : PropertyValue();

	// Use the optimizer to choose the best scan strategy
	optimizer::rules::IndexPushdownRule pushdown(im_);
	NodeScanConfig config = pushdown.apply(
		scan->getVariable(), scan->getLabels(), key, val,
		scan->getRangePredicates(), scan->getCompositeEquality());

	std::unique_ptr<PhysicalOperator> root =
		std::make_unique<NodeScanOperator>(dm_, im_, config);

	// Residual multi-label filter (first label handled by index/scan)
	if (scan->getLabels().size() > 1) {
		std::vector<int64_t> allLabelIds;
		allLabelIds.reserve(scan->getLabels().size());
		for (const auto &lbl : scan->getLabels()) {
			allLabelIds.push_back(dm_->getOrCreateLabelId(lbl));
		}
		std::string variable = scan->getVariable();
		auto predicate = [variable, allLabelIds](const Record &r) -> bool {
			auto n = r.getNode(variable);
			if (!n) return false;
			for (int64_t lid : allLabelIds) {
				if (!n->hasLabelId(lid)) return false;
			}
			return true;
		};
		std::string desc = "MultiLabel(" + variable + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	// Residual property filters for all predicates not handled by index scan
	for (size_t i = 0; i < predicates.size(); ++i) {
		const auto &[pKey, pVal] = predicates[i];
		// Skip the first predicate if it was handled by a property or composite scan
		if (i == 0 && config.type == ScanType::PROPERTY_SCAN) continue;
		if (config.type == ScanType::COMPOSITE_SCAN) {
			// Skip predicates that are part of the composite index
			bool inComposite = false;
			for (const auto &ck : config.compositeKeys) {
				if (ck == pKey) { inComposite = true; break; }
			}
			if (inComposite) continue;
		}

		std::string variable = scan->getVariable();
		std::string filterKey = pKey;
		PropertyValue filterVal = pVal;
		auto predicate = [variable, filterKey, filterVal](const Record &r) -> bool {
			auto n = r.getNode(variable);
			if (!n) return false;
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
		if (config.type == ScanType::RANGE_SCAN && rp.key == config.indexKey) continue;

		std::string variable = scan->getVariable();
		std::string filterKey = rp.key;
		PropertyValue filterMin = rp.minValue;
		PropertyValue filterMax = rp.maxValue;
		bool minIncl = rp.minInclusive;
		bool maxIncl = rp.maxInclusive;
		auto predicate = [variable, filterKey, filterMin, filterMax, minIncl, maxIncl](const Record &r) -> bool {
			auto n = r.getNode(variable);
			if (!n) return false;
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

	auto childPhys = convert(project->getChildren()[0]);

	std::vector<ProjectItem> items;
	items.reserve(project->getItems().size());
	for (const auto &litem : project->getItems()) {
		items.emplace_back(litem.expression, litem.alias);
	}

	return std::make_unique<ProjectOperator>(std::move(childPhys), std::move(items),
	                                         project->isDistinct(), dm_.get());
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertAggregate(
	const LogicalOperator *op) const {

	const auto *agg = static_cast<const LogicalAggregate *>(op);

	auto childPhys = convert(agg->getChildren()[0]);

	// Map function name string to AggregateFunctionType enum
	auto mapFn = [](const std::string &name) -> AggregateFunctionType {
		if (name == "count")   return AggregateFunctionType::AGG_COUNT;
		if (name == "sum")     return AggregateFunctionType::AGG_SUM;
		if (name == "avg")     return AggregateFunctionType::AGG_AVG;
		if (name == "min")     return AggregateFunctionType::AGG_MIN;
		if (name == "max")     return AggregateFunctionType::AGG_MAX;
		if (name == "collect") return AggregateFunctionType::AGG_COLLECT;
		throw std::runtime_error("PhysicalPlanConverter: unknown aggregate function: " + name);
	};

	std::vector<AggregateItem> aggregates;
	aggregates.reserve(agg->getAggregations().size());
	for (const auto &litem : agg->getAggregations()) {
		aggregates.emplace_back(mapFn(litem.functionName), litem.argument,
		                        litem.alias, litem.distinct);
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

	std::vector<SortItem> items;
	items.reserve(sort->getSortItems().size());
	for (const auto &litem : sort->getSortItems()) {
		items.emplace_back(litem.expression, litem.ascending);
	}

	return std::make_unique<SortOperator>(std::move(childPhys), std::move(items));
}

std::unique_ptr<PhysicalOperator> PhysicalPlanConverter::convertLimit(
	const LogicalOperator *op) const {

	const auto *limit = static_cast<const LogicalLimit *>(op);
	auto childPhys = convert(limit->getChildren()[0]);
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
		                                    trav->getTargetVar(), trav->getEdgeLabel(),
		                                    trav->getDirection());

	// Add target label filter if specified
	if (!trav->getTargetLabels().empty()) {
		std::vector<int64_t> labelIds;
		labelIds.reserve(trav->getTargetLabels().size());
		for (const auto &lbl : trav->getTargetLabels()) {
			labelIds.push_back(dm_->getOrCreateLabelId(lbl));
		}
		std::string targetVar = trav->getTargetVar();
		auto predicate = [targetVar, labelIds](const Record &r) -> bool {
			auto n = r.getNode(targetVar);
			if (!n) return false;
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
			if (!n) return false;
			const auto &nodeProps = n->getProperties();
			for (const auto &[key, val] : props) {
				auto it = nodeProps.find(key);
				if (it == nodeProps.end() || it->second != val) return false;
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
			if (!e) return false;
			const auto &ep = e->getProperties();
			for (const auto &[key, val] : edgeProps) {
				auto it = ep.find(key);
				if (it == ep.end() || it->second != val) return false;
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
	std::unique_ptr<PhysicalOperator> root =
		std::make_unique<VarLengthTraversalOperator>(dm_, std::move(childPhys),
		                                             vlt->getSourceVar(), vlt->getTargetVar(),
		                                             vlt->getEdgeLabel(), vlt->getMinHops(),
		                                             vlt->getMaxHops(), vlt->getDirection());

	// Add target label filter if specified
	if (!vlt->getTargetLabels().empty()) {
		std::vector<int64_t> labelIds;
		labelIds.reserve(vlt->getTargetLabels().size());
		for (const auto &lbl : vlt->getTargetLabels()) {
			labelIds.push_back(dm_->getOrCreateLabelId(lbl));
		}
		std::string targetVar = vlt->getTargetVar();
		auto predicate = [targetVar, labelIds](const Record &r) -> bool {
			auto n = r.getNode(targetVar);
			if (!n) return false;
			for (int64_t lid : labelIds) {
				if (!n->hasLabelId(lid)) return false;
			}
			return true;
		};
		std::string desc = "TargetLabel(" + targetVar + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

	// Add target property filter if specified
	if (!vlt->getTargetProperties().empty()) {
		std::string targetVar = vlt->getTargetVar();
		auto props = vlt->getTargetProperties();
		auto predicate = [targetVar, props](const Record &r) -> bool {
			auto n = r.getNode(targetVar);
			if (!n) return false;
			const auto &nodeProps = n->getProperties();
			for (const auto &[key, val] : props) {
				auto it = nodeProps.find(key);
				if (it == nodeProps.end() || it->second != val) return false;
			}
			return true;
		};
		std::string desc = "TargetProps(" + targetVar + ")";
		root = std::make_unique<FilterOperator>(std::move(root), predicate, desc);
	}

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

	auto physOp = std::make_unique<CreateEdgeOperator>(dm_, ce->getVariable(), ce->getLabel(),
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
	                                                     me->getTargetVar(), me->getEdgeLabel(),
	                                                     me->getMatchProps(), me->getDirection(),
	                                                     convertActions(me->getOnCreateActions()),
	                                                     convertActions(me->getOnMatchActions()));

	// If the logical merge edge has a child (node resolution chain), convert it
	auto children = me->getChildren();
	if (!children.empty() && children[0]) {
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

	const planner::ProcedureContext ctx{dm_, im_};
	if (const auto factory = planner::ProcedureRegistry::instance().get(cp->getProcedureName())) {
		return factory(ctx, cp->getArgs());
	}

	throw std::runtime_error("PhysicalPlanConverter: unknown procedure: " + cp->getProcedureName());
}

} // namespace graph::query
