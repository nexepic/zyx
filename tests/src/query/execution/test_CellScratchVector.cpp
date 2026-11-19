#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>
#include "query/execution/CellScratchVector.hpp"
using graph::query::execution::detail::CellScratchVector;

TEST(CellScratchVectorTest, SingleOrdinalAndKeyViewStayInsideTheirOwnersWithoutHeap) {
    CellScratchVector<size_t> ordinal; ordinal.reserve(1U); ordinal.push_back(42U);
    CellScratchVector<std::string_view> key; key.reserve(1U); key.push_back("payload");
    const auto inside = [](const auto &owner) {
        const auto begin = reinterpret_cast<uintptr_t>(&owner);
        const auto data = reinterpret_cast<uintptr_t>(owner.data());
        return data >= begin && data + sizeof(*owner.data()) <= begin + sizeof(owner);
    };
    EXPECT_TRUE(inside(ordinal)); EXPECT_TRUE(inside(key));
    EXPECT_EQ(ordinal.heapBytes(), 0U); EXPECT_EQ(key.heapBytes(), 0U);
    EXPECT_EQ(CellScratchVector<size_t>::allocationUpper(1U), 0U);
    EXPECT_EQ(CellScratchVector<std::string_view>::allocationUpper(1U), 0U);
    EXPECT_EQ(CellScratchVector<size_t>::workingSetUpper(1U), sizeof(size_t));
    EXPECT_EQ(CellScratchVector<std::string_view>::workingSetUpper(1U), sizeof(std::string_view));
    EXPECT_EQ(ordinal[0], 42U); EXPECT_EQ(key[0], "payload");
}

TEST(CellScratchVectorTest, MultipleColumnsReserveOnceAndClearWithoutDroppingTheirCharge) {
    for (const size_t count : {2U, 16U, 1024U}) {
        CellScratchVector<size_t> columns; columns.reserve(count);
        EXPECT_EQ(columns.heapBytes(), CellScratchVector<size_t>::allocationUpper(count));
        EXPECT_EQ(columns.heapBytes(), CellScratchVector<size_t>::workingSetUpper(count));
        auto *storage = columns.data();
        for (size_t cycle = 0U; cycle < 3U; ++cycle) {
            for (size_t index = 0U; index < count; ++index) columns.push_back(index);
            EXPECT_THROW(columns.push_back(count), std::length_error);
            for (size_t index = 0U; index < count; ++index) EXPECT_EQ(columns[index], index);
            columns.clear();
            EXPECT_TRUE(columns.empty()); EXPECT_EQ(columns.data(), storage);
            EXPECT_EQ(columns.heapBytes(), CellScratchVector<size_t>::allocationUpper(count));
        }
    }
}

TEST(CellScratchVectorTest, UnreservedGrowthAndOverflowRejectWithoutChangingStoredValues) {
    CellScratchVector<size_t> failed;
    EXPECT_THROW(failed.reserve(SIZE_MAX), std::length_error);
    EXPECT_EQ(failed.heapBytes(), 0U);
    EXPECT_NO_THROW(failed.reserve(1U)); failed.push_back(9U);
    EXPECT_EQ(failed[0], 9U);
    CellScratchVector<size_t> values;
    EXPECT_THROW(values.push_back(1U), std::length_error);
    EXPECT_TRUE(values.empty()); EXPECT_EQ(values.heapBytes(), 0U);
    values.reserve(1U); values.push_back(7U);
    EXPECT_THROW(values.reserve(2U), std::logic_error);
    EXPECT_THROW(values.push_back(8U), std::length_error);
    EXPECT_EQ(values[0], 7U);
    EXPECT_THROW(CellScratchVector<size_t>::allocationUpper(SIZE_MAX), std::overflow_error);
}

