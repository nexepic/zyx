/**
 * @file CypherASTBuilder.cpp
 * @brief Converts ANTLR parse tree contexts into QueryAST structures.
 *
 * This file lives in the parser directory because it depends on ANTLR-generated
 * types and parser helper classes (ExpressionBuilder, AstExtractor).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/ir/CypherASTBuilder.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "helpers/AstExtractor.hpp"
#include "generated/CypherParser.h"
#include "generated/CypherLexer.h"
#include "graph/query/expressions/Expression.hpp"
#include <functional>
#include <stdexcept>

namespace graph::query::ir {

using graph::parser::cypher::helpers::ExpressionBuilder;
using graph::parser::cypher::helpers::AstExtractor;

namespace {

// =============================================================================
// Shared helpers
// =============================================================================

ProjectionBody buildProjectionBody(CypherParser::ProjectionBodyContext* body) {
	ProjectionBody result;

	result.distinct = (body->K_DISTINCT() != nullptr);

	auto items = body->projectionItems();

	if (items->MULTIPLY()) {
		result.returnStar = true;
	} else {
		for (auto item : items->projectionItem()) {
			ProjectionItem pi;
			auto exprAST = ExpressionBuilder::buildExpression(item->expression());
			pi.expression = std::shared_ptr<graph::query::expressions::Expression>(exprAST.release());

			if (item->K_AS()) {
				pi.alias = AstExtractor::extractVariable(item->variable());
			} else {
				pi.alias = item->expression()->getText();
			}

			result.items.push_back(std::move(pi));
		}
	}

	// ORDER BY
	if (body->orderStatement()) {
		auto sortItemList = body->orderStatement()->sortItem();
		for (auto item : sortItemList) {
			SortItem si;
			auto expressionAST = ExpressionBuilder::buildExpression(item->expression());
			si.expression = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

			si.ascending = true;
			if (item->K_DESC() || item->K_DESCENDING()) {
				si.ascending = false;
			}

			result.orderBy.push_back(std::move(si));
		}
	}

	// SKIP
	if (body->skipStatement()) {
		auto skipExpr = body->skipStatement()->expression();
		try {
			result.skip = std::stoll(skipExpr->getText());
		} catch (...) {
			throw std::runtime_error("SKIP requires an integer literal.");
		}
	}

	// LIMIT
	if (body->limitStatement()) {
		auto limitExpr = body->limitStatement()->expression();
		try {
			result.limit = std::stoll(limitExpr->getText());
		} catch (...) {
			throw std::runtime_error("LIMIT requires an integer literal.");
		}
	}

	return result;
}

/**
 * @brief Extract a node pattern from ANTLR context into AST.
 */
NodePatternAST extractNodePattern(CypherParser::NodePatternContext* nodePat) {
	NodePatternAST node;
	node.variable = AstExtractor::extractVariable(nodePat->variable());
	node.labels = AstExtractor::extractLabels(nodePat->nodeLabels());

	if (nodePat->properties() && nodePat->properties()->mapLiteral()) {
		auto mapLit = nodePat->properties()->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();
		for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
			std::string key = keys[i]->getText();
			PropertyValue litVal = ExpressionBuilder::evaluateLiteralExpression(exprs[i]);
			if (litVal.getType() != PropertyType::NULL_TYPE || exprs[i]->getText() == "null") {
				node.properties.emplace_back(key, litVal);
			} else {
				// Non-literal (e.g. $param) — store as expression for runtime evaluation
				auto exprAST = ExpressionBuilder::buildExpression(exprs[i]);
				node.propertyExpressions[key] =
					std::shared_ptr<graph::query::expressions::Expression>(exprAST.release());
			}
		}
	}

	return node;
}

/**
 * @brief Extract a node pattern for CREATE with expression-based properties.
 */
