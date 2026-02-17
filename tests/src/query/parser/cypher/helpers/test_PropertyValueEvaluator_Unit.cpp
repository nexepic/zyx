/**
 * @file test_PropertyValueEvaluator_Unit.cpp
 * @brief Unit tests for PropertyValueEvaluator helper class
 */

#include <gtest/gtest.h>
#include "helpers/PropertyValueEvaluator.hpp"

using namespace graph::parser::cypher::helpers;

// Test class for PropertyValueEvaluator
class PropertyValueEvaluatorUnitTest : public ::testing::Test {
protected:
	// Helper methods
};

// ============== evaluate Tests ==============

TEST_F(PropertyValueEvaluatorUnitTest, Evaluate_NullContext) {
	graph::PropertyValue result = PropertyValueEvaluator::evaluate(nullptr);
	// Default PropertyValue is NULL_TYPE
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}
