/**
 * @file test_ShortestPathExpression.cpp
 * @brief Unit tests for ShortestPathExpression.
 */

#include <gtest/gtest.h>
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"

using namespace graph::query::expressions;

// ============================================================================
// ShortestPathExpression
// ============================================================================

TEST(ShortestPathExpressionTest, GetExpressionType) {
	ShortestPathExpression expr("a", "b", "KNOWS", PatternDirection::PAT_OUTGOING, 1, 10, false);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::SHORTEST_PATH);
}

TEST(ShortestPathExpressionTest, Getters) {
	ShortestPathExpression expr("start", "end", "FOLLOWS", PatternDirection::PAT_INCOMING, 2, 5, true);
	EXPECT_EQ(expr.getStartVar(), "start");
	EXPECT_EQ(expr.getEndVar(), "end");
	EXPECT_EQ(expr.getRelType(), "FOLLOWS");
	EXPECT_EQ(expr.getDirection(), PatternDirection::PAT_INCOMING);
	EXPECT_EQ(expr.getMinHops(), 2);
	EXPECT_EQ(expr.getMaxHops(), 5);
	EXPECT_TRUE(expr.isAll());
}

TEST(ShortestPathExpressionTest, ToString_ShortestPath) {
	ShortestPathExpression expr("a", "b", "KNOWS", PatternDirection::PAT_OUTGOING, 1, -1, false);
	std::string str = expr.toString();
	EXPECT_NE(str.find("shortestPath"), std::string::npos);
	EXPECT_NE(str.find("a"), std::string::npos);
	EXPECT_NE(str.find("b"), std::string::npos);
	EXPECT_NE(str.find("KNOWS"), std::string::npos);
}

TEST(ShortestPathExpressionTest, ToString_AllShortestPaths) {
	ShortestPathExpression expr("x", "y", "", PatternDirection::PAT_BOTH, 1, -1, true);
	std::string str = expr.toString();
	EXPECT_NE(str.find("allShortestPaths"), std::string::npos);
}

TEST(ShortestPathExpressionTest, Clone) {
	ShortestPathExpression expr("a", "b", "REL", PatternDirection::PAT_BOTH, 1, 10, true);
	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getExpressionType(), ExpressionType::SHORTEST_PATH);

	auto* sp = dynamic_cast<ShortestPathExpression*>(cloned.get());
	ASSERT_NE(sp, nullptr);
	EXPECT_EQ(sp->getStartVar(), "a");
	EXPECT_EQ(sp->getEndVar(), "b");
	EXPECT_EQ(sp->getRelType(), "REL");
	EXPECT_EQ(sp->getDirection(), PatternDirection::PAT_BOTH);
	EXPECT_EQ(sp->getMinHops(), 1);
	EXPECT_EQ(sp->getMaxHops(), 10);
	EXPECT_TRUE(sp->isAll());
}

TEST(ShortestPathExpressionTest, AcceptConstVisitor) {
	struct TestVisitor : public ConstExpressionVisitor {
		bool visited = false;
		void visit(const LiteralExpression*) override {}
		void visit(const VariableReferenceExpression*) override {}
		void visit(const BinaryOpExpression*) override {}
		void visit(const UnaryOpExpression*) override {}
		void visit(const FunctionCallExpression*) override {}
		void visit(const CaseExpression*) override {}
		void visit(const InExpression*) override {}
		void visit(const class ListSliceExpression*) override {}
		void visit(const class ListComprehensionExpression*) override {}
		void visit(const class ListLiteralExpression*) override {}
		void visit(const IsNullExpression*) override {}
		void visit(const class QuantifierFunctionExpression*) override {}
		void visit(const class ExistsExpression*) override {}
		void visit(const class PatternComprehensionExpression*) override {}
		void visit(const class ReduceExpression*) override {}
		void visit(const class ParameterExpression*) override {}
		void visit(const ShortestPathExpression*) override { visited = true; }
		void visit(const class MapProjectionExpression*) override {}
	};

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_BOTH, 1, -1, false);
	TestVisitor visitor;
	const auto& constExpr = static_cast<const Expression&>(expr);
	constExpr.accept(visitor);
	EXPECT_TRUE(visitor.visited);
}

