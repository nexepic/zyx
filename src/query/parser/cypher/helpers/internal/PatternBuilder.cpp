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
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/query/execution/operators/MergeEdgeOperator.hpp"

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

		if (!rootOp) {
			// First component or no prior pipeline: build from scratch
			rootOp = processMatchPatternElement(element, nullptr, planner);
		} else if (parts.size() == 1) {
			// Single pattern part with existing pipeline: chain directly
			rootOp = processMatchPatternElement(element, std::move(rootOp), planner);
		} else {
			// Multiple pattern parts with existing pipeline:
			// Build this part independently, then CartesianProduct with root
			auto partOp = processMatchPatternElement(element, nullptr, planner);
			rootOp = planner->cartesianProductOp(std::move(rootOp), std::move(partOp));
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
	auto labels = AstExtractor::extractLabels(headNodePat->nodeLabels());

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

	auto currentOp = planner->scanOp(var, labels, pushKey, pushVal);

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
		auto targetLabels = AstExtractor::extractLabels(nodePat->nodeLabels());
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

		// Filter Target by labels (AND semantics: node must have ALL labels)
		if (!targetLabels.empty()) {
			auto dm = planner->getDataManager();
			std::vector<int64_t> targetLabelIds;
			std::string labelDesc;
			for (const auto &lbl : targetLabels) {
				targetLabelIds.push_back(dm->getOrCreateLabelId(lbl));
				if (!labelDesc.empty()) labelDesc += ":";
				labelDesc += lbl;
			}

			auto labelPred = [targetVar, targetLabelIds](const query::execution::Record &r) {
				auto n = r.getNode(targetVar);
				if (!n) return false;
				for (int64_t tid : targetLabelIds) {
					if (!n->hasLabelId(tid)) return false;
				}
				return true;
			};
			rootOp = planner->filterOp(std::move(rootOp), labelPred, "Label(" + targetVar + ")=" + labelDesc);
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
		auto ast = ExpressionBuilder::buildExpression(where->expression());
		auto astShared = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
		std::string desc = astShared->toString();
		auto dm = planner->getDataManager().get();
		auto predicate = [astShared, dm](const query::execution::Record &r) -> bool {
			return graph::query::expressions::ExpressionEvaluationHelper::evaluateBool(astShared.get(), r, dm);
		};
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

	std::unique_ptr<query::execution::PhysicalOperator> headOp;
	if (!headPropExprs.empty()) {
		headOp = planner->createOp(headVar, headLabels, headProps, std::move(headPropExprs));
	} else {
		headOp = planner->createOp(headVar, headLabels, headProps);
	}
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

		auto targetOp = planner->createOp(targetVar, targetLabels, targetProps);
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

	auto element = patternPart->patternElement();
	auto headNodePat = element->nodePattern();

	std::string headVar = AstExtractor::extractVariable(headNodePat->variable());
	auto headLabels = AstExtractor::extractLabels(headNodePat->nodeLabels());
	auto headProps = AstExtractor::extractProperties(headNodePat->properties(),
		[](CypherParser::ExpressionContext *expr) {
			return ExpressionBuilder::evaluateLiteralExpression(expr);
		});

	// Single node MERGE: MERGE (n:Label {props})
	if (element->patternElementChain().empty()) {
		return planner->mergeOp(headVar, headLabels, headProps, onCreateItems, onMatchItems);
	}

	// Edge MERGE: MERGE (a)-[r:TYPE {props}]->(b)
	// Strategy: First merge source node, then merge target node, then merge edge
	std::unique_ptr<query::execution::PhysicalOperator> rootOp = nullptr;

	// Merge head node (no ON CREATE/MATCH items for intermediate nodes)
	std::vector<query::execution::operators::SetItem> emptyItems;
	rootOp = planner->mergeOp(headVar, headLabels, headProps, emptyItems, emptyItems);

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

		// Merge target node
		auto targetMergeOp = planner->mergeOp(targetVar, targetLabels, targetProps, emptyItems, emptyItems);
		// Chain the target merge as child of root
		auto* mergeNodeOp = dynamic_cast<query::execution::operators::MergeNodeOperator*>(targetMergeOp.get());
		if (mergeNodeOp) {
			mergeNodeOp->setChild(std::move(rootOp));
			rootOp = std::move(targetMergeOp);
		}

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
		auto mergeEdge = planner->mergeEdgeOp(
			prevVar, edgeVar, targetVar, edgeLabel, edgeProps, direction,
			isLastChain ? onCreateItems : emptyItems,
			isLastChain ? onMatchItems : emptyItems);

		auto* edgeOp = dynamic_cast<query::execution::operators::MergeEdgeOperator*>(mergeEdge.get());
		if (edgeOp) {
			edgeOp->setChild(std::move(rootOp));
			rootOp = std::move(mergeEdge);
		}

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
