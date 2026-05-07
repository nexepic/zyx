/**
 * @file LogicalPlanBuilder.cpp
 * @brief Builds logical operator trees from analyzed QueryAST structures.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/ir/LogicalPlanBuilder.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/planner/PipelineValidator.hpp"
#include "graph/query/planner/ProcedureRegistry.hpp"
#include <algorithm>

namespace graph::query::ir {

using namespace graph::query::expressions;
using namespace graph::query::logical;

namespace {

/**
 * @brief Build the logical plan for a projection body with aggregates.
 *
 * Plan order: input -> Aggregate -> [Project] -> Sort -> Skip -> Limit
 */
std::unique_ptr<LogicalOperator> buildAggregateProjection(
	const ProjectionBody& body,
	std::unique_ptr<LogicalOperator> input)
{
	std::vector<LogicalAggItem> aggItems;
	std::vector<LogicalProjectItem> groupByProjItems;
	std::vector<LogicalProjectItem> postAggProjItems;
	size_t aggCounter = 0;

	for (const auto& item : body.items) {
		if (item.isTopLevelAggregate) {
			// Case 1: Top-level aggregate function (e.g., count(n), sum(n.age))
			auto* funcCall = static_cast<const FunctionCallExpression*>(item.expression.get());
			std::string funcName = funcCall->getFunctionName();
			std::string funcNameLower = funcName;
			std::transform(funcNameLower.begin(), funcNameLower.end(), funcNameLower.begin(),
			               [](unsigned char c) { return std::tolower(c); });

			std::shared_ptr<Expression> argExpr(nullptr);
			if (funcCall->getArgumentCount() > 0) {
				const auto& args = funcCall->getArguments();
				if (!args.empty()) {
					argExpr = std::shared_ptr<Expression>(args[0]->clone().release());
				}
			}

			aggItems.emplace_back(funcNameLower, argExpr, item.alias, funcCall->isDistinct());
		} else if (item.containsAggregate) {
			// Case 2: Expression containing nested aggregates (e.g., count(n) + 1)
			auto modifiedExpr = ExpressionUtils::extractAndReplaceAggregates(
				item.expression.get(), aggItems, aggCounter);
			auto modifiedShared = std::shared_ptr<Expression>(modifiedExpr.release());
			postAggProjItems.emplace_back(modifiedShared, item.alias);
		} else {
			// Case 3: Non-aggregate expression (group-by key)
			groupByProjItems.emplace_back(item.expression, item.alias);
		}
	}

	// Build group-by expressions and aliases
	std::vector<std::shared_ptr<Expression>> groupByExprs;
	std::vector<std::string> groupByAliases;
	for (const auto& pi : groupByProjItems) {
		groupByExprs.push_back(pi.expression);
		groupByAliases.push_back(pi.alias);
	}

	// Create Aggregate operator
	input = std::make_unique<LogicalAggregate>(
		std::move(input), std::move(groupByExprs), std::move(aggItems),
		std::move(groupByAliases));

	// If there are post-aggregate projections (nested aggregate expressions),
	// add a Project operator to compute them
	if (!postAggProjItems.empty()) {
		std::vector<LogicalProjectItem> fullProjItems;
		for (const auto& pi : groupByProjItems) {
			auto varRef = std::make_shared<VariableReferenceExpression>(pi.alias);
			fullProjItems.emplace_back(varRef, pi.alias);
		}
		for (auto& pi : postAggProjItems) {
			fullProjItems.push_back(std::move(pi));
		}
		input = std::make_unique<LogicalProject>(
			std::move(input), std::move(fullProjItems), body.distinct);
	}

	return input;
}

/**
 * @brief Build the logical plan for a projection body without aggregates.
 *
 * Plan order: input -> Sort -> Project -> Skip -> Limit
 */
