/**
 * @file test_OwnedFunction.cpp
 * @brief Callback copy semantics and admission/lifetime contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include <array>
#include <functional>
#include <memory>
#include <type_traits>
#include <gtest/gtest.h>
#include "graph/utils/OwnedFunction.hpp"
#include "graph/utils/OwnedStdString.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::utils {
namespace {
using query::execution::MaterializedByteBudget;
using query::execution::MaterializedByteLimitExceeded;
struct Counter {
    std::array<size_t, 8U> padding{};
    int value = 0;
    int operator()(int delta) { return value += delta; }
};
struct Payload {
    OwnedStdString text;
    int count = 0;
    size_t operator()() { ++count; return text.size(); }
};

TEST(OwnedFunctionTest, EmptyAndNullTargetsDoNotAcquireHeapStorage) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U);
    OwnedAllocationScope scope(memory);
    OwnedFunction<int()> empty, explicitNull(nullptr);
    int (*pointer)() = nullptr;
    OwnedFunction<int()> nullPointer(pointer), standard(std::function<int()>{});
    EXPECT_FALSE(empty); EXPECT_EQ(empty, nullptr); EXPECT_EQ(nullptr, explicitNull);
    EXPECT_FALSE(nullPointer); EXPECT_FALSE(standard);
    EXPECT_EQ(empty.target_type(), typeid(void)); EXPECT_EQ(empty.target<int>(), nullptr);
    EXPECT_THROW(empty(), std::bad_function_call);
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedFunctionTest, SmallTrivialMutableCopiesStayIndependentWithoutAdmission) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U);
    OwnedAllocationScope scope(memory);
    OwnedFunction<int()> first = [count = 0]() mutable { return ++count; };
    EXPECT_EQ(first(), 1);
    auto second = first;
    EXPECT_EQ(first(), 2); EXPECT_EQ(first(), 3); EXPECT_EQ(second(), 2);
    EXPECT_EQ(memory->used(), 0U);
    auto moved = std::move(first);
    EXPECT_FALSE(first); EXPECT_EQ(moved(), 4); EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedFunctionTest, InvocationSupportsVoidDiscardReferencesAndBorrowedTargets) {
    OwnedFunction<void()> discarded = [] { return 7; }; EXPECT_NO_THROW(discarded());
    int value = 3;
    OwnedFunction<int &(int &)> reference = [](int &item) -> int & { ++item; return item; };
    EXPECT_EQ(&reference(value), &value); EXPECT_EQ(value, 4);
    auto counter = [count = 0]() mutable { return ++count; };
    OwnedFunction<int()> borrowed = std::ref(counter);
    auto copied = borrowed;
    EXPECT_EQ(borrowed(), 1); EXPECT_EQ(copied(), 2); EXPECT_EQ(counter(), 3);
}

TEST(OwnedFunctionTest, HeapCopiesOwnIndependentStateAndMovesTransferOneAdmission) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    {
        OwnedFunction<int(int)> first;
        { OwnedAllocationScope scope(memory); first = Counter{}; }
        const auto one = memory->used(); ASSERT_GT(one, sizeof(Counter));
        EXPECT_EQ(first(4), 4);
        auto second = first;
        EXPECT_EQ(memory->used(), 2U * one);
        EXPECT_EQ(first(1), 5); EXPECT_EQ(second(2), 6);
        auto moved = std::move(second);
        EXPECT_FALSE(second); EXPECT_EQ(moved(3), 9); EXPECT_EQ(memory->used(), 2U * one);
        first = nullptr; EXPECT_EQ(memory->used(), one);
        swap(first, moved); EXPECT_TRUE(first); EXPECT_FALSE(moved); EXPECT_EQ(first(1), 10);
        first.swap(first); EXPECT_EQ(first(0), 10);
    }
    EXPECT_EQ(memory->used(), 0U);
    static_assert(std::is_nothrow_move_constructible_v<OwnedFunction<int(int)>>);
    static_assert(std::is_nothrow_move_assignable_v<OwnedFunction<int(int)>>);
}

TEST(OwnedFunctionTest, CopyDenialKeepsBothSourceAndAssignmentDestination) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    OwnedFunction<int(int)> source;
    { OwnedAllocationScope scope(memory); source = Counter{}; }
    OwnedFunction<int(int)> destination = [](int) { return 99; };
    const auto before = memory->used();
    {
        OwnedHeapReservation pressure(memory, memory->available());
        const auto full = memory->used();
        EXPECT_THROW(destination = source, MaterializedByteLimitExceeded);
        EXPECT_EQ(source(1), 1); EXPECT_EQ(destination(0), 99); EXPECT_EQ(memory->used(), full);
    }
    EXPECT_EQ(memory->used(), before);
    destination = source; EXPECT_EQ(destination(2), 3); EXPECT_EQ(source(0), 1);
    source = std::move(source); EXPECT_EQ(source(0), 1);
}

TEST(OwnedFunctionTest, ActiveCopyOwnerRebindsOwnedCapturesAndEscapedBoxesKeepTheirAuthority) {
    auto first = std::make_shared<MaterializedByteBudget>(65536U);
    auto second = std::make_shared<MaterializedByteBudget>(65536U);
    std::weak_ptr<std::pmr::memory_resource> firstOwner = first, secondOwner = second;
    OwnedFunction<size_t()> source, copy;
    {
        OwnedAllocationScope scope(first);
        source = Payload{OwnedStdString(std::string(4096U, 'p'))};
    }
    EXPECT_GT(first->used(), 4096U);
    {
        OwnedAllocationScope scope(second);
        copy = source;
    }
    EXPECT_GT(second->used(), 4096U);
    EXPECT_EQ(source(), 4096U); EXPECT_EQ(copy(), 4096U);
    source = nullptr; first.reset(); EXPECT_TRUE(firstOwner.expired());
    second.reset(); EXPECT_FALSE(secondOwner.expired());
    EXPECT_EQ(copy(), 4096U); copy = nullptr; EXPECT_TRUE(secondOwner.expired());
}

TEST(OwnedFunctionTest, PartialCaptureCloneFailureRefundsTheNewBoxAndPreservesTheOriginal) {
    auto owner = std::make_shared<MaterializedByteBudget>(65536U);
    OwnedFunction<size_t()> source;
    { OwnedAllocationScope scope(owner); source = Payload{OwnedStdString(std::string(8192U, 'p'))}; }
    const auto retained = owner->used();
    auto small = std::make_shared<MaterializedByteBudget>(1024U);
    OwnedFunction<size_t()> destination = [] { return size_t{5U}; };
    {
        OwnedAllocationScope scope(small);
        EXPECT_THROW(destination = source, MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(small->used(), 0U); EXPECT_EQ(owner->used(), retained);
    EXPECT_EQ(source(), 8192U); EXPECT_EQ(destination(), 5U);
}

TEST(OwnedFunctionTest, TargetInspectionAndCopyReplacementHandleInlineAndHeapTargets) {
    OwnedFunction<int(int)> heap(Counter{});
    ASSERT_NE(heap.target<Counter>(), nullptr);
    EXPECT_EQ(heap.target_type(), typeid(Counter));
    EXPECT_EQ(heap.target<int>(), nullptr);
    const auto &readOnly = heap;
    EXPECT_EQ(readOnly.target<Counter>(), heap.target<Counter>());
    auto small = [](int value) { return value * 2; };
    heap = small;
    EXPECT_EQ(heap.target<Counter>(), nullptr); ASSERT_NE(heap.target<decltype(small)>(), nullptr);
    EXPECT_EQ(heap(7), 14);
    std::function<int(int)> legacy = heap; EXPECT_EQ(legacy(8), 16);
    OwnedFunction<int(int)> wrapped(legacy); EXPECT_EQ(wrapped(9), 18);
}
} // namespace
} // namespace graph::utils
