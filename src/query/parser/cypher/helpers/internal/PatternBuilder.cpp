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

#include "../PatternBuilder.hpp"
#include "../AstExtractor.hpp"
#include "../ExpressionBuilder.hpp"
#include "../OperatorChain.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::parser::cypher::helpers {

std::unique_ptr<query::execution::PhysicalOperator> PatternBuilder::buildMatchPattern(
	CypherParser::PatternContext *pattern,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner,
	CypherParser::WhereContext *where,
	bool isOptional) {

	if (!pattern)
		return rootOp;

	auto parts = pattern->patternPart();
	if (parts.empty())
		return rootOp;

	// For OPTIONAL MATCH, we need to build the pattern operator separately
	// and then wrap it in OptionalMatchOperator
	if (isOptional && rootOp) {
		// Build the pattern operator starting from nullptr (independent of input)
		std::unique_ptr<query::execution::PhysicalOperator> patternOp = nullptr;

		// Collect required variables and build pattern operator
		std::vector<std::string> requiredVariables;

		for (auto part : parts) {
			auto element = part->patternElement();

			// Extract variables from this pattern element
			collectVariablesFromPatternElement(element, requiredVariables);

			// Build operator for this element
			patternOp = processMatchPatternElement(element, std::move(patternOp), planner);
		}

		// Apply WHERE clause if present
		patternOp = applyWhereFilter(std::move(patternOp), where, planner);

		// Wrap in OptionalMatchOperator
		return planner->optionalMatchOp(std::move(rootOp), std::move(patternOp), requiredVariables);
	}

	// Regular MATCH or standalone OPTIONAL MATCH (no input)
	// Iterate over all pattern parts (e.g. MATCH (a), (b))
	for (auto part : parts) {
		auto element = part->patternElement();
		auto currentOp = processMatchPatternElement(element, std::move(rootOp), planner);

		// Merge with Global Pipeline
		if (!rootOp) {
			// First component: It becomes the root
			rootOp = std::move(currentOp);
		} else {
			// Subsequent component: Cross Join with existing root
			rootOp = planner->cartesianProductOp(std::move(rootOp), std::move(currentOp));
		}
	}

	// Apply WHERE clause if present
	rootOp = applyWhereFilter(std::move(rootOp), where, planner);

	return rootOp;
}

std::unique_ptr<query::execution::PhysicalOperator> PatternBuilder::processMatchPatternElement(
	CypherParser::PatternElementContext *element,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	// 1. Head Node Processing (Scan)
	auto headNodePat = element->nodePattern();

	std::string var = AstExtractor::extractVariable(headNodePat->variable());
	std::string label = AstExtractor::extractLabel(headNodePat->nodeLabels());

	// Extract properties using AST-based evaluation
	auto props = AstExtractor::extractProperties(headNodePat->properties(),
		[](CypherParser::ExpressionContext *expr) {
			return ExpressionBuilder::evaluateLiteralExpression(expr);
		});

	// Optimizer: Index Pushdown
	std::string pushKey = "";
	PropertyValue pushVal;
	std::vector<std::pair<std::string, PropertyValue>> residualFilters;

	if (!props.empty()) {
		auto it = props.begin();
		pushKey = it->first;
		pushVal = it->second;
		for (++it; it != props.end(); ++it) {
			residualFilters.emplace_back(it->first, it->second);
		}
	}

	auto currentOp = planner->scanOp(var, label, pushKey, pushVal);

	// Apply Filters
	for (const auto &[key, val] : residualFilters) {
		auto predicate = [var, key, val](const query::execution::Record &r) {
			auto n = r.getNode(var);
			if (!n)
				return false;
			const auto &p = n->getProperties();
			return p.contains(key) && p.at(key) == val;
		};
		std::string desc = var + "." + key + " == " + val.toString();
		currentOp = planner->filterOp(std::move(currentOp), predicate, desc);
	}

	// If this is the first component, set it as root
	if (!rootOp) {
		rootOp = std::move(currentOp);
	} else {
		// Cross join with existing root
		rootOp = planner->cartesianProductOp(std::move(rootOp), std::move(currentOp));
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
		std::string targetLabel = AstExtractor::extractLabel(nodePat->nodeLabels());
		auto targetProps = AstExtractor::extractProperties(nodePat->properties(),
			[](CypherParser::ExpressionContext *expr) {
				return ExpressionBuilder::evaluateLiteralExpression(expr);
			});

		// Build Traversal
		if (isVarLength) {
			rootOp = planner->traverseVarLengthOp(std::move(rootOp), var, targetVar, edgeLabel, minHops,
												  maxHops, direction);
		} else {
			rootOp = planner->traverseOp(std::move(rootOp), var, edgeVar, targetVar, edgeLabel, direction);
		}

		// Filter Target
		if (!targetLabel.empty()) {
			auto dm = planner->getDataManager();
			int64_t targetLabelId = dm->getOrCreateLabelId(targetLabel);

			auto labelPred = [targetVar, targetLabelId](const query::execution::Record &r) {
				auto n = r.getNode(targetVar);
				return n && n->getLabelId() == targetLabelId;
			};
			rootOp = planner->filterOp(std::move(rootOp), labelPred, "Label(" + targetVar + ")=" + targetLabel);
		}

		for (const auto &[key, val] : targetProps) {
			auto pred = [targetVar, key, val](const query::execution::Record &r) {
				auto n = r.getNode(targetVar);
				return n && n->getProperties().contains(key) && n->getProperties().at(key) == val;
			};
			rootOp = planner->filterOp(std::move(rootOp), pred, targetVar + "." + key + "=" + val.toString());
		}

		// Filter Edge (Single Hop Only)
		if (!isVarLength) {
			for (const auto &[key, val] : edgeProps) {
				auto pred = [edgeVar, key, val](const query::execution::Record &r) {
					auto e = r.getEdge(edgeVar);
					return e && e->getProperties().contains(key) && e->getProperties().at(key) == val;
				};
				rootOp = planner->filterOp(std::move(rootOp), pred, edgeVar + "." + key + "=" + val.toString());
			}
		}

		// Update current variable for next hop
		var = targetVar;
	}

	return rootOp;
}

