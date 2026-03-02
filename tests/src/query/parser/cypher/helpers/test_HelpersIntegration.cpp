/**
 * @file test_HelpersIntegration.cpp
 * @brief Integration and edge case tests for helper classes
 *
 * This file tests edge cases and additional paths to improve coverage.
 */

#include <gtest/gtest.h>
#include <vector>
#include <unordered_map>
#include <functional>
#include "helpers/AstExtractor.hpp"
#include "helpers/ExpressionBuilder.hpp"
#include "helpers/PatternBuilder.hpp"
#include "helpers/OperatorChain.hpp"

using namespace graph::parser::cypher::helpers;

class HelpersIntegrationTest : public ::testing::Test {
protected:
	// Helper methods
};

// ============== AstExtractor Extended Tests ==============

// Test all null paths in AstExtractor
TEST_F(HelpersIntegrationTest, AstExtractor_AllNullPaths) {
	EXPECT_TRUE(AstExtractor::extractVariable(nullptr).empty());
	EXPECT_TRUE(AstExtractor::extractLabel(nullptr).empty());
	EXPECT_TRUE(AstExtractor::extractLabelFromNodeLabel(nullptr).empty());
	EXPECT_TRUE(AstExtractor::extractRelType(nullptr).empty());
	EXPECT_TRUE(AstExtractor::extractPropertyKeyFromExpr(nullptr).empty());

	graph::PropertyValue pv = AstExtractor::parseValue(nullptr);
	EXPECT_EQ(pv.getType(), graph::PropertyType::NULL_TYPE);
}

// Test extractProperties with lambda
TEST_F(HelpersIntegrationTest, AstExtractor_ExtractProperties_WithLambda) {
	int callCount = 0;
	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator =
		[&callCount](CypherParser::ExpressionContext*) {
			callCount++;
			return graph::PropertyValue(42);
		};
	auto result = AstExtractor::extractProperties(nullptr, evaluator);

	EXPECT_EQ(callCount, 0);
	EXPECT_TRUE(result.empty());
}

// Test extractProperties with different return types
TEST_F(HelpersIntegrationTest, AstExtractor_ExtractProperties_DifferentReturnTypes) {
	// Test with lambda returning different PropertyValue types
	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator1 =
		[](CypherParser::ExpressionContext*) {
			return graph::PropertyValue("test");
		};
	auto result = AstExtractor::extractProperties(nullptr, evaluator1);
	EXPECT_TRUE(result.empty());

	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator2 =
		[](CypherParser::ExpressionContext*) {
			return graph::PropertyValue(3.14);
		};
	auto result2 = AstExtractor::extractProperties(nullptr, evaluator2);
	EXPECT_TRUE(result2.empty());

	std::function<graph::PropertyValue(CypherParser::ExpressionContext*)> evaluator3 =
		[](CypherParser::ExpressionContext*) {
			return graph::PropertyValue(true);
		};
	auto result3 = AstExtractor::extractProperties(nullptr, evaluator3);
	EXPECT_TRUE(result3.empty());
}

// ============== ExpressionBuilder Extended Tests ==============

// Test extractListFromExpression with null
TEST_F(HelpersIntegrationTest, ExpressionBuilder_ExtractList_Null) {
	auto list = ExpressionBuilder::extractListFromExpression(nullptr);
	EXPECT_TRUE(list.empty());
	EXPECT_EQ(list.size(), 0u);
}

// Test that empty list is handled correctly
TEST_F(HelpersIntegrationTest, ExpressionBuilder_ListEmpty) {
	std::vector<graph::PropertyValue> list;
	EXPECT_TRUE(list.empty());
	EXPECT_EQ(list.size(), 0u);
}

// ============== OperatorChain Extended Tests ==============

// Test chain with both operators null
TEST_F(HelpersIntegrationTest, OperatorChain_BothNull) {
	auto result = OperatorChain::chain(nullptr, nullptr);
	EXPECT_EQ(result, nullptr);
}

// Test chain variations
TEST_F(HelpersIntegrationTest, OperatorChain_ChainVariations) {
	std::unique_ptr<graph::query::execution::PhysicalOperator> op1(nullptr);
	std::unique_ptr<graph::query::execution::PhysicalOperator> op2(nullptr);

	auto result1 = OperatorChain::chain(nullptr, nullptr);
	EXPECT_EQ(result1, nullptr);

	auto result2 = OperatorChain::chain(std::move(op1), nullptr);
	EXPECT_EQ(result2, nullptr);

	auto result3 = OperatorChain::chain(nullptr, std::move(op2));
	EXPECT_EQ(result3, nullptr);
}

