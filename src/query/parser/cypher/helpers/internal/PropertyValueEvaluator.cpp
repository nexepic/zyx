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
#include "../ExpressionBuilder.hpp"

namespace graph::parser::cypher::helpers {

PropertyValue PropertyValueEvaluator::evaluate(CypherParser::ExpressionContext *ctx) {
	// DEPRECATED: This class is now a thin wrapper around ExpressionBuilder::evaluateLiteralExpression
	// Use ExpressionBuilder::evaluateLiteralExpression() directly for new code
	return ExpressionBuilder::evaluateLiteralExpression(ctx);
}

CypherParser::AtomContext *PropertyValueEvaluator::getAtomFromExpression(CypherParser::ExpressionContext *e) {
	// DEPRECATED: Use ExpressionBuilder::getAtomFromExpression() instead
	return ExpressionBuilder::getAtomFromExpression(e);
}

PropertyValue PropertyValueEvaluator::parseLiteral(CypherParser::LiteralContext *ctx) {
	// DEPRECATED: Use ExpressionBuilder::parseValue() instead
	return ExpressionBuilder::parseValue(ctx);
}

PropertyValue PropertyValueEvaluator::parseListLiteral(CypherParser::ListLiteralContext *ctx) {
	// DEPRECATED: This method is no longer supported
	// Return empty value for null, throw error for non-null contexts
	if (!ctx) {
		return PropertyValue();
	}
	throw std::runtime_error("parseListLiteral(ListLiteralContext) is deprecated. Use evaluateLiteralExpression(ExpressionContext) instead.");
}

} // namespace graph::parser::cypher::helpers
