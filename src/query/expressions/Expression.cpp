/**
 * @file Expression.cpp
 * @author Metrix Contributors
 * @date 2025
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

#include "graph/query/expressions/Expression.hpp"
#include <sstream>

namespace graph::query::expressions {

// ============================================================================
// LiteralExpression Implementation
// ============================================================================

LiteralExpression::LiteralExpression(std::string value)
	: stringValue_(std::move(value)), integerValue_(0), doubleValue_(0.0), booleanValue_(false),
	  literalType_(LiteralType::STRING) {}

LiteralExpression::LiteralExpression(int64_t value)
	: stringValue_(), integerValue_(value), doubleValue_(0.0), booleanValue_(false),
	  literalType_(LiteralType::INTEGER) {}

LiteralExpression::LiteralExpression(double value)
	: stringValue_(), integerValue_(0), doubleValue_(value), booleanValue_(false),
	  literalType_(LiteralType::DOUBLE) {}

LiteralExpression::LiteralExpression(bool value)
	: stringValue_(), integerValue_(0), doubleValue_(0.0), booleanValue_(value),
	  literalType_(LiteralType::BOOLEAN) {}

LiteralExpression::LiteralExpression()
	: stringValue_(), integerValue_(0), doubleValue_(0.0), booleanValue_(false),
	  literalType_(LiteralType::NULL_VAL) {}

void LiteralExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void LiteralExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string LiteralExpression::toString() const {
	switch (literalType_) {
		case LiteralType::NULL_VAL:
			return "null";
		case LiteralType::BOOLEAN:
			return booleanValue_ ? "true" : "false";
		case LiteralType::INTEGER:
			return std::to_string(integerValue_);
		case LiteralType::DOUBLE: {
			std::ostringstream oss;
			oss << doubleValue_;
			return oss.str();
		}
		case LiteralType::STRING:
			return "'" + stringValue_ + "'";
	}
	return "";
}

std::unique_ptr<Expression> LiteralExpression::clone() const {
	switch (literalType_) {
		case LiteralType::NULL_VAL:
			return std::make_unique<LiteralExpression>();
		case LiteralType::BOOLEAN:
			return std::make_unique<LiteralExpression>(booleanValue_);
		case LiteralType::INTEGER:
			return std::make_unique<LiteralExpression>(integerValue_);
		case LiteralType::DOUBLE:
			return std::make_unique<LiteralExpression>(doubleValue_);
		case LiteralType::STRING:
			return std::make_unique<LiteralExpression>(stringValue_);
	}
	return std::make_unique<LiteralExpression>();
}

bool LiteralExpression::isNull() const {
	return literalType_ == LiteralType::NULL_VAL;
}

bool LiteralExpression::isBoolean() const {
	return literalType_ == LiteralType::BOOLEAN;
}

bool LiteralExpression::isInteger() const {
	return literalType_ == LiteralType::INTEGER;
}

bool LiteralExpression::isDouble() const {
	return literalType_ == LiteralType::DOUBLE;
}

bool LiteralExpression::isString() const {
	return literalType_ == LiteralType::STRING;
}

bool LiteralExpression::getBooleanValue() const {
	return booleanValue_;
}

int64_t LiteralExpression::getIntegerValue() const {
	return integerValue_;
}

double LiteralExpression::getDoubleValue() const {
	return doubleValue_;
}

const std::string &LiteralExpression::getStringValue() const {
	return stringValue_;
}

// ============================================================================
// VariableReferenceExpression Implementation
// ============================================================================

VariableReferenceExpression::VariableReferenceExpression(std::string variableName)
	: variableName_(std::move(variableName)), hasProperty_(false), propertyName_() {}

VariableReferenceExpression::VariableReferenceExpression(std::string variableName,
                                                          std::string propertyName)
	: variableName_(std::move(variableName)), hasProperty_(true), propertyName_(std::move(propertyName)) {}

void VariableReferenceExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void VariableReferenceExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string VariableReferenceExpression::toString() const {
	if (hasProperty_) {
		return variableName_ + "." + propertyName_;
	}
	return variableName_;
}

std::unique_ptr<Expression> VariableReferenceExpression::clone() const {
	if (hasProperty_) {
		return std::make_unique<VariableReferenceExpression>(variableName_, propertyName_);
	}
	return std::make_unique<VariableReferenceExpression>(variableName_);
}

// ============================================================================
// BinaryOpExpression Implementation
// ============================================================================

BinaryOpExpression::BinaryOpExpression(std::unique_ptr<Expression> left,
                                       BinaryOperatorType op,
                                       std::unique_ptr<Expression> right)
	: left_(std::move(left)), right_(std::move(right)), op_(op) {}

void BinaryOpExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void BinaryOpExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string BinaryOpExpression::toString() const {
	std::ostringstream oss;
	oss << "(" << left_->toString() << " " << graph::query::expressions::toString(op_) << " " << right_->toString() << ")";
	return oss.str();
}

std::unique_ptr<Expression> BinaryOpExpression::clone() const {
	return std::make_unique<BinaryOpExpression>(left_->clone(), op_, right_->clone());
}

// ============================================================================
// UnaryOpExpression Implementation
// ============================================================================

UnaryOpExpression::UnaryOpExpression(UnaryOperatorType op, std::unique_ptr<Expression> operand)
	: operand_(std::move(operand)), op_(op) {}

void UnaryOpExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void UnaryOpExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string UnaryOpExpression::toString() const {
	std::ostringstream oss;
	oss << graph::query::expressions::toString(op_) << "(" << operand_->toString() << ")";
	return oss.str();
}

std::unique_ptr<Expression> UnaryOpExpression::clone() const {
	return std::make_unique<UnaryOpExpression>(op_, operand_->clone());
}

// ============================================================================
// FunctionCallExpression Implementation
// ============================================================================

FunctionCallExpression::FunctionCallExpression(std::string functionName,
                                               std::vector<std::unique_ptr<Expression>> arguments)
	: functionName_(std::move(functionName)), arguments_(std::move(arguments)) {}

void FunctionCallExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void FunctionCallExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string FunctionCallExpression::toString() const {
	std::ostringstream oss;
	oss << functionName_ << "(";
	for (size_t i = 0; i < arguments_.size(); ++i) {
		if (i > 0) oss << ", ";
		oss << arguments_[i]->toString();
	}
	oss << ")";
	return oss.str();
}

std::unique_ptr<Expression> FunctionCallExpression::clone() const {
	std::vector<std::unique_ptr<Expression>> clonedArgs;
	clonedArgs.reserve(arguments_.size());
	for (const auto &arg : arguments_) {
		clonedArgs.push_back(arg->clone());
	}
	return std::make_unique<FunctionCallExpression>(functionName_, std::move(clonedArgs));
}

// ============================================================================
// InExpression Implementation
// ============================================================================

void InExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void InExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string InExpression::toString() const {
	std::ostringstream oss;
	oss << "(" << value_->toString() << " IN [";
	for (size_t i = 0; i < listValues_.size(); ++i) {
		if (i > 0) oss << ", ";
		oss << listValues_[i].toString();
	}
	oss << "])";
	return oss.str();
}

std::unique_ptr<Expression> InExpression::clone() const {
	return std::make_unique<InExpression>(value_->clone(), listValues_);
}

// ============================================================================
// CaseExpression Implementation
// ============================================================================

CaseExpression::CaseExpression(std::unique_ptr<Expression> comparisonExpr)
	: comparisonExpression_(std::move(comparisonExpr)), elseExpression_(std::make_unique<LiteralExpression>()) {}

CaseExpression::CaseExpression()
	: comparisonExpression_(nullptr), elseExpression_(std::make_unique<LiteralExpression>()) {}

void CaseExpression::addBranch(std::unique_ptr<Expression> whenExpr, std::unique_ptr<Expression> thenExpr) {
	branches_.emplace_back(std::move(whenExpr), std::move(thenExpr));
}

void CaseExpression::setElseExpression(std::unique_ptr<Expression> elseExpr) {
	elseExpression_ = std::move(elseExpr);
}

void CaseExpression::accept(ExpressionVisitor &visitor) {
	visitor.visit(this);
}

void CaseExpression::accept(ConstExpressionVisitor &visitor) const {
	visitor.visit(this);
}

std::string CaseExpression::toString() const {
	std::ostringstream oss;
	oss << "CASE";
	if (comparisonExpression_) {
		oss << " " << comparisonExpression_->toString();
	}
	for (const auto &branch : branches_) {
		oss << " WHEN " << branch.whenExpression->toString()
		    << " THEN " << branch.thenExpression->toString();
	}
	if (elseExpression_) {
		oss << " ELSE " << elseExpression_->toString();
	}
	oss << " END";
	return oss.str();
}

std::unique_ptr<Expression> CaseExpression::clone() const {
	auto cloned = isSimpleCase()
		? std::make_unique<CaseExpression>(comparisonExpression_->clone())
		: std::make_unique<CaseExpression>();

	for (const auto &branch : branches_) {
		cloned->addBranch(branch.whenExpression->clone(), branch.thenExpression->clone());
	}
	if (elseExpression_) {
		cloned->setElseExpression(elseExpression_->clone());
	}
	return cloned;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string toString(BinaryOperatorType op) {
	switch (op) {
		case BinaryOperatorType::ADD: return "+";
		case BinaryOperatorType::SUBTRACT: return "-";
		case BinaryOperatorType::MULTIPLY: return "*";
		case BinaryOperatorType::DIVIDE: return "/";
		case BinaryOperatorType::MODULO: return "%";
		case BinaryOperatorType::EQUAL: return "=";
		case BinaryOperatorType::NOT_EQUAL: return "<>";
		case BinaryOperatorType::LESS: return "<";
		case BinaryOperatorType::GREATER: return ">";
		case BinaryOperatorType::LESS_EQUAL: return "<=";
		case BinaryOperatorType::GREATER_EQUAL: return ">=";
		case BinaryOperatorType::IN: return "IN";
		case BinaryOperatorType::AND: return "AND";
		case BinaryOperatorType::OR: return "OR";
		case BinaryOperatorType::XOR: return "XOR";
	}
	return "?";
}

std::string toString(UnaryOperatorType op) {
	switch (op) {
		case UnaryOperatorType::MINUS: return "-";
		case UnaryOperatorType::NOT: return "NOT";
	}
	return "?";
}

bool isArithmeticOperator(BinaryOperatorType op) {
	return op == BinaryOperatorType::ADD ||
	       op == BinaryOperatorType::SUBTRACT ||
	       op == BinaryOperatorType::MULTIPLY ||
	       op == BinaryOperatorType::DIVIDE ||
	       op == BinaryOperatorType::MODULO;
}

bool isComparisonOperator(BinaryOperatorType op) {
	return op == BinaryOperatorType::EQUAL ||
	       op == BinaryOperatorType::NOT_EQUAL ||
	       op == BinaryOperatorType::LESS ||
	       op == BinaryOperatorType::GREATER ||
	       op == BinaryOperatorType::LESS_EQUAL ||
	       op == BinaryOperatorType::GREATER_EQUAL;
}

bool isLogicalOperator(BinaryOperatorType op) {
	return op == BinaryOperatorType::AND ||
	       op == BinaryOperatorType::OR ||
	       op == BinaryOperatorType::XOR;
}

} // namespace graph::query::expressions
