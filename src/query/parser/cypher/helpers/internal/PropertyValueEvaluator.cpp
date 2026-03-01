/**
 * @file PropertyValueEvaluator.cpp
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

#include "../PropertyValueEvaluator.hpp"
#include "../AstExtractor.hpp"

namespace graph::parser::cypher::helpers {

PropertyValue PropertyValueEvaluator::evaluate(CypherParser::ExpressionContext *ctx) {
	if (!ctx)
		return PropertyValue();

	// Navigate to the unary expression level
	auto or_ = ctx->orExpression();
	if (!or_)
		return PropertyValue();

	auto xor_ = or_->xorExpression(0);
	if (!xor_)
		return PropertyValue();

	auto and_ = xor_->andExpression(0);
	if (!and_)
		return PropertyValue();

	auto not_ = and_->notExpression(0);
	if (!not_)
		return PropertyValue();

	auto comp = not_->comparisonExpression();
	if (!comp)
		return PropertyValue();

	auto arith = comp->arithmeticExpression(0);
	if (!arith)
		return PropertyValue();

	auto unary = arith->unaryExpression(0);
	if (!unary)
		return PropertyValue();

	// Check for unary minus (e.g., -10, -3.14)
	bool hasUnaryMinus = (unary->MINUS() != nullptr);

	// Navigate to the atom
	auto atom = unary->atom();
	if (!atom)
		return PropertyValue(); // Complex expressions not supported yet

	// Case A: Literal (String, Number, Bool, Null)
	if (atom->literal()) {
		PropertyValue result = parseLiteral(atom->literal());

		// Apply unary negation if needed
		if (hasUnaryMinus) {
			if (result.getType() == PropertyType::INTEGER) {
				result = PropertyValue(-std::get<int64_t>(result.getVariant()));
			} else if (result.getType() == PropertyType::DOUBLE) {
				result = PropertyValue(-std::get<double>(result.getVariant()));
			}
			// For non-numeric types, unary minus doesn't make sense - leave as-is
		}

		return result;
	}

	// Case B: List Literal ([1, 2, 3])
	if (atom->listLiteral()) {
		return parseListLiteral(atom->listLiteral());
	}

	// Case C: Other types not yet supported (variables, function calls, etc.)
	return PropertyValue();
}

CypherParser::AtomContext *PropertyValueEvaluator::getAtomFromExpression(CypherParser::ExpressionContext *e) {
	if (!e)
		return nullptr;

	// Navigate the chain: Expression -> Or -> Xor -> And -> Not -> Comparison -> Arithmetic -> Unary -> Atom
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

PropertyValue PropertyValueEvaluator::parseLiteral(CypherParser::LiteralContext *ctx) {
	return AstExtractor::parseValue(ctx);
}

PropertyValue PropertyValueEvaluator::parseListLiteral(CypherParser::ListLiteralContext *ctx) {
	if (!ctx)
		return PropertyValue();

	std::vector<float> vec;

	// Evaluate each item in the list
	for (auto itemExpr : ctx->expression()) {
		// Recursively evaluate the item expression
		PropertyValue itemVal = evaluate(itemExpr);

		// Convert to float for vector storage
		if (itemVal.getType() == PropertyType::INTEGER) {
			vec.push_back(static_cast<float>(std::get<int64_t>(itemVal.getVariant())));
		} else if (itemVal.getType() == PropertyType::DOUBLE) {
			vec.push_back(static_cast<float>(std::get<double>(itemVal.getVariant())));
		// LCOV_EXCL_LINE - Edge case: non-numeric items in lists, difficult to test in unit tests
		} else {
			// Try parsing as string if text
			try {
				vec.push_back(std::stof(itemVal.toString()));
			} catch (...) {
				// Skip non-numeric items
			}
		}
	}

	return PropertyValue(std::move(vec));
}

} // namespace graph::parser::cypher::helpers
