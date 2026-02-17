/**
 * @file test_AstExtractor_Unit.cpp
 * @brief Unit tests for AstExtractor helper class
 */

#include <gtest/gtest.h>
#include "helpers/AstExtractor.hpp"

using namespace graph::parser::cypher::helpers;

// Test class for AstExtractor
class AstExtractorUnitTest : public ::testing::Test {
protected:
	// Helper to check if string contains substring
	bool containsSubstring(const std::string& str, const std::string& substr) {
		if (str.empty() || substr.empty()) return false;
		return str.find(substr) != std::string::npos;
	}
};

// ============== extractVariable Tests ==============

TEST_F(AstExtractorUnitTest, ExtractVariable_Null) {
	std::string result = AstExtractor::extractVariable(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractLabel Tests ==============

TEST_F(AstExtractorUnitTest, ExtractLabel_Null) {
	std::string result = AstExtractor::extractLabel(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractLabelFromNodeLabel Tests ==============

TEST_F(AstExtractorUnitTest, ExtractLabelFromNodeLabel_Null) {
	std::string result = AstExtractor::extractLabelFromNodeLabel(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractRelType Tests ==============

TEST_F(AstExtractorUnitTest, ExtractRelType_Null) {
	std::string result = AstExtractor::extractRelType(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== extractPropertyKeyFromExpr Tests ==============

TEST_F(AstExtractorUnitTest, ExtractPropertyKeyFromExpr_Null) {
	std::string result = AstExtractor::extractPropertyKeyFromExpr(nullptr);
	EXPECT_TRUE(result.empty());
}

// ============== parseValue Tests ==============

TEST_F(AstExtractorUnitTest, ParseValue_NullLiteralContext) {
	graph::PropertyValue result = AstExtractor::parseValue(nullptr);
	// Default PropertyValue is NULL_TYPE, not UNKNOWN
	EXPECT_EQ(result.getType(), graph::PropertyType::NULL_TYPE);
}

// ============== extractProperties Tests ==============

TEST_F(AstExtractorUnitTest, ExtractProperties_NullContext) {
	std::function<graph::PropertyValue(graph::parser::cypher::CypherParser::ExpressionContext*)> evaluator =
		[](graph::parser::cypher::CypherParser::ExpressionContext*) {
			return graph::PropertyValue();
		};
	auto result = AstExtractor::extractProperties(nullptr, evaluator);
	EXPECT_TRUE(result.empty());
}

TEST_F(AstExtractorUnitTest, ExtractProperties_WithEvaluator) {
	// Tests with null context - evaluator should not be called
	int callCount = 0;
	std::function<graph::PropertyValue(graph::parser::cypher::CypherParser::ExpressionContext*)> evaluator =
		[&callCount](graph::parser::cypher::CypherParser::ExpressionContext*) {
			callCount++;
			return graph::PropertyValue();
		};
	auto result = AstExtractor::extractProperties(nullptr, evaluator);

	EXPECT_EQ(callCount, 0);
	EXPECT_TRUE(result.empty());
}
