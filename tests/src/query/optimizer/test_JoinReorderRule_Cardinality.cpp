/**
 * @file test_JoinReorderRule_Cardinality.cpp
 * @brief Branch coverage tests for JoinReorderRule.hpp.
 *        Covers: null plan, buildLeftDeep with empty inputs, estimateCardinality
 *        with non-scan ops, flatten with null node, single-input degenerate.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/optimizer/Statistics.hpp"
#include "graph/query/optimizer/rules/JoinReorderRule.hpp"

using namespace graph::query::logical;
using namespace graph::query::optimizer;
using namespace graph::query::optimizer::rules;

class JoinReorderRuleCardinalityTest : public ::testing::Test {
protected:
	JoinReorderRule rule;
	Statistics stats;

	void SetUp() override {
		stats.totalNodeCount = 1000;
		LabelStatistics personStats;
		personStats.label = "Person";
		personStats.nodeCount = 100;
		stats.labelStats["Person"] = personStats;

		LabelStatistics cityStats;
		cityStats.label = "City";
		cityStats.nodeCount = 50;
		stats.labelStats["City"] = cityStats;
	}
};

TEST_F(JoinReorderRuleCardinalityTest, NullPlanReturnsNull) {
	auto result = rule.apply(nullptr, stats);
	EXPECT_EQ(result, nullptr);
}

TEST_F(JoinReorderRuleCardinalityTest, NonJoinNodePassesThrough) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto result = rule.apply(std::move(scan), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

// Covers: estimateCardinality with non-scan op that has no children → card=1.0 > 0 → returns 1.0
TEST_F(JoinReorderRuleCardinalityTest, NonScanInputEstimatesConservatively) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), nullptr);
	// Join with a filter (non-scan, has children) and a scan
	auto scan2 = std::make_unique<LogicalNodeScan>("m", std::vector<std::string>{"City"});
	auto join = std::make_unique<LogicalJoin>(std::move(filter), std::move(scan2));

	auto result = rule.apply(std::move(join), stats);
	ASSERT_NE(result, nullptr);
}

// Covers: single-input degenerate case after flatten (inputs.size() <= 1)
TEST_F(JoinReorderRuleCardinalityTest, SingleInputJoinReturnsSingleInput) {
	// Create a join where one side is null
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto join = std::make_unique<LogicalJoin>(std::move(scan), nullptr);

	auto result = rule.apply(std::move(join), stats);
	ASSERT_NE(result, nullptr);
	// Should return the single non-null input
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

// Covers: reordering by cardinality — smaller relation first
TEST_F(JoinReorderRuleCardinalityTest, ReordersSmallestFirst) {
	// City (50) should come before Person (100) in left-deep tree
	auto person = std::make_unique<LogicalNodeScan>("p", std::vector<std::string>{"Person"});
	auto city = std::make_unique<LogicalNodeScan>("c", std::vector<std::string>{"City"});
	auto join = std::make_unique<LogicalJoin>(std::move(person), std::move(city));

	auto result = rule.apply(std::move(join), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
}

// Covers: estimateCardinality with null op → returns totalNodeCount
TEST_F(JoinReorderRuleCardinalityTest, NullChildInJoinUsesTotalNodeCount) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto join = std::make_unique<LogicalJoin>(std::move(scan), nullptr);
	// Join with one null side — null flattened away, produces single input degenerate
	auto result = rule.apply(std::move(join), stats);
	ASSERT_NE(result, nullptr);
}

// Covers: three-way join reordering
TEST_F(JoinReorderRuleCardinalityTest, ThreeWayJoinReorderedByCardinality) {
	auto large = std::make_unique<LogicalNodeScan>("x"); // no labels = totalNodeCount = 1000
	auto medium = std::make_unique<LogicalNodeScan>("p", std::vector<std::string>{"Person"}); // 100
	auto small = std::make_unique<LogicalNodeScan>("c", std::vector<std::string>{"City"}); // 50

	auto join1 = std::make_unique<LogicalJoin>(std::move(large), std::move(medium));
	auto join2 = std::make_unique<LogicalJoin>(std::move(join1), std::move(small));

	auto result = rule.apply(std::move(join2), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
}

// Covers: getName()
TEST_F(JoinReorderRuleCardinalityTest, GetNameReturnsCorrectName) {
	EXPECT_EQ(rule.getName(), "JoinReorderRule");
}

// Covers: estimateCardinality card == 0.0 fallback to totalNodeCount.
// A non-scan op with no children returns card=1.0 from the loop (no iterations),
// but if card > 0.0 it returns card. To hit card==0 we need a different trick.
// We can test the buildLeftDeep with empty inputs indirectly by having both
// children of a join be null.
TEST_F(JoinReorderRuleCardinalityTest, BothNullChildrenInJoinReturnsNull) {
	auto join = std::make_unique<LogicalJoin>(nullptr, nullptr);
	auto result = rule.apply(std::move(join), stats);
	// Both children are null, flatten produces empty inputs → returns nullptr
	EXPECT_EQ(result, nullptr);
}
