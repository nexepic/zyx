#include <gtest/gtest.h>
#include "query/execution/FixedWidthColumn.hpp"

using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::detail::ColumnCellState;
using graph::query::execution::detail::FixedWidthColumn;

TEST(FixedWidthColumnTest, BoundedReservationPreservesRowsAndAvoidsAppendGrowth) {
	MaterializedByteBudget budget(4096U);
	FixedWidthColumn<int64_t> column(7U, budget);
	column.append(42);
	column.reserve(7U);
	const size_t retained = budget.used();
	for (int64_t value = 1; value < 7; ++value) column.append(value);
	EXPECT_EQ(budget.used(), retained);
	EXPECT_EQ(column.value(0U), 42);
	EXPECT_EQ(column.size(), 7U);
	column.reserve(0U);
	EXPECT_THROW(column.reserve(8U), std::out_of_range);
	EXPECT_EQ(column.value(0U), 42);
}

TEST(FixedWidthColumnTest, PartialReservationFailureKeepsRowsAndChargesRetainedCapacity) {
	MaterializedByteBudget budget(4U * sizeof(int64_t));
	FixedWidthColumn<int64_t> column(4U, budget);
	EXPECT_THROW(column.reserve(4U), graph::query::execution::MaterializedByteLimitExceeded);
	EXPECT_EQ(column.size(), 0U);
	EXPECT_EQ(budget.used(), 4U * sizeof(int64_t));
	column.release();
	EXPECT_EQ(budget.used(), 0U);
}

TEST(FixedWidthColumnTest, DistinguishesValueNullAndMissingWithoutStringKeys) {
	MaterializedByteBudget budget(1024);
	FixedWidthColumn<int64_t> column(8, budget);
	column.append(42);
	column.appendNull();
	column.appendMissing();
	EXPECT_EQ(column.size(), 3U);
	EXPECT_EQ(column.value(0), 42);
	EXPECT_EQ(column.state(1), ColumnCellState::CCS_NULL);
	EXPECT_EQ(column.state(2), ColumnCellState::CCS_MISSING);
	EXPECT_THROW((void) column.value(1), std::logic_error);
	EXPECT_THROW((void) column.value(2), std::logic_error);
	EXPECT_THROW((void) column.state(3), std::out_of_range);
	EXPECT_THROW((void) column.value(3), std::out_of_range);
}

TEST(FixedWidthColumnTest, BoolUsesContiguousByteStorageInsteadOfVectorBool) {
	MaterializedByteBudget budget(1024);
	FixedWidthColumn<bool> column(4, budget);
	column.append(true);
	column.append(false);
	EXPECT_TRUE(column.value(0));
	EXPECT_FALSE(column.value(1));
	EXPECT_EQ(column.values()[0], 1U);
	EXPECT_EQ(column.values()[1], 0U);
}

TEST(FixedWidthColumnTest, ClearReusesCapacityAndReleaseRefundsIt) {
	MaterializedByteBudget budget(1024);
	FixedWidthColumn<double> column(4, budget);
	column.append(1.5);
	const auto retained = budget.used();
	EXPECT_GE(retained, sizeof(double) + sizeof(ColumnCellState));
	column.clear();
	EXPECT_EQ(column.size(), 0U);
	EXPECT_EQ(budget.used(), retained);
	column.append(2.5);
	EXPECT_EQ(column.value(0), 2.5);
	EXPECT_EQ(budget.used(), retained);
	column.release();
	EXPECT_EQ(column.size(), 0U);
	EXPECT_EQ(budget.used(), 0U);
	column.append(3.5);
	EXPECT_EQ(column.value(0), 3.5);
}

TEST(FixedWidthColumnTest, DestructionRefundsOnlyItsOwnSharedCharge) {
	MaterializedByteBudget budget(1024);
	FixedWidthColumn<int64_t> retained(8, budget);
	retained.append(1);
	const auto previous = budget.used();
	{
		FixedWidthColumn<double> temporary(8, budget);
		temporary.append(2.0);
		EXPECT_GT(budget.used(), previous);
	}
	EXPECT_EQ(budget.used(), previous);
	EXPECT_EQ(retained.value(0), 1);
}

TEST(FixedWidthColumnTest, FailedAllocationPreservesRowsAndAccountsRetainedCapacity) {
	MaterializedByteBudget budget(sizeof(int64_t));
	FixedWidthColumn<int64_t> column(8, budget);
	EXPECT_THROW(column.append(1), std::runtime_error);
	EXPECT_EQ(column.size(), 0U);
	EXPECT_TRUE(column.states().empty());
	EXPECT_EQ(budget.used(), sizeof(int64_t));
	column.release();
	EXPECT_EQ(budget.used(), 0U);
}

TEST(FixedWidthColumnTest, GrowthChargesOldAndNewAllocationsWithoutLosingPreviousCells) {
	MaterializedByteBudget budget(18);
	FixedWidthColumn<int64_t> column(2, budget);
	column.append(7);
	EXPECT_THROW(column.append(8), std::runtime_error);
	EXPECT_EQ(column.size(), 1U);
	EXPECT_EQ(column.value(0), 7);
	EXPECT_EQ(budget.used(), 9U);
}

TEST(FixedWidthColumnTest, RowBoundAndInvalidLimitFailExplicitly) {
	MaterializedByteBudget budget(1024);
	EXPECT_THROW((FixedWidthColumn<int64_t>(0, budget)), std::invalid_argument);
	FixedWidthColumn<int64_t> column(1, budget);
	column.appendNull();
	EXPECT_THROW(column.appendMissing(), std::runtime_error);
	EXPECT_EQ(column.size(), 1U);
	EXPECT_EQ(column.state(0), ColumnCellState::CCS_NULL);
}

TEST(FixedWidthColumnTest, UpstreamAllocationFailureRollsBackBudgetAdmission) {
	MaterializedByteBudget budget(1024);
	FixedWidthColumn<int64_t> column(4, budget, std::pmr::null_memory_resource());
	EXPECT_THROW(column.append(1), std::bad_alloc);
	EXPECT_EQ(column.size(), 0U);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(FixedWidthColumnTest, NonPowerOfTwoBoundRetainsEveryCellAndRejectsNullAllocator) {
	MaterializedByteBudget budget(1024);
	EXPECT_THROW((FixedWidthColumn<int64_t>(4, budget, nullptr)), std::invalid_argument);
	FixedWidthColumn<int64_t> column(3, budget);
	for (int64_t value = 0; value < 3; ++value) column.append(value);
	for (size_t index = 0; index < 3; ++index) EXPECT_EQ(column.value(index), static_cast<int64_t>(index));
	EXPECT_EQ(column.states().size(), column.values().size());
	EXPECT_THROW(column.append(3), std::runtime_error);
}

TEST(FixedWidthColumnTest, BatchRollbackDropsUnpublishedCellsWithoutRefundingLiveCapacity) {
	MaterializedByteBudget budget(1024);
	FixedWidthColumn<int64_t> column(4, budget);
	column.append(1);
	column.appendNull();
	const auto charged = budget.used();
	column.truncate(1);
	EXPECT_EQ(column.size(), 1U);
	EXPECT_EQ(column.value(0), 1);
	EXPECT_EQ(budget.used(), charged);
	column.append(3);
	EXPECT_EQ(column.value(1), 3);
	EXPECT_EQ(column.state(1), ColumnCellState::CCS_VALUE);
	EXPECT_THROW(column.truncate(3), std::out_of_range);
	EXPECT_EQ(column.size(), 2U);
}
