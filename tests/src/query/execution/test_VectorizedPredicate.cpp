#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeColumnBatch.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"

namespace {

using graph::PropertyValue;
using graph::debug::PerfTrace;
using graph::query::execution::NodeColumnBatch;
using graph::query::execution::VectorPredicateOp;
using graph::query::execution::VectorizedPropertyPredicate;
using graph::query::execution::applyPredicate;
using graph::query::execution::applyPredicates;
using graph::query::execution::evaluatePredicateValue;

TEST(VectorizedPredicateTest, EqualitySelectsMatchingRows) {
	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3};
	batch.propertyColumns["age"] = {PropertyValue(42), PropertyValue(7), PropertyValue(42)};

	VectorizedPropertyPredicate predicate;
	predicate.variable = "n";
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(42);

	applyPredicate(batch, predicate);

	ASSERT_EQ(batch.selected.size(), 3U);
	EXPECT_EQ(batch.selected[0], 1U);
	EXPECT_EQ(batch.selected[1], 0U);
	EXPECT_EQ(batch.selected[2], 1U);
	EXPECT_EQ(batch.selectedCount(), 2U);
}

TEST(VectorizedPredicateTest, ClosedRangeSelectsRowsInsideInclusiveBounds) {
	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3, 4};
	batch.propertyColumns["score"] = {PropertyValue(9), PropertyValue(10), PropertyValue(20), PropertyValue(21)};

	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "score";
	predicate.op = VectorPredicateOp::VPO_RANGE_CLOSED;
	predicate.value = PropertyValue(10);
	predicate.upperValue = PropertyValue(20);

	applyPredicate(batch, predicate);

	ASSERT_EQ(batch.selected.size(), 4U);
	EXPECT_EQ(batch.selected[0], 0U);
	EXPECT_EQ(batch.selected[1], 1U);
	EXPECT_EQ(batch.selected[2], 1U);
	EXPECT_EQ(batch.selected[3], 0U);
	EXPECT_EQ(batch.selectedCount(), 2U);
}

TEST(VectorizedPredicateTest, MissingColumnDeselectsAllRows) {
	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3};
	batch.selected = {1, 1, 0};

	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "missing";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(42);

	applyPredicate(batch, predicate);

	ASSERT_EQ(batch.selected.size(), 3U);
	EXPECT_EQ(batch.selected[0], 0U);
	EXPECT_EQ(batch.selected[1], 0U);
	EXPECT_EQ(batch.selected[2], 0U);
	EXPECT_EQ(batch.selectedCount(), 0U);
}

TEST(VectorizedPredicateTest, ExistingDeselectedRowsStayDeselected) {
	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3};
	batch.selected = {0, 1, 1};
	batch.propertyColumns["age"] = {PropertyValue(42), PropertyValue(42), PropertyValue(7)};

	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(42);

	applyPredicate(batch, predicate);

	ASSERT_EQ(batch.selected.size(), 3U);
	EXPECT_EQ(batch.selected[0], 0U);
	EXPECT_EQ(batch.selected[1], 1U);
	EXPECT_EQ(batch.selected[2], 0U);
	EXPECT_EQ(batch.selectedCount(), 1U);
}

TEST(VectorizedPredicateTest, MissingActualPropertyEvaluatesFalseForEveryOperator) {
	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "age";
	predicate.value = PropertyValue(42);
	predicate.upperValue = PropertyValue(50);

	predicate.op = VectorPredicateOp::VPO_EQ;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
	predicate.op = VectorPredicateOp::VPO_NE;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
	predicate.op = VectorPredicateOp::VPO_LT;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
	predicate.op = VectorPredicateOp::VPO_LE;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
	predicate.op = VectorPredicateOp::VPO_GT;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
	predicate.op = VectorPredicateOp::VPO_GE;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
	predicate.op = VectorPredicateOp::VPO_RANGE_CLOSED;
	EXPECT_FALSE(evaluatePredicateValue(std::nullopt, predicate));
}

