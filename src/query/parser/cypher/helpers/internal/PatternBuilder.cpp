/**
 * @file PatternBuilder.cpp
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

#include <functional>
#include <iostream>
#include "../PatternBuilder.hpp"
#include "../AstExtractor.hpp"
#include "../ExpressionBuilder.hpp"
#include "../OperatorChain.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"

namespace graph::parser::cypher::helpers {

std::unique_ptr<query::logical::LogicalOperator> PatternBuilder::buildMatchPattern(
	CypherParser::PatternContext *pattern,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/,
	CypherParser::WhereContext *where,
	bool isOptional) {

	if (!pattern)
		return rootOp;

	auto parts = pattern->patternPart();
	if (parts.empty())
		return rootOp;

	// For OPTIONAL MATCH, we need to build the pattern operator separately
	// and then wrap it in LogicalOptionalMatch
	if (isOptional && rootOp) {
		// Build the pattern operator starting from nullptr (independent of input)
		std::unique_ptr<query::logical::LogicalOperator> patternOp = nullptr;

		// Collect required variables and build pattern operator
		std::vector<std::string> requiredVariables;

		for (auto part : parts) {
			auto element = part->patternElement();

			// Extract variables from this pattern element
			collectVariablesFromPatternElement(element, requiredVariables);

			// Build operator for this element
			patternOp = processMatchPatternElement(element, std::move(patternOp));
		}

		// Apply WHERE clause if present
		patternOp = applyWhereFilter(std::move(patternOp), where);

		// Wrap in LogicalOptionalMatch
		return std::make_unique<query::logical::LogicalOptionalMatch>(
			std::move(rootOp), std::move(patternOp), requiredVariables);
	}

	// Regular MATCH or standalone OPTIONAL MATCH (no input)
	// Iterate over all pattern parts (e.g. MATCH (a), (b))
	for (auto part : parts) {
		auto element = part->patternElement();

		if (!rootOp) {
			// First component or no prior pipeline: build from scratch
			rootOp = processMatchPatternElement(element, nullptr);
		} else if (parts.size() == 1) {
			// Single pattern part with existing pipeline: chain directly
			rootOp = processMatchPatternElement(element, std::move(rootOp));
		} else {
			// Multiple pattern parts with existing pipeline:
			// Build this part independently, then Join with root
			auto partOp = processMatchPatternElement(element, nullptr);
			rootOp = std::make_unique<query::logical::LogicalJoin>(
				std::move(rootOp), std::move(partOp));
		}
	}

	// Apply WHERE clause if present
	rootOp = applyWhereFilter(std::move(rootOp), where);

	return rootOp;
}

std::unique_ptr<query::logical::LogicalOperator> PatternBuilder::processMatchPatternElement(
	CypherParser::PatternElementContext *element,
	std::unique_ptr<query::logical::LogicalOperator> rootOp) {

	// 1. Head Node Processing (Scan)
	auto headNodePat = element->nodePattern();

	std::string var = AstExtractor::extractVariable(headNodePat->variable());
	auto labels = AstExtractor::extractLabels(headNodePat->nodeLabels());

	// Extract properties using AST-based evaluation
	auto props = AstExtractor::extractProperties(headNodePat->properties(),
		[](CypherParser::ExpressionContext *expr) {
			return ExpressionBuilder::evaluateLiteralExpression(expr);
		});

	// Build property predicates vector for LogicalNodeScan
	std::vector<std::pair<std::string, PropertyValue>> propertyPredicates;
	for (const auto &[key, val] : props) {
		propertyPredicates.emplace_back(key, val);
	}

	auto currentOp = std::make_unique<query::logical::LogicalNodeScan>(var, labels, propertyPredicates);

	// If this is the first component, set it as root
	if (!rootOp) {
		rootOp = std::move(currentOp);
	} else {
		// Cross join with existing root
		rootOp = std::make_unique<query::logical::LogicalJoin>(
			std::move(rootOp), std::move(currentOp));
	}

	// 2. Traversal Chain (e.g. -[r]->(b))
	for (auto chain : element->patternElementChain()) {
		auto relPat = chain->relationshipPattern();
		auto nodePat = chain->nodePattern();

		// Direction
		std::string direction = "both";
		bool hasLeft = (relPat->LT() != nullptr);
		bool hasRight = (relPat->GT() != nullptr);
		if (hasLeft && !hasRight)
			direction = "in";
		else if (!hasLeft && hasRight)
			direction = "out";

		// Rel Details
		auto relDetail = relPat->relationshipDetail();
		std::string edgeVar = "", edgeLabel = "";
		std::unordered_map<std::string, PropertyValue> edgeProps;

		bool isVarLength = false;
		int minHops = 1;
		int maxHops = 1;

		if (relDetail) {
			edgeVar = AstExtractor::extractVariable(relDetail->variable());
			edgeLabel = AstExtractor::extractRelType(relDetail->relationshipTypes());
			edgeProps = AstExtractor::extractProperties(relDetail->properties(),
				[](CypherParser::ExpressionContext *expr) {
					return ExpressionBuilder::evaluateLiteralExpression(expr);
				});

			// Variable Length Logic
			if (relDetail->rangeLiteral()) {
				isVarLength = true;
				auto range = relDetail->rangeLiteral();
				minHops = 1;
				maxHops = 15; // Defaults

				auto ints = range->integerLiteral();

				if (range->RANGE() != nullptr) {
					if (ints.size() == 2) { // *1..5
						minHops = std::stoi(ints[0]->getText());
						maxHops = std::stoi(ints[1]->getText());
					} else if (ints.size() == 1) {
						std::string rawText = range->getText();
						if (rawText.find("..") < rawText.find(ints[0]->getText())) {
							// ..5
							maxHops = std::stoi(ints[0]->getText());
						} else {
							// 1..
							minHops = std::stoi(ints[0]->getText());
						}
					}
				} else if (!ints.empty()) {
					// *3
					minHops = maxHops = std::stoi(ints[0]->getText());
				}
			}
		}

		// Target Node
		std::string targetVar = AstExtractor::extractVariable(nodePat->variable());
		auto targetLabels = AstExtractor::extractLabels(nodePat->nodeLabels());
		auto targetPropsMap = AstExtractor::extractProperties(nodePat->properties(),
			[](CypherParser::ExpressionContext *expr) {
				return ExpressionBuilder::evaluateLiteralExpression(expr);
			});

		// Convert target properties map to vector of pairs
		std::vector<std::pair<std::string, PropertyValue>> targetProps;
		for (const auto &[k, v] : targetPropsMap) {
			targetProps.emplace_back(k, v);
		}

		// Build Traversal with target/edge filter data stored on the operator
		if (isVarLength) {
			rootOp = std::make_unique<query::logical::LogicalVarLengthTraversal>(
				std::move(rootOp), var, edgeVar, targetVar, edgeLabel, direction,
				minHops, maxHops, targetLabels, targetProps);
		} else {
			rootOp = std::make_unique<query::logical::LogicalTraversal>(
				std::move(rootOp), var, edgeVar, targetVar, edgeLabel, direction,
				targetLabels, targetProps, edgeProps);
		}

		// Update current variable for next hop
		var = targetVar;
	}

	return rootOp;
}

std::unique_ptr<query::logical::LogicalOperator> PatternBuilder::applyWhereFilter(
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	CypherParser::WhereContext *where) {

	if (!where)
		return rootOp;

	try {
		auto ast = ExpressionBuilder::buildExpression(where->expression());
		auto astShared = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
		return std::make_unique<query::logical::LogicalFilter>(std::move(rootOp), astShared);
	} catch (const std::exception &e) {
		std::cerr << "Warning: Failed to parse WHERE clause: " << e.what() << std::endl;
		throw;
	}
}

std::unique_ptr<query::logical::LogicalOperator> PatternBuilder::buildCreatePattern(
	CypherParser::PatternContext *pattern,
	std::unique_ptr<query::logical::LogicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	if (!pattern)
		return rootOp;

	for (auto part : pattern->patternPart()) {
		processCreatePatternElement(part->patternElement(), rootOp);
	}

	return rootOp;
}

void PatternBuilder::processCreatePatternElement(
	CypherParser::PatternElementContext *element,
	std::unique_ptr<query::logical::LogicalOperator> &rootOp) {

	auto headNodePat = element->nodePattern();

	std::string headVar = AstExtractor::extractVariable(headNodePat->variable());
	auto headLabels = AstExtractor::extractLabels(headNodePat->nodeLabels());

	// Extract properties: split into static literals and expression-based
	std::unordered_map<std::string, PropertyValue> headProps;
	std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> headPropExprs;

	if (headNodePat->properties() && headNodePat->properties()->mapLiteral()) {
		auto mapLit = headNodePat->properties()->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();
		for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
			std::string key = keys[i]->getText();
			PropertyValue litVal = ExpressionBuilder::evaluateLiteralExpression(exprs[i]);
			if (litVal.getType() != PropertyType::NULL_TYPE || exprs[i]->getText() == "null") {
				headProps[key] = litVal;
			} else {
				// Non-literal value (variable reference) — build expression AST
				auto exprAST = ExpressionBuilder::buildExpression(exprs[i]);
				headPropExprs[key] = std::shared_ptr<graph::query::expressions::Expression>(exprAST.release());
			}
		}
	}

	auto headOp = std::make_unique<query::logical::LogicalCreateNode>(
		headVar, headLabels, headProps, headPropExprs);
	rootOp = OperatorChain::chain(std::move(rootOp), std::move(headOp));

	std::string prevVar = headVar;

	for (auto chain : element->patternElementChain()) {
		auto relPat = chain->relationshipPattern();
		auto targetNodePat = chain->nodePattern();

		std::string targetVar = AstExtractor::extractVariable(targetNodePat->variable());
		auto targetLabels = AstExtractor::extractLabels(targetNodePat->nodeLabels());
		auto targetProps = AstExtractor::extractProperties(targetNodePat->properties(),
			[](CypherParser::ExpressionContext *expr) {
				return ExpressionBuilder::evaluateLiteralExpression(expr);
			});

		std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> emptyPropExprs;
		auto targetOp = std::make_unique<query::logical::LogicalCreateNode>(
			targetVar, targetLabels, targetProps, emptyPropExprs);
		rootOp = OperatorChain::chain(std::move(rootOp), std::move(targetOp));

		auto relDetail = relPat->relationshipDetail();
		std::string edgeVar = "", edgeLabel = "";
		std::unordered_map<std::string, graph::PropertyValue> edgeProps;

		if (relDetail) {
			edgeVar = AstExtractor::extractVariable(relDetail->variable());
			edgeLabel = AstExtractor::extractRelType(relDetail->relationshipTypes());
			edgeProps = AstExtractor::extractProperties(relDetail->properties(),
				[](CypherParser::ExpressionContext *expr) {
					return ExpressionBuilder::evaluateLiteralExpression(expr);
				});
		}

		auto edgeOp = std::make_unique<query::logical::LogicalCreateEdge>(
			edgeVar, edgeLabel, edgeProps, prevVar, targetVar);
		rootOp = OperatorChain::chain(std::move(rootOp), std::move(edgeOp));

		prevVar = targetVar;
	}
}

std::unique_ptr<query::logical::LogicalOperator> PatternBuilder::buildMergePattern(
	CypherParser::PatternPartContext *patternPart,
	const std::vector<query::execution::operators::SetItem> &onCreateItems,
	const std::vector<query::execution::operators::SetItem> &onMatchItems,
	const std::shared_ptr<query::QueryPlanner> & /*planner*/) {

	if (!patternPart)
		return nullptr;

	auto element = patternPart->patternElement();
	auto headNodePat = element->nodePattern();

	std::string headVar = AstExtractor::extractVariable(headNodePat->variable());
	auto headLabels = AstExtractor::extractLabels(headNodePat->nodeLabels());
	auto headProps = AstExtractor::extractProperties(headNodePat->properties(),
		[](CypherParser::ExpressionContext *expr) {
			return ExpressionBuilder::evaluateLiteralExpression(expr);
		});

	// Convert SetItem vectors to MergeSetAction vectors
	auto convertToMergeActions = [](const std::vector<query::execution::operators::SetItem> &items) {
		std::vector<query::logical::MergeSetAction> actions;
		actions.reserve(items.size());
		for (const auto &item : items) {
			actions.push_back({item.variable, item.key, item.expression});
		}
		return actions;
	};

	auto onCreateActions = convertToMergeActions(onCreateItems);
	auto onMatchActions = convertToMergeActions(onMatchItems);

	// Single node MERGE: MERGE (n:Label {props})
	if (element->patternElementChain().empty()) {
		return std::make_unique<query::logical::LogicalMergeNode>(
			headVar, headLabels, headProps,
			std::move(onCreateActions), std::move(onMatchActions));
	}

	// Edge MERGE: MERGE (a)-[r:TYPE {props}]->(b)
	// Strategy: First merge source node, then merge target node, then merge edge
	std::vector<query::logical::MergeSetAction> emptyActions;

	// Merge head node (no ON CREATE/MATCH items for intermediate nodes)
	std::unique_ptr<query::logical::LogicalOperator> rootOp =
		std::make_unique<query::logical::LogicalMergeNode>(
			headVar, headLabels, headProps, emptyActions, emptyActions);

	std::string prevVar = headVar;
	auto chains = element->patternElementChain();

	for (size_t i = 0; i < chains.size(); ++i) {
		auto chain = chains[i];
		auto relPat = chain->relationshipPattern();
		auto targetNodePat = chain->nodePattern();

		// Target node
		std::string targetVar = AstExtractor::extractVariable(targetNodePat->variable());
		auto targetLabels = AstExtractor::extractLabels(targetNodePat->nodeLabels());
		auto targetProps = AstExtractor::extractProperties(targetNodePat->properties(),
			[](CypherParser::ExpressionContext *expr) {
				return ExpressionBuilder::evaluateLiteralExpression(expr);
			});

		// Merge target node (set previous merge as child)
		auto targetMergeOp = std::make_unique<query::logical::LogicalMergeNode>(
			targetVar, targetLabels, targetProps, emptyActions, emptyActions,
			std::move(rootOp));
		rootOp = std::move(targetMergeOp);

		// Edge details
		auto relDetail = relPat->relationshipDetail();
		std::string edgeVar, edgeLabel;
		std::unordered_map<std::string, PropertyValue> edgeProps;

		if (relDetail) {
			edgeVar = AstExtractor::extractVariable(relDetail->variable());
			edgeLabel = AstExtractor::extractRelType(relDetail->relationshipTypes());
			edgeProps = AstExtractor::extractProperties(relDetail->properties(),
				[](CypherParser::ExpressionContext *expr) {
					return ExpressionBuilder::evaluateLiteralExpression(expr);
				});
		}

		// Direction
		std::string direction = "out";
		bool hasLeft = (relPat->LT() != nullptr);
		bool hasRight = (relPat->GT() != nullptr);
		if (hasLeft && !hasRight)
			direction = "in";
		else if (!hasLeft && hasRight)
			direction = "out";
		else
			direction = "both";

		// Apply ON CREATE/MATCH items only to the last edge in the chain
		bool isLastChain = (i == chains.size() - 1);
		rootOp = std::make_unique<query::logical::LogicalMergeEdge>(
			prevVar, edgeVar, targetVar, edgeLabel, direction, edgeProps,
			isLastChain ? std::move(onCreateActions) : emptyActions,
			isLastChain ? std::move(onMatchActions) : emptyActions,
			std::move(rootOp));

		prevVar = targetVar;
	}

	return rootOp;
}