#include "query/execution/CellKeySelection.hpp"
using graph::query::execution::detail::CellKeySelection;
TEST(CellKeySelectionTest, SingleKeyBorrowsExistingViewWithoutOrdinalOrKeyScratch) {
    const std::array<std::string_view, 1> keys{"payload"};
    CellKeySelection selection(keys);
    EXPECT_EQ(CellKeySelection::workingSetUpper(1U), 0U);
    EXPECT_EQ(selection.heapBytes(), 0U);
    EXPECT_TRUE(selection.empty());
    EXPECT_THROW(selection.column(0U), std::out_of_range);
    for (int retry = 0; retry < 3; ++retry) {
        selection.append(0U);
        EXPECT_EQ(selection.keys().data(), keys.data());
        EXPECT_EQ(selection.keys().front(), "payload");
        EXPECT_EQ(selection.column(0U), 0U);
        EXPECT_THROW(selection.append(0U), std::length_error);
        EXPECT_THROW(selection.append(1U), std::out_of_range);
        selection.clear();
        EXPECT_TRUE(selection.empty());
        EXPECT_EQ(selection.heapBytes(), 0U);
    }
}
TEST(CellKeySelectionTest, MultipleKeysPreserveSelectionOrderAndRetainedCapacityAcrossRetry) {
    const std::array<std::string_view, 3> keys{"a", "b", "c"};
    CellKeySelection selection(keys);
    const auto bytes = selection.heapBytes();
    EXPECT_EQ(bytes, CellKeySelection::workingSetUpper(keys.size()));
    EXPECT_GT(bytes, 0U);
    for (int retry = 0; retry < 3; ++retry) {
        selection.append(2U); selection.append(0U);
        EXPECT_EQ(selection.size(), 2U);
        EXPECT_EQ(selection.column(0U), 2U); EXPECT_EQ(selection.column(1U), 0U);
        EXPECT_EQ(selection.keys()[0], "c"); EXPECT_EQ(selection.keys()[1], "a");
        selection.append(1U);
        EXPECT_THROW(selection.append(0U), std::length_error);
        EXPECT_EQ(selection.size(), 3U);
        selection.clear();
        EXPECT_EQ(selection.heapBytes(), bytes);
    }
    CellKeySelection empty(std::span<const std::string_view>{});
    EXPECT_TRUE(empty.keys().empty());
    EXPECT_EQ(CellKeySelection::workingSetUpper(0U), 0U);
    EXPECT_THROW(empty.append(0U), std::out_of_range);
    EXPECT_THROW(CellKeySelection::workingSetUpper(SIZE_MAX), std::overflow_error);
}

#include "query/execution/ReleaseEmptyMapStorage.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/AdmissionPmrContainers.hpp"
TEST(EmptyAdmissionMapStorageTest, ReleasesBucketsAtZeroHeadroomAndCanAllocateAgain) {
    graph::query::execution::MaterializedByteBudget budget(8192U);
    graph::utils::AdmissionPmrUnorderedMap<int, int> cache{&budget};
    for (int id = 0; id < 32; ++id) cache.emplace(id, id);
    cache.clear();
    const size_t buckets = budget.used();
    ASSERT_GT(buckets, 0U);
    graph::query::execution::MaterializedByteReservation competitor(&budget);
    competitor.set(budget.available());
    ASSERT_EQ(budget.available(), 0U);
    EXPECT_NO_THROW(graph::query::execution::detail::releaseEmptyMapStorage(cache));
    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(budget.available(), buckets);
    competitor.reset();
    cache.emplace(7, 42);
    EXPECT_EQ(cache.at(7), 42);
    const size_t retained = budget.used();
    EXPECT_THROW(graph::query::execution::detail::releaseEmptyMapStorage(cache), std::logic_error);
    EXPECT_EQ(cache.at(7), 42); EXPECT_EQ(budget.used(), retained);
    cache.clear(); graph::query::execution::detail::releaseEmptyMapStorage(cache);
    EXPECT_EQ(budget.used(), 0U);
}
