/**
 * @file test_FilterPushdownRule_Predicates.cpp
 * @brief Branch coverage tests for FilterPushdownRule.hpp.
 *        Covers: range predicate tightening with existing min/max bounds,
 *        same-value exclusive wins, non-tighter bound kept, null plan,
 *        non-filter passthrough.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/optimizer/Statistics.hpp"
#include "graph/query/optimizer/rules/FilterPushdownRule.hpp"

using namespace graph::query::expressions;
using namespace graph::query::logical;
using namespace graph::query::optimizer;

namespace {

std::shared_ptr<Expression> litInt(int64_t value) {
	return std::make_shared<LiteralExpression>(value);
}

std::shared_ptr<Expression> propRef(const std::string &var, const std::string &prop) {
	return std::shared_ptr<Expression>(
		std::make_unique<VariableReferenceExpression>(var, prop).release());
}

std::shared_ptr<Expression> binaryExpr(
	const std::shared_ptr<Expression> &lhs,
	BinaryOperatorType op,
	const std::shared_ptr<Expression> &rhs) {
	return std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			lhs ? lhs->clone() : nullptr,
			op,
			rhs ? rhs->clone() : nullptr)
			.release());
}

std::shared_ptr<Expression> andExpr(
	const std::shared_ptr<Expression> &lhs,
	const std::shared_ptr<Expression> &rhs) {
	return binaryExpr(lhs, BinaryOperatorType::BOP_AND, rhs);
}

} // namespace

class FilterPushdownRulePredicatesTest : public ::testing::Test {
protected:
	rules::FilterPushdownRule rule;
	Statistics stats;
};

// Null plan returns null.
TEST_F(FilterPushdownRulePredicatesTest, NullPlanReturnsNull) {
	auto result = rule.apply(nullptr, stats);
	EXPECT_EQ(result, nullptr);
}

// Non-filter node passes through unchanged.
TEST_F(FilterPushdownRulePredicatesTest, NonFilterPassesThrough) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto result = rule.apply(std::move(scan), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

// Covers: range tightening — min bound replaced by tighter (larger) value.
// Start with existing min = 5, then push n.age > 10 → min becomes 10.
TEST_F(FilterPushdownRulePredicatesTest, RangeTighteningMinBound) {
	// Set up a scan that already has a range predicate with minValue = 5
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(int64_t(5)),
									  graph::PropertyValue(), true, true});
	scan->setRangePredicates(std::move(existing));

	// n.age > 10 → min bound should tighten from 5 to 10 (exclusive)
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_GREATER, litInt(10));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	EXPECT_EQ(s->getRangePredicates()[0].minValue, graph::PropertyValue(int64_t(10)));
	EXPECT_FALSE(s->getRangePredicates()[0].minInclusive);
}

// Covers: range tightening — max bound replaced by tighter (smaller) value.
// Start with existing max = 100, then push n.age < 50 → max becomes 50.
TEST_F(FilterPushdownRulePredicatesTest, RangeTighteningMaxBound) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(),
									  graph::PropertyValue(int64_t(100)), true, true});
	scan->setRangePredicates(std::move(existing));

	// n.age < 50 → max bound should tighten from 100 to 50 (exclusive)
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_LESS, litInt(50));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	EXPECT_EQ(s->getRangePredicates()[0].maxValue, graph::PropertyValue(int64_t(50)));
	EXPECT_FALSE(s->getRangePredicates()[0].maxInclusive);
}

// Covers: same-value exclusive wins for min bound.
// existing min = 10 (inclusive), push n.age > 10 → exclusive wins.
TEST_F(FilterPushdownRulePredicatesTest, SameValueExclusiveWinsMin) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(int64_t(10)),
									  graph::PropertyValue(), true, true});
	scan->setRangePredicates(std::move(existing));

	// n.age > 10 (exclusive) at same value → exclusive should win
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_GREATER, litInt(10));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	EXPECT_EQ(s->getRangePredicates()[0].minValue, graph::PropertyValue(int64_t(10)));
	EXPECT_FALSE(s->getRangePredicates()[0].minInclusive);
}

// Covers: same-value exclusive wins for max bound.
// existing max = 50 (inclusive), push n.age < 50 → exclusive wins.
TEST_F(FilterPushdownRulePredicatesTest, SameValueExclusiveWinsMax) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(),
									  graph::PropertyValue(int64_t(50)), true, true});
	scan->setRangePredicates(std::move(existing));

	// n.age < 50 (exclusive) at same value → exclusive should win
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_LESS, litInt(50));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	EXPECT_EQ(s->getRangePredicates()[0].maxValue, graph::PropertyValue(int64_t(50)));
	EXPECT_FALSE(s->getRangePredicates()[0].maxInclusive);
}

// Covers: non-tighter min bound is kept (new value is smaller than existing).
// existing min = 20, push n.age > 5 → min stays 20.
TEST_F(FilterPushdownRulePredicatesTest, NonTighterMinBoundKept) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(int64_t(20)),
									  graph::PropertyValue(), false, true});
	scan->setRangePredicates(std::move(existing));

	// n.age > 5 → NOT tighter than existing min=20
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_GREATER, litInt(5));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	// Min should stay at 20, not change to 5
	EXPECT_EQ(s->getRangePredicates()[0].minValue, graph::PropertyValue(int64_t(20)));
	EXPECT_FALSE(s->getRangePredicates()[0].minInclusive);
}

// Covers: non-tighter max bound is kept (new value is larger than existing).
// existing max = 30, push n.age < 80 → max stays 30.
TEST_F(FilterPushdownRulePredicatesTest, NonTighterMaxBoundKept) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(),
									  graph::PropertyValue(int64_t(30)), true, false});
	scan->setRangePredicates(std::move(existing));

	// n.age < 80 → NOT tighter than existing max=30
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_LESS, litInt(80));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	// Max should stay at 30, not change to 80
	EXPECT_EQ(s->getRangePredicates()[0].maxValue, graph::PropertyValue(int64_t(30)));
	EXPECT_FALSE(s->getRangePredicates()[0].maxInclusive);
}

// Covers: same-value inclusive stays when existing is already exclusive.
// existing min = 10 (exclusive), push n.age >= 10 → inclusive does NOT override.
TEST_F(FilterPushdownRulePredicatesTest, SameValueInclusiveDoesNotOverrideExclusiveMin) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	std::vector<RangePredicate> existing;
	existing.push_back(RangePredicate{"age", graph::PropertyValue(int64_t(10)),
									  graph::PropertyValue(), false, true});
	scan->setRangePredicates(std::move(existing));

	// n.age >= 10 → same value but inclusive, should NOT override existing exclusive
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_GREATER_EQUAL, litInt(10));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
	auto *s = static_cast<LogicalNodeScan *>(result.get());
	ASSERT_EQ(s->getRangePredicates().size(), 1u);
	// Min should stay exclusive
	EXPECT_EQ(s->getRangePredicates()[0].minValue, graph::PropertyValue(int64_t(10)));
	EXPECT_FALSE(s->getRangePredicates()[0].minInclusive);
}

// Covers: getName returns correct name.
TEST_F(FilterPushdownRulePredicatesTest, GetNameReturnsCorrectName) {
	EXPECT_EQ(rule.getName(), "FilterPushdownRule");
}
