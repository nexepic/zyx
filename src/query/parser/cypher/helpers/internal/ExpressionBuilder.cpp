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

namespace graph::parser::cypher::helpers {

std::function<bool(const query::execution::Record &)> ExpressionBuilder::buildWherePredicate(
	CypherParser::ExpressionContext *expr,
	std::string &outDesc) {

	if (!expr)
		throw std::runtime_error("Invalid WHERE expression tree");

	// 1. Navigate down to Comparison
	auto comparison = expr->orExpression()->xorExpression(0)->andExpression(0)->notExpression(0)->comparisonExpression();

	if (!comparison)
		throw std::runtime_error("Invalid WHERE expression tree");

	// Ensure Binary Operation (LHS op RHS)
	if (comparison->arithmeticExpression().size() < 2) {
		throw std::runtime_error("WHERE clause currently only supports binary comparisons (e.g., n.age > 10)");
	}

	auto lhsArith = comparison->arithmeticExpression(0);
	auto rhsArith = comparison->arithmeticExpression(1);

	// 2. Parse LHS (Left Hand Side) - Expecting: n.prop
	// Structure: unary -> atom (variable) -> accessor -> propertyKeyName
	auto lhsUnary = lhsArith->unaryExpression(0);

	// Check for Atom (Variable 'n')
	if (!lhsUnary->atom() || !lhsUnary->atom()->variable()) {
		throw std::runtime_error("LHS must start with a variable (e.g., 'n.age')");
	}
	std::string varName = lhsUnary->atom()->variable()->getText();

	// Check for Accessor (Property '.age')
	if (lhsUnary->accessor().empty()) {
		throw std::runtime_error("LHS must be a property (e.g., 'n.age', not just 'n')");
	}

	// Get the first accessor
	auto firstAccessor = lhsUnary->accessor(0);

	// Verify it is a PropertyAccess (DOT name), not a ListIndex ([0])
	if (firstAccessor->DOT() == nullptr) {
		throw std::runtime_error("Array indexing (e.g. n.tags[0]) not supported in WHERE yet.");
	}

	std::string propKey = firstAccessor->propertyKeyName()->getText();

	// 3. Parse RHS (Right Hand Side) - Expecting: Literal
	// Structure: unary -> atom -> literal
	auto rhsUnary = rhsArith->unaryExpression(0);
	auto rhsAtom = rhsUnary->atom();

	if (!rhsAtom || !rhsAtom->literal()) {
		throw std::runtime_error("RHS of WHERE must be a literal value (e.g., 10, 'text')");
	}
	auto literalVal = parseValue(rhsAtom->literal());

	// 4. Parse Operator
	std::string op = "==";
	if (!comparison->EQ().empty())
		op = "=";
	else if (!comparison->NEQ().empty())
		op = "<>";
	else if (!comparison->LT().empty())
		op = "<";
	else if (!comparison->GT().empty())
		op = ">";
	else if (!comparison->LTE().empty())
		op = "<=";
	else if (!comparison->GTE().empty())
		op = ">=";

	// 5. Build Result
	outDesc = varName + "." + propKey + " " + op + " " + literalVal.toString();

	return [varName, propKey, literalVal, op](const query::execution::Record &r) -> bool {
		auto n = r.getNode(varName);
		if (!n)
			return false;

		const auto &props = n->getProperties();
		auto it = props.find(propKey);
		if (it == props.end())
			return false; // Property missing

		const auto &val = it->second;

		if (op == "=")
			return val == literalVal;
		if (op == "<>")
			return val != literalVal;
		if (op == ">")
			return val > literalVal;
		if (op == "<")
			return val < literalVal;
		if (op == ">=")
			return val >= literalVal;
		if (op == "<=")
			return val <= literalVal;

		return false;
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
