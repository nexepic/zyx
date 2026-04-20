/**
 * @file test_ListExpressions.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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

#include "query/expressions/ExpressionsTestFixture.hpp"

TEST_F(ExpressionsTest, ListSliceExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto list = std::make_unique<VariableReferenceExpression>("myList");
	auto start = std::make_unique<LiteralExpression>(int64_t(0));
	auto end = std::make_unique<LiteralExpression>(int64_t(5));

	ListSliceExpression expr(std::move(list), std::move(start), std::move(end), true);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_SLICE);
}

TEST_F(ExpressionsTest, ListComprehensionExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto listExpr = std::make_unique<VariableReferenceExpression>("myList");
	auto mapExpr = std::make_unique<VariableReferenceExpression>("x");

	ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_COMPREHENSION);
}

TEST_F(ExpressionsTest, ListLiteralExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	std::vector<PropertyValue> values = {PropertyValue(int64_t(1))};
	PropertyValue listValue(values);

	ListLiteralExpression expr(listValue);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::EXPR_LIST_LITERAL);
}

TEST_F(ExpressionsTest, ListLiteralExpression_CompleteCoverage) {
	// Test constructor throws on non-LIST type
	EXPECT_THROW({
		ListLiteralExpression expr(PropertyValue(int64_t(42)));
	}, std::runtime_error);

	EXPECT_THROW({
		ListLiteralExpression expr(PropertyValue(std::string("test")));
	}, std::runtime_error);

	EXPECT_THROW({
		ListLiteralExpression expr(PropertyValue(true));
	}, std::runtime_error);

	// Test empty list
	{
		std::vector<PropertyValue> emptyList;
		PropertyValue listValue(emptyList);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.toString(), "[]");
		EXPECT_EQ(expr.getValue().getList().size(), 0ULL);
		EXPECT_EQ(expr.getExpressionType(), ExpressionType::EXPR_LIST_LITERAL);
	}

	// Test single element
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(42)) };
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.toString(), "[42]");
		EXPECT_EQ(expr.getValue().getList().size(), 1ULL);
	}

	// Test heterogeneous list
	{
		std::vector<PropertyValue> list = {
			PropertyValue(int64_t(42)),
			PropertyValue(std::string("hello")),
			PropertyValue(true),
			PropertyValue(3.14)
		};
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.getValue().getList().size(), 4ULL);
	}

	// Test nested lists
	{
		std::vector<PropertyValue> inner1 = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)) };
		std::vector<PropertyValue> inner2 = { PropertyValue(int64_t(3)), PropertyValue(int64_t(4)) };
		std::vector<PropertyValue> outer = { PropertyValue(inner1), PropertyValue(inner2) };
		PropertyValue listValue(outer);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.getValue().getList().size(), 2ULL);
		EXPECT_EQ(expr.getValue().getList()[0].getType(), PropertyType::LIST);
	}

	// Test list with nulls
	{
		std::vector<PropertyValue> list = {
			PropertyValue(int64_t(1)),
			PropertyValue(),
			PropertyValue(std::string("test"))
		};
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.toString(), "[1, null, test]");
	}

	// Test clone
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)) };
		PropertyValue listValue(list);
		ListLiteralExpression original(listValue);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[1, 2]");
	}

	// Test evaluation
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(10)), PropertyValue(int64_t(20)) };
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::LIST);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 10);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 20);
	}

	// Test getValue getter
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(42)) };
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.getValue(), listValue);
	}
}

TEST_F(ExpressionsTest, ListSliceExpression_CompleteCoverage) {
	// Helper to create list
	auto createList = [](const std::vector<int64_t>& values) -> std::unique_ptr<ListLiteralExpression> {
		std::vector<PropertyValue> list;
		for (int64_t v : values) {
			list.push_back(PropertyValue(v));
		}
		return std::make_unique<ListLiteralExpression>(PropertyValue(list));
	};

	// Single index: list[0]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 10);
	}

	// Single negative index: list[-1]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(-1));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 50);
	}

	// Single index out of bounds: list[100]
	{
		auto listExpr = createList({10, 20, 30});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(100));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}

	// Single negative index out of bounds: list[-100]
	{
		auto listExpr = createList({10, 20, 30});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(-100));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}

	// Range: list[0..2]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 10);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 20);
	}

	// Range negative: list[-3..-1]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(-3));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(-1));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 30);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 40);
	}

	// Range without start: list[..2]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(3));
		ListSliceExpression expr(std::move(listExpr), nullptr, std::move(endExpr), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
	}

	// Range without end: list[2..]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
	}

	// Full range: list[..]
	{
		auto listExpr = createList({10, 20, 30});
		ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
	}

	// Test toString variations
	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		EXPECT_EQ(expr.toString(), "[1, 2, 3][0]");
	}

	{
		auto listExpr = createList({1, 2, 3});
		ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, true);
		EXPECT_EQ(expr.toString(), "[1, 2, 3][..]");
	}

	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		EXPECT_EQ(expr.toString(), "[1, 2, 3][0..2]");
	}

	// Test clone with all parameters
	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression original(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[1, 2, 3][0..2]");
	}

	// Test clone with no start/end
	{
		auto listExpr = createList({1, 2, 3});
		ListSliceExpression original(std::move(listExpr), nullptr, nullptr, true);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[1, 2, 3][..]");
	}

	// Test getters
	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		EXPECT_FALSE(expr.hasRange());
		EXPECT_NE(expr.getList(), nullptr);
		EXPECT_NE(expr.getStart(), nullptr);
		EXPECT_EQ(expr.getEnd(), nullptr);
	}

	{
		auto listExpr = createList({1, 2, 3});
		ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, true);
		EXPECT_TRUE(expr.hasRange());
		EXPECT_EQ(expr.getStart(), nullptr);
		EXPECT_EQ(expr.getEnd(), nullptr);
	}
}

TEST_F(ExpressionsTest, ListComprehensionExpression_CompleteCoverage) {
	// Helper to create list
	auto createList = [](const std::vector<int64_t>& values) -> std::unique_ptr<ListLiteralExpression> {
		std::vector<PropertyValue> list;
		for (int64_t v : values) {
			list.push_back(PropertyValue(v));
		}
		return std::make_unique<ListLiteralExpression>(PropertyValue(list));
	};

	// FILTER without WHERE: [x IN list]
	{
		auto listExpr = createList({1, 2, 3, 4, 5});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::LIST);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 5ULL);
	}

	// FILTER with WHERE: [x IN list WHERE x > 3]
	{
		auto listExpr = createList({1, 2, 3, 4, 5});
		auto varRef = std::make_unique<VariableReferenceExpression>("x");
		auto lit3 = std::make_unique<LiteralExpression>(int64_t(3));
		auto whereExpr = std::make_unique<BinaryOpExpression>(std::move(varRef), BinaryOperatorType::BOP_GREATER, std::move(lit3));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL); // 4 and 5
	}

	// EXTRACT with MAP: [x IN list | x * 2]
	{
		auto listExpr = createList({1, 2, 3});
		auto varRef = std::make_unique<VariableReferenceExpression>("x");
		auto lit2 = std::make_unique<LiteralExpression>(int64_t(2));
		auto mapExpr = std::make_unique<BinaryOpExpression>(std::move(varRef), BinaryOperatorType::BOP_MULTIPLY, std::move(lit2));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 2);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 4);
		EXPECT_EQ(std::get<int64_t>(result[2].getVariant()), 6);
	}

	// EXTRACT with WHERE and MAP: [x IN list WHERE x > 2 | x * 3]
	{
		auto listExpr = createList({1, 2, 3, 4, 5});
		auto varRef1 = std::make_unique<VariableReferenceExpression>("x");
		auto lit2 = std::make_unique<LiteralExpression>(int64_t(2));
		auto whereExpr = std::make_unique<BinaryOpExpression>(std::move(varRef1), BinaryOperatorType::BOP_GREATER, std::move(lit2));

		auto varRef2 = std::make_unique<VariableReferenceExpression>("x");
		auto lit3 = std::make_unique<LiteralExpression>(int64_t(3));
		auto mapExpr = std::make_unique<BinaryOpExpression>(std::move(varRef2), BinaryOperatorType::BOP_MULTIPLY, std::move(lit3));

		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL); // 3, 4, 5
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 9);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 12);
		EXPECT_EQ(std::get<int64_t>(result[2].getVariant()), 15);
	}

	// REDUCE - Currently returns list like FILTER (REDUCE not fully implemented)
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3])");
	}

	// Empty list
	{
		std::vector<PropertyValue> emptyList;
		auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(emptyList));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 0ULL);
	}

	// Test toString variations - FILTER without WHERE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3]]");
	}

	// FILTER with WHERE
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] WHERE true]");
	}

	// EXTRACT without MAP (edge case - just filters elements)
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3]]");
	}

	// EXTRACT with MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] | 42]");
	}

	// EXTRACT with WHERE and MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] WHERE true | 42]");
	}

	// REDUCE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3])");
	}

	// REDUCE with MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3] | 0)");
	}

	// Test clone
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression original("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[x IN [1, 2, 3] WHERE true | 42]");
	}

	// Test clone FILTER without WHERE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression original("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[x IN [1, 2, 3]]");
	}

	// Test clone REDUCE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression original("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "reduce(total = [1, 2, 3])");
	}

	// Test getters
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.getVariable(), "x");
		EXPECT_EQ(expr.getType(), ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_NE(expr.getListExpr(), nullptr);
		EXPECT_EQ(expr.getWhereExpr(), nullptr);
		EXPECT_EQ(expr.getMapExpr(), nullptr);
	}

	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_NE(expr.getWhereExpr(), nullptr);
		EXPECT_NE(expr.getMapExpr(), nullptr);
	}

	// Test getExpressionType
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_COMPREHENSION);
	}
}

TEST_F(ExpressionsTest, SliceNonListValue) {
	// Test slicing non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(0));
	ListSliceExpression expr(std::move(nonListExpr), std::move(indexExpr), nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, SliceMissingIndex) {
	// Test slice with missing start index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, SliceWithNonIntegerIndex) {
	// Test slice with string index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, SliceWithNonIntegerStartIndex) {
	// Test slice with string start index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, SliceWithNonIntegerEndIndex) {
	// Test slice with string end index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, ListComprehensionNonListValue) {
	// Test list comprehension on non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	ListComprehensionExpression expr("x", std::move(nonListExpr), nullptr, nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, ListComprehensionNonBooleanWhere) {
	// Test list comprehension with non-boolean WHERE clause throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42)); // Non-boolean
	ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, ListSlice_DoubleIndex) {
	// Test list slicing with double index
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(double(1.0));

	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 2);
}

TEST_F(ExpressionsTest, ListSlice_NegativeIndex) {
	// Test list slicing with negative index
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(10)),
		PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-1));

	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 30);
}

TEST_F(ExpressionsTest, ListSlice_OutOfBoundsIndex) {
	// Test list slicing with out of bounds index returns NULL
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(10));

	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, ListSlice_NegativeRange) {
	// Test list range slice with negative start/end
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)),
		PropertyValue(int64_t(4)),
		PropertyValue(int64_t(5))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(-3));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(-1));

	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	const auto& result = std::get<std::vector<PropertyValue>>(evaluator.getResult().getVariant());
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 4);
}

TEST_F(ExpressionsTest, ListComprehension_FilterMode) {
	// Test list comprehension in FILTER mode
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)),
		PropertyValue(int64_t(4))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));

	// WHERE clause: x > 2
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");
	auto intExpr = std::make_unique<LiteralExpression>(int64_t(2));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_GREATER,
		std::move(intExpr)
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	const auto& result = std::get<std::vector<PropertyValue>>(evaluator.getResult().getVariant());
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 4);
}

TEST_F(ExpressionsTest, ListComprehension_NonBooleanWhere_Throws) {
	// Test list comprehension with non-boolean WHERE throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));

	// WHERE clause returns non-boolean (integer)
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42));

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, ListComprehension_NonListValue_Throws) {
	// Test list comprehension with non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	ListComprehensionExpression expr(
		"x",
		std::move(nonListExpr),
		nullptr,
		std::move(varExpr),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

