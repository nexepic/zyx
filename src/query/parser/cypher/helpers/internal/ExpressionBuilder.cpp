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
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"

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
			BinaryOperatorType::OR,
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
			BinaryOperatorType::XOR,
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
			BinaryOperatorType::AND,
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
			UnaryOperatorType::NOT,
			std::move(operand)
		);
	}

	return operand;
}

std::unique_ptr<Expression> ExpressionBuilder::buildComparisonExpression(CypherParser::ComparisonExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto unaryExprs = ctx->arithmeticExpression();
	if (unaryExprs.empty()) {
		return buildArithmeticExpression(ctx->arithmeticExpression(0));
	}

	// Handle comparison operators: arith (COMPARE arith)*
	auto left = buildArithmeticExpression(unaryExprs[0]);

	// Check for comparison operators
	if (!ctx->children.empty()) {
		// Find the operator token in the children
		for (size_t i = 1; i < unaryExprs.size(); ++i) {
			BinaryOperatorType op = BinaryOperatorType::ADD; // default

			// Check if this is an IN operator with a list literal
			if (!ctx->K_IN().empty() && i - 1 < ctx->K_IN().size()) {
				// Check if RHS is a list literal
				auto rhsArith = unaryExprs[i];
				// Get the first (and should be only) unary expression from the arithmetic expression
				auto rhsUnaryList = rhsArith->unaryExpression();
				if (!rhsUnaryList.empty() && rhsUnaryList[0]->atom() && rhsUnaryList[0]->atom()->listLiteral()) {
					// Extract list values
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

					// Create an InExpression
					left = std::make_unique<InExpression>(std::move(left), std::move(listValues));
					continue;
				}
				// If not a list literal, fall through to normal handling
			}

			// Determine the operator by checking which token is present
			if (i - 1 < ctx->EQ().size()) op = BinaryOperatorType::EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size()) op = BinaryOperatorType::NOT_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size()) op = BinaryOperatorType::LESS;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size()) op = BinaryOperatorType::GREATER;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size() + ctx->LTE().size()) op = BinaryOperatorType::LESS_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size() + ctx->LTE().size() + ctx->GTE().size()) op = BinaryOperatorType::GREATER_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size() + ctx->LTE().size() + ctx->GTE().size() + ctx->K_IN().size()) {
				op = BinaryOperatorType::IN;
			}

			auto right = buildArithmeticExpression(unaryExprs[i]);
			left = std::make_unique<BinaryOpExpression>(std::move(left), op, std::move(right));
		}
	}

	return left;
}

std::unique_ptr<Expression> ExpressionBuilder::buildArithmeticExpression(CypherParser::ArithmeticExpressionContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	auto unaryExprs = ctx->unaryExpression();
	if (unaryExprs.empty()) {
		return buildUnaryExpression(ctx->unaryExpression(0));
	}

	// Handle arithmetic operators: unary (PLUS|MINUS|MULTIPLY|DIVIDE|MODULO unary)*
	auto left = buildUnaryExpression(unaryExprs[0]);

	// Process operators from left to right
	// Note: The grammar has all operators at the same level, so we need to respect precedence
	// For now, process in order (proper precedence handling requires more complex logic)
	for (size_t i = 1; i < unaryExprs.size(); ++i) {
		BinaryOperatorType op = BinaryOperatorType::ADD; // default

		// Determine the operator
		// In the actual grammar, operators are mixed together, so we need to count them
		size_t plusCount = ctx->PLUS().size();
		size_t minusCount = ctx->MINUS().size();
		size_t multiplyCount = ctx->MULTIPLY().size();
		size_t divideCount = ctx->DIVIDE().size();
		size_t moduloCount = ctx->MODULO().size();

		// Simple approach: use position to determine operator
		// This is a simplification - proper handling would require parsing the tree structure
		size_t opIndex = i - 1;
		if (opIndex < plusCount) op = BinaryOperatorType::ADD;
		else if (opIndex < plusCount + minusCount) op = BinaryOperatorType::SUBTRACT;
		else if (opIndex < plusCount + minusCount + multiplyCount) op = BinaryOperatorType::MULTIPLY;
		else if (opIndex < plusCount + minusCount + multiplyCount + divideCount) op = BinaryOperatorType::DIVIDE;
		else if (opIndex < plusCount + minusCount + multiplyCount + divideCount + moduloCount) op = BinaryOperatorType::MODULO;

		auto right = buildUnaryExpression(unaryExprs[i]);
		left = std::make_unique<BinaryOpExpression>(std::move(left), op, std::move(right));
	}

	return left;
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
				UnaryOperatorType::MINUS,
				std::move(atomExpr)
			);
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

	throw std::runtime_error("Unsupported atom expression: " + ctx->getText());
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

	// Check for property accessors: n.prop
	if (!ctx->accessor().empty()) {
		auto firstAccessor = ctx->accessor(0);

		// Property access: n.prop
		if (firstAccessor->DOT()) {
			std::string propName = firstAccessor->propertyKeyName()->getText();
			return std::make_unique<VariableReferenceExpression>(varName, propName);
		}
		// List indexing: n.tags[0] (not yet supported)
		else if (firstAccessor->LBRACK()) {
			throw std::runtime_error("List indexing is not yet supported");
		}
	}

	// Simple variable reference
	return std::make_unique<VariableReferenceExpression>(varName);
}

