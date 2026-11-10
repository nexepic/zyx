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
    EXPECT_EQ(ordinal[0], 42U); EXPECT_EQ(key[0], "payload");
}

TEST(CellScratchVectorTest, MultipleColumnsReserveOnceAndClearWithoutDroppingTheirCharge) {
    for (const size_t count : {2U, 16U, 1024U}) {
        CellScratchVector<size_t> columns; columns.reserve(count);
        EXPECT_EQ(columns.heapBytes(), CellScratchVector<size_t>::allocationUpper(count));
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
    CellScratchVector<size_t> values;
    EXPECT_THROW(values.push_back(1U), std::length_error);
    EXPECT_TRUE(values.empty()); EXPECT_EQ(values.heapBytes(), 0U);
    values.reserve(1U); values.push_back(7U);
    EXPECT_THROW(values.reserve(2U), std::logic_error);
    EXPECT_THROW(values.push_back(8U), std::length_error);
    EXPECT_EQ(values[0], 7U);
    EXPECT_THROW(CellScratchVector<size_t>::allocationUpper(SIZE_MAX), std::overflow_error);
}