std::unique_ptr<query::execution::PhysicalOperator> PatternBuilder::applyWhereFilter(
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	CypherParser::WhereContext *where,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	if (!where)
		return rootOp;

	try {
		std::string desc;
		auto predicate = ExpressionBuilder::buildWherePredicate(where->expression(), desc);
		return planner->filterOp(std::move(rootOp), predicate, desc);
	} catch (const std::exception &e) {
		std::cerr << "Warning: Failed to parse WHERE clause: " << e.what() << std::endl;
		throw;
	}
}

std::unique_ptr<query::execution::PhysicalOperator> PatternBuilder::buildCreatePattern(
	CypherParser::PatternContext *pattern,
	std::unique_ptr<query::execution::PhysicalOperator> rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	if (!pattern)
		return rootOp;

	for (auto part : pattern->patternPart()) {
		processCreatePatternElement(part->patternElement(), rootOp, planner);
	}

	return rootOp;
}

void PatternBuilder::processCreatePatternElement(
	CypherParser::PatternElementContext *element,
	std::unique_ptr<query::execution::PhysicalOperator> &rootOp,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	auto headNodePat = element->nodePattern();

	std::string headVar = AstExtractor::extractVariable(headNodePat->variable());
	std::string headLabel = AstExtractor::extractLabel(headNodePat->nodeLabels());
	auto headProps = AstExtractor::extractProperties(headNodePat->properties(),
		[](CypherParser::ExpressionContext *expr) {
			return ExpressionBuilder::evaluateLiteralExpression(expr);
		});

	auto headOp = planner->createOp(headVar, headLabel, headProps);
	rootOp = OperatorChain::chain(std::move(rootOp), std::move(headOp));

	std::string prevVar = headVar;

	for (auto chain : element->patternElementChain()) {
		auto relPat = chain->relationshipPattern();
		auto targetNodePat = chain->nodePattern();

		std::string targetVar = AstExtractor::extractVariable(targetNodePat->variable());
		std::string targetLabel = AstExtractor::extractLabel(targetNodePat->nodeLabels());
		auto targetProps = AstExtractor::extractProperties(targetNodePat->properties(),
			[](CypherParser::ExpressionContext *expr) {
				return ExpressionBuilder::evaluateLiteralExpression(expr);
			});

		auto targetOp = planner->createOp(targetVar, targetLabel, targetProps);
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

		auto edgeOp = planner->createOp(edgeVar, edgeLabel, edgeProps, prevVar, targetVar);
		rootOp = OperatorChain::chain(std::move(rootOp), std::move(edgeOp));

		prevVar = targetVar;
	}
}

std::unique_ptr<query::execution::PhysicalOperator> PatternBuilder::buildMergePattern(
	CypherParser::PatternPartContext *patternPart,
	const std::vector<query::execution::operators::SetItem> &onCreateItems,
	const std::vector<query::execution::operators::SetItem> &onMatchItems,
	const std::shared_ptr<query::QueryPlanner> &planner) {

	if (!patternPart || !planner)
		return nullptr;

	// Currently only supporting MERGE (n) (Single Node)
	// Edge Merge requires traversal logic which is more complex.
	auto element = patternPart->patternElement();
	auto headNodePat = element->nodePattern();

	std::string var = AstExtractor::extractVariable(headNodePat->variable());
	std::string label = AstExtractor::extractLabel(headNodePat->nodeLabels());
	auto matchProps = AstExtractor::extractProperties(headNodePat->properties(),
		[](CypherParser::ExpressionContext *expr) {
			return ExpressionBuilder::evaluateLiteralExpression(expr);
		});

	return planner->mergeOp(var, label, matchProps, onCreateItems, onMatchItems);
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
		// Case 2: Label Assignment (n:Label)
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
