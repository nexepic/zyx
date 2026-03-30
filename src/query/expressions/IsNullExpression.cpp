/**
 * @file IsNullExpression.cpp
 * @date 2025/03/05
 *
 * @copyright Copyright (c) 2025
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

#include "graph/query/expressions/IsNullExpression.hpp"

namespace graph::query::expressions {

IsNullExpression::IsNullExpression(std::unique_ptr<Expression> expr, bool isNegated)
	: expr_(std::move(expr))
	, isNegated_(isNegated) {
}

void IsNullExpression::accept(ExpressionVisitor& visitor) {
	visitor.visit(this);
}

void IsNullExpression::accept(ConstExpressionVisitor& visitor) const {
	visitor.visit(this);
}

std::unique_ptr<Expression> IsNullExpression::clone() const {
	return std::make_unique<IsNullExpression>(
		expr_ ? expr_->clone() : nullptr,
		isNegated_
	);
}

ExpressionType IsNullExpression::getExpressionType() const {
	return ExpressionType::IS_NULL;
}

std::string IsNullExpression::toString() const {
	std::string result;
	if (expr_) {
		result = expr_->toString();
	}
	result += isNegated_ ? " IS NOT NULL" : " IS NULL";
	return result;
}

} // namespace graph::query::expressions
