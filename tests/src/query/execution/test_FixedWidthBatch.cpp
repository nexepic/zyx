#include <gtest/gtest.h>
#include "query/execution/FixedWidthBatch.hpp"

using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::detail::ColumnCellState;
using graph::query::execution::detail::FixedWidthBatch;
using graph::query::execution::detail::FixedWidthColumn;
using graph::query::execution::detail::TypedCell;

TEST(FixedWidthBatchTest, PublishesRectangularTypedColumnsAndPreservesThreeStates) {
	MaterializedByteBudget budget(1024);
	FixedWidthBatch<int64_t, double, bool> batch(4, budget);
	batch.append(TypedCell<int64_t>::withValue(42), TypedCell<double>::null(), TypedCell<bool>::missing());
	batch.append(TypedCell<int64_t>::missing(), TypedCell<double>::withValue(1.5), TypedCell<bool>::withValue(true));
	EXPECT_EQ(batch.columnCount(), 3U);
	EXPECT_EQ(batch.size(), 2U);
	EXPECT_EQ(batch.column<0>().size(), batch.size());
	EXPECT_EQ(batch.column<1>().size(), batch.size());
	EXPECT_EQ(batch.column<2>().size(), batch.size());
	EXPECT_EQ(batch.column<0>().value(0), 42);
	EXPECT_EQ(batch.column<1>().state(0), ColumnCellState::CCS_NULL);
	EXPECT_EQ(batch.column<2>().state(0), ColumnCellState::CCS_MISSING);
	EXPECT_EQ(batch.column<0>().state(1), ColumnCellState::CCS_MISSING);
	EXPECT_EQ(batch.column<1>().value(1), 1.5);
	EXPECT_TRUE(batch.column<2>().value(1));
}

TEST(FixedWidthBatchTest, LaterColumnFailureRollsBackAllLengthsAndCanRetryAfterPressureRelease) {
	MaterializedByteBudget budget(12);
	FixedWidthColumn<bool> unrelated(1, budget);
	unrelated.append(true);
	FixedWidthBatch<int64_t, bool> batch(2, budget);
	EXPECT_THROW(batch.append(TypedCell<int64_t>::withValue(9), TypedCell<bool>::withValue(false)), std::runtime_error);
	EXPECT_EQ(batch.size(), 0U);
	EXPECT_EQ(batch.column<0>().size(), 0U);
	EXPECT_EQ(batch.column<1>().size(), 0U);
	EXPECT_EQ(budget.used(), 12U);
	unrelated.release();
	batch.append(TypedCell<int64_t>::withValue(10), TypedCell<bool>::withValue(true));
	EXPECT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch.column<0>().value(0), 10);
	EXPECT_TRUE(batch.column<1>().value(0));
	batch.release();
	EXPECT_EQ(budget.used(), 0U);
}

TEST(FixedWidthBatchTest, FailedGrowthKeepsPreviouslyPublishedRows) {
	MaterializedByteBudget budget(20);
	FixedWidthBatch<int64_t, bool> batch(3, budget);
	batch.append(TypedCell<int64_t>::withValue(5), TypedCell<bool>::null());
	EXPECT_THROW(batch.append(TypedCell<int64_t>::withValue(6), TypedCell<bool>::missing()), std::runtime_error);
	EXPECT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch.column<0>().value(0), 5);
	EXPECT_EQ(batch.column<1>().state(0), ColumnCellState::CCS_NULL);
	EXPECT_EQ(batch.column<0>().size(), 1U);
	EXPECT_EQ(batch.column<1>().size(), 1U);
}