NodePatternAST extractCreateNodePattern(CypherParser::NodePatternContext* nodePat) {
	NodePatternAST node;
	node.variable = AstExtractor::extractVariable(nodePat->variable());
	node.labels = AstExtractor::extractLabels(nodePat->nodeLabels());

	if (nodePat->properties() && nodePat->properties()->mapLiteral()) {
		auto mapLit = nodePat->properties()->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();
		for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
			std::string key = keys[i]->getText();
			PropertyValue litVal = ExpressionBuilder::evaluateLiteralExpression(exprs[i]);
			if (litVal.getType() != PropertyType::NULL_TYPE || exprs[i]->getText() == "null") {
				node.properties.emplace_back(key, litVal);
			} else {
				auto exprAST = ExpressionBuilder::buildExpression(exprs[i]);
				node.propertyExpressions[key] =
					std::shared_ptr<graph::query::expressions::Expression>(exprAST.release());
			}
		}
	}

	return node;
}

/**
 * @brief Extract relationship pattern from ANTLR context into AST.
 */
RelationshipPatternAST extractRelationshipPattern(CypherParser::RelationshipPatternContext* relPat) {
	RelationshipPatternAST rel;

	// Direction
	bool hasLeft = (relPat->LT() != nullptr);
	bool hasRight = (relPat->GT() != nullptr);
	if (hasLeft && !hasRight)
		rel.direction = "in";
	else if (!hasLeft && hasRight)
		rel.direction = "out";
	else
		rel.direction = "both";

	auto relDetail = relPat->relationshipDetail();
	if (relDetail) {
		rel.variable = AstExtractor::extractVariable(relDetail->variable());
		rel.type = AstExtractor::extractRelType(relDetail->relationshipTypes());

		// Split edge properties into literals and expressions
		if (relDetail->properties() && relDetail->properties()->mapLiteral()) {
			auto mapLit = relDetail->properties()->mapLiteral();
			auto keys = mapLit->propertyKeyName();
			auto exprs = mapLit->expression();
			for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
				std::string key = keys[i]->getText();
				PropertyValue litVal = ExpressionBuilder::evaluateLiteralExpression(exprs[i]);
				if (litVal.getType() != PropertyType::NULL_TYPE || exprs[i]->getText() == "null") {
					rel.properties[key] = litVal;
				} else {
					auto exprAST = ExpressionBuilder::buildExpression(exprs[i]);
					rel.propertyExpressions[key] =
						std::shared_ptr<graph::query::expressions::Expression>(exprAST.release());
				}
			}
		}

		// Variable-length path
		if (relDetail->rangeLiteral()) {
			rel.isVarLength = true;
			auto range = relDetail->rangeLiteral();
			rel.minHops = 1;
			rel.maxHops = 15;

			auto ints = range->integerLiteral();

			if (range->RANGE() != nullptr) {
				if (ints.size() == 2) {
					rel.minHops = std::stoi(ints[0]->getText());
					rel.maxHops = std::stoi(ints[1]->getText());
				} else if (ints.size() == 1) {
					std::string rawText = range->getText();
					if (rawText.find("..") < rawText.find(ints[0]->getText())) {
						rel.maxHops = std::stoi(ints[0]->getText());
					} else {
						rel.minHops = std::stoi(ints[0]->getText());
					}
				}
			} else if (!ints.empty()) {
				rel.minHops = rel.maxHops = std::stoi(ints[0]->getText());
			}
		}
	}

	return rel;
}

/**
 * @brief Extract constraint body into clause fields.
 */
void extractConstraintBody(CypherParser::ConstraintBodyContext* body,
	CreateConstraintClause& clause) {
	if (auto* unique = dynamic_cast<CypherParser::UniqueConstraintBodyContext*>(body)) {
		clause.constraintType = "unique";
		clause.properties.push_back(
			AstExtractor::extractPropertyKeyFromExpr(unique->propertyExpression()));
	} else if (auto* notNull = dynamic_cast<CypherParser::NotNullConstraintBodyContext*>(body)) {
		clause.constraintType = "not_null";
		clause.properties.push_back(
			AstExtractor::extractPropertyKeyFromExpr(notNull->propertyExpression()));
	} else if (auto* propType = dynamic_cast<CypherParser::PropertyTypeConstraintBodyContext*>(body)) {
		clause.constraintType = "property_type";
		clause.properties.push_back(
			AstExtractor::extractPropertyKeyFromExpr(propType->propertyExpression()));
		clause.typeName = propType->constraintTypeName()->getText();
	} else if (auto* nodeKey = dynamic_cast<CypherParser::NodeKeyConstraintBodyContext*>(body)) {
		clause.constraintType = "node_key";
		for (auto* propExpr : nodeKey->propertyExpression()) {
			clause.properties.push_back(
				AstExtractor::extractPropertyKeyFromExpr(propExpr));
		}
	} else {
		throw std::runtime_error("Unknown constraint body type");
	}
}

} // anonymous namespace