TEST(ShortestPathExpressionTest, AcceptNonConstVisitor) {
	struct TestVisitor : public ExpressionVisitor {
		bool visited = false;
		void visit(LiteralExpression*) override {}
		void visit(VariableReferenceExpression*) override {}
		void visit(BinaryOpExpression*) override {}
		void visit(UnaryOpExpression*) override {}
		void visit(FunctionCallExpression*) override {}
		void visit(CaseExpression*) override {}
		void visit(InExpression*) override {}
		void visit(class ListSliceExpression*) override {}
		void visit(class ListComprehensionExpression*) override {}
		void visit(class ListLiteralExpression*) override {}
		void visit(IsNullExpression*) override {}
		void visit(class QuantifierFunctionExpression*) override {}
		void visit(class ExistsExpression*) override {}
		void visit(class PatternComprehensionExpression*) override {}
		void visit(class ReduceExpression*) override {}
		void visit(class ParameterExpression*) override {}
		void visit(ShortestPathExpression*) override { visited = true; }
		void visit(class MapProjectionExpression*) override {}
	};

	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_BOTH, 1, -1, false);
	TestVisitor visitor;
	expr.accept(visitor);
	EXPECT_TRUE(visitor.visited);
}

TEST(ShortestPathExpressionTest, EmptyRelType) {
	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_OUTGOING, 1, -1, false);
	EXPECT_TRUE(expr.getRelType().empty());
}

// ============================================================================
// MapProjectionExpression
// ============================================================================

TEST(MapProjectionExpressionTest, GetExpressionType) {
	std::vector<MapProjectionItem> items;
	MapProjectionExpression expr("n", std::move(items));
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::MAP_PROJECTION);
}

TEST(MapProjectionExpressionTest, GetVariable) {
	std::vector<MapProjectionItem> items;
	MapProjectionExpression expr("myVar", std::move(items));
	EXPECT_EQ(expr.getVariable(), "myVar");
}

TEST(MapProjectionExpressionTest, GetItems_Property) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "name");
	MapProjectionExpression expr("n", std::move(items));
	ASSERT_EQ(expr.getItems().size(), 1u);
	EXPECT_EQ(expr.getItems()[0].type, MapProjectionItemType::MPROP_PROPERTY);
	EXPECT_EQ(expr.getItems()[0].key, "name");
}

TEST(MapProjectionExpressionTest, GetItems_Literal) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "count",
		std::make_unique<LiteralExpression>(int64_t(42)));
	MapProjectionExpression expr("n", std::move(items));
	ASSERT_EQ(expr.getItems().size(), 1u);
	EXPECT_EQ(expr.getItems()[0].type, MapProjectionItemType::MPROP_LITERAL);
	EXPECT_NE(expr.getItems()[0].expression, nullptr);
}

TEST(MapProjectionExpressionTest, GetItems_AllProperties) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	MapProjectionExpression expr("n", std::move(items));
	ASSERT_EQ(expr.getItems().size(), 1u);
	EXPECT_EQ(expr.getItems()[0].type, MapProjectionItemType::MPROP_ALL_PROPERTIES);
}

TEST(MapProjectionExpressionTest, ToString_PropertySelector) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "name");
	MapProjectionExpression expr("n", std::move(items));
	std::string str = expr.toString();
	EXPECT_NE(str.find(".name"), std::string::npos);
	EXPECT_NE(str.find("n"), std::string::npos);
}

TEST(MapProjectionExpressionTest, ToString_LiteralWithExpr) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "age",
		std::make_unique<LiteralExpression>(int64_t(30)));
	MapProjectionExpression expr("n", std::move(items));
	std::string str = expr.toString();
	EXPECT_NE(str.find("age:"), std::string::npos);
}