std::unique_ptr<Expression> ExpressionBuilder::buildListLiteral(CypherParser::ListLiteralContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	// Build a list literal expression
	// For now, we'll handle it as a special case in the IN operator
	// Store the list items for later use
	std::vector<PropertyValue> listValues;

	for (auto exprCtx : ctx->expression()) {
		auto atom = getAtomFromExpression(exprCtx);
		if (atom && atom->literal()) {
			listValues.push_back(parseValue(atom->literal()));
		} else {
			throw std::runtime_error("List literals with expressions are not yet supported");
		}
	}

	// For now, throw an error - list literals will be handled specially by IN operator
	// In the future, we could create a ListLiteralExpression
	throw std::runtime_error("List literals should only be used with IN operator");
}

std::unique_ptr<Expression> ExpressionBuilder::buildFunctionCall(CypherParser::FunctionInvocationContext *ctx) {
	if (!ctx) {
		return nullptr;
	}

	std::string functionName = ctx->functionName()->getText();

	// Parse arguments
	std::vector<std::unique_ptr<Expression>> arguments;
	auto exprs = ctx->expression();
	for (auto exprCtx : exprs) {
		arguments.push_back(buildExpression(exprCtx));
	}

	return std::make_unique<FunctionCallExpression>(functionName, std::move(arguments));
}

bool ExpressionBuilder::isAggregateFunction(const std::string& functionName) {
	std::string nameLower = functionName;
	std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
	               [](unsigned char c) { return std::tolower(c); });

	return nameLower == "count" || nameLower == "sum" || nameLower == "avg" ||
	       nameLower == "min" || nameLower == "max" || nameLower == "collect";
}

// ============================================================================
// LEGACY API: Direct predicate generation (uses new AST internally)
// ============================================================================

std::function<bool(const query::execution::Record &)> ExpressionBuilder::buildWherePredicate(
	CypherParser::ExpressionContext *expr,
	std::string &outDesc) {

	// Build the AST using the new API
	auto ast = buildExpression(expr);
	outDesc = ast->toString();

	// Convert to shared_ptr to make the lambda copyable
	auto astShared = std::shared_ptr<Expression>(ast.release());

	// Return a lambda that uses ExpressionEvaluator
	return [astShared](const query::execution::Record &record) -> bool {
		EvaluationContext context(record);
		ExpressionEvaluator evaluator(context);
		PropertyValue result = evaluator.evaluate(astShared.get());
		return EvaluationContext::toBoolean(result);
	};
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

	auto unary = arith->unaryExpression(0);
	if (!unary)
		return nullptr;

	return unary->atom();
}

PropertyValue ExpressionBuilder::parseValue(CypherParser::LiteralContext *ctx) {
	if (!ctx)
		return PropertyValue();

	if (ctx->StringLiteral()) {
		std::string s = ctx->StringLiteral()->getText();
		return PropertyValue(s.substr(1, s.length() - 2));
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

	return PropertyValue();
}

} // namespace graph::parser::cypher::helpers
