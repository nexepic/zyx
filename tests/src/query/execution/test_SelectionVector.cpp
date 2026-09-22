/**
 * @file test_SelectionVector.cpp
 * @brief Tests for the bounded batch selection bitmap.
 */

#include <cstddef>
#include <vector>
#include <gtest/gtest.h>

#include "graph/query/execution/SelectionVector.hpp"

using graph::query::execution::SelectionVector;

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