// =============================================================================
// Projection Clauses
// =============================================================================

ReturnClause CypherASTBuilder::buildReturnClause(void* ctx) {
	auto* returnCtx = static_cast<CypherParser::ReturnStatementContext*>(ctx);
	ReturnClause clause;
	clause.body = buildProjectionBody(returnCtx->projectionBody());
	return clause;
}

WithClause CypherASTBuilder::buildWithClause(void* ctx) {
	auto* withCtx = static_cast<CypherParser::WithClauseContext*>(ctx);
	WithClause clause;
	clause.body = buildProjectionBody(withCtx->projectionBody());

	if (withCtx->where()) {
		auto ast = ExpressionBuilder::buildExpression(withCtx->where()->expression());
		clause.where = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
	}

	return clause;
}

// =============================================================================
// Reading Clauses
// =============================================================================

MatchClause CypherASTBuilder::buildMatchClause(void* ctx) {
	auto* matchCtx = static_cast<CypherParser::MatchStatementContext*>(ctx);
	MatchClause clause;
	clause.optional = (matchCtx->K_OPTIONAL() != nullptr);

	auto* pattern = matchCtx->pattern();
	if (pattern) {
		for (auto* part : pattern->patternPart()) {
			PatternPartAST patternPart;
			if (part->variable()) {
				patternPart.pathVariable = part->variable()->getText();
			}
			patternPart.element = extractPatternElement(part->patternElement());
			clause.patterns.push_back(std::move(patternPart));
		}
	}

	if (matchCtx->where()) {
		auto ast = ExpressionBuilder::buildExpression(matchCtx->where()->expression());
		clause.where = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
	}

	return clause;
}

UnwindClause CypherASTBuilder::buildUnwindClause(void* ctx) {
	auto* unwindCtx = static_cast<CypherParser::UnwindStatementContext*>(ctx);
	UnwindClause clause;
	clause.alias = AstExtractor::extractVariable(unwindCtx->variable());

	auto* expr = unwindCtx->expression();
	clause.literalList = ExpressionBuilder::extractListFromExpression(expr);

	if (clause.literalList.empty()) {
		auto ast = ExpressionBuilder::buildExpression(expr);
		clause.expression = std::shared_ptr<graph::query::expressions::Expression>(ast.release());
	}

	return clause;
}

CallClause CypherASTBuilder::buildStandaloneCallClause(void* ctx) {
	auto* callCtx = static_cast<CypherParser::StandaloneCallStatementContext*>(ctx);
	CallClause clause;
	clause.standalone = true;

	auto invoc = callCtx->explicitProcedureInvocation();
	clause.procedureName = invoc->procedureName()->getText();

	for (auto expr : invoc->expression()) {
		clause.arguments.push_back(ExpressionBuilder::evaluateLiteralExpression(expr));
	}

	return clause;
}

CallClause CypherASTBuilder::buildInQueryCallClause(void* ctx) {
	auto* callCtx = static_cast<CypherParser::InQueryCallStatementContext*>(ctx);
	CallClause clause;
	clause.standalone = false;

	auto invoc = callCtx->explicitProcedureInvocation();
	clause.procedureName = invoc->procedureName()->getText();

	for (auto expr : invoc->expression()) {
		clause.arguments.push_back(ExpressionBuilder::evaluateLiteralExpression(expr));
	}

	if (callCtx->K_YIELD() && callCtx->yieldItems()) {
		for (auto item : callCtx->yieldItems()->yieldItem()) {
			CallClause::YieldItem yi;
			yi.alias = AstExtractor::extractVariable(item->variable());
			if (item->procedureResultField()) {
				yi.sourceField = item->procedureResultField()->getText();
			} else {
				yi.sourceField = yi.alias;
			}
			clause.yieldItems.push_back(std::move(yi));
		}
	}

	return clause;
}