std::unique_ptr<LogicalOperator> buildSimpleProjection(
	const ProjectionBody& body,
	std::unique_ptr<LogicalOperator> input)
{
	// Build sort items (sort goes BEFORE projection so it can access full record)
	std::vector<LogicalSortItem> sortItems;
	for (const auto& si : body.orderBy) {
		sortItems.emplace_back(si.expression, si.ascending);
	}

	if (!sortItems.empty()) {
		input = std::make_unique<LogicalSort>(std::move(input), std::move(sortItems));
	}

	// Build projection items
	std::vector<LogicalProjectItem> projItems;
	for (const auto& item : body.items) {
		projItems.emplace_back(item.expression, item.alias);
	}

	if (!projItems.empty()) {
		input = std::make_unique<LogicalProject>(
			std::move(input), std::move(projItems), body.distinct);
	}

	return input;
}

/**
 * @brief Append Sort, Skip, Limit to the plan (for aggregate path).
 */
std::unique_ptr<LogicalOperator> appendSortSkipLimit(
	const ProjectionBody& body,
	std::unique_ptr<LogicalOperator> input)
{
	// Sort AFTER Aggregate (sort expressions reference output aliases)
	if (!body.orderBy.empty()) {
		std::vector<LogicalSortItem> sortItems;
		for (const auto& si : body.orderBy) {
			sortItems.emplace_back(si.expression, si.ascending);
		}
		input = std::make_unique<LogicalSort>(std::move(input), std::move(sortItems));
	}

	if (body.skip.has_value()) {
		input = std::make_unique<LogicalSkip>(std::move(input), body.skip.value());
	}

	if (body.limit.has_value()) {
		input = std::make_unique<LogicalLimit>(std::move(input), body.limit.value());
	}

	return input;
}

} // anonymous namespace

// =============================================================================
// Constructor
// =============================================================================

LogicalPlanBuilder::LogicalPlanBuilder(const planner::ProcedureRegistry *registry)
	: procedureRegistry_(registry) {}