std::vector<query::execution::operators::SetItem> PatternBuilder::extractSetItems(
	CypherParser::SetStatementContext *ctx) {

	std::vector<query::execution::operators::SetItem> items;
	if (!ctx)
		return items;

	for (auto item : ctx->setItem()) {
		// Case 1: Property Update (n.prop = expr)
		if (item->propertyExpression() && item->EQ()) {
			auto propExpr = item->propertyExpression();

			// 1. Extract Variable
			std::string varName = "";
			if (propExpr->atom() && propExpr->atom()->variable()) {
				varName = AstExtractor::extractVariable(propExpr->atom()->variable());
			}

			// 2. Extract Key
			std::string keyName = AstExtractor::extractPropertyKeyFromExpr(propExpr);

			// 3. Build Expression AST from the right-hand side
			auto expressionAST = ExpressionBuilder::buildExpression(item->expression());

			// Convert to shared_ptr for storage in SetItem
			auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

			items.emplace_back(
				query::execution::operators::SetActionType::PROPERTY,
				varName,
				keyName,
				expressionShared
			);
		}
		// Case 2: Map Merge (n += {key: value, ...})
		else if (item->variable() && item->PLUS() && item->EQ() && item->expression()) {
			std::string varName = AstExtractor::extractVariable(item->variable());

			// Find mapLiteral by traversing the parse tree
			CypherParser::MapLiteralContext* mapLit = nullptr;
			std::function<void(antlr4::tree::ParseTree*)> findMap = [&](antlr4::tree::ParseTree* tree) {
				if (!tree || mapLit) return;
				if (auto* m = dynamic_cast<CypherParser::MapLiteralContext*>(tree)) {
					mapLit = m;
					return;
				}
				for (size_t i = 0; i < tree->children.size(); ++i) {
					findMap(tree->children[i]);
				}
			};
			findMap(item->expression());

			if (mapLit) {
				// Extract key-value pairs from map literal and create individual PROPERTY items
				auto keys = mapLit->propertyKeyName();
				auto exprs = mapLit->expression();
				for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
					std::string keyName = keys[i]->getText();
					auto expressionAST = ExpressionBuilder::buildExpression(exprs[i]);
					auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

					items.emplace_back(
						query::execution::operators::SetActionType::PROPERTY,
						varName,
						keyName,
						expressionShared
					);
				}
			} else {
				// Non-map expression - build as MAP_MERGE for runtime evaluation
				auto expressionAST = ExpressionBuilder::buildExpression(item->expression());
				auto expressionShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

				items.emplace_back(
					query::execution::operators::SetActionType::MAP_MERGE,
					varName,
					"",
					expressionShared
				);
			}
		}
		// Case 3: Label Assignment (n:Label)
		else if (item->variable() && item->nodeLabels()) {
			std::string varName = AstExtractor::extractVariable(item->variable());
			std::string labelName = AstExtractor::extractLabel(item->nodeLabels());

			items.emplace_back(
				query::execution::operators::SetActionType::LABEL,
				varName,
				labelName,
				nullptr // No expression for labels
			);
		}
	}

	return items;
}

