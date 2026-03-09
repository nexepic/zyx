/**
 * @file Expression.hpp
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

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "graph/core/PropertyTypes.hpp"

namespace graph::query::expressions {

// Forward declarations
class ExpressionVisitor;
class ConstExpressionVisitor;
class IsNullExpression;

/**
 * @enum ExpressionType
 * @brief Enumerates all expression types for type identification and RTTI.
 */
enum class ExpressionType {
	LITERAL,
	VARIABLE_REFERENCE,
	PROPERTY_ACCESS,
	BINARY_OP,
	UNARY_OP,
	FUNCTION_CALL,
	CASE_EXPRESSION,
	LIST_SLICE,
	LIST_COMPREHENSION
};

/**
 * @enum BinaryOperatorType
 * @brief Binary operation types.
 */
enum class BinaryOperatorType {
	// Arithmetic
	BOP_ADD,
	BOP_SUBTRACT,
	BOP_MULTIPLY,
	BOP_DIVIDE,
	BOP_MODULO,
	BOP_POWER,

	// Comparison
	BOP_EQUAL,
	BOP_NOT_EQUAL,
	BOP_LESS,
	BOP_GREATER,
	BOP_LESS_EQUAL,
	BOP_GREATER_EQUAL,

	// Special operators
	BOP_IN,

	// Logical
	BOP_AND,
	BOP_OR,
	BOP_XOR
};

/**
 * @enum UnaryOperatorType
 * @brief Unary operation types.
 */
enum class UnaryOperatorType {
	MINUS,
	NOT
};

/**
 * @class Expression
 * @brief Abstract base class for all expression nodes in the AST.
 *
 * Expressions are immutable after construction (thread-safe).
 * All concrete expression types use std::unique_ptr for ownership of child expressions.
 */
class Expression {
public:
	virtual ~Expression() = default;

	/**
	 * @brief Returns the type of this expression.
	 */
	[[nodiscard]] virtual ExpressionType getExpressionType() const = 0;

	/**
	 * @brief Accepts a visitor for double dispatch.
	 */
	virtual void accept(ExpressionVisitor &visitor) = 0;

	/**
	 * @brief Accepts a const visitor for double dispatch.
	 */
	virtual void accept(ConstExpressionVisitor &visitor) const = 0;

	/**
	 * @brief Returns a string representation of this expression (for debugging).
	 */
	[[nodiscard]] virtual std::string toString() const = 0;

	/**
	 * @brief Creates a deep copy of this expression.
	 */
	[[nodiscard]] virtual std::unique_ptr<Expression> clone() const = 0;
};

// ============================================================================
// Literal Expressions
// ============================================================================

/**
 * @class LiteralExpression
 * @brief Represents a literal value (number, string, boolean, or null).
 */
class LiteralExpression : public Expression {
public:
	explicit LiteralExpression(std::string value);
	explicit LiteralExpression(int64_t value);
	explicit LiteralExpression(double value);
	explicit LiteralExpression(bool value);
	explicit LiteralExpression(); // NULL

	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::LITERAL; }
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	// Value accessors
	[[nodiscard]] bool isNull() const;
	[[nodiscard]] bool isBoolean() const;
	[[nodiscard]] bool isInteger() const;
	[[nodiscard]] bool isDouble() const;
	[[nodiscard]] bool isString() const;

	[[nodiscard]] bool getBooleanValue() const;
	[[nodiscard]] int64_t getIntegerValue() const;
	[[nodiscard]] double getDoubleValue() const;
	[[nodiscard]] const std::string &getStringValue() const;

private:
	std::string stringValue_;
	int64_t integerValue_;
	double doubleValue_;
	bool booleanValue_;
	enum class LiteralType { NULL_VAL, BOOLEAN, INTEGER, DOUBLE, STRING } literalType_;
};

// ============================================================================
// Variable Reference Expressions
// ============================================================================

/**
 * @class VariableReferenceExpression
 * @brief Represents a reference to a query variable (e.g., 'n' or 'n.prop').
 */
class VariableReferenceExpression : public Expression {
public:
	/**
	 * @brief Constructor for simple variable reference (e.g., 'n')
	 */
	explicit VariableReferenceExpression(std::string variableName);

	/**
	 * @brief Constructor for property access (e.g., 'n.age')
	 */
	VariableReferenceExpression(std::string variableName, std::string propertyName);