TEST(MapProjectionExpressionTest, ToString_LiteralWithoutExpr) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "missing", nullptr);
	MapProjectionExpression expr("n", std::move(items));
	std::string str = expr.toString();
	EXPECT_NE(str.find("null"), std::string::npos);
}

TEST(MapProjectionExpressionTest, ToString_AllProperties) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	MapProjectionExpression expr("n", std::move(items));
	std::string str = expr.toString();
	EXPECT_NE(str.find(".*"), std::string::npos);
}

TEST(MapProjectionExpressionTest, ToString_MultipleItems) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "name");
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "age");
	MapProjectionExpression expr("n", std::move(items));
	std::string str = expr.toString();
	EXPECT_NE(str.find(", "), std::string::npos);
}

TEST(MapProjectionExpressionTest, Clone) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "name");
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "count",
		std::make_unique<LiteralExpression>(int64_t(5)));
	items.emplace_back(MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	MapProjectionExpression expr("n", std::move(items));

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	auto* mp = dynamic_cast<MapProjectionExpression*>(cloned.get());
	ASSERT_NE(mp, nullptr);
	EXPECT_EQ(mp->getVariable(), "n");
	ASSERT_EQ(mp->getItems().size(), 3u);
	EXPECT_EQ(mp->getItems()[0].type, MapProjectionItemType::MPROP_PROPERTY);
	EXPECT_EQ(mp->getItems()[1].type, MapProjectionItemType::MPROP_LITERAL);
	EXPECT_EQ(mp->getItems()[2].type, MapProjectionItemType::MPROP_ALL_PROPERTIES);
}

TEST(MapProjectionExpressionTest, CloneWithNullExpression) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "key", nullptr);
	MapProjectionExpression expr("n", std::move(items));

	auto cloned = expr.clone();
	auto* mp = dynamic_cast<MapProjectionExpression*>(cloned.get());
	ASSERT_NE(mp, nullptr);
	EXPECT_EQ(mp->getItems()[0].expression, nullptr);
}

TEST(MapProjectionExpressionTest, AcceptConstVisitor) {
	struct TestVisitor : public ConstExpressionVisitor {
		bool visited = false;
		void visit(const LiteralExpression*) override {}
		void visit(const VariableReferenceExpression*) override {}
		void visit(const BinaryOpExpression*) override {}
		void visit(const UnaryOpExpression*) override {}
		void visit(const FunctionCallExpression*) override {}
		void visit(const CaseExpression*) override {}
		void visit(const InExpression*) override {}
		void visit(const class ListSliceExpression*) override {}
		void visit(const class ListComprehensionExpression*) override {}
		void visit(const class ListLiteralExpression*) override {}
		void visit(const IsNullExpression*) override {}
		void visit(const class QuantifierFunctionExpression*) override {}
		void visit(const class ExistsExpression*) override {}
		void visit(const class PatternComprehensionExpression*) override {}
		void visit(const class ReduceExpression*) override {}
		void visit(const class ParameterExpression*) override {}
		void visit(const class ShortestPathExpression*) override {}
		void visit(const MapProjectionExpression*) override { visited = true; }
	};

	std::vector<MapProjectionItem> items;
	MapProjectionExpression expr("n", std::move(items));
	TestVisitor visitor;
	const auto& constExpr = static_cast<const Expression&>(expr);
	constExpr.accept(visitor);
	EXPECT_TRUE(visitor.visited);
}

// ============================================================================
// ParameterExpression
// ============================================================================

TEST(ParameterExpressionTest, GetExpressionType) {
	ParameterExpression expr("limit");
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::PARAMETER);
}

TEST(ParameterExpressionTest, GetParameterName) {
	ParameterExpression expr("myParam");
	EXPECT_EQ(expr.getParameterName(), "myParam");
}

