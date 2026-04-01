/**
 * @file ExpressionBuilder.cpp
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

#include "../ExpressionBuilder.hpp"
#include "generated/CypherLexer.h"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/execution/Record.hpp"

namespace graph::parser::cypher::helpers {
using namespace graph::query::expressions;

// ============================================================================
// NEW API: AST-based expression building
// ============================================================================

std::unique_ptr<Expression> ExpressionBuilder::buildExpression(CypherParser::ExpressionContext *expr) {
	if (!expr) {
		throw std::runtime_error("Invalid expression tree: null context");
	}

	// Navigate the ANTLR4 AST hierarchy
	// Expression → OrExpression → XorExpression → AndExpression → NotExpression → Comparison → ...
	auto orExpr = expr->orExpression();
	if (!orExpr) {
		throw std::runtime_error("Invalid expression: missing OrExpression");
	}

	return buildOrExpression(orExpr);
}

PropertyValue ExpressionBuilder::evaluateLiteralExpression(CypherParser::ExpressionContext *expr) {
	if (!expr) {
		return PropertyValue();
	}

	// Special case: Check if it's a list literal BEFORE calling buildExpression()
	// buildListLiteral() throws an error for non-IN contexts, so we need to handle it here
	auto atom = getAtomFromExpression(expr);
	if (atom && atom->listLiteral()) {
		// It's a list literal - parse directly into PropertyValue
		return parseListLiteral(atom->listLiteral());
	}

	// Build the AST
	auto ast = buildExpression(expr);
	if (!ast) {
		return PropertyValue();
	}

	// Check if it's a literal expression (no variables, no function calls)
	// For pattern properties, we only support literals
	if (auto *litExpr = dynamic_cast<LiteralExpression*>(ast.get())) {
		// It's a literal - we can extract the value directly
		if (litExpr->isNull()) {
			return PropertyValue();
		} else if (litExpr->isBoolean()) {
			return PropertyValue(litExpr->getBooleanValue());
		} else if (litExpr->isInteger()) {
			return PropertyValue(litExpr->getIntegerValue());
		} else if (litExpr->isDouble()) {
			return PropertyValue(litExpr->getDoubleValue());
		} else if (litExpr->isString()) {
			return PropertyValue(litExpr->getStringValue());
		}
	}

	// If it's a more complex expression (unary minus on literal, etc.)
	// Try to evaluate with an empty record context
	// This will work for literals and literal-only expressions
	try {
		// Create an empty record for evaluation
		graph::query::execution::Record emptyRecord;
		graph::query::expressions::EvaluationContext context(emptyRecord);
		graph::query::expressions::ExpressionEvaluator evaluator(context);
		return evaluator.evaluate(ast.get());
	} catch (const std::exception& e) {
		// Non-literal expressions (variables, function calls) are not supported in pattern properties
		// Return empty PropertyValue to maintain backward compatibility
		// These will be handled by runtime filters in the WHERE clause
		return PropertyValue();
	}
}

std::unique_ptr<Expression> ExpressionBuilder::buildOrExpression(CypherParser::OrExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Handle OR: xor (OR xor)*
	auto left = buildXorExpression(ctx->xorExpression(0));

	// If there are multiple XOR expressions connected by OR
	for (size_t i = 1; i < ctx->xorExpression().size(); ++i) {
		auto right = buildXorExpression(ctx->xorExpression(i));
		left = std::make_unique<BinaryOpExpression>(
			std::move(left),
			BinaryOperatorType::BOP_OR,
			std::move(right)
		);
	}

	return left;
}

std::unique_ptr<Expression> ExpressionBuilder::buildXorExpression(CypherParser::XorExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Handle XOR: and (XOR and)*
	auto left = buildAndExpression(ctx->andExpression(0));

	// If there are multiple AND expressions connected by XOR
	for (size_t i = 1; i < ctx->andExpression().size(); ++i) {
		auto right = buildAndExpression(ctx->andExpression(i));
		left = std::make_unique<BinaryOpExpression>(
			std::move(left),
			BinaryOperatorType::BOP_XOR,
			std::move(right)
		);
	}

	return left;
}

std::unique_ptr<Expression> ExpressionBuilder::buildAndExpression(CypherParser::AndExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Handle AND: not (AND not)*
	auto left = buildNotExpression(ctx->notExpression(0));

	// If there are multiple NOT expressions connected by AND
	for (size_t i = 1; i < ctx->notExpression().size(); ++i) {
		auto right = buildNotExpression(ctx->notExpression(i));
		left = std::make_unique<BinaryOpExpression>(
			std::move(left),
			BinaryOperatorType::BOP_AND,
			std::move(right)
		);
	}

	return left;
}

std::unique_ptr<Expression> ExpressionBuilder::buildNotExpression(CypherParser::NotExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto operand = buildComparisonExpression(ctx->comparisonExpression());

	// Handle NOT operator (single token, not array)
	if (ctx->K_NOT()) {
		return std::make_unique<UnaryOpExpression>(
			UnaryOperatorType::UOP_NOT,
			std::move(operand)
		);
	}

	return operand;
}

std::unique_ptr<Expression> ExpressionBuilder::buildComparisonExpression(CypherParser::ComparisonExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto arithExprs = ctx->arithmeticExpression();
	if (arithExprs.empty()) {
		return buildArithmeticExpression(ctx->arithmeticExpression(0));
	}

	// Handle BETWEEN: expr BETWEEN lo AND hi → (expr >= lo) AND (expr <= hi)
	if (ctx->K_BETWEEN() && arithExprs.size() == 3) {
		auto expr = buildArithmeticExpression(arithExprs[0]);
		auto lo = buildArithmeticExpression(arithExprs[1]);
		auto hi = buildArithmeticExpression(arithExprs[2]);

		// We need to duplicate expr for the two comparisons.
		// Since expr is a unique_ptr, we build it once and clone via re-parsing is not possible.
		// Instead, build a second copy from the same parse tree node.
		auto exprCopy = buildArithmeticExpression(arithExprs[0]);

		auto geExpr = std::make_unique<BinaryOpExpression>(
			std::move(expr), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(lo));
		auto leExpr = std::make_unique<BinaryOpExpression>(
			std::move(exprCopy), BinaryOperatorType::BOP_LESS_EQUAL, std::move(hi));

		return std::make_unique<BinaryOpExpression>(
			std::move(geExpr), BinaryOperatorType::BOP_AND, std::move(leExpr));
	}

	// Handle comparison operators: arith (COMPARE arith)* (IS_NULL)?
	auto left = buildArithmeticExpression(arithExprs[0]);

	// Process binary comparison operators (EQ, NEQ, LT, GT, LTE, GTE, IN, STARTS WITH, ENDS WITH, CONTAINS)
	if (!ctx->children.empty() && arithExprs.size() > 1) {
		// Walk children in parse order to determine operator type by position
		size_t arithIdx = 0;
		BinaryOperatorType pendingOp = BinaryOperatorType::BOP_EQUAL;
		bool hasPendingOp = false;
		bool expectWith = false; // For STARTS WITH / ENDS WITH multi-token operators

		for (auto* child : ctx->children) {
			auto* termNode = dynamic_cast<antlr4::tree::TerminalNode*>(child);
			if (!termNode) {
				// This is a rule context (arithmeticExpression)
				if (arithIdx == 0) {
					arithIdx++;
					continue; // Skip the first arithmetic expression (already in 'left')
				}
				if (!hasPendingOp) {
					arithIdx++;
					continue;
				}

				// Check if IN operator with a list literal on the RHS
				if (pendingOp == BinaryOperatorType::BOP_IN) {
					auto rhsArith = arithExprs[arithIdx];
					auto rhsPowerList = rhsArith->powerExpression();
					if (!rhsPowerList.empty()) {
						auto rhsUnaryList = rhsPowerList[0]->unaryExpression();
						if (!rhsUnaryList.empty() && rhsUnaryList[0]->atom() && rhsUnaryList[0]->atom()->listLiteral()) {
							std::vector<PropertyValue> listValues;
							auto listLit = rhsUnaryList[0]->atom()->listLiteral();
							for (auto itemExpr : listLit->expression()) {
								auto itemAtom = getAtomFromExpression(itemExpr);
								if (itemAtom && itemAtom->literal()) {
									listValues.push_back(parseValue(itemAtom->literal()));
								} else {
									throw std::runtime_error("IN list only supports literal values");
								}
							}
							left = std::make_unique<InExpression>(std::move(left), std::move(listValues));
							hasPendingOp = false;
							arithIdx++;
							continue;
						}
					}
				}

				auto right = buildArithmeticExpression(arithExprs[arithIdx]);
				left = std::make_unique<BinaryOpExpression>(std::move(left), pendingOp, std::move(right));
				hasPendingOp = false;
				arithIdx++;
				continue;
			}

			// Terminal node - determine operator type
			auto tokenType = termNode->getSymbol()->getType();

			if (expectWith) {
				// We just saw K_STARTS or K_ENDS, expecting K_WITH
				expectWith = false;
				continue; // K_WITH consumed, pendingOp already set
			}

			switch (tokenType) {
				case CypherLexer::EQ: pendingOp = BinaryOperatorType::BOP_EQUAL; hasPendingOp = true; break;
				case CypherLexer::NEQ: pendingOp = BinaryOperatorType::BOP_NOT_EQUAL; hasPendingOp = true; break;
				case CypherLexer::LT: pendingOp = BinaryOperatorType::BOP_LESS; hasPendingOp = true; break;
				case CypherLexer::GT: pendingOp = BinaryOperatorType::BOP_GREATER; hasPendingOp = true; break;
				case CypherLexer::LTE: pendingOp = BinaryOperatorType::BOP_LESS_EQUAL; hasPendingOp = true; break;
				case CypherLexer::GTE: pendingOp = BinaryOperatorType::BOP_GREATER_EQUAL; hasPendingOp = true; break;
				case CypherLexer::K_IN: pendingOp = BinaryOperatorType::BOP_IN; hasPendingOp = true; break;
				case CypherLexer::K_STARTS: pendingOp = BinaryOperatorType::BOP_STARTS_WITH; hasPendingOp = true; expectWith = true; break;
				case CypherLexer::K_ENDS: pendingOp = BinaryOperatorType::BOP_ENDS_WITH; hasPendingOp = true; expectWith = true; break;
				case CypherLexer::K_CONTAINS: pendingOp = BinaryOperatorType::BOP_CONTAINS; hasPendingOp = true; break;
				default: break;
			}
		}
	}

	// Handle IS NULL / IS NOT NULL as a postfix operator
	if (ctx->K_IS()) {
		bool isNot = (ctx->K_NOT() != nullptr);
		left = std::make_unique<IsNullExpression>(std::move(left), isNot);
	}

	return left;
}

std::unique_ptr<Expression> ExpressionBuilder::buildArithmeticExpression(CypherParser::ArithmeticExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto powerExprs = ctx->powerExpression();
	if (powerExprs.empty()) {
		return buildPowerExpression(ctx->powerExpression(0));
	}

	// Handle arithmetic operators: power (PLUS|MINUS|MULTIPLY|DIVIDE|MODULO power)*
	auto left = buildPowerExpression(powerExprs[0]);

	// Walk children in parse order to determine operator type by position
	size_t powerIdx = 0;
	BinaryOperatorType pendingOp = BinaryOperatorType::BOP_ADD;
	bool hasPendingOp = false;

	for (auto* child : ctx->children) {
		auto* termNode = dynamic_cast<antlr4::tree::TerminalNode*>(child);
		if (!termNode) {
			// This is a rule context (powerExpression)
			if (powerIdx == 0) {
				powerIdx++;
				continue; // Skip the first power expression (already in 'left')
			}
			if (!hasPendingOp) {
				powerIdx++;
				continue;
			}
			auto right = buildPowerExpression(powerExprs[powerIdx]);
			left = std::make_unique<BinaryOpExpression>(std::move(left), pendingOp, std::move(right));
			hasPendingOp = false;
			powerIdx++;
			continue;
		}

		// Terminal node - determine operator type
		switch (termNode->getSymbol()->getType()) {
			case CypherLexer::PLUS: pendingOp = BinaryOperatorType::BOP_ADD; hasPendingOp = true; break;
			case CypherLexer::MINUS: pendingOp = BinaryOperatorType::BOP_SUBTRACT; hasPendingOp = true; break;
			case CypherLexer::MULTIPLY: pendingOp = BinaryOperatorType::BOP_MULTIPLY; hasPendingOp = true; break;
			case CypherLexer::DIVIDE: pendingOp = BinaryOperatorType::BOP_DIVIDE; hasPendingOp = true; break;
			case CypherLexer::MODULO: pendingOp = BinaryOperatorType::BOP_MODULO; hasPendingOp = true; break;
			default: break;
		}
	}

	return left;
}

std::unique_ptr<Expression> ExpressionBuilder::buildPowerExpression(CypherParser::PowerExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto unaryExprs = ctx->unaryExpression();
	if (unaryExprs.empty()) {
		return buildUnaryExpression(ctx->unaryExpression(0));
	}

	// Handle power operator: unary (POWER unary)*
	// Power is right-associative, so we process from right to left
	auto right = buildUnaryExpression(unaryExprs[unaryExprs.size() - 1]);

	for (int i = static_cast<int>(unaryExprs.size()) - 2; i >= 0; --i) {
		auto left = buildUnaryExpression(unaryExprs[i]);
		right = std::make_unique<BinaryOpExpression>(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	}

	return right;
}

std::unique_ptr<Expression> ExpressionBuilder::buildUnaryExpression(CypherParser::UnaryExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Handle property access and variable reference
	// Accessors are attached to UnaryExpression
	if (ctx->atom() && ctx->atom()->variable()) {
		return buildVariableOrProperty(ctx);
	}

	auto atomExpr = buildAtomExpression(ctx->atom());

	// Handle unary plus/minus (single TerminalNode pointers, not arrays)
	if (ctx->PLUS() || ctx->MINUS()) {
		// Check if we have a minus (plus is a no-op)
		bool hasMinus = (ctx->MINUS() != nullptr);

		if (hasMinus) {
			// Special case: if the operand is a numeric literal, fold the negation
			// This handles cases like: -10, -3.14
			if (auto *litExpr = dynamic_cast<LiteralExpression*>(atomExpr.get())) {
				if (litExpr->isInteger()) {
					return std::make_unique<LiteralExpression>(-litExpr->getIntegerValue());
				} else if (litExpr->isDouble()) {
					return std::make_unique<LiteralExpression>(-litExpr->getDoubleValue());
				}
			}

			// Otherwise, create a unary negation expression
			return std::make_unique<UnaryOpExpression>(
				UnaryOperatorType::UOP_MINUS,
				std::move(atomExpr)
			);
		}
	}

	// Handle list slicing accessors on non-variable atoms (e.g., [1,2,3][0])
	if (!ctx->accessor().empty()) {
		auto accessors = ctx->accessor();
		auto accessorIt = std::find_if(accessors.begin(), accessors.end(),
			[](auto* a) { return a->LBRACK() != nullptr; });

		if (accessorIt != accessors.end()) {
			auto* accessorCtx = *accessorIt;
			return buildListSliceFromAccessor(accessorCtx, std::move(atomExpr));
		}
	}

	return atomExpr;
}

std::unique_ptr<Expression> ExpressionBuilder::buildAtomExpression(CypherParser::AtomContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Literal
	if (ctx->literal()) {
		auto lit = parseValue(ctx->literal());
		if (std::holds_alternative<std::monostate>(lit.getVariant())) {
			return std::make_unique<LiteralExpression>();
		} else if (std::holds_alternative<bool>(lit.getVariant())) {
			return std::make_unique<LiteralExpression>(std::get<bool>(lit.getVariant()));
		} else if (std::holds_alternative<int64_t>(lit.getVariant())) {
			return std::make_unique<LiteralExpression>(std::get<int64_t>(lit.getVariant()));
		} else if (std::holds_alternative<double>(lit.getVariant())) {
			return std::make_unique<LiteralExpression>(std::get<double>(lit.getVariant()));
		} else if (std::holds_alternative<std::string>(lit.getVariant())) {
			return std::make_unique<LiteralExpression>(std::get<std::string>(lit.getVariant()));
		} else if (lit.getType() == PropertyType::MAP) {
			// Map literals: {key: value, ...} - wrap in ListLiteralExpression (generic value wrapper)
			return std::make_unique<ListLiteralExpression>(std::move(lit));
		}
	}

	// Variable reference: n (will be handled by parent UnaryExpression)
	// Note: We don't handle variables here because accessors are attached to UnaryExpression

	// Parenthesized expression: (expr)
	if (ctx->expression()) {
		return buildExpression(ctx->expression());
	}

	// List literal: [1, 2, 3]
	if (ctx->listLiteral()) {
		return buildListLiteral(ctx->listLiteral());
	}

	// Function call: count(*), sum(n.salary)
	if (ctx->functionInvocation()) {
		return buildFunctionCall(ctx->functionInvocation());
	}

	// CASE expression: CASE WHEN...THEN...ELSE...END
	if (ctx->caseExpression()) {
		return buildCaseExpression(ctx->caseExpression());
	}

	// Special case: count(*) - COUNT keyword with MULTIPLY token
	if (ctx->K_COUNT() && ctx->MULTIPLY()) {
		// Create a FunctionCallExpression for count(*) with no arguments
		// The * indicates "count all rows" which is handled specially in the aggregate operator
		return std::make_unique<FunctionCallExpression>("count", std::vector<std::unique_ptr<Expression>>());
	}

	// Parameter: $param (not yet supported)
	if (ctx->parameter()) {
		throw std::runtime_error("Parameters ($param) are not yet supported");
	}

	// Quantifier functions: all(x IN list WHERE cond), any(...), none(...), single(...)
	if (ctx->quantifierExpression()) {
		return buildQuantifierExpression(ctx->quantifierExpression());
	}

	// List comprehension: [x IN list WHERE x > 5]
	if (ctx->listComprehension()) {
		return buildListComprehensionExpression(ctx->listComprehension(), evaluateLiteralExpression);
	}

	// Reduce expression: reduce(total = 0, x IN list | total + x)
	if (ctx->reduceExpression()) {
		return buildReduceExpression(ctx->reduceExpression());
	}

	// Pattern comprehension: [(n)-[:KNOWS]->(m) | m.name]
	if (ctx->patternComprehension()) {
		return buildPatternComprehension(ctx->patternComprehension());
	}

	// EXISTS expression: exists((n)-[:KNOWS]->())
	if (ctx->existsExpression()) {
		return buildExistsExpression(ctx->existsExpression());
	}

	throw std::runtime_error("Unsupported atom expression: " + ctx->getText());
}

std::unique_ptr<Expression> ExpressionBuilder::buildQuantifierExpression(CypherParser::QuantifierExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Determine function name from the keyword token
	std::string funcName;
	if (ctx->K_ALL()) funcName = "all";
	else if (ctx->K_ANY()) funcName = "any";
	else if (ctx->K_NONE()) funcName = "none";
	else if (ctx->K_SINGLE()) funcName = "single";
	else throw std::runtime_error("Unknown quantifier function in: " + ctx->getText());

	// Extract variable name
	std::string variable = ctx->variable()->getText();

	// Build list expression (first expression after IN)
	auto expressions = ctx->expression();
	auto listExpr = buildExpression(expressions[0]);

	// Build WHERE condition expression (second expression after WHERE)
	auto whereExpr = buildExpression(expressions[1]);

	return std::make_unique<QuantifierFunctionExpression>(
		funcName,
		variable,
		std::move(listExpr),
		std::move(whereExpr)
	);
}

std::unique_ptr<Expression> ExpressionBuilder::buildVariableOrProperty(CypherParser::UnaryExpressionContext *ctx) {
	if (!ctx || !ctx->atom()) {
		throw std::runtime_error("Invalid variable or property expression");
	}

	auto atom = ctx->atom();
	if (!atom->variable()) {
		throw std::runtime_error("Expected variable in expression");
	}

	std::string varName = atom->variable()->getText();

	// Check for property accessors: n.prop or n.prop[index]
	if (!ctx->accessor().empty()) {
		// Build the base expression starting with just the variable
		std::unique_ptr<Expression> baseExpr = std::make_unique<VariableReferenceExpression>(varName);

		// Process all accessors in order
		for (auto* accessor : ctx->accessor()) {
			// Property access: .prop
			if (accessor->DOT()) {
				std::string propName = accessor->propertyKeyName()->getText();

				// Check if there's a list indexing accessor after this property
				// We need to look ahead to see if the next accessor is a list bracket
				auto accessors = ctx->accessor();
				auto currentIt = std::find(accessors.begin(), accessors.end(), accessor);
				auto nextIt = currentIt + 1;

				if (nextIt != accessors.end() && (*nextIt)->LBRACK()) {
					// Next accessor is list indexing, so create property access now
					// and the list slice will be applied in the next iteration
					baseExpr = std::make_unique<VariableReferenceExpression>(varName, propName);
				} else {
					// No more accessors or next is not list indexing, just return the property access
					return std::make_unique<VariableReferenceExpression>(varName, propName);
				}
			}
			// List indexing/slicing: [0] or [0..2]
			else if (accessor->LBRACK()) {
				return buildListSliceFromAccessor(accessor, std::move(baseExpr));
			}
		}

		// If we processed all accessors and didn't return, return the base expression
		return baseExpr;
	}

	// Simple variable reference
	return std::make_unique<VariableReferenceExpression>(varName);
}

std::unique_ptr<Expression> ExpressionBuilder::buildListLiteral(CypherParser::ListLiteralContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Parse the list literal into a PropertyValue
	PropertyValue listValue = parseListLiteral(ctx);

	// Create and return a ListLiteralExpression
	return std::make_unique<ListLiteralExpression>(listValue);
}

std::unique_ptr<Expression> ExpressionBuilder::buildFunctionCall(CypherParser::FunctionInvocationContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	std::string functionName = ctx->functionName()->getText();

	// Convert to lowercase for comparison
	std::string functionNameLower = functionName;
	std::transform(functionNameLower.begin(), functionNameLower.end(),
	               functionNameLower.begin(), [](unsigned char c) { return std::tolower(c); });

	// Check if this is the EXISTS function
	if (functionNameLower == "exists") {
		// EXISTS has special syntax: EXISTS((n)-[:FRIENDS]->())
		// Parse the pattern argument to extract structured fields
		auto exprs = ctx->expression();
		if (!exprs.empty()) {
			std::string patternStr = exprs[0]->getText();

			// Parse pattern string to extract source var, rel type, target label, direction
			// Pattern format: (var)-[:TYPE]->([:Label]) or (var)<-[:TYPE]-() etc.
			std::string sourceVar, relType, targetLabel;
			PatternDirection direction = PatternDirection::PAT_BOTH;

			// Try to parse the pattern text using regex-like logic
			// Patterns: (n)-[:KNOWS]->(), (n)<-[:KNOWS]-(), (n)-[:KNOWS]-()
			auto parseExistsPattern = [&]() {
				std::string p = patternStr;
				// Find source variable: first (xxx)
				size_t firstParen = p.find('(');
				size_t closeParen = p.find(')');
				if (firstParen != std::string::npos && closeParen != std::string::npos) {
					std::string inside = p.substr(firstParen + 1, closeParen - firstParen - 1);
					// Remove label if present (e.g., "n:Person" -> "n")
					size_t colonPos = inside.find(':');
					if (colonPos != std::string::npos) {
						sourceVar = inside.substr(0, colonPos);
					} else {
						sourceVar = inside;
					}
				}

				// Find relationship type: [:TYPE]
				size_t lbrack = p.find('[');
				size_t rbrack = p.find(']');
				if (lbrack != std::string::npos && rbrack != std::string::npos) {
					std::string relContent = p.substr(lbrack + 1, rbrack - lbrack - 1);
					size_t colonInRel = relContent.find(':');
					if (colonInRel != std::string::npos) {
						relType = relContent.substr(colonInRel + 1);
					}
				}

				// Find direction: -> or <-
				if (p.find("->") != std::string::npos) {
					direction = PatternDirection::PAT_OUTGOING;
				} else if (p.find("<-") != std::string::npos) {
					direction = PatternDirection::PAT_INCOMING;
				}

				// Find target label: last (:Label)
				if (rbrack != std::string::npos) {
					size_t lastOpen = p.find('(', rbrack);
					size_t lastClose = p.find(')', lastOpen != std::string::npos ? lastOpen : 0);
					if (lastOpen != std::string::npos && lastClose != std::string::npos) {
						std::string targetInside = p.substr(lastOpen + 1, lastClose - lastOpen - 1);
						size_t colonPos = targetInside.find(':');
						if (colonPos != std::string::npos) {
							targetLabel = targetInside.substr(colonPos + 1);
						}
					}
				}
			};
			parseExistsPattern();

			// Build WHERE expression if present
			std::unique_ptr<Expression> whereExpr;
			if (exprs.size() >= 2) {
				whereExpr = buildExpression(exprs[1]);
			}

			return std::make_unique<ExistsExpression>(
				patternStr, sourceVar, relType, targetLabel, direction, std::move(whereExpr));
		}

		// If no arguments, return an empty EXISTS expression
		return std::make_unique<ExistsExpression>("");
	}

	// Check if this is a quantifier function (all, any, none, single)
	bool isQuantifier = (functionNameLower == "all" || functionNameLower == "any" ||
	                     functionNameLower == "none" || functionNameLower == "single");

	if (isQuantifier) {
		// Quantifier functions have special syntax: all(variable IN list WHERE condition)
		// Parse the arguments to extract the components
		auto exprs = ctx->expression();
		if (exprs.size() >= 2) {
			// We expect at least 2 arguments: variable and list expression
			// The third argument (if present) is the WHERE condition

			// First argument should be a variable reference
			auto firstArg = buildExpression(exprs[0]);
			if (auto *varRef = dynamic_cast<VariableReferenceExpression*>(firstArg.get())) {
				std::string variable = varRef->getVariableName();

				// Second argument should be the list expression
				auto listExpr = buildExpression(exprs[1]);

				// Third argument (if present) is the WHERE condition
				std::unique_ptr<Expression> whereExpr;
				if (exprs.size() >= 3) {
					whereExpr = buildExpression(exprs[2]);
				} else {
					// If no WHERE clause, use a literal true expression
					whereExpr = std::make_unique<LiteralExpression>(true);
				}

				return std::make_unique<QuantifierFunctionExpression>(
					functionNameLower,
					variable,
					std::move(listExpr),
					std::move(whereExpr)
				);
			}
		}

		// If we can't parse the special syntax, fall through to normal function call
		// This will result in an error at runtime
	}

	// Check for DISTINCT modifier in function call (e.g., count(DISTINCT expr))
	bool isDistinctFunc = (ctx->K_DISTINCT() != nullptr);

	// Parse arguments for normal function calls
	std::vector<std::unique_ptr<Expression>> arguments;
	auto exprs = ctx->expression();
	for (auto exprCtx : exprs) {
		arguments.push_back(buildExpression(exprCtx));
	}

	return std::make_unique<FunctionCallExpression>(functionName, std::move(arguments), isDistinctFunc);
}

bool ExpressionBuilder::isAggregateFunction(const std::string& functionName) {
	std::string nameLower = functionName;
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
	               [](unsigned char c) { return std::tolower(c); });

	return nameLower == "count" || nameLower == "sum" || nameLower == "avg" ||
	       nameLower == "min" || nameLower == "max" || nameLower == "collect";
}

std::vector<PropertyValue> ExpressionBuilder::extractListFromExpression(CypherParser::ExpressionContext *ctx) {
	std::vector<PropertyValue> results;

	// Navigate AST: expression -> or -> xor -> and -> not -> comparison -> arithmetic -> unary -> atom
	auto atom = getAtomFromExpression(ctx);
	if (atom && atom->listLiteral()) {
		auto listLit = atom->listLiteral();

		for (auto itemExpr : listLit->expression()) {
			// Recursively extract literal value from the item expression
			auto itemAtom = getAtomFromExpression(itemExpr);
			if (itemAtom && itemAtom->literal()) {
				results.push_back(parseValue(itemAtom->literal()));
			} else {
				// Fallback for non-literal items inside list (e.g. identifiers)
				results.push_back(PropertyValue(itemExpr->getText()));
			}
		}
	}
	// If not a list literal, return empty (could throw error in strict mode)

	return results;
}

CypherParser::AtomContext *ExpressionBuilder::getAtomFromExpression(CypherParser::ExpressionContext *e) {
	if (!e)
		return nullptr;

	auto or_ = e->orExpression();
	if (!or_)
		return nullptr;

	auto xor_ = or_->xorExpression(0);
	if (!xor_)
		return nullptr;

	auto and_ = xor_->andExpression(0);
	if (!and_)
		return nullptr;

	auto not_ = and_->notExpression(0);
	if (!not_)
		return nullptr;

	auto comp = not_->comparisonExpression();
	if (!comp)
		return nullptr;

	auto arith = comp->arithmeticExpression(0);
	if (!arith)
		return nullptr;

	auto power = arith->powerExpression(0);
	if (!power)
		return nullptr;

	auto unary = power->unaryExpression(0);
	if (!unary)
		return nullptr;

	return unary->atom();
}

PropertyValue ExpressionBuilder::parseValue(CypherParser::LiteralContext *ctx) {
	if (!ctx)
		return PropertyValue();

	if (ctx->StringLiteral()) {
		std::string s = ctx->StringLiteral()->getText();
		std::string raw = s.substr(1, s.length() - 2);
		// Process escape sequences per Cypher spec
		std::string unescaped;
		unescaped.reserve(raw.size());
		for (size_t i = 0; i < raw.size(); ++i) {
			if (raw[i] == '\\' && i + 1 < raw.size()) {
				char next = raw[i + 1];
				switch (next) {
					case 'b': unescaped += '\b'; ++i; break;
					case 't': unescaped += '\t'; ++i; break;
					case 'n': unescaped += '\n'; ++i; break;
					case 'f': unescaped += '\f'; ++i; break;
					case 'r': unescaped += '\r'; ++i; break;
					case '"': unescaped += '"'; ++i; break;
					case '\'': unescaped += '\''; ++i; break;
					case '\\': unescaped += '\\'; ++i; break;
					case 'u':
						if (i + 5 < raw.size()) {
							std::string hex = raw.substr(i + 2, 4);
							char16_t cp = static_cast<char16_t>(std::stoul(hex, nullptr, 16));
							if (cp < 0x80) {
								unescaped += static_cast<char>(cp);
							}
							i += 5;
						} else {
							unescaped += raw[i];
						}
						break;
					default:
						unescaped += raw[i];
						break;
				}
			} else {
				unescaped += raw[i];
			}
		}
		return PropertyValue(unescaped);
	}
	if (ctx->numberLiteral()) {
		std::string s = ctx->numberLiteral()->getText();
		// Check for decimal point or scientific notation (both lowercase 'e' and uppercase 'E')
		if (s.find('.') != std::string::npos || s.find('e') != std::string::npos || s.find('E') != std::string::npos) {
			// Force double constructor to avoid integer template deduction
			double dval = std::stod(s);
			return PropertyValue(dval);
		}
		return PropertyValue(std::stoll(s));
	}
	if (ctx->booleanLiteral()) {
		return PropertyValue(ctx->booleanLiteral()->K_TRUE() != nullptr);
	}
	if (ctx->K_NULL())
		return PropertyValue();

	if (ctx->mapLiteral()) {
		auto mapLit = ctx->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();
		std::unordered_map<std::string, PropertyValue> map;
		for (size_t i = 0; i < keys.size() && i < exprs.size(); ++i) {
			map[keys[i]->getText()] = evaluateLiteralExpression(exprs[i]);
		}
		return PropertyValue(std::move(map));
	}

	return PropertyValue();
}

PropertyValue ExpressionBuilder::parseListLiteral(CypherParser::ListLiteralContext *ctx) {
	if (!ctx)
		return PropertyValue();

	std::vector<PropertyValue> vec;

	// Evaluate each item in the list
	for (auto itemExpr : ctx->expression()) {
		// Recursively evaluate the item expression
		PropertyValue itemVal = evaluateLiteralExpression(itemExpr);

		// Store the PropertyValue directly for heterogeneous list support
		vec.push_back(itemVal);
	}

	return PropertyValue(std::move(vec));
}

std::unique_ptr<Expression> ExpressionBuilder::buildCaseExpression(CypherParser::CaseExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Get the WHEN...THEN pairs
	auto whenExprs = ctx->K_WHEN();  // Get all WHEN keywords
	auto thenExprs = ctx->expression();  // All expressions in order

	// Structure:
	// Simple CASE: CASE comparisonExpr WHEN expr1 THEN expr2 ... [ELSE expr] END
	// Searched CASE: CASE WHEN boolExpr1 THEN expr2 ... [ELSE expr] END

	// Check if there's a comparison expression (simple CASE)
	// Simple CASE: CASE expr WHEN ... — the first expression starts BEFORE the first WHEN keyword
	// Searched CASE: CASE WHEN ... — the first expression starts AFTER the first WHEN keyword
	std::unique_ptr<Expression> comparisonExpr = nullptr;
	size_t startIndex = 0;

	if (!thenExprs.empty() && !whenExprs.empty()) {
		auto firstExprStart = thenExprs[0]->getStart()->getTokenIndex();
		auto firstWhenStart = whenExprs[0]->getSymbol()->getTokenIndex();

		if (firstExprStart < firstWhenStart) {
			// First expression appears before first WHEN → simple CASE
			comparisonExpr = buildExpression(ctx->expression(0));
			startIndex = 1;
		}
	}

	// Create CaseExpression
	auto caseExpr = comparisonExpr
		? std::make_unique<CaseExpression>(std::move(comparisonExpr))
		: std::make_unique<CaseExpression>();

	// Process WHEN...THEN pairs
	size_t pairCount = whenExprs.size();
	for (size_t i = 0; i < pairCount; ++i) {
		// WHEN expression
		size_t whenIndex = startIndex + (i * 2);
		// THEN expression
		size_t thenIndex = whenIndex + 1;

		if (thenIndex >= ctx->expression().size()) {
			throw std::runtime_error("Invalid CASE expression: missing THEN expression");
		}

		auto whenExpr = buildExpression(ctx->expression(whenIndex));
		auto thenExpr = buildExpression(ctx->expression(thenIndex));

		caseExpr->addBranch(std::move(whenExpr), std::move(thenExpr));
	}

	// Handle ELSE clause if present
	size_t expectedEndIndex = startIndex + (pairCount * 2);
	if (expectedEndIndex < ctx->expression().size()) {
		// There's an ELSE expression
		auto elseExpr = buildExpression(ctx->expression(expectedEndIndex));
		caseExpr->setElseExpression(std::move(elseExpr));
	}

	return caseExpr;
}

std::unique_ptr<Expression> ExpressionBuilder::buildListSliceFromAccessor(
    CypherParser::AccessorContext* ctx,
    std::unique_ptr<Expression> baseExpr) {

    if (!ctx || !ctx->LBRACK()) {
        return nullptr;
    }

    // Get the expressions inside brackets
    auto expressions = ctx->expression();

    std::unique_ptr<Expression> startExpr = nullptr;
    std::unique_ptr<Expression> endExpr = nullptr;
    bool hasRange = (ctx->RANGE() != nullptr);

    if (hasRange) {
        // Has '..' - range slice: [start..end] or [..end] or [start..]
        size_t exprCount = expressions.size();
        if (exprCount > 0) {
            startExpr = buildExpression(expressions[0]);
        }
        if (exprCount > 1) {
            endExpr = buildExpression(expressions[1]);
        }
    } else {
        // Single index: [index]
        if (expressions.size() > 0) {
            startExpr = buildExpression(expressions[0]);
        }
    }

    return std::make_unique<ListSliceExpression>(
        std::move(baseExpr),
        std::move(startExpr),
        std::move(endExpr),
        hasRange
    );
}

std::unique_ptr<Expression> ExpressionBuilder::buildListComprehensionExpression(
		CypherParser::ListComprehensionContext* ctx,
		const std::function<PropertyValue(CypherParser::ExpressionContext*)>& /*evaluateLiteral*/) {

	if (!ctx) return nullptr;

	// Extract variable name
	std::string variable = ctx->variable()->getText();

	// Build list expression
	auto listExpr = buildExpression(ctx->expression(0));

	// Build WHERE expression if present
	std::unique_ptr<Expression> whereExpr = nullptr;
	if (ctx->whereExpression) {
		whereExpr = buildExpression(ctx->whereExpression);
	}

	// Build MAP expression if present
	std::unique_ptr<Expression> mapExpr = nullptr;
	if (ctx->mapExpression) {
		mapExpr = buildExpression(ctx->mapExpression);
	}

	// Determine type
	ListComprehensionExpression::ComprehensionType type;
	if (mapExpr) {
		type = ListComprehensionExpression::ComprehensionType::COMP_EXTRACT;
	} else if (whereExpr) {
		type = ListComprehensionExpression::ComprehensionType::COMP_FILTER;
	} else {
		type = ListComprehensionExpression::ComprehensionType::COMP_EXTRACT;  // Default
	}

	return std::make_unique<ListComprehensionExpression>(
			variable, std::move(listExpr), std::move(whereExpr), std::move(mapExpr), type);
}