TEST(FixedWidthBatchTest, RollsBackEarlierColumnGrowthWhenLaterColumnFailsAfterPublishedRows) {
	MaterializedByteBudget budget(20);
	FixedWidthBatch<bool, int64_t> batch(3, budget);
	batch.append(TypedCell<bool>::null(), TypedCell<int64_t>::withValue(5));
	const auto previousCharge = budget.used();
	EXPECT_THROW(batch.append(TypedCell<bool>::withValue(true), TypedCell<int64_t>::withValue(6)), std::runtime_error);
	EXPECT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch.column<0>().size(), 1U);
	EXPECT_EQ(batch.column<1>().size(), 1U);
	EXPECT_EQ(batch.column<0>().state(0), ColumnCellState::CCS_NULL);
	EXPECT_EQ(batch.column<1>().value(0), 5);
	EXPECT_GT(budget.used(), previousCharge); // Successfully grown capacity still exists.
	batch.clear();
	batch.append(TypedCell<bool>::missing(), TypedCell<int64_t>::withValue(7));
	EXPECT_EQ(batch.column<0>().state(0), ColumnCellState::CCS_MISSING);
	EXPECT_EQ(batch.column<1>().value(0), 7);
}

TEST(FixedWidthBatchTest, ClearAndReleaseApplyToEveryColumnAndRemainReusable) {
	MaterializedByteBudget budget(1024);
	FixedWidthBatch<int64_t, bool> batch(1, budget);
	batch.append(TypedCell<int64_t>::withValue(1), TypedCell<bool>::withValue(false));
	const auto charged = budget.used();
	batch.clear();
	EXPECT_EQ(batch.size(), 0U);
	EXPECT_EQ(budget.used(), charged);
	batch.append(TypedCell<int64_t>::null(), TypedCell<bool>::missing());
	EXPECT_EQ(budget.used(), charged);
	EXPECT_THROW(batch.append(TypedCell<int64_t>::missing(), TypedCell<bool>::null()), std::runtime_error);
	EXPECT_EQ(batch.size(), 1U);
	batch.release();
	EXPECT_EQ(budget.used(), 0U);
	batch.append(TypedCell<int64_t>::withValue(2), TypedCell<bool>::withValue(true));
	EXPECT_EQ(batch.column<0>().value(0), 2);
}

TEST(FixedWidthBatchTest, InvalidConstructionAndAllocatorFailureLeaveNoPublishedRows) {
	MaterializedByteBudget budget(1024);
	using Batch = FixedWidthBatch<int64_t, bool>;
	EXPECT_THROW((Batch(0, budget)), std::invalid_argument);
	EXPECT_THROW((Batch(2, budget, nullptr)), std::invalid_argument);
	Batch batch(2, budget, std::pmr::null_memory_resource());
	EXPECT_THROW(batch.append(TypedCell<int64_t>::null(), TypedCell<bool>::null()), std::bad_alloc);
	EXPECT_EQ(batch.size(), 0U);
	EXPECT_EQ(budget.used(), 0U);
}

namespace {
class FailingBatchResource final : public std::pmr::memory_resource {
public:
	size_t remaining = 2U;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (remaining == 0U) throw std::bad_alloc();
		--remaining;
		return std::pmr::new_delete_resource()->allocate(bytes, alignment);
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(FixedWidthBatchTest, UpstreamFailureInLaterColumnRollsBackAndSupportsRetry) {
	MaterializedByteBudget budget(1024);
	FailingBatchResource resource;
	FixedWidthBatch<int64_t, bool> batch(2, budget, &resource);
	EXPECT_THROW(batch.append(TypedCell<int64_t>::withValue(1), TypedCell<bool>::withValue(true)), std::bad_alloc);
	EXPECT_EQ(batch.size(), 0U);
	EXPECT_EQ(batch.column<0>().size(), 0U);
	EXPECT_EQ(batch.column<1>().size(), 0U);
	EXPECT_EQ(budget.used(), 9U);
	resource.remaining = 2U;
	batch.append(TypedCell<int64_t>::withValue(2), TypedCell<bool>::withValue(false));
	EXPECT_EQ(batch.size(), 1U);
	EXPECT_EQ(batch.column<0>().value(0), 2);
	EXPECT_FALSE(batch.column<1>().value(0));
	batch.release();
	EXPECT_EQ(budget.used(), 0U);
}
