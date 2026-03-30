/**
 * @file test_PatternBuilder.cpp
 * @brief Unit tests for PatternBuilder helper class
 */

#include <gtest/gtest.h>
#include "helpers/PatternBuilder.hpp"
#include "graph/query/logical/LogicalOperator.hpp"

using namespace graph::parser::cypher::helpers;

// Test class for PatternBuilder
class PatternBuilderUnitTest : public ::testing::Test {
protected:
	// Helper methods
	std::shared_ptr<graph::query::QueryPlanner> createNullPlanner() {
		return nullptr;
	}
};

// ============== buildMatchPattern Tests ==============

TEST_F(PatternBuilderUnitTest, BuildMatchPattern_NullPattern) {
	auto result = PatternBuilder::buildMatchPattern(
		nullptr,
		nullptr,
		createNullPlanner(),
		nullptr
	);
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderUnitTest, BuildMatchPattern_NullPatternWithRoot) {
	std::unique_ptr<graph::query::logical::LogicalOperator> mockRoot = nullptr;
	auto result = PatternBuilder::buildMatchPattern(
		nullptr,
		std::move(mockRoot),
		createNullPlanner(),
		nullptr
	);
	EXPECT_EQ(result, nullptr);
}

// ============== buildCreatePattern Tests ==============

TEST_F(PatternBuilderUnitTest, BuildCreatePattern_NullPattern) {
	auto result = PatternBuilder::buildCreatePattern(
		nullptr,
		nullptr,
		createNullPlanner()
	);
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderUnitTest, BuildCreatePattern_NullPatternWithRoot) {
	std::unique_ptr<graph::query::logical::LogicalOperator> mockRoot = nullptr;
	auto result = PatternBuilder::buildCreatePattern(
		nullptr,
		std::move(mockRoot),
		createNullPlanner()
	);
	EXPECT_EQ(result, nullptr);
}

// ============== buildMergePattern Tests ==============

TEST_F(PatternBuilderUnitTest, BuildMergePattern_NullPatternPart) {
	std::vector<graph::query::execution::operators::SetItem> onCreateItems;
	std::vector<graph::query::execution::operators::SetItem> onMatchItems;

	auto result = PatternBuilder::buildMergePattern(
		nullptr,
		onCreateItems,
		onMatchItems,
		createNullPlanner()
	);
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderUnitTest, BuildMergePattern_NullPlanner) {
	std::vector<graph::query::execution::operators::SetItem> onCreateItems;
	std::vector<graph::query::execution::operators::SetItem> onMatchItems;

	auto result = PatternBuilder::buildMergePattern(
		nullptr,
		onCreateItems,
		onMatchItems,
		nullptr
	);
	EXPECT_EQ(result, nullptr);
}

TEST_F(PatternBuilderUnitTest, BuildMergePattern_EmptyVectorsNullPlanner) {
	std::vector<graph::query::execution::operators::SetItem> onCreateItems;
	std::vector<graph::query::execution::operators::SetItem> onMatchItems;

	auto result = PatternBuilder::buildMergePattern(
		nullptr,
		onCreateItems,
		onMatchItems,
		nullptr
	);
	EXPECT_EQ(result, nullptr);
}

// ============== extractSetItems Tests ==============

TEST_F(PatternBuilderUnitTest, ExtractSetItems_NullContext) {
	auto result = PatternBuilder::extractSetItems(nullptr);
	EXPECT_TRUE(result.empty());
}
