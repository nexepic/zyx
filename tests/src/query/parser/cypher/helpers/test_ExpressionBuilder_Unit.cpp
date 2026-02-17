/**
 * @file test_ExpressionBuilder_Unit.cpp
 * @brief Unit tests for ExpressionBuilder helper class
 */

#include <gtest/gtest.h>
#include "helpers/ExpressionBuilder.hpp"

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

// ============== buildWherePredicate Tests ==============

TEST_F(ExpressionBuilderUnitTest, BuildWherePredicate_NullExpression) {
	std::string desc;
	EXPECT_THROW(
		ExpressionBuilder::buildWherePredicate(nullptr, desc),
		std::runtime_error
	);
}

TEST_F(ExpressionBuilderUnitTest, BuildWherePredicate_CheckErrorMessage) {
	std::string desc;
	try {
		ExpressionBuilder::buildWherePredicate(nullptr, desc);
		FAIL() << "Expected std::runtime_error";
	} catch (const std::runtime_error& e) {
		EXPECT_STREQ("Invalid WHERE expression tree", e.what());
	} catch (...) {
		FAIL() << "Expected std::runtime_error";
	}
}