// =============================================================================
// Writing Clauses
// =============================================================================

CreateClause CypherASTBuilder::buildCreateClause(void* ctx) {
	auto* createCtx = static_cast<CypherParser::CreateStatementContext*>(ctx);
	CreateClause clause;

	auto* pattern = createCtx->pattern();
	if (pattern) {
		for (auto* part : pattern->patternPart()) {
			PatternPartAST patternPart;
			// Extract head and chains using CREATE-specific extraction (expression properties)
			auto* element = part->patternElement();
			PatternElementAST elemAST;
			elemAST.headNode = extractCreateNodePattern(element->nodePattern());

			for (auto* chain : element->patternElementChain()) {
				PatternChainAST chainAST;
				chainAST.relationship = extractRelationshipPattern(chain->relationshipPattern());
				// For CREATE target nodes, use literal extraction only
				auto targetNodePat = chain->nodePattern();
				chainAST.targetNode.variable = AstExtractor::extractVariable(targetNodePat->variable());
				chainAST.targetNode.labels = AstExtractor::extractLabels(targetNodePat->nodeLabels());
				auto targetProps = AstExtractor::extractProperties(targetNodePat->properties(),
					[](CypherParser::ExpressionContext* expr) {
						return ExpressionBuilder::evaluateLiteralExpression(expr);
					});
				for (const auto& [k, v] : targetProps) {
					chainAST.targetNode.properties.emplace_back(k, v);
				}
				elemAST.chain.push_back(std::move(chainAST));
			}

			patternPart.element = std::move(elemAST);
			clause.patterns.push_back(std::move(patternPart));
		}
	}

	return clause;
}

SetClause CypherASTBuilder::buildSetClause(void* ctx) {
	auto* setCtx = static_cast<CypherParser::SetStatementContext*>(ctx);
	SetClause clause;
	clause.items = extractSetItems(setCtx);
	return clause;
}

DeleteClause CypherASTBuilder::buildDeleteClause(void* ctx) {
	auto* deleteCtx = static_cast<CypherParser::DeleteStatementContext*>(ctx);
	DeleteClause clause;
	clause.detach = (deleteCtx->K_DETACH() != nullptr);

	for (auto expr : deleteCtx->expression()) {
		clause.variables.push_back(expr->getText());
	}

	return clause;
}

RemoveClause CypherASTBuilder::buildRemoveClause(void* ctx) {
	auto* removeCtx = static_cast<CypherParser::RemoveStatementContext*>(ctx);
	RemoveClause clause;

	for (auto item : removeCtx->removeItem()) {
		RemoveItemAST ri;
		if (item->propertyExpression()) {
			ri.type = RemoveItemType::RIT_PROPERTY;
			ri.variable = item->propertyExpression()->atom()->getText();
			ri.key = item->propertyExpression()->propertyKeyName(0)->getText();
		} else if (item->variable() && item->nodeLabels()) {
			ri.type = RemoveItemType::RIT_LABEL;
			ri.variable = AstExtractor::extractVariable(item->variable());
			ri.key = AstExtractor::extractLabel(item->nodeLabels());
		}
		clause.items.push_back(std::move(ri));
	}

	return clause;
}