void PatternBuilder::collectVariablesFromPatternElement(
	CypherParser::PatternElementContext *element,
	std::vector<std::string> &variables) {

	if (!element)
		return;

	// Extract variable from head node
	auto headNodePat = element->nodePattern();
	std::string headVar = AstExtractor::extractVariable(headNodePat->variable());
	if (!headVar.empty()) {
		// Avoid duplicates
		if (std::find(variables.begin(), variables.end(), headVar) == variables.end()) {
			variables.push_back(headVar);
		}
	}

	// Extract variables from traversal chains
	for (auto chain : element->patternElementChain()) {
		auto relPat = chain->relationshipPattern();
		auto nodePat = chain->nodePattern();

		// Extract edge variable
		auto relDetail = relPat->relationshipDetail();
		if (relDetail) {
			std::string edgeVar = AstExtractor::extractVariable(relDetail->variable());
			if (!edgeVar.empty()) {
				if (std::find(variables.begin(), variables.end(), edgeVar) == variables.end()) {
					variables.push_back(edgeVar);
				}
			}
		}

		// Extract target node variable
		std::string targetVar = AstExtractor::extractVariable(nodePat->variable());
		if (!targetVar.empty()) {
			if (std::find(variables.begin(), variables.end(), targetVar) == variables.end()) {
				variables.push_back(targetVar);
			}
		}
	}
}

} // namespace graph::parser::cypher::helpers
