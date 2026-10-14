/**
 * @file test_SelectionVector.cpp
 * @brief Tests for the bounded batch selection bitmap.
 */

#include <cstddef>
#include <memory_resource>
#include <vector>
#include <gtest/gtest.h>

#include "graph/query/execution/SelectionVector.hpp"
#include "query/execution/BudgetMemoryResource.hpp"

using graph::query::execution::SelectionVector;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
using graph::query::execution::MaterializedByteReservation;
using graph::query::execution::detail::BudgetMemoryResource;

namespace {
class BitmapAllocator final : public std::pmr::memory_resource {
public:
	size_t calls = 0U;
	size_t live_bytes = 0U;
	bool fail = false;
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		++calls;
		if (fail) throw std::bad_alloc();
		void *value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
		live_bytes += bytes;
		return value;
	}
	void do_deallocate(void *value, size_t bytes, size_t alignment) override {
		std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
		live_bytes -= bytes;
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST(SelectionVectorTest, BitmapBudgetAdmissionPrecedesAllocationAndRefundsActualCapacity) {
	MaterializedByteBudget budget(24U);
	MaterializedByteReservation other(&budget);
	other.set(8U);
	BitmapAllocator allocator;
	BudgetMemoryResource memory(budget, &allocator);
	EXPECT_THROW((SelectionVector(129U, &memory)), MaterializedByteLimitExceeded);
	EXPECT_EQ(allocator.calls, 0U);
	EXPECT_EQ(budget.used(), 8U);
	{
		SelectionVector selection(65U, &memory);
		EXPECT_EQ(budget.used(), 8U + selection.storageBytes());
		EXPECT_EQ(allocator.live_bytes, selection.storageBytes());
		selection.rejectAll();
		EXPECT_EQ(budget.used(), 24U); // Filtering does not refund retained storage.
	}
	EXPECT_EQ(budget.used(), 8U);
	EXPECT_EQ(allocator.live_bytes, 0U);
}

TEST(SelectionVectorTest, FailedUpstreamAllocationRefundsAdmissionAndCanRetry) {
	MaterializedByteBudget budget(64U);
	BitmapAllocator allocator;
	BudgetMemoryResource memory(budget, &allocator);
	allocator.fail = true;
	EXPECT_THROW((SelectionVector(65U, &memory)), std::bad_alloc);
	EXPECT_EQ(budget.used(), 0U);
	allocator.fail = false;
	{ SelectionVector retry(65U, &memory); EXPECT_EQ(retry.selectedCount(), 65U); }
	EXPECT_EQ(budget.used(), 0U);
}

TEST(SelectionVectorTest, CopiesPreserveBudgetResourceAndDoNotShareRejectedBits) {
	MaterializedByteBudget budget(64U);
	BudgetMemoryResource memory(budget);
	{
		SelectionVector original(65U, &memory);
		original.reject(64U);
		SelectionVector copied(original);
		EXPECT_EQ(budget.used(), original.storageBytes() + copied.storageBytes());
		original.reject(0U);
		EXPECT_TRUE(copied.includes(0U));
		copied.select(64U);
		EXPECT_FALSE(original.includes(64U));
		EXPECT_EQ(copied.selectedCount(), 65U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(SelectionVectorTest, FailedCopyAssignmentPreservesDestinationAndItsAllocator) {
	MaterializedByteBudget sourceBudget(64U), targetBudget(16U);
	BudgetMemoryResource sourceMemory(sourceBudget), targetMemory(targetBudget);
	SelectionVector source(130U, &sourceMemory), target(65U, &targetMemory);
	target.reject(0U);
	EXPECT_THROW(target = source, MaterializedByteLimitExceeded);
	EXPECT_EQ(target.size(), 65U);
	EXPECT_EQ(target.selectedCount(), 64U);
	EXPECT_FALSE(target.includes(0U));
	EXPECT_EQ(targetBudget.used(), target.storageBytes());
	EXPECT_EQ(source.selectedCount(), 130U);
}

TEST(SelectionVectorTest, MoveConstructionAndSameResourceAssignmentTransferChargesAndEmptySource) {
	MaterializedByteBudget budget(64U);
	BitmapAllocator allocator;
	BudgetMemoryResource memory(budget, &allocator);
	{
		SelectionVector source(65U, &memory);
		source.reject(64U);
		const auto calls = allocator.calls;
		SelectionVector moved(std::move(source));
		EXPECT_EQ(allocator.calls, calls);
		EXPECT_TRUE(source.empty());
		EXPECT_EQ(source.selectedCount(), 0U);
		EXPECT_TRUE(source.noneSelected());
		SelectionVector target(1U, &memory);
		target = std::move(moved);
		EXPECT_EQ(target.size(), 65U);
		EXPECT_EQ(target.selectedCount(), 64U);
		EXPECT_TRUE(moved.empty());
		EXPECT_EQ(budget.used(), target.storageBytes());
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(SelectionVectorTest, CrossResourceMovePreservesAllocatorsAndRetainedSourceCapacityAccounting) {
	MaterializedByteBudget sourceBudget(64U), targetBudget(64U);
	BudgetMemoryResource sourceMemory(sourceBudget), targetMemory(targetBudget);
	{
		SelectionVector source(65U, &sourceMemory), target(1U, &targetMemory);
		source.reject(64U);
		target = std::move(source);
		EXPECT_EQ(target.size(), 65U);
		EXPECT_FALSE(target.includes(64U));
		EXPECT_TRUE(source.empty());
		EXPECT_TRUE(source.noneSelected());
		EXPECT_EQ(sourceBudget.used(), source.storageBytes());
		EXPECT_EQ(targetBudget.used(), target.storageBytes());
	}
	EXPECT_EQ(sourceBudget.used(), 0U);
	EXPECT_EQ(targetBudget.used(), 0U);
}

TEST(SelectionVectorTest, FailedCrossResourceMovePreservesBothSelectionsAndBudgets) {
	MaterializedByteBudget sourceBudget(64U), targetBudget(8U);
	BudgetMemoryResource sourceMemory(sourceBudget), targetMemory(targetBudget);
	SelectionVector source(65U, &sourceMemory), target(1U, &targetMemory);
	source.reject(64U);
	EXPECT_THROW(target = std::move(source), MaterializedByteLimitExceeded);
	EXPECT_EQ(source.size(), 65U);
	EXPECT_EQ(source.selectedCount(), 64U);
	EXPECT_EQ(target.size(), 1U);
	EXPECT_EQ(target.selectedCount(), 1U);
	EXPECT_EQ(sourceBudget.used(), source.storageBytes());
	EXPECT_EQ(targetBudget.used(), target.storageBytes());
}

TEST(SelectionVectorTest, EmptyBitmapsNeedNoAllocationAndNullResourcesRejectBeforeUse) {
	MaterializedByteBudget budget(1U);
	BitmapAllocator allocator;
	BudgetMemoryResource memory(budget, &allocator);
	SelectionVector empty(0U, &memory);
	EXPECT_EQ(allocator.calls, 0U);
	EXPECT_EQ(budget.used(), 0U);
	EXPECT_THROW((SelectionVector(1U, nullptr)), std::invalid_argument);
}

TEST(SelectionVectorTest, StartsWithAllRowsSelectedAndSupportsWordBoundaries) {
	SelectionVector selection(65U);
	EXPECT_EQ(selection.size(), 65U);
	EXPECT_EQ(selection.storageBytes(), 2U * sizeof(uint64_t));
	EXPECT_EQ(selection.selectedCount(), 65U);
	EXPECT_TRUE(selection.includes(0U));
	EXPECT_TRUE(selection.includes(63U));
	EXPECT_TRUE(selection.includes(64U));

	selection.reject(0U);
	selection.reject(0U);
	selection.reject(64U);
	EXPECT_FALSE(selection.includes(0U));
	EXPECT_FALSE(selection.includes(64U));
	EXPECT_EQ(selection.selectedCount(), 63U);
}

TEST(SelectionVectorTest, RejectAllClearsEveryPosition) {
	SelectionVector selection(130U);
	selection.rejectAll();
	EXPECT_EQ(selection.selectedCount(), 0U);
	EXPECT_FALSE(selection.includes(0U));
	EXPECT_FALSE(selection.includes(129U));
}

TEST(SelectionVectorTest, RejectsOutOfRangePositions) {
	SelectionVector selection(2U);
	EXPECT_THROW(static_cast<void>(selection.includes(2U)), std::out_of_range);
	EXPECT_THROW(selection.reject(2U), std::out_of_range);
	EXPECT_THROW(selection.select(2U), std::out_of_range);
}

TEST(SelectionVectorTest, SupportsEmptyBatch) {
	SelectionVector selection(0U);
	EXPECT_EQ(selection.selectedCount(), 0U);
	selection.rejectAll();
}

TEST(SelectionVectorTest, SupportsComposableSelectionAndSelectedIteration) {
	SelectionVector selection(70U);
	selection.rejectAll();
	selection.select(1U);
	selection.select(1U);
	selection.select(64U);
	selection.select(69U);
	SelectionVector allowed(70U);
	allowed.reject(64U);
	selection.intersect(allowed);

	std::vector<size_t> selected;
	selection.forEachSelected([&selected](size_t index) { selected.push_back(index); });
	EXPECT_EQ(selected, (std::vector<size_t>{1U, 69U}));
	EXPECT_FALSE(selection.empty());
	EXPECT_FALSE(selection.noneSelected());
	EXPECT_FALSE(selection.allSelected());
	selection.rejectAll();
	EXPECT_TRUE(selection.noneSelected());
}

TEST(SelectionVectorTest, RejectsMismatchedIntersection) {
	SelectionVector left(2U);
	SelectionVector right(3U);
	EXPECT_THROW(left.intersect(right), std::invalid_argument);
}

TEST(SelectionVectorTest, StopsSelectedIterationAtTheFirstRejectedContinuation) {
	SelectionVector selection(130U);
	std::vector<size_t> visited;
	const bool exhausted = selection.forEachSelectedUntil([&visited](size_t index) {
		visited.push_back(index);
		return index != 64U;
	});
	EXPECT_FALSE(exhausted);
	EXPECT_EQ(visited, (std::vector<size_t>{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U,
										 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U,
										 26U, 27U, 28U, 29U, 30U, 31U, 32U, 33U, 34U, 35U, 36U, 37U,
										 38U, 39U, 40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 48U, 49U,
										 50U, 51U, 52U, 53U, 54U, 55U, 56U, 57U, 58U, 59U, 60U, 61U,
										 62U, 63U, 64U}));
}
