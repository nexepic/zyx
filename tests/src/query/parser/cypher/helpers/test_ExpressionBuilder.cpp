/**
 * @file test_ExpressionBuilder.cpp
 * @brief Unit tests for ExpressionBuilder helper class
 */

#include <gtest/gtest.h>
#include "helpers/ExpressionBuilder.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph::parser::cypher::helpers;

// Test class for ExpressionBuilder
class ExpressionBuilderUnitTest : public ::testing::Test {
protected:
	// Helper methods
};

// ============== extractListFromExpression Tests ==============

TEST_F(ExpressionBuilderUnitTest, ExtractListFromExpression_Null) {
	auto list = ExpressionBuilder::extractListFromExpression(nullptr);
	EXPECT_TRUE(list.empty());
}

// ============== buildExpression Tests ==============

TEST_F(ExpressionBuilderUnitTest, BuildExpression_NullExpression) {
	EXPECT_THROW(
		ExpressionBuilder::buildExpression(nullptr),
		std::runtime_error
	);
}

TEST_F(ExpressionBuilderUnitTest, BuildExpression_NullCheckErrorMessage) {
	try {
		ExpressionBuilder::buildExpression(nullptr);
		FAIL() << "Expected std::runtime_error";
	} catch (const std::runtime_error& e) {
		EXPECT_STREQ("Invalid expression tree: null context", e.what());
	} catch (...) {
		FAIL() << "Expected std::runtime_error";
	}
}