// =============================================================================
// buildReturn
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildReturn(
	const ProjectionBody& body,
	std::unique_ptr<LogicalOperator> input)
{
	// Inject LogicalSingleRow for standalone RETURN (e.g., RETURN 1+2)
	input = graph::query::PipelineValidator::ensureValidLogicalPipeline(
		std::move(input), "RETURN",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	if (body.returnStar) {
		// RETURN * — no projection/aggregation
		if (body.skip.has_value()) {
			input = std::make_unique<LogicalSkip>(std::move(input), body.skip.value());
		}
		if (body.limit.has_value()) {
			input = std::make_unique<LogicalLimit>(std::move(input), body.limit.value());
		}
		return input;
	}

	if (body.hasAggregates) {
		input = buildAggregateProjection(body, std::move(input));
		input = appendSortSkipLimit(body, std::move(input));
	} else {
		input = buildSimpleProjection(body, std::move(input));
		// Skip/Limit after Project for non-aggregate
		if (body.skip.has_value()) {
			input = std::make_unique<LogicalSkip>(std::move(input), body.skip.value());
		}
		if (body.limit.has_value()) {
			input = std::make_unique<LogicalLimit>(std::move(input), body.limit.value());
		}
	}

	return input;
}

// =============================================================================
// buildWith
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildWith(
	const WithClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	if (!input) {
		throw std::runtime_error("WITH clause must follow a MATCH or CREATE clause.");
	}

	const auto& body = clause.body;

	if (!body.returnStar) {
		if (body.hasAggregates) {
			input = buildAggregateProjection(body, std::move(input));
		} else if (!body.items.empty()) {
			// Non-aggregate WITH: just project
			std::vector<LogicalProjectItem> projItems;
			for (const auto& item : body.items) {
				projItems.emplace_back(item.expression, item.alias);
			}
			input = std::make_unique<LogicalProject>(
				std::move(input), std::move(projItems), body.distinct);
		}
	}

	// Handle WHERE clause in WITH
	if (clause.where) {
		input = std::make_unique<LogicalFilter>(std::move(input), clause.where);
	}

	return input;
}

// =============================================================================
// buildMatch
// =============================================================================

namespace {

/**
 * @brief Build logical operators for a single pattern element (MATCH).
 */
/**
 * @brief Build comparison expression filters from propertyExpressions.
 *
 * For each entry (key, expr), creates: variable.key = expr
 * These are used for runtime parameter resolution in MATCH inline properties.
 */
void addPropertyExpressionFilters(
	std::unique_ptr<LogicalOperator>& rootOp,
	const std::string& variable,
	const std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>>& propExprs)
{
	using namespace graph::query::expressions;
	for (const auto& [key, expr] : propExprs) {
		auto lhs = std::make_unique<VariableReferenceExpression>(variable, key);
		auto rhs = expr->clone();
		auto cmp = std::make_unique<BinaryOpExpression>(
			std::move(lhs),
			BinaryOperatorType::BOP_EQUAL,
			std::move(rhs));
		auto filter = std::shared_ptr<Expression>(cmp.release());
		rootOp = std::make_unique<LogicalFilter>(std::move(rootOp), filter);
	}
}

std::unique_ptr<LogicalOperator> buildMatchElement(
	const PatternElementAST& element,
	std::unique_ptr<LogicalOperator> rootOp)
{
	// Head Node → LogicalNodeScan
	auto currentOp = std::make_unique<LogicalNodeScan>(
		element.headNode.variable,
		element.headNode.labels,
		element.headNode.properties);

	if (!rootOp) {
		rootOp = std::move(currentOp);
	} else {
		rootOp = std::make_unique<LogicalJoin>(std::move(rootOp), std::move(currentOp));
	}

	// Add expression-based filters for parameterized head node properties
	addPropertyExpressionFilters(rootOp, element.headNode.variable, element.headNode.propertyExpressions);

	// Traversal chains
	std::string var = element.headNode.variable;
	for (const auto& chain : element.chain) {
		const auto& rel = chain.relationship;
		const auto& target = chain.targetNode;

		std::vector<std::pair<std::string, PropertyValue>> targetProps;
		for (const auto& [k, v] : target.properties) {
			targetProps.emplace_back(k, v);
		}

		if (rel.isVarLength) {
			rootOp = std::make_unique<LogicalVarLengthTraversal>(
				std::move(rootOp), var, rel.variable, target.variable,
				rel.type, rel.direction, rel.minHops, rel.maxHops,
				target.labels, targetProps);
		} else {
			rootOp = std::make_unique<LogicalTraversal>(
				std::move(rootOp), var, rel.variable, target.variable,
				rel.type, rel.direction, target.labels, targetProps,
				rel.properties);
		}

		// Add expression-based filters for parameterized target node properties
		addPropertyExpressionFilters(rootOp, target.variable, target.propertyExpressions);

		// Add expression-based filters for parameterized edge properties
		if (!rel.variable.empty()) {
			addPropertyExpressionFilters(rootOp, rel.variable, rel.propertyExpressions);
		}

		var = target.variable;
	}

	return rootOp;
}

/**
 * @brief Collect node and edge variable names from a pattern element.
 */
void collectPatternVariables(const PatternElementAST& element, std::vector<std::string>& vars) {
	if (!element.headNode.variable.empty()) {
		if (std::find(vars.begin(), vars.end(), element.headNode.variable) == vars.end()) {
			vars.push_back(element.headNode.variable);
		}
	}
	for (const auto& chain : element.chain) {
		if (!chain.relationship.variable.empty()) {
			if (std::find(vars.begin(), vars.end(), chain.relationship.variable) == vars.end()) {
				vars.push_back(chain.relationship.variable);
			}
		}
		if (!chain.targetNode.variable.empty()) {
			if (std::find(vars.begin(), vars.end(), chain.targetNode.variable) == vars.end()) {
				vars.push_back(chain.targetNode.variable);
			}
		}
	}
}

} // anonymous namespace

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildMatch(
	const MatchClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	if (clause.patterns.empty())
		return input;

	// OPTIONAL MATCH with existing pipeline
	if (clause.optional && input) {
		std::unique_ptr<LogicalOperator> patternOp = nullptr;
		std::vector<std::string> requiredVariables;

		for (const auto& part : clause.patterns) {
			collectPatternVariables(part.element, requiredVariables);
			patternOp = buildMatchElement(part.element, std::move(patternOp));
		}

		// Apply WHERE
		if (clause.where) {
			patternOp = std::make_unique<LogicalFilter>(std::move(patternOp), clause.where);
		}

		return std::make_unique<LogicalOptionalMatch>(
			std::move(input), std::move(patternOp), requiredVariables);
	}

	// Regular MATCH
	for (const auto& part : clause.patterns) {
		if (!input) {
			input = buildMatchElement(part.element, nullptr);
		} else if (clause.patterns.size() == 1) {
			input = buildMatchElement(part.element, std::move(input));
		} else {
			auto partOp = buildMatchElement(part.element, nullptr);
			input = std::make_unique<LogicalJoin>(std::move(input), std::move(partOp));
		}

		// Named path: p = (a)-[r]->(b)
		if (!part.pathVariable.empty()) {
			std::vector<std::string> nodeVars;
			std::vector<std::string> edgeVars;

			if (!part.element.headNode.variable.empty()) {
				nodeVars.push_back(part.element.headNode.variable);
			}
			for (const auto& chain : part.element.chain) {
				if (!chain.relationship.variable.empty()) {
					edgeVars.push_back(chain.relationship.variable);
				}
				if (!chain.targetNode.variable.empty()) {
					nodeVars.push_back(chain.targetNode.variable);
				}
			}

			input = std::make_unique<LogicalNamedPath>(
				std::move(input), part.pathVariable, nodeVars, edgeVars);
		}
	}

	// Apply WHERE
	if (clause.where) {
		input = std::make_unique<LogicalFilter>(std::move(input), clause.where);
	}

	return input;
}

// =============================================================================
// buildUnwind
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildUnwind(
	const UnwindClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	input = graph::query::PipelineValidator::ensureValidLogicalPipeline(
		std::move(input), "UNWIND",
		graph::query::PipelineValidator::ValidationMode::ALLOW_EMPTY);

	if (!clause.literalList.empty()) {
		return std::make_unique<LogicalUnwind>(
			std::move(input), clause.alias, clause.literalList);
	} else {
		return std::make_unique<LogicalUnwind>(
			std::move(input), clause.alias, clause.expression);
	}
}

// =============================================================================
// buildCall
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildCall(
	const CallClause& clause,
	std::unique_ptr<LogicalOperator> /*input*/)
{
	// Look up procedure mutation flags from registry
	if (procedureRegistry_) {
		if (auto *desc = procedureRegistry_->getDescriptor(clause.procedureName)) {
			if (desc->mutatesData) mutatesData_ = true;
			if (desc->mutatesSchema) mutatesSchema_ = true;
		}
	}

	std::unique_ptr<LogicalOperator> result =
		std::make_unique<LogicalCallProcedure>(clause.procedureName, clause.arguments);

	// YIELD items → Project
	if (!clause.yieldItems.empty()) {
		std::vector<LogicalProjectItem> projItems;
		for (const auto& yi : clause.yieldItems) {
			auto expr = std::make_shared<VariableReferenceExpression>(yi.sourceField);
			projItems.emplace_back(expr, yi.alias);
		}
		result = std::make_unique<LogicalProject>(
			std::move(result), std::move(projItems));
	}

	return result;
}

// =============================================================================
// buildCreate
// =============================================================================

namespace {

/**
 * @brief Chain a write operator with existing pipeline.
 * Write operators (CreateNode, CreateEdge) wrap root as child.
 */
std::unique_ptr<LogicalOperator> chainWriteOp(
	std::unique_ptr<LogicalOperator> rootOp,
	std::unique_ptr<LogicalOperator> newOp)
{
	if (!rootOp) {
		return newOp;
	}
	// Write operators: set existing root as child
	if (auto* edgeOp = dynamic_cast<LogicalCreateEdge*>(newOp.get())) {
		edgeOp->setChild(0, std::move(rootOp));
		return newOp;
	}
	if (auto* nodeOp = dynamic_cast<LogicalCreateNode*>(newOp.get())) {
		nodeOp->setChild(0, std::move(rootOp));
		return newOp;
	}
	return newOp;
}

} // anonymous namespace

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildCreate(
	const CreateClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	mutatesData_ = true;
	// Counter for generating unique anonymous variable names.
	// Anonymous nodes (no user-specified variable) all get variable == "".
	// Without unique names, chained CreateNodeOperators would collide:
	// the outer operator finds the inner's node via record.getNode("")
	// and skips creation. E.g. CREATE (:A), (:B) would only create 1 node.
	int anonCounter = 0;

	for (const auto& part : clause.patterns) {
		const auto& elem = part.element;

		// Head node — assign unique anonymous variable if empty
		std::string headVar = elem.headNode.variable;
		if (headVar.empty()) {
			headVar = query::ANONYMOUS_VAR_PREFIX + std::to_string(anonCounter++);
		}

		std::unordered_map<std::string, PropertyValue> headProps;
		for (const auto& [k, v] : elem.headNode.properties) {
			headProps[k] = v;
		}
		auto headOp = std::make_unique<LogicalCreateNode>(
			headVar, elem.headNode.labels,
			headProps, elem.headNode.propertyExpressions);
		input = chainWriteOp(std::move(input), std::move(headOp));

		std::string prevVar = headVar;

		for (const auto& chain : elem.chain) {
			// Target node — assign unique anonymous variable if empty
			std::string targetVar = chain.targetNode.variable;
			if (targetVar.empty()) {
				targetVar = query::ANONYMOUS_VAR_PREFIX + std::to_string(anonCounter++);
			}

			std::unordered_map<std::string, PropertyValue> targetProps;
			for (const auto& [k, v] : chain.targetNode.properties) {
				targetProps[k] = v;
			}
			std::unordered_map<std::string, std::shared_ptr<Expression>> emptyPropExprs;
			auto targetOp = std::make_unique<LogicalCreateNode>(
				targetVar, chain.targetNode.labels,
				targetProps, emptyPropExprs);
			input = chainWriteOp(std::move(input), std::move(targetOp));

			// Edge — assign unique anonymous variable if empty
			std::string edgeVar = chain.relationship.variable;
			if (edgeVar.empty()) {
				edgeVar = query::ANONYMOUS_VAR_PREFIX + std::to_string(anonCounter++);
			}

			auto edgeOp = std::make_unique<LogicalCreateEdge>(
				edgeVar, chain.relationship.type,
				chain.relationship.properties, prevVar, targetVar);
			input = chainWriteOp(std::move(input), std::move(edgeOp));

			prevVar = targetVar;
		}
	}

	return input;
}

// =============================================================================
// buildSet
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildSet(
	const SetClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	mutatesData_ = true;
	if (!input) {
		throw std::runtime_error("SET clause must follow a MATCH or CREATE clause.");
	}

	std::vector<LogicalSetItem> logicalItems;
	logicalItems.reserve(clause.items.size());
	for (const auto& item : clause.items) {
		SetActionType logType;
		switch (item.type) {
			case SetItemType::SIT_PROPERTY:
				logType = SetActionType::LSET_PROPERTY;
				break;
			case SetItemType::SIT_LABEL:
				logType = SetActionType::LSET_LABEL;
				break;
			case SetItemType::SIT_MAP_MERGE:
				logType = SetActionType::LSET_MAP_MERGE;
				break;
			default:
				logType = SetActionType::LSET_PROPERTY;
				break;
		}
		logicalItems.push_back({logType, item.variable, item.key, item.expression});
	}

	return std::make_unique<LogicalSet>(std::move(logicalItems), std::move(input));
}

// =============================================================================
// buildDelete
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildDelete(
	const DeleteClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	mutatesData_ = true;
	if (!input) {
		throw std::runtime_error("DELETE clause must follow a MATCH or CREATE clause.");
	}

	return std::make_unique<LogicalDelete>(clause.variables, clause.detach, std::move(input));
}

// =============================================================================
// buildRemove
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildRemove(
	const RemoveClause& clause,
	std::unique_ptr<LogicalOperator> input)
{
	mutatesData_ = true;
	if (!input) {
		throw std::runtime_error("REMOVE clause must follow a MATCH or CREATE clause.");
	}

	std::vector<LogicalRemoveItem> logicalItems;
	for (const auto& item : clause.items) {
		LogicalRemoveActionType logType = (item.type == RemoveItemType::RIT_PROPERTY)
			? LogicalRemoveActionType::LREM_PROPERTY
			: LogicalRemoveActionType::LREM_LABEL;
		logicalItems.push_back({logType, item.variable, item.key});
	}

	return std::make_unique<LogicalRemove>(std::move(logicalItems), std::move(input));
}

// =============================================================================
// buildMerge
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildMerge(
	const MergeClause& clause,
	std::unique_ptr<LogicalOperator> /*input*/)
{
	mutatesData_ = true;
	const auto& elem = clause.pattern.element;

	// Convert SetItemAST → MergeSetAction
	auto convertToMergeActions = [](const std::vector<SetItemAST>& items) {
		std::vector<MergeSetAction> actions;
		actions.reserve(items.size());
		for (const auto& item : items) {
			actions.push_back({item.variable, item.key, item.expression});
		}
		return actions;
	};

	auto onCreateActions = convertToMergeActions(clause.onCreateItems);
	auto onMatchActions = convertToMergeActions(clause.onMatchItems);

	// Head node properties
	std::unordered_map<std::string, PropertyValue> headProps;
	for (const auto& [k, v] : elem.headNode.properties) {
		headProps[k] = v;
	}

	// Single node MERGE
	if (elem.chain.empty()) {
		return std::make_unique<LogicalMergeNode>(
			elem.headNode.variable, elem.headNode.labels, headProps,
			std::move(onCreateActions), std::move(onMatchActions));
	}

	// Edge MERGE
	std::vector<MergeSetAction> emptyActions;

	std::unique_ptr<LogicalOperator> rootOp =
		std::make_unique<LogicalMergeNode>(
			elem.headNode.variable, elem.headNode.labels, headProps,
			emptyActions, emptyActions);

	std::string prevVar = elem.headNode.variable;

	for (size_t i = 0; i < elem.chain.size(); ++i) {
		const auto& chain = elem.chain[i];
		const auto& target = chain.targetNode;
		const auto& rel = chain.relationship;

		std::unordered_map<std::string, PropertyValue> targetProps;
		for (const auto& [k, v] : target.properties) {
			targetProps[k] = v;
		}

		auto targetMergeOp = std::make_unique<LogicalMergeNode>(
			target.variable, target.labels, targetProps,
			emptyActions, emptyActions, std::move(rootOp));
		rootOp = std::move(targetMergeOp);

		bool isLastChain = (i == elem.chain.size() - 1);
		rootOp = std::make_unique<LogicalMergeEdge>(
			prevVar, rel.variable, target.variable, rel.type, rel.direction,
			rel.properties,
			isLastChain ? std::move(onCreateActions) : emptyActions,
			isLastChain ? std::move(onMatchActions) : emptyActions,
			std::move(rootOp));

		prevVar = target.variable;
	}

	return rootOp;
}

// =============================================================================
// Admin Clauses
// =============================================================================

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildShowIndexes(
	const ShowIndexesClause& /*clause*/)
{
	return std::make_unique<LogicalShowIndexes>();
}

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildCreateIndex(
	const CreateIndexClause& clause)
{
	mutatesSchema_ = true;
	if (clause.properties.size() == 1) {
		return std::make_unique<LogicalCreateIndex>(clause.name, clause.label, clause.properties[0]);
	}
	return std::make_unique<LogicalCreateIndex>(clause.name, clause.label, clause.properties);
}

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildDropIndex(
	const DropIndexClause& clause)
{
	mutatesSchema_ = true;
	return std::make_unique<LogicalDropIndex>(clause.name, clause.label, clause.property);
}

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildCreateVectorIndex(
	const CreateVectorIndexClause& clause)
{
	mutatesSchema_ = true;
	return std::make_unique<LogicalCreateVectorIndex>(
		clause.name, clause.label, clause.property, clause.dimension, clause.metric);
}

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildCreateConstraint(
	const CreateConstraintClause& clause)
{
	mutatesSchema_ = true;
	return std::make_unique<LogicalCreateConstraint>(
		clause.name, clause.targetType, clause.constraintType,
		clause.label, clause.properties, clause.typeName);
}

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildDropConstraint(
	const DropConstraintClause& clause)
{
	mutatesSchema_ = true;
	return std::make_unique<LogicalDropConstraint>(clause.name, clause.ifExists);
}

std::unique_ptr<LogicalOperator> LogicalPlanBuilder::buildShowConstraints(
	const ShowConstraintsClause& /*clause*/)
{
	return std::make_unique<LogicalShowConstraints>();
}

} // namespace graph::query::ir
