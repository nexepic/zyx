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
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
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

	// Handle comparison operators: arith (COMPARE arith)* (IS_NULL)?
	auto left = buildArithmeticExpression(arithExprs[0]);

	// Process binary comparison operators (EQ, NEQ, LT, GT, LTE, GTE, IN)
	if (!ctx->children.empty() && arithExprs.size() > 1) {
		// Find the operator token in the children
		for (size_t i = 1; i < arithExprs.size(); ++i) {
			BinaryOperatorType op = BinaryOperatorType::BOP_ADD; // default

			// Check if this is an IN operator with a list literal
			if (!ctx->K_IN().empty() && i - 1 < ctx->K_IN().size()) {
				// Check if RHS is a list literal
				auto rhsArith = arithExprs[i];
				// Get the first (and should be only) power expression from the arithmetic expression
				auto rhsPowerList = rhsArith->powerExpression();
				if (!rhsPowerList.empty()) {
					auto rhsUnaryList = rhsPowerList[0]->unaryExpression();
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
				}
				// If not a list literal, fall through to normal handling
			}

			// Determine the operator by checking which token is present
			if (i - 1 < ctx->EQ().size()) op = BinaryOperatorType::BOP_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size()) op = BinaryOperatorType::BOP_NOT_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size()) op = BinaryOperatorType::BOP_LESS;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size()) op = BinaryOperatorType::BOP_GREATER;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size() + ctx->LTE().size()) op = BinaryOperatorType::BOP_LESS_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size() + ctx->LTE().size() + ctx->GTE().size()) op = BinaryOperatorType::BOP_GREATER_EQUAL;
			else if (i - 1 < ctx->EQ().size() + ctx->NEQ().size() + ctx->LT().size() + ctx->GT().size() + ctx->LTE().size() + ctx->GTE().size() + ctx->K_IN().size()) {
				op = BinaryOperatorType::BOP_IN;
			}

			auto right = buildArithmeticExpression(arithExprs[i]);
			left = std::make_unique<BinaryOpExpression>(std::move(left), op, std::move(right));
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

	// Process operators from left to right
	for (size_t i = 1; i < powerExprs.size(); ++i) {
		BinaryOperatorType op = BinaryOperatorType::BOP_ADD; // default

		// Determine the operator
		size_t plusCount = ctx->PLUS().size();
		size_t minusCount = ctx->MINUS().size();
		size_t multiplyCount = ctx->MULTIPLY().size();
		size_t divideCount = ctx->DIVIDE().size();
		size_t moduloCount = ctx->MODULO().size();

		// Simple approach: use position to determine operator
		size_t opIndex = i - 1;
		if (opIndex < plusCount) op = BinaryOperatorType::BOP_ADD;
		else if (opIndex < plusCount + minusCount) op = BinaryOperatorType::BOP_SUBTRACT;
		else if (opIndex < plusCount + minusCount + multiplyCount) op = BinaryOperatorType::BOP_MULTIPLY;
		else if (opIndex < plusCount + minusCount + multiplyCount + divideCount) op = BinaryOperatorType::BOP_DIVIDE;
		else if (opIndex < plusCount + minusCount + multiplyCount + divideCount + moduloCount) op = BinaryOperatorType::BOP_MODULO;

		auto right = buildPowerExpression(powerExprs[i]);
		left = std::make_unique<BinaryOpExpression>(std::move(left), op, std::move(right));
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

	// List comprehension: [x IN list WHERE x > 5]
	if (ctx->listComprehension()) {
		return buildListComprehensionExpression(ctx->listComprehension(), evaluateLiteralExpression);
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
		// For now, extract the pattern string and create an ExistsExpression
		auto exprs = ctx->expression();
		if (!exprs.empty()) {
			// Get the pattern string from the first argument
			std::string pattern = exprs[0]->getText();

			// Check if there's a WHERE clause (second argument)
			std::unique_ptr<Expression> whereExpr;
			if (exprs.size() >= 2) {
				whereExpr = buildExpression(exprs[1]);
			}

			return std::make_unique<ExistsExpression>(pattern, std::move(whereExpr));
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

	// Parse arguments for normal function calls
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
	std::unique_ptr<Expression> comparisonExpr = nullptr;
	size_t startIndex = 0;

	if (ctx->expression().size() > whenExprs.size() * 2) {
		// First expression is the comparison expression
		// Format: CASE expr WHEN expr THEN expr [WHEN expr THEN expr]* [ELSE expr] END
		comparisonExpr = buildExpression(ctx->expression(0));
		startIndex = 1;
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
		const std::function<PropertyValue(CypherParser::ExpressionContext*)>& evaluateLiteral) {

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

} // namespace graph::parser::cypher::helpers