	[[nodiscard]] ExpressionType getExpressionType() const override {
		return ExpressionType::PROPERTY_ACCESS;
	}
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] const std::string &getVariableName() const { return variableName_; }
	[[nodiscard]] bool hasProperty() const { return hasProperty_; }
	[[nodiscard]] const std::string &getPropertyName() const { return propertyName_; }

private:
	std::string variableName_;
	bool hasProperty_;
	std::string propertyName_;
};

// ============================================================================
// Binary Operation Expressions
// ============================================================================

/**
 * @class BinaryOpExpression
 * @brief Represents a binary operation (e.g., 'n.age + 10', 'a AND b').
 */
class BinaryOpExpression : public Expression {
public:
	BinaryOpExpression(std::unique_ptr<Expression> left,
	                   BinaryOperatorType op,
	                   std::unique_ptr<Expression> right);

	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::BINARY_OP; }
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] Expression *getLeft() const { return left_.get(); }
	[[nodiscard]] Expression *getRight() const { return right_.get(); }
	[[nodiscard]] BinaryOperatorType getOperator() const { return op_; }

private:
	std::unique_ptr<Expression> left_;
	std::unique_ptr<Expression> right_;
	BinaryOperatorType op_;
};

// ============================================================================
// Unary Operation Expressions
// ============================================================================

/**
 * @class UnaryOpExpression
 * @brief Represents a unary operation (e.g., '-n.value', 'NOT active').
 */
class UnaryOpExpression : public Expression {
public:
	UnaryOpExpression(UnaryOperatorType op, std::unique_ptr<Expression> operand);

	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::UNARY_OP; }
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] Expression *getOperand() const { return operand_.get(); }
	[[nodiscard]] UnaryOperatorType getOperator() const { return op_; }

private:
	std::unique_ptr<Expression> operand_;
	UnaryOperatorType op_;
};

// ============================================================================
// Function Call Expressions
// ============================================================================

/**
 * @class FunctionCallExpression
 * @brief Represents a function call (e.g., 'count(*)', 'sum(n.salary)').
 */
class FunctionCallExpression : public Expression {
public:
	FunctionCallExpression(std::string functionName,
	                       std::vector<std::unique_ptr<Expression>> arguments);

	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::FUNCTION_CALL; }
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] const std::string &getFunctionName() const { return functionName_; }
	[[nodiscard]] size_t getArgumentCount() const { return arguments_.size(); }
	[[nodiscard]] const std::vector<std::unique_ptr<Expression>> &getArguments() const { return arguments_; }

private:
	std::string functionName_;
	std::vector<std::unique_ptr<Expression>> arguments_;
};

// ============================================================================
// IN Expression
// ============================================================================

/**
 * @class InExpression
 * @brief Represents an IN operator expression (e.g., 'n.age IN [25, 30, 35]').
 *
 * This is a specialized expression for the IN operator which checks if a value
 * is in a list of literal values.
 */
class InExpression : public Expression {
public:
	InExpression(std::unique_ptr<Expression> value, std::vector<PropertyValue> listValues)
		: value_(std::move(value)), listValues_(std::move(listValues)) {}

	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::BINARY_OP; }
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] Expression *getValue() const { return value_.get(); }
	[[nodiscard]] const std::vector<PropertyValue> &getListValues() const { return listValues_; }

private:
	std::unique_ptr<Expression> value_;
	std::vector<PropertyValue> listValues_;
};

// ============================================================================
// CASE Expression
// ============================================================================

/**
 * @struct CaseBranch
 * @brief Represents a WHEN...THEN branch in a CASE expression.
 */
struct CaseBranch {
	std::unique_ptr<Expression> whenExpression;
	std::unique_ptr<Expression> thenExpression;

	CaseBranch(std::unique_ptr<Expression> when, std::unique_ptr<Expression> then)
		: whenExpression(std::move(when)), thenExpression(std::move(then)) {}
};

/**
 * @class CaseExpression
 * @brief Represents CASE...WHEN...THEN...ELSE...END expression.
 *
 * Two forms:
 * 1. Simple CASE: CASE expr WHEN expr THEN expr ... ELSE expr END
 * 2. Searched CASE: CASE WHEN bool_expr THEN expr ... ELSE expr END
 */
class CaseExpression : public Expression {
public:
	/**
	 * @brief Constructor for simple CASE (with comparison expression)
	 */
	explicit CaseExpression(std::unique_ptr<Expression> comparisonExpr);

