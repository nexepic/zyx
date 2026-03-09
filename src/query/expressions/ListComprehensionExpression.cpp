#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::query::expressions {

ListComprehensionExpression::ListComprehensionExpression(
		std::string variable,
		std::unique_ptr<Expression> listExpr,
		std::unique_ptr<Expression> whereExpr,
		std::unique_ptr<Expression> mapExpr,
		ComprehensionType type)
	: variable_(std::move(variable))
	, listExpr_(std::move(listExpr))
	, whereExpr_(std::move(whereExpr))
	, mapExpr_(std::move(mapExpr))
	, type_(type) {
}

void ListComprehensionExpression::accept(ExpressionVisitor& visitor) {
	visitor.visit(this);
}

void ListComprehensionExpression::accept(ConstExpressionVisitor& visitor) const {
	visitor.visit(this);
}

std::string ListComprehensionExpression::toString() const {
	std::string result;

	switch (type_) {
		case ComprehensionType::COMP_FILTER:
			result = "[" + variable_ + " IN " + listExpr_->toString();
			if (whereExpr_) {
				result += " WHERE " + whereExpr_->toString();
			}
			result += "]";
			break;
		case ComprehensionType::COMP_EXTRACT:
			result = "[" + variable_ + " IN " + listExpr_->toString();
			if (whereExpr_) {
				result += " WHERE " + whereExpr_->toString();
			}
			if (mapExpr_) {
				result += " | " + mapExpr_->toString();
			}
			result += "]";
			break;
		case ComprehensionType::COMP_REDUCE:
			result = "reduce(" + variable_ + " = " + listExpr_->toString();
			if (mapExpr_) {
				result += " | " + mapExpr_->toString();
			}
			result += ")";
			break;
	}

	return result;
}

std::unique_ptr<Expression> ListComprehensionExpression::clone() const {
	return std::make_unique<ListComprehensionExpression>(
		variable_,
		listExpr_ ? listExpr_->clone() : nullptr,
		whereExpr_ ? whereExpr_->clone() : nullptr,
		mapExpr_ ? mapExpr_->clone() : nullptr,
		type_
	);
}

} // namespace graph::query::expressions
