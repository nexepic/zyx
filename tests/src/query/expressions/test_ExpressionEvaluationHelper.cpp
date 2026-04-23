/**
 * @file test_ExpressionEvaluationHelper.cpp
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <gtest/gtest.h>
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

class ExpressionEvaluationHelperTest : public ::testing::Test {
protected:
	void SetUp() override {
		record_.setValue("x", PropertyValue(int64_t(42)));
		record_.setValue("pi", PropertyValue(3.14));
		record_.setValue("flag", PropertyValue(true));
	}

	Record record_;
};

TEST_F(ExpressionEvaluationHelperTest, Evaluate_NullExpr_ReturnsNull) {
	auto result = ExpressionEvaluationHelper::evaluate(nullptr, record_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluationHelperTest, Evaluate_WithDataManager_NullExpr) {
	auto result = ExpressionEvaluationHelper::evaluate(nullptr, record_, nullptr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluationHelperTest, Evaluate_WithParams_NullExpr) {
	std::unordered_map<std::string, PropertyValue> params;
	auto result = ExpressionEvaluationHelper::evaluate(nullptr, record_, nullptr, &params);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluationHelperTest, Evaluate_WithParams_NullParams_DelegatesToBasic) {
	LiteralExpression lit(int64_t(7));
	auto result = ExpressionEvaluationHelper::evaluate(&lit, record_, nullptr, nullptr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 7);
}

TEST_F(ExpressionEvaluationHelperTest, Evaluate_WithParams_ValidParams) {
	std::unordered_map<std::string, PropertyValue> params;
	params["limit"] = PropertyValue(int64_t(100));
	ParameterExpression paramExpr("limit");
	auto result = ExpressionEvaluationHelper::evaluate(&paramExpr, record_, nullptr, &params);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 100);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateBool_WithDataManager) {
	LiteralExpression lit(true);
	bool result = ExpressionEvaluationHelper::evaluateBool(&lit, record_, nullptr);
	EXPECT_TRUE(result);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateBool_FalseExpr) {
	LiteralExpression lit(false);
	bool result = ExpressionEvaluationHelper::evaluateBool(&lit, record_, nullptr);
	EXPECT_FALSE(result);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateBool_WithParams) {
	std::unordered_map<std::string, PropertyValue> params;
	params["flag"] = PropertyValue(true);
	ParameterExpression paramExpr("flag");
	bool result = ExpressionEvaluationHelper::evaluateBool(&paramExpr, record_, nullptr, &params);
	EXPECT_TRUE(result);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateBool_WithParams_NullParams) {
	LiteralExpression lit(false);
	bool result = ExpressionEvaluationHelper::evaluateBool(&lit, record_, nullptr, nullptr);
	EXPECT_FALSE(result);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateInt) {
	LiteralExpression lit(int64_t(99));
	int64_t result = ExpressionEvaluationHelper::evaluateInt(&lit, record_);
	EXPECT_EQ(result, 99);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateDouble) {
	LiteralExpression lit(2.71);
	double result = ExpressionEvaluationHelper::evaluateDouble(&lit, record_);
	EXPECT_DOUBLE_EQ(result, 2.71);
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateBatch) {
	LiteralExpression lit(int64_t(5));
	std::vector<Record> records(3);
	auto results = ExpressionEvaluationHelper::evaluateBatch(&lit, records);
	EXPECT_EQ(results.size(), 3u);
	for (const auto& r : results) {
		EXPECT_EQ(std::get<int64_t>(r.getVariant()), 5);
	}
}

TEST_F(ExpressionEvaluationHelperTest, EvaluateBatch_EmptyRecords) {
	LiteralExpression lit(int64_t(1));
	std::vector<Record> empty;
	auto results = ExpressionEvaluationHelper::evaluateBatch(&lit, empty);
	EXPECT_TRUE(results.empty());
}
