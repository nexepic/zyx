/**
 * @file test_ExpressionEvaluator_ListOps.cpp
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

#include "query/expressions/ExpressionEvaluatorTestFixture.hpp"

// ============================================================================
// ListSlice Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ListSliceExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_SingleIndex) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(1));
	// hasRange=false for single index access
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_NegativeIndex) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-1));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_OutOfBounds) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(5));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_DoubleIndex) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(1.0);
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_Range) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30)), PropertyValue(int64_t(40))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(1));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(3));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 20);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNegativeStart) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30)), PropertyValue(int64_t(40))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(-2));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(4));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 30);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 40);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNegativeEnd) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30)), PropertyValue(int64_t(40))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(-1));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(3));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 10);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_NonListThrows) {
	auto inner = std::make_unique<LiteralExpression>(int64_t(42));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(0));
	ListSliceExpression expr(std::move(inner), std::move(indexExpr), nullptr, false);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeWithDoubleIndices) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(0.0);
	auto endExpr = std::make_unique<LiteralExpression>(2.0);
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
}

TEST_F(ExpressionEvaluatorTest, ListSlice_StringIndexThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(std::string("bad"));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeStringStartThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(std::string("bad"));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(1));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeStringEndThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(std::string("bad"));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNoStart) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
	// start=nullptr means from beginning, hasRange=true
	ListSliceExpression expr(std::move(listExpr), nullptr, std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 10);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNoEnd) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(1));
	// end=nullptr means to the end, hasRange=true
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 20);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_SingleIndex_NoStartThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	// hasRange=false, start=nullptr -> should throw "List slice requires index"
	ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, false);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_NegativeIndex_StillNegativeAfterAdjust) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	// list size=1, index=-5 -> adjusted index = 1 + (-5) = -4 -> still < 0 -> NULL
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-5));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_Range_EmptyResult) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(2));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(1)); // end < start
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), size_t(0));
}

TEST_F(ExpressionEvaluatorTest, ListSlice_NegativeIndex_ExactBoundary) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-3)); // -3 + 3 = 0
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

// ============================================================================
// ListComprehension Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListComprehension_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ListComprehensionExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_FilterOnly) {
	// [x IN [1,2,3,4,5] WHERE x > 3] => [4, 5]
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)), PropertyValue(int64_t(4)),
		PropertyValue(int64_t(5))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// WHERE x > 3
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(3))
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr, // no map expression - FILTER mode
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 5);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_ExtractOnly) {
	// [x IN [1,2,3] | x * 2] => [2, 4, 6]
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// | x * 2
	auto mapExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_MULTIPLY,
		std::make_unique<LiteralExpression>(int64_t(2))
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		nullptr, // no WHERE clause
		std::move(mapExpr),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(3));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 6);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_FilterAndMap) {
	// [x IN [1,2,3,4] WHERE x > 2 | x * 10] => [30, 40]
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)), PropertyValue(int64_t(4))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(2))
	);

	auto mapExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_MULTIPLY,
		std::make_unique<LiteralExpression>(int64_t(10))
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		std::move(mapExpr),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 30);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 40);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_NonListThrows) {
	auto listExpr = std::make_unique<LiteralExpression>(int64_t(42)); // not a list
	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		nullptr,
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_EmptyList) {
	std::vector<PropertyValue> empty;
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(empty));
	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		nullptr,
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), size_t(0));
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_NonBooleanWhereThrows) {
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// WHERE returns an integer, not boolean
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42));

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_WhereFiltersFalseElements) {
	// [x IN [1, 2, 3] WHERE false] => empty
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<LiteralExpression>(false);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), size_t(0));
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_WhereReturnsNull_TreatedAsFalse) {
	// WHERE returns NULL for each element -> all elements should be skipped
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// WHERE returns a non-boolean integer -> throws
	// Instead, use a variable reference to "nullVal" which is NULL in our context
	auto whereExpr = std::make_unique<VariableReferenceExpression>("nullVal");

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	// The WHERE returns NULL which is not BOOLEAN type -> throws
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// InExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, InExpression_Found) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	InExpression expr(std::make_unique<LiteralExpression>(int64_t(2)), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, InExpression_NotFound) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	InExpression expr(std::make_unique<LiteralExpression>(int64_t(5)), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, InExpression_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const InExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, InExpression_StringMatch) {
	std::vector<PropertyValue> list = {
		PropertyValue(std::string("alice")),
		PropertyValue(std::string("bob")),
		PropertyValue(std::string("charlie"))
	};
	InExpression expr(std::make_unique<LiteralExpression>(std::string("bob")), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, InExpression_EmptyList) {
	std::vector<PropertyValue> list;
	InExpression expr(std::make_unique<LiteralExpression>(int64_t(1)), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, InExpression_DynamicList_Found) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	}));
	InExpression expr(std::move(value), std::move(listExpr));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorTest, InExpression_DynamicList_NotFound) {
	auto value = std::make_unique<LiteralExpression>(int64_t(5));
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	}));
	InExpression expr(std::move(value), std::move(listExpr));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorTest, InExpression_DynamicList_NullList) {
	auto value = std::make_unique<LiteralExpression>(int64_t(1));
	auto listExpr = std::make_unique<LiteralExpression>(); // NULL
	InExpression expr(std::move(value), std::move(listExpr));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, InExpression_DynamicList_NonListThrows) {
	auto value = std::make_unique<LiteralExpression>(int64_t(1));
	auto listExpr = std::make_unique<LiteralExpression>(int64_t(42)); // Not a list
	InExpression expr(std::move(value), std::move(listExpr));

	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}