MergeClause CypherASTBuilder::buildMergeClause(void* ctx) {
	auto* mergeCtx = static_cast<CypherParser::MergeStatementContext*>(ctx);
	MergeClause clause;

	auto* patternPart = mergeCtx->patternPart();
	clause.pattern.element = extractPatternElement(patternPart->patternElement());

	// Parse ON MATCH / ON CREATE set actions
	bool sawOn = false;
	bool isOnMatch = false;

	for (auto* child : mergeCtx->children) {
		auto* termNode = dynamic_cast<antlr4::tree::TerminalNode*>(child);
		if (termNode) {
			auto tokenType = termNode->getSymbol()->getType();
			if (tokenType == CypherLexer::K_ON) {
				sawOn = true;
				continue;
			}
			if (sawOn) {
				if (tokenType == CypherLexer::K_MATCH) {
					isOnMatch = true;
				} else if (tokenType == CypherLexer::K_CREATE) {
					isOnMatch = false;
				}
				sawOn = false;
				continue;
			}
		} else {
			auto* setCtx = dynamic_cast<CypherParser::SetStatementContext*>(child);
			if (setCtx) {
				auto items = extractSetItems(setCtx);
				if (isOnMatch) {
					clause.onMatchItems.insert(clause.onMatchItems.end(), items.begin(), items.end());
				} else {
					clause.onCreateItems.insert(clause.onCreateItems.end(), items.begin(), items.end());
				}
			}
		}
	}

	return clause;
}

// =============================================================================
// Admin Clauses
// =============================================================================

ShowIndexesClause CypherASTBuilder::buildShowIndexesClause() {
	return ShowIndexesClause{};
}

CreateIndexClause CypherASTBuilder::buildCreateIndexByPatternClause(void* ctx) {
	auto* createCtx = static_cast<CypherParser::CreateIndexByPatternContext*>(ctx);
	CreateIndexClause clause;

	if (createCtx->symbolicName()) {
		clause.name = createCtx->symbolicName()->getText();
	}

	auto pat = createCtx->nodePattern();
	if (pat->nodeLabels() && !pat->nodeLabels()->nodeLabel().empty()) {
		clause.label = AstExtractor::extractLabelFromNodeLabel(pat->nodeLabels()->nodeLabel(0));
	} else {
		throw std::runtime_error("CREATE INDEX pattern must specify a Label (e.g. :User)");
	}

	for (auto* propExpr : createCtx->propertyExpression()) {
		clause.properties.push_back(AstExtractor::extractPropertyKeyFromExpr(propExpr));
	}

	return clause;
}

CreateIndexClause CypherASTBuilder::buildCreateIndexByLabelClause(void* ctx) {
	auto* createCtx = static_cast<CypherParser::CreateIndexByLabelContext*>(ctx);
	CreateIndexClause clause;

	if (createCtx->symbolicName()) {
		clause.name = createCtx->symbolicName()->getText();
	}
	clause.label = AstExtractor::extractLabelFromNodeLabel(createCtx->nodeLabel());
	clause.properties.push_back(createCtx->propertyKeyName()->getText());

	return clause;
}

CreateVectorIndexClause CypherASTBuilder::buildCreateVectorIndexClause(void* ctx) {
	auto* createCtx = static_cast<CypherParser::CreateVectorIndexContext*>(ctx);
	CreateVectorIndexClause clause;

	clause.name = createCtx->symbolicName() ? createCtx->symbolicName()->getText() : "";
	clause.label = AstExtractor::extractLabelFromNodeLabel(createCtx->nodeLabel());
	clause.property = createCtx->propertyKeyName()->getText();

	if (createCtx->K_OPTIONS() && createCtx->mapLiteral()) {
		auto mapLit = createCtx->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();
		for (size_t i = 0; i < keys.size(); ++i) {
			std::string k = keys[i]->getText();
			std::string v = exprs[i]->getText();
			if (k == "dimension" || k == "dim")
				clause.dimension = std::stoi(v);
			else if (k == "metric")
				clause.metric = v.substr(1, v.size() - 2); // remove quotes
		}
	}

	if (clause.dimension == 0)
		throw std::runtime_error("Vector Index requires 'dimension'");

	return clause;
}

DropIndexClause CypherASTBuilder::buildDropIndexByNameClause(void* ctx) {
	auto* dropCtx = static_cast<CypherParser::DropIndexByNameContext*>(ctx);
	DropIndexClause clause;
	clause.name = dropCtx->symbolicName()->getText();
	return clause;
}

