/**
 * @file test_ExpressionAcceptNonConst.cpp
 * @brief Focused tests for non-const ExpressionVisitor accept overloads.
 */

#include <gtest/gtest.h>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"

using namespace graph;
using namespace graph::query::expressions;

namespace {

enum class VisitKind {
	NONE,
	LIST_LITERAL,
	LIST_COMPREHENSION,
	LIST_SLICE,
	REDUCE
};

class TrackingVisitor : public ExpressionVisitor {
public:
	int count = 0;
	VisitKind kind = VisitKind::NONE;

	void visit(LiteralExpression *) override {}
	void visit(VariableReferenceExpression *) override {}
	void visit(BinaryOpExpression *) override {}
	void visit(UnaryOpExpression *) override {}
	void visit(FunctionCallExpression *) override {}
	void visit(CaseExpression *) override {}
	void visit(InExpression *) override {}
	void visit(IsNullExpression *) override {}
	void visit(QuantifierFunctionExpression *) override {}
	void visit(ExistsExpression *) override {}
	void visit(PatternComprehensionExpression *) override {}
	void visit(ParameterExpression *) override {}

	void visit(ListLiteralExpression *) override {
		count++;
		kind = VisitKind::LIST_LITERAL;
	}

	void visit(ListComprehensionExpression *) override {
		count++;
		kind = VisitKind::LIST_COMPREHENSION;
	}

	void visit(ListSliceExpression *) override {
		count++;
		kind = VisitKind::LIST_SLICE;
	}

	void visit(ReduceExpression *) override {
		count++;
		kind = VisitKind::REDUCE;
	}
};

} // namespace

TEST(ExpressionAcceptNonConstTest, ListLiteralAcceptUsesNonConstVisitor) {
	TrackingVisitor visitor;
	ListLiteralExpression expr(PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}));

	expr.accept(visitor);

	EXPECT_EQ(visitor.count, 1);
	EXPECT_EQ(visitor.kind, VisitKind::LIST_LITERAL);
}

TEST(ExpressionAcceptNonConstTest, ListComprehensionAcceptUsesNonConstVisitor) {
	TrackingVisitor visitor;
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	}));
	ListComprehensionExpression expr(
		"x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);

	expr.accept(visitor);

	EXPECT_EQ(visitor.count, 1);
	EXPECT_EQ(visitor.kind, VisitKind::LIST_COMPREHENSION);
}

TEST(ExpressionAcceptNonConstTest, ListSliceAcceptUsesNonConstVisitor) {
	TrackingVisitor visitor;
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	}));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(1));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);

	expr.accept(visitor);

	EXPECT_EQ(visitor.count, 1);
	EXPECT_EQ(visitor.kind, VisitKind::LIST_SLICE);
}

TEST(ExpressionAcceptNonConstTest, ReduceAcceptUsesNonConstVisitor) {
	TrackingVisitor visitor;
	auto initialExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	}));
	auto bodyExpr = std::make_unique<LiteralExpression>(int64_t(1));
	ReduceExpression expr("acc", std::move(initialExpr), "x", std::move(listExpr), std::move(bodyExpr));

	expr.accept(visitor);

	EXPECT_EQ(visitor.count, 1);
	EXPECT_EQ(visitor.kind, VisitKind::REDUCE);
}