std::unique_ptr<Expression> ExpressionBuilder::buildReduceExpression(CypherParser::ReduceExpressionContext *ctx) {
	if (!ctx) return nullptr;

	// Grammar: K_REDUCE LPAREN variable EQ expression COMMA variable K_IN expression PIPE expression RPAREN
	auto variables = ctx->variable();
	auto expressions = ctx->expression();

	std::string accumulator = variables[0]->getText();
	auto initialExpr = buildExpression(expressions[0]);

	std::string iterVar = variables[1]->getText();
	auto listExpr = buildExpression(expressions[1]);
	auto bodyExpr = buildExpression(expressions[2]);

	return std::make_unique<ReduceExpression>(
		accumulator,
		std::move(initialExpr),
		iterVar,
		std::move(listExpr),
		std::move(bodyExpr)
	);
}

std::unique_ptr<Expression> ExpressionBuilder::buildPatternComprehension(CypherParser::PatternComprehensionContext *ctx) {
	if (!ctx) return nullptr;

	// Grammar: LBRACK patternElement (K_WHERE expression)? PIPE expression RBRACK
	// patternElement: nodePattern patternElementChain*
	auto patternElem = ctx->patternElement();

	// Determine which expression is WHERE and which is MAP
	auto expressions = ctx->expression();
	std::unique_ptr<Expression> mapExpr;
	std::unique_ptr<Expression> whereExpr;
	if (ctx->K_WHERE() && expressions.size() >= 2) {
		whereExpr = buildExpression(expressions[0]);
		mapExpr = buildExpression(expressions[1]);
	} else if (!expressions.empty()) {
		mapExpr = buildExpression(expressions[expressions.size() - 1]);
	}

	// Parse the pattern to extract structured info
	std::string sourceVar;
	std::string targetVar;
	std::string relType;
	std::string targetLabel;
	PatternDirection direction = PatternDirection::PAT_BOTH;

	// Get source node pattern
	auto nodePattern = patternElem->nodePattern();
	if (nodePattern && nodePattern->variable()) {
		sourceVar = nodePattern->variable()->getText();
	}

	// Get pattern chain (relationship + target node)
	auto chains = patternElem->patternElementChain();
	if (!chains.empty()) {
		auto chain = chains[0];

		// Parse relationship
		auto relPattern = chain->relationshipPattern();
		if (relPattern) {
			// Determine direction from arrow tokens
			bool hasLeft = (relPattern->LT() != nullptr);
			bool hasRight = (relPattern->GT() != nullptr);
			if (hasRight && !hasLeft) {
				direction = PatternDirection::PAT_OUTGOING;
			} else if (hasLeft && !hasRight) {
				direction = PatternDirection::PAT_INCOMING;
			}

			// Get relationship type
			auto relDetail = relPattern->relationshipDetail();
			if (relDetail && relDetail->relationshipTypes()) {
				auto relTypes = relDetail->relationshipTypes()->relTypeName();
				if (!relTypes.empty()) {
					relType = relTypes[0]->getText();
				}
			}
		}

		// Parse target node
		auto targetNodePattern = chain->nodePattern();
		if (targetNodePattern) {
			if (targetNodePattern->variable()) {
				targetVar = targetNodePattern->variable()->getText();
			}
			if (targetNodePattern->nodeLabels()) {
				auto labels = targetNodePattern->nodeLabels()->nodeLabel();
				if (!labels.empty()) {
					targetLabel = labels[0]->labelName()->getText();
				}
			}
		}
	}

	std::string pattern = patternElem->getText();

	return std::make_unique<PatternComprehensionExpression>(
		pattern,
		sourceVar,
		targetVar,
		relType,
		targetLabel,
		direction,
		std::move(mapExpr),
		nullptr  // no WHERE clause in basic pattern comprehension syntax
	);
}