	/**
	 * @brief Constructor for searched CASE (no comparison expression)
	 */
	CaseExpression();

	/**
	 * @brief Adds a WHEN...THEN branch
	 */
	void addBranch(std::unique_ptr<Expression> whenExpr, std::unique_ptr<Expression> thenExpr);

	/**
	 * @brief Sets the ELSE expression (defaults to NULL if not set)
	 */
	void setElseExpression(std::unique_ptr<Expression> elseExpr);

	[[nodiscard]] ExpressionType getExpressionType() const override { return ExpressionType::CASE_EXPRESSION; }
	void accept(ExpressionVisitor &visitor) override;
	void accept(ConstExpressionVisitor &visitor) const override;
	[[nodiscard]] std::string toString() const override;
	[[nodiscard]] std::unique_ptr<Expression> clone() const override;

	[[nodiscard]] bool isSimpleCase() const { return comparisonExpression_ != nullptr; }
	[[nodiscard]] Expression *getComparisonExpression() const { return comparisonExpression_.get(); }
	[[nodiscard]] const std::vector<CaseBranch> &getBranches() const { return branches_; }
	[[nodiscard]] Expression *getElseExpression() const { return elseExpression_.get(); }

private:
	std::unique_ptr<Expression> comparisonExpression_; // NULL for searched CASE
	std::vector<CaseBranch> branches_;
	std::unique_ptr<Expression> elseExpression_;
};

// ============================================================================
// Visitor Pattern
// ============================================================================

/**
 * @class ExpressionVisitor
 * @brief Visitor interface for mutable expression traversal.
 *
 * Usage: Implement this interface to perform operations on expression ASTs
 * (e.g., evaluation, optimization, analysis).
 */
class ExpressionVisitor {
public:
	virtual ~ExpressionVisitor() = default;

	virtual void visit(LiteralExpression *expr) = 0;
	virtual void visit(VariableReferenceExpression *expr) = 0;
	virtual void visit(BinaryOpExpression *expr) = 0;
	virtual void visit(UnaryOpExpression *expr) = 0;
	virtual void visit(FunctionCallExpression *expr) = 0;
	virtual void visit(CaseExpression *expr) = 0;
	virtual void visit(InExpression *expr) = 0;
	virtual void visit(class ListSliceExpression *expr) = 0;
	virtual void visit(class ListComprehensionExpression *expr) = 0;
	virtual void visit(class ListLiteralExpression *expr) = 0;
	virtual void visit(IsNullExpression *expr) = 0;
};

/**
 * @class ConstExpressionVisitor
 * @brief Visitor interface for const expression traversal.
 */
class ConstExpressionVisitor {
public:
	virtual ~ConstExpressionVisitor() = default;

	virtual void visit(const LiteralExpression *expr) = 0;
	virtual void visit(const VariableReferenceExpression *expr) = 0;
	virtual void visit(const BinaryOpExpression *expr) = 0;
	virtual void visit(const UnaryOpExpression *expr) = 0;
	virtual void visit(const FunctionCallExpression *expr) = 0;
	virtual void visit(const CaseExpression *expr) = 0;
	virtual void visit(const InExpression *expr) = 0;
	virtual void visit(const class ListSliceExpression *expr) = 0;
	virtual void visit(const class ListComprehensionExpression *expr) = 0;
	virtual void visit(const class ListLiteralExpression *expr) = 0;
	virtual void visit(const IsNullExpression *expr) = 0;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief Returns a human-readable string representation of a binary operator.
 */
[[nodiscard]] std::string toString(BinaryOperatorType op);

/**
 * @brief Returns a human-readable string representation of a unary operator.
 */
[[nodiscard]] std::string toString(UnaryOperatorType op);

/**
 * @brief Returns true if the operator is arithmetic (+, -, *, /, %).
 */
[[nodiscard]] bool isArithmeticOperator(BinaryOperatorType op);

/**
 * @brief Returns true if the operator is comparison (=, <>, <, >, <=, >=).
 */
[[nodiscard]] bool isComparisonOperator(BinaryOperatorType op);

/**
 * @brief Returns true if the operator is logical (AND, OR, XOR).
 */
[[nodiscard]] bool isLogicalOperator(BinaryOperatorType op);

} // namespace graph::query::expressions