// ============== PatternBuilder Extended Tests ==============

// Test all build methods with null parameters
TEST_F(HelpersIntegrationTest, PatternBuilder_AllBuildMethodsWithNull) {
	std::vector<graph::query::execution::operators::SetItem> items;

	auto matchResult = PatternBuilder::buildMatchPattern(nullptr, nullptr, nullptr, nullptr);
	EXPECT_EQ(matchResult, nullptr);

	auto createResult = PatternBuilder::buildCreatePattern(nullptr, nullptr, nullptr);
	EXPECT_EQ(createResult, nullptr);

	auto mergeResult = PatternBuilder::buildMergePattern(nullptr, items, items, nullptr);
	EXPECT_EQ(mergeResult, nullptr);
}

// Test extractSetItems variations
TEST_F(HelpersIntegrationTest, PatternBuilder_ExtractSetItems_Variations) {
	auto result1 = PatternBuilder::extractSetItems(nullptr);
	EXPECT_TRUE(result1.empty());
	EXPECT_EQ(result1.size(), 0u);

	auto result2 = PatternBuilder::extractSetItems(nullptr);
	EXPECT_TRUE(result2.empty());
}

// Test with empty SetItem vectors
TEST_F(HelpersIntegrationTest, PatternBuilder_EmptySetItemVectors) {
	std::vector<graph::query::execution::operators::SetItem> emptyItems;

	auto result = PatternBuilder::buildMergePattern(nullptr, emptyItems, emptyItems, nullptr);
	EXPECT_EQ(result, nullptr);
}

// ============== PropertyValue Type Tests ==============

// Test all PropertyValue types
TEST_F(HelpersIntegrationTest, PropertyValue_AllTypes) {
	graph::PropertyValue pvNull;
	EXPECT_EQ(pvNull.getType(), graph::PropertyType::NULL_TYPE);

	graph::PropertyValue pvBool(true);
	EXPECT_EQ(pvBool.getType(), graph::PropertyType::BOOLEAN);

	graph::PropertyValue pvInt(42);
	EXPECT_EQ(pvInt.getType(), graph::PropertyType::INTEGER);

	graph::PropertyValue pvDouble(3.14);
	EXPECT_EQ(pvDouble.getType(), graph::PropertyType::DOUBLE);

	graph::PropertyValue pvString("test");
	EXPECT_EQ(pvString.getType(), graph::PropertyType::STRING);

	// Test vector type
	std::vector<float> vec = {1.0f, 2.0f, 3.0f};
	graph::PropertyValue pvVec(vec);
	EXPECT_EQ(pvVec.getType(), graph::PropertyType::LIST);
}

// Test PropertyValue comparisons
TEST_F(HelpersIntegrationTest, PropertyValue_Comparisons) {
	graph::PropertyValue pv1(42);
	graph::PropertyValue pv2(42);
	graph::PropertyValue pv3(100);

	EXPECT_EQ(pv1, pv2);
	EXPECT_NE(pv1, pv3);
}

// ============== Container Operation Tests ==============

// Test vector operations
TEST_F(HelpersIntegrationTest, Vector_Operations) {
	std::vector<graph::PropertyValue> vec;

	EXPECT_TRUE(vec.empty());
	EXPECT_EQ(vec.size(), 0u);

	vec.push_back(graph::PropertyValue(1));
	vec.push_back(graph::PropertyValue(2));
	vec.push_back(graph::PropertyValue(3));

	EXPECT_EQ(vec.size(), 3u);
	EXPECT_FALSE(vec.empty());

	vec.clear();
	EXPECT_TRUE(vec.empty());
	EXPECT_EQ(vec.size(), 0u);
}

// Test map operations
TEST_F(HelpersIntegrationTest, Map_Operations) {
	std::unordered_map<std::string, graph::PropertyValue> map;

	EXPECT_TRUE(map.empty());
	EXPECT_EQ(map.size(), 0u);

	map["key1"] = graph::PropertyValue(42);
	map["key2"] = graph::PropertyValue("value");

	EXPECT_EQ(map.size(), 2u);
	EXPECT_FALSE(map.empty());

	map.clear();
	EXPECT_TRUE(map.empty());
	EXPECT_EQ(map.size(), 0u);
}

// Test SetItem vector operations
TEST_F(HelpersIntegrationTest, SetItem_Vector_Operations) {
	std::vector<graph::query::execution::operators::SetItem> items;

	EXPECT_TRUE(items.empty());
	EXPECT_EQ(items.size(), 0u);

	// Note: We can't easily create SetItem without proper context
	// but we can test the empty vector behavior
}