std::unique_ptr<Expression> ExpressionBuilder::buildExistsExpression(CypherParser::ExistsExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto patternElem = ctx->patternElement();
	if (!patternElem) {
		return std::make_unique<ExistsExpression>("");
	}

	std::string patternStr = patternElem->getText();

	// Extract structured pattern info
	std::string sourceVar, relType, targetLabel;
	PatternDirection direction = PatternDirection::PAT_BOTH;

	// Parse source node
	auto nodePattern = patternElem->nodePattern();
	if (nodePattern && nodePattern->variable()) {
		sourceVar = nodePattern->variable()->getText();
	}

	// Parse relationship chain
	auto chains = patternElem->patternElementChain();
	if (!chains.empty()) {
		auto chain = chains[0];
		auto relPattern = chain->relationshipPattern();
		if (relPattern) {
			// Determine direction from arrow tokens
			bool hasLeft = (relPattern->LT() != nullptr);
			bool hasRight = (relPattern->GT() != nullptr);
			if (hasRight && !hasLeft) {
				direction = PatternDirection::PAT_OUTGOING;
			} else if (hasLeft && !hasRight) {
				direction = PatternDirection::PAT_INCOMING;
			}

			// Relationship type
			auto relDetail = relPattern->relationshipDetail();
			if (relDetail && relDetail->relationshipTypes()) {
				auto relTypes = relDetail->relationshipTypes()->relTypeName();
				if (!relTypes.empty()) {
					relType = relTypes[0]->getText();
				}
			}
		}

		// Target node label
		auto targetNode = chain->nodePattern();
		if (targetNode) {
			if (targetNode->nodeLabels()) {
				auto labels = targetNode->nodeLabels()->nodeLabel();
				if (!labels.empty()) {
					targetLabel = labels[0]->labelName()->getText();
				}
			}
		}
	}

	// Build WHERE expression if present
	std::unique_ptr<Expression> whereExpr;
	if (ctx->K_WHERE()) {
		whereExpr = buildExpression(ctx->expression());
	}

	return std::make_unique<ExistsExpression>(
		patternStr, sourceVar, relType, targetLabel, direction, std::move(whereExpr));
}

} // namespace graph::parser::cypher::helpers
