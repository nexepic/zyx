/**
 * @file test_ExpressionEvaluator_Advanced.cpp
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
// CaseExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_Match) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(2)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "two");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NullWhenBranch) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(1)));
	// First branch has NULL WHEN -> should be skipped
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(), // NULL
		std::make_unique<LiteralExpression>(std::string("null branch"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "one");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_Match) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(std::string("yes"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "yes");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_NullWhenSkipped) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(), // NULL - skipped
		std::make_unique<LiteralExpression>(std::string("null"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(std::string("yes"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "yes");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_NoMatch_ReturnsElse) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "default");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const CaseExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_NoMatch_NoElse_ReturnsNull) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	// No else expression set

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NoMatch_NoElse) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(99)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	// No match and no else

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_FalseBranch_NotMatched) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("still nope"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "default");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NoMatch_WithElse) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(99)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "default");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_FalseCondition_SkippedToMatch) {
	auto caseExpr = std::make_unique<CaseExpression>();
	// First branch: false -> skip (conditionMet = false)
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("skipped"))
	);
	// Second branch: true -> match (conditionMet = true)
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(std::string("matched"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "matched");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_NoElse_ExplicitNull) {
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	// Explicitly set else to nullptr to cover the nullptr branch
	caseExpr->setElseExpression(nullptr);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NoElse_ExplicitNull) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(99)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	// Explicitly set else to nullptr
	caseExpr->setElseExpression(nullptr);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_MultipleNonMatchBranches) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(5)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(5)),
		std::make_unique<LiteralExpression>(std::string("five"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "five");
}

// ============================================================================
// QuantifierFunctionExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const QuantifierFunctionExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_All_True) {
	// all(x IN [2, 4, 6] WHERE x > 0) => true
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(2)), PropertyValue(int64_t(4)),
		PropertyValue(int64_t(6))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_All_False) {
	// all(x IN [2, -1, 6] WHERE x > 0) => false
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(2)), PropertyValue(int64_t(-1)),
		PropertyValue(int64_t(6))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Any_True) {
	// any(x IN [1, -2, -3] WHERE x > 0) => true
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("any", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Any_False) {
	// any(x IN [-1, -2, -3] WHERE x > 0) => false
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("any", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_None_True) {
	// none(x IN [-1, -2, -3] WHERE x > 0) => true
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("none", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Single_True) {
	// single(x IN [1, -2, -3] WHERE x > 0) => true (exactly one)
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("single", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Single_False_Multiple) {
	// single(x IN [1, 2, -3] WHERE x > 0) => false (two match)
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("single", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_UnknownFunction) {
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<LiteralExpression>(true);

	QuantifierFunctionExpression expr("nonexistent_quantifier", "x", std::move(listExpr), std::move(whereExpr));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_NonPredicateFunction) {
	// "upper" is a scalar function but not a PredicateFunction
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<LiteralExpression>(true);

	QuantifierFunctionExpression expr("upper", "x", std::move(listExpr), std::move(whereExpr));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// ReduceExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Reduce_SumIntegers) {
	// reduce(total = 0, x IN [1,2,3] | total + x) → 6
	auto initialExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	}));
	// Body: total + x
	auto accumRef = std::make_unique<VariableReferenceExpression>("total");
	auto varRef = std::make_unique<VariableReferenceExpression>("x");
	auto bodyExpr = std::make_unique<BinaryOpExpression>(
		std::move(accumRef), BinaryOperatorType::BOP_ADD, std::move(varRef));

	ReduceExpression reduce("total", std::move(initialExpr), "x", std::move(listExpr), std::move(bodyExpr));
	auto result = eval(&reduce);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 6);
}

TEST_F(ExpressionEvaluatorTest, Reduce_ConcatStrings) {
	// reduce(s = '', x IN ['a','b','c'] | s + x) → 'abc'
	auto initialExpr = std::make_unique<LiteralExpression>(std::string(""));
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(std::string("a")),
		PropertyValue(std::string("b")),
		PropertyValue(std::string("c"))
	}));
	auto accumRef = std::make_unique<VariableReferenceExpression>("s");
	auto varRef = std::make_unique<VariableReferenceExpression>("x");
	auto bodyExpr = std::make_unique<BinaryOpExpression>(
		std::move(accumRef), BinaryOperatorType::BOP_ADD, std::move(varRef));

	ReduceExpression reduce("s", std::move(initialExpr), "x", std::move(listExpr), std::move(bodyExpr));
	auto result = eval(&reduce);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "abc");
}

TEST_F(ExpressionEvaluatorTest, Reduce_EmptyList) {
	// reduce(total = 42, x IN [] | total + x) → 42
	auto initialExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto bodyExpr = std::make_unique<LiteralExpression>(int64_t(0));

	ReduceExpression reduce("total", std::move(initialExpr), "x", std::move(listExpr), std::move(bodyExpr));
	auto result = eval(&reduce);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(ExpressionEvaluatorTest, Reduce_NonListThrows) {
	auto initialExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto listExpr = std::make_unique<LiteralExpression>(int64_t(5)); // Not a list
	auto bodyExpr = std::make_unique<LiteralExpression>(int64_t(0));

	ReduceExpression reduce("acc", std::move(initialExpr), "x", std::move(listExpr), std::move(bodyExpr));
	EXPECT_THROW(eval(&reduce), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, Reduce_NullPtr) {
	const ReduceExpression* nullExpr = nullptr;
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// MapProjectionExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, MapProjection_NullPtr) {
	const MapProjectionExpression* nullExpr = nullptr;
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, MapProjection_PropertySelector) {
	// n {.age} should produce {age: 30}
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "age");
	MapProjectionExpression expr("n", std::move(items));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::MAP);
	auto map = result.getMap();
	ASSERT_TRUE(map.count("age"));
	EXPECT_EQ(std::get<int64_t>(map.at("age").getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, MapProjection_MissingProperty_ReturnsNull) {
	// n {.nonexistent} should produce {nonexistent: null}
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "nonexistent");
	MapProjectionExpression expr("n", std::move(items));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::MAP);
	auto map = result.getMap();
	ASSERT_TRUE(map.count("nonexistent"));
	EXPECT_EQ(map.at("nonexistent").getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, MapProjection_LiteralValue) {
	// n {count: 42} should produce {count: 42}
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "count",
		std::make_unique<LiteralExpression>(int64_t(42)));
	MapProjectionExpression expr("n", std::move(items));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::MAP);
	auto map = result.getMap();
	ASSERT_TRUE(map.count("count"));
	EXPECT_EQ(std::get<int64_t>(map.at("count").getVariant()), 42);
}

TEST_F(ExpressionEvaluatorTest, MapProjection_LiteralNullExpression) {
	// n {key: null} - literal item with null expression
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "key", nullptr);
	MapProjectionExpression expr("n", std::move(items));

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::MAP);
	auto map = result.getMap();
	ASSERT_TRUE(map.count("key"));
	EXPECT_EQ(map.at("key").getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, MapProjection_MultipleItems) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "age");
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "city");
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "extra",
		std::make_unique<LiteralExpression>(std::string("value")));
	MapProjectionExpression expr("n", std::move(items));

	auto result = eval(&expr);
	auto map = result.getMap();
	EXPECT_EQ(map.size(), 3u);
	EXPECT_EQ(std::get<int64_t>(map.at("age").getVariant()), 30);
	EXPECT_EQ(std::get<std::string>(map.at("city").getVariant()), "NYC");
	EXPECT_EQ(std::get<std::string>(map.at("extra").getVariant()), "value");
}