TEST(VectorizedPredicateTest, PresentActualPropertySupportsInequalityAndComparisons) {
	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "age";
	predicate.value = PropertyValue(42);

	predicate.op = VectorPredicateOp::VPO_NE;
	predicate.value = PropertyValue(7);
	EXPECT_TRUE(evaluatePredicateValue(PropertyValue(42), predicate));
	predicate.value = PropertyValue(42);
	EXPECT_FALSE(evaluatePredicateValue(PropertyValue(42), predicate));

	predicate.op = VectorPredicateOp::VPO_LT;
	predicate.value = PropertyValue(50);
	EXPECT_TRUE(evaluatePredicateValue(PropertyValue(42), predicate));
	predicate.value = PropertyValue(40);
	EXPECT_FALSE(evaluatePredicateValue(PropertyValue(42), predicate));

	predicate.op = VectorPredicateOp::VPO_LE;
	predicate.value = PropertyValue(42);
	EXPECT_TRUE(evaluatePredicateValue(PropertyValue(42), predicate));

	predicate.op = VectorPredicateOp::VPO_GT;
	predicate.value = PropertyValue(40);
	EXPECT_TRUE(evaluatePredicateValue(PropertyValue(42), predicate));
	predicate.value = PropertyValue(50);
	EXPECT_FALSE(evaluatePredicateValue(PropertyValue(42), predicate));

	predicate.op = VectorPredicateOp::VPO_GE;
	predicate.value = PropertyValue(42);
	EXPECT_TRUE(evaluatePredicateValue(PropertyValue(42), predicate));
}

TEST(VectorizedPredicateTest, EmptySelectionVectorTreatsAllRowsAsInitiallySelected) {
	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3};
	batch.propertyColumns["age"] = {PropertyValue(42), PropertyValue(7), PropertyValue(42)};

	VectorizedPropertyPredicate predicate;
	predicate.propertyKey = "age";
	predicate.op = VectorPredicateOp::VPO_EQ;
	predicate.value = PropertyValue(42);

	applyPredicate(batch, predicate);

	ASSERT_EQ(batch.selected.size(), 3U);
	EXPECT_EQ(batch.selected[0], 1U);
	EXPECT_EQ(batch.selected[1], 0U);
	EXPECT_EQ(batch.selected[2], 1U);
}

TEST(VectorizedPredicateTest, ApplyPredicatesUsesConjunctionAndEmitsPerfTrace) {
	PerfTrace::reset();
	PerfTrace::setEnabled(true);

	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3};
	batch.propertyColumns["age"] = {PropertyValue(30), PropertyValue(40), PropertyValue(50)};
	batch.propertyColumns["score"] = {PropertyValue(10), PropertyValue(20), PropertyValue(30)};

	VectorizedPropertyPredicate olderThanThirty;
	olderThanThirty.propertyKey = "age";
	olderThanThirty.op = VectorPredicateOp::VPO_GT;
	olderThanThirty.value = PropertyValue(30);

	VectorizedPropertyPredicate scoreAtMostTwenty;
	scoreAtMostTwenty.propertyKey = "score";
	scoreAtMostTwenty.op = VectorPredicateOp::VPO_LE;
	scoreAtMostTwenty.value = PropertyValue(20);

	applyPredicates(batch, {olderThanThirty, scoreAtMostTwenty});

	ASSERT_EQ(batch.selected.size(), 3U);
	EXPECT_EQ(batch.selected[0], 0U);
	EXPECT_EQ(batch.selected[1], 1U);
	EXPECT_EQ(batch.selected[2], 0U);

	auto snapshot = PerfTrace::snapshotAndReset();
	PerfTrace::setEnabled(false);
	ASSERT_TRUE(snapshot.contains("node_scan.filter"));
	EXPECT_EQ(snapshot["node_scan.filter"].calls, 1U);
}

} // namespace