TEST(ParameterExpressionTest, ToString) {
	ParameterExpression expr("x");
	EXPECT_EQ(expr.toString(), "$x");
}

TEST(ParameterExpressionTest, Clone) {
	ParameterExpression expr("name");
	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	auto* p = dynamic_cast<ParameterExpression*>(cloned.get());
	ASSERT_NE(p, nullptr);
	EXPECT_EQ(p->getParameterName(), "name");
}

TEST(ParameterExpressionTest, AcceptConstVisitor) {
	struct TestVisitor : public ConstExpressionVisitor {
		bool visited = false;
		void visit(const LiteralExpression*) override {}
		void visit(const VariableReferenceExpression*) override {}
		void visit(const BinaryOpExpression*) override {}
		void visit(const UnaryOpExpression*) override {}
		void visit(const FunctionCallExpression*) override {}
		void visit(const CaseExpression*) override {}
		void visit(const InExpression*) override {}
		void visit(const class ListSliceExpression*) override {}
		void visit(const class ListComprehensionExpression*) override {}
		void visit(const class ListLiteralExpression*) override {}
		void visit(const IsNullExpression*) override {}
		void visit(const class QuantifierFunctionExpression*) override {}
		void visit(const class ExistsExpression*) override {}
		void visit(const class PatternComprehensionExpression*) override {}
		void visit(const class ReduceExpression*) override {}
		void visit(const ParameterExpression*) override { visited = true; }
		void visit(const class ShortestPathExpression*) override {}
		void visit(const class MapProjectionExpression*) override {}
	};

	ParameterExpression expr("x");
	TestVisitor visitor;
	const auto& constExpr = static_cast<const Expression&>(expr);
	constExpr.accept(visitor);
	EXPECT_TRUE(visitor.visited);
}

TEST(ParameterExpressionTest, AcceptNonConstVisitor) {
	struct TestVisitor : public ExpressionVisitor {
		bool visited = false;
		void visit(LiteralExpression*) override {}
		void visit(VariableReferenceExpression*) override {}
		void visit(BinaryOpExpression*) override {}
		void visit(UnaryOpExpression*) override {}
		void visit(FunctionCallExpression*) override {}
		void visit(CaseExpression*) override {}
		void visit(InExpression*) override {}
		void visit(class ListSliceExpression*) override {}
		void visit(class ListComprehensionExpression*) override {}
		void visit(class ListLiteralExpression*) override {}
		void visit(IsNullExpression*) override {}
		void visit(class QuantifierFunctionExpression*) override {}
		void visit(class ExistsExpression*) override {}
		void visit(class PatternComprehensionExpression*) override {}
		void visit(class ReduceExpression*) override {}
		void visit(ParameterExpression*) override { visited = true; }
		void visit(class ShortestPathExpression*) override {}
		void visit(class MapProjectionExpression*) override {}
	};

	ParameterExpression expr("x");
	TestVisitor visitor;
	expr.accept(visitor);
	EXPECT_TRUE(visitor.visited);
}

// ============================================================================
// MapProjectionItem clone
// ============================================================================

TEST(MapProjectionItemTest, CloneProperty) {
	MapProjectionItem item(MapProjectionItemType::MPROP_PROPERTY, "name");
	auto cloned = item.clone();
	EXPECT_EQ(cloned.type, MapProjectionItemType::MPROP_PROPERTY);
	EXPECT_EQ(cloned.key, "name");
	EXPECT_EQ(cloned.expression, nullptr);
}

TEST(MapProjectionItemTest, CloneLiteralWithExpression) {
	MapProjectionItem item(MapProjectionItemType::MPROP_LITERAL, "key",
		std::make_unique<LiteralExpression>(int64_t(42)));
	auto cloned = item.clone();
	EXPECT_EQ(cloned.type, MapProjectionItemType::MPROP_LITERAL);
	EXPECT_EQ(cloned.key, "key");
	EXPECT_NE(cloned.expression, nullptr);
}