DropIndexClause CypherASTBuilder::buildDropIndexByLabelClause(void* ctx) {
	auto* dropCtx = static_cast<CypherParser::DropIndexByLabelContext*>(ctx);
	DropIndexClause clause;
	clause.label = AstExtractor::extractLabelFromNodeLabel(dropCtx->nodeLabel());
	clause.property = dropCtx->propertyKeyName()->getText();
	return clause;
}

CreateConstraintClause CypherASTBuilder::buildCreateNodeConstraintClause(void* ctx) {
	auto* createCtx = static_cast<CypherParser::CreateNodeConstraintContext*>(ctx);
	CreateConstraintClause clause;
	clause.name = createCtx->symbolicName()->getText();
	clause.targetType = "node";
	clause.label = createCtx->constraintNodePattern()->labelName()->getText();
	extractConstraintBody(createCtx->constraintBody(), clause);
	return clause;
}

CreateConstraintClause CypherASTBuilder::buildCreateEdgeConstraintClause(void* ctx) {
	auto* createCtx = static_cast<CypherParser::CreateEdgeConstraintContext*>(ctx);
	CreateConstraintClause clause;
	clause.name = createCtx->symbolicName()->getText();
	clause.targetType = "edge";
	clause.label = createCtx->constraintEdgePattern()->labelName()->getText();
	extractConstraintBody(createCtx->constraintBody(), clause);
	return clause;
}

DropConstraintClause CypherASTBuilder::buildDropConstraintClause(const std::string& name, bool ifExists) {
	return DropConstraintClause{name, ifExists};
}

ShowConstraintsClause CypherASTBuilder::buildShowConstraintsClause() {
	return ShowConstraintsClause{};
}

// =============================================================================
// Pattern helpers
// =============================================================================

PatternElementAST CypherASTBuilder::extractPatternElement(void* ctx) {
	auto* element = static_cast<CypherParser::PatternElementContext*>(ctx);
	PatternElementAST result;

	result.headNode = extractNodePattern(element->nodePattern());

	for (auto* chain : element->patternElementChain()) {
		PatternChainAST chainAST;
		chainAST.relationship = extractRelationshipPattern(chain->relationshipPattern());
		chainAST.targetNode = extractNodePattern(chain->nodePattern());
		result.chain.push_back(std::move(chainAST));
	}

	return result;
}

std::vector<SetItemAST> CypherASTBuilder::extractSetItems(void* ctx) {
	auto* setCtx = static_cast<CypherParser::SetStatementContext*>(ctx);
	std::vector<SetItemAST> items;
	if (!setCtx)
		return items;

	for (auto item : setCtx->setItem()) {
		// Case 1: Property Update (n.prop = expr)
		if (item->propertyExpression() && item->EQ()) {
			auto propExpr = item->propertyExpression();
			std::string varName;
			if (propExpr->atom() && propExpr->atom()->variable()) {
				varName = AstExtractor::extractVariable(propExpr->atom()->variable());
			}
			std::string keyName = AstExtractor::extractPropertyKeyFromExpr(propExpr);

			auto expressionAST = ExpressionBuilder::buildExpression(item->expression());
			auto exprShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());

			items.push_back({SetItemType::SIT_PROPERTY, varName, keyName, exprShared});
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
				auto keys = mapLit->propertyKeyName();
				auto exprs = mapLit->expression();
				for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
					auto expressionAST = ExpressionBuilder::buildExpression(exprs[i]);
					auto exprShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());
					items.push_back({SetItemType::SIT_PROPERTY, varName, keys[i]->getText(), exprShared});
				}
			} else {
				auto expressionAST = ExpressionBuilder::buildExpression(item->expression());
				auto exprShared = std::shared_ptr<graph::query::expressions::Expression>(expressionAST.release());
				items.push_back({SetItemType::SIT_MAP_MERGE, varName, "", exprShared});
			}
		}
		// Case 3: Label Assignment (n:Label)
		else if (item->variable() && item->nodeLabels()) {
			std::string varName = AstExtractor::extractVariable(item->variable());
			std::string labelName = AstExtractor::extractLabel(item->nodeLabels());
			items.push_back({SetItemType::SIT_LABEL, varName, labelName, nullptr});
		}
	}

	return items;
}

} // namespace graph::query::ir
