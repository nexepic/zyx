/**
 * @file test_LogicalPropertyPredicate.cpp
 * @brief Native logical property-predicate contract tests.
 */

#include <gtest/gtest.h>

#include "graph/query/execution/LogicalPropertyPredicate.hpp"

namespace {

using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
using graph::query::execution::evaluateLogicalPropertyPredicate;
using graph::query::execution::LogicalPropertyPredicate;
using graph::query::execution::LogicalPropertyPredicateOp;

LogicalPropertyPredicate predicate(
	LogicalPropertyPredicateOp op,
	PropertyValue value,
	std::optional<PropertyValue> upperValue = std::nullopt
) {
	return {"value", op, std::move(value), std::move(upperValue)};
}

} // namespace

TEST(LogicalPropertyPredicateTest, RejectsMissingPropertiesAndCoversAllTypedIntegerOperators) {
	const PropertyValue actual(int64_t{10});

	EXPECT_FALSE(evaluateLogicalPropertyPredicate(std::nullopt,
		predicate(LogicalPropertyPredicateOp::LPP_NE, PropertyValue(int64_t{0}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(int64_t{10}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_NE, PropertyValue(int64_t{9}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_LT, PropertyValue(int64_t{11}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_LE, PropertyValue(int64_t{10}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_GT, PropertyValue(int64_t{9}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_GE, PropertyValue(int64_t{10}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(int64_t{9}), PropertyValue(int64_t{11}))));
	EXPECT_FALSE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(int64_t{11}), PropertyValue(int64_t{12}))));
	EXPECT_FALSE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(int64_t{8}), PropertyValue(int64_t{9}))));
	EXPECT_FALSE(evaluateLogicalPropertyPredicate(actual,
		predicate(LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(int64_t{9}))));
}

TEST(LogicalPropertyPredicateTest, DispatchesEverySupportedTypedRepresentation) {
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue(1.5),
		predicate(LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(1.5))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue(true),
		predicate(LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(true))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue("native"),
		predicate(LogicalPropertyPredicateOp::LPP_EQ, PropertyValue("native"))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue(TemporalDate{4}),
		predicate(LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(TemporalDate{4}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue(TemporalDateTime{5}),
		predicate(LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(TemporalDateTime{5}))));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue(TemporalDuration{0, 2, 0}),
		predicate(LogicalPropertyPredicateOp::LPP_RANGE_CLOSED,
			PropertyValue(TemporalDuration{0, 1, 0}), PropertyValue(TemporalDuration{0, 3, 0}))));
}

TEST(LogicalPropertyPredicateTest, FallsBackForStructuredValuesAndRejectsUnknownOperators) {
	const PropertyValue::MapType map{{"name", PropertyValue("zyx")}};
	const PropertyValue structured(map);
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(structured,
		predicate(LogicalPropertyPredicateOp::LPP_EQ, structured)));
	EXPECT_TRUE(evaluateLogicalPropertyPredicate(PropertyValue(int64_t{1}),
		predicate(LogicalPropertyPredicateOp::LPP_LT, PropertyValue(1.0))));
	EXPECT_FALSE(evaluateLogicalPropertyPredicate(structured,
		predicate(static_cast<LogicalPropertyPredicateOp>(99), structured)));
	EXPECT_FALSE(evaluateLogicalPropertyPredicate(PropertyValue(int64_t{1}),
		predicate(static_cast<LogicalPropertyPredicateOp>(99), PropertyValue(int64_t{1}))));
}
