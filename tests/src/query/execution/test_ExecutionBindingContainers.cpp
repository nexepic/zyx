#include <gtest/gtest.h>
#include <memory_resource>
#include "graph/query/execution/ExecutionBindingContainers.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "query/execution/ProcedurePayloadAdmission.hpp"

namespace graph::query::execution::detail {
namespace {
struct BindingRequests final : std::pmr::memory_resource {
    MaterializedByteBudget *observed = nullptr;
    size_t calls = 0U, live = 0U, peak = 0U, late_admissions = 0U;
    bool deny = false;
    void *do_allocate(size_t bytes, size_t alignment) override {
        ++calls;
        if (observed && observed->used() < live + bytes) ++late_admissions;
        if (deny) throw std::bad_alloc();
        auto *value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live += bytes; peak = (std::max)(peak, live); return value;
    }
    void do_deallocate(void *value, size_t bytes, size_t alignment) override {
        live -= bytes; std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
using Pair = std::pair<const uint64_t, uint64_t>;
using TestMap = ExecutionBindingMap<uint64_t, uint64_t, std::pmr::polymorphic_allocator<Pair>>;
}

TEST(ExecutionBindingContainersTest, EmptyConstructionAndReleaseHaveNoHiddenAllocatorRequests) {
    MaterializedByteBudget budget(65536U); BindingRequests upstream; BudgetMemoryResource resource(budget, &upstream);
    EXPECT_LE(sizeof(TestMap), 64U);
    EXPECT_LE(executionBindingNodeBytes<TestMap>(), 64U);
    {
        upstream.deny = true;
        TestMap entries(std::less<>{}, TestMap::allocator_type(&resource));
        entries.reserve(1000U);
        EXPECT_EQ(upstream.calls, 0U); EXPECT_EQ(budget.used(), 0U);
        upstream.deny = false;
        for (uint64_t i = 0; i < 1000U; ++i) entries.emplace(i, i + 1U);
        EXPECT_EQ(executionBindingStructuralBytes(entries), upstream.live);
        EXPECT_EQ(budget.used(), upstream.live);
        upstream.deny = true;
        const auto calls = upstream.calls;
        entries.clear();
        EXPECT_EQ(upstream.calls, calls); EXPECT_EQ(upstream.live, 0U); EXPECT_EQ(budget.used(), 0U);
    }
    EXPECT_EQ(upstream.live, 0U); EXPECT_EQ(budget.used(), 0U);
}

TEST(ExecutionBindingContainersTest, BorrowedValuesStayAtTheSameAddressAcrossInsertionExtractAndMove) {
    ExecutionBindingMap<std::string, uint64_t> entries;
    entries.emplace("owner", 42U); auto *borrowed = &entries.at("owner");
    for (uint64_t i = 0; i < 1000U; ++i) entries.emplace(std::to_string(i), i);
    EXPECT_EQ(&entries.at("owner"), borrowed);
    auto node = entries.extract("owner"); node.key() = "renamed"; entries.insert(std::move(node));
    EXPECT_EQ(&entries.at("renamed"), borrowed);
    auto moved = std::move(entries);
    EXPECT_EQ(&moved.at("renamed"), borrowed); EXPECT_EQ(*borrowed, 42U);
    EXPECT_EQ(eraseExecutionBindingsIf(moved, [](const auto &entry) { return entry.first != "renamed"; }), 1000U);
    EXPECT_EQ(&moved.at("renamed"), borrowed); EXPECT_EQ(moved.size(), 1U);
}

TEST(ExecutionBindingContainersTest, ProcedureAdmissionPrecedesAllocationAndDuplicateInsertionDoesNotSpendCredit) {
    MaterializedByteBudget budget(4096U); BindingRequests upstream; upstream.observed = &budget;
    ProcedurePayloadCredit credit(&budget, 0U); ProcedurePayloadAdmissionScope scope(credit);
    TestMap entries(std::less<>{}, TestMap::allocator_type(&upstream));
    ASSERT_TRUE(procedureMapEmplace(entries, uint64_t{1}, uint64_t{7}).second);
    const auto used = budget.used(), calls = upstream.calls;
    EXPECT_EQ(used, upstream.live); EXPECT_EQ(upstream.late_admissions, 0U);
    EXPECT_FALSE(procedureMapEmplace(entries, uint64_t{1}, uint64_t{9}).second);
    EXPECT_FALSE(procedureMapTryEmplace(entries, uint64_t{1}).second);
    EXPECT_EQ(upstream.calls, calls); EXPECT_EQ(budget.used(), used); EXPECT_EQ(entries.at(1U), 7U);
    entries.clear(); credit.set(0U);
    EXPECT_EQ(upstream.live, 0U); EXPECT_EQ(budget.used(), 0U);
}

TEST(ExecutionBindingContainersTest, FailedInsertionRefundsItsUnusedAdmissionAndCanRetryWithoutDestroyingTheOwner) {
    MaterializedByteBudget budget(4096U); BindingRequests upstream; upstream.observed = &budget;
    ProcedurePayloadCredit credit(&budget, 0U); ProcedurePayloadAdmissionScope scope(credit);
    TestMap entries(std::less<>{}, TestMap::allocator_type(&upstream));
    ASSERT_TRUE(procedureMapEmplace(entries, uint64_t{1}, uint64_t{7}).second);
    const auto before = budget.used(); upstream.deny = true;
    EXPECT_THROW(procedureMapEmplace(entries, uint64_t{2}, uint64_t{8}), std::bad_alloc);
    EXPECT_EQ(budget.used(), before); EXPECT_EQ(entries.size(), 1U); EXPECT_EQ(upstream.live, before);
    upstream.deny = false;
    ASSERT_TRUE(procedureMapEmplace(entries, uint64_t{2}, uint64_t{8}).second);
    EXPECT_EQ(budget.used(), upstream.live); EXPECT_EQ(upstream.late_admissions, 0U);
    entries.clear(); credit.set(0U);
    EXPECT_EQ(upstream.live, 0U); EXPECT_EQ(budget.used(), 0U);
}

TEST(ExecutionBindingContainersTest, DefaultMappedInsertionRejectsBeforeUpstreamAndRefundsConstructorFailure) {
    {
        MaterializedByteBudget budget(executionBindingNodeBytes<TestMap>() - 1U);
        BindingRequests upstream; upstream.observed = &budget;
        ProcedurePayloadCredit credit(&budget, 0U); ProcedurePayloadAdmissionScope scope(credit);
        TestMap entries(std::less<>{}, TestMap::allocator_type(&upstream));
        EXPECT_THROW(procedureMapTryEmplace(entries, uint64_t{1}), MaterializedByteLimitExceeded);
        EXPECT_EQ(upstream.calls, 0U); EXPECT_EQ(budget.used(), 0U); EXPECT_TRUE(entries.empty());
    }
    MaterializedByteBudget budget(4096U); BindingRequests upstream; upstream.observed = &budget;
    ProcedurePayloadCredit credit(&budget, 0U); ProcedurePayloadAdmissionScope scope(credit);
    TestMap entries(std::less<>{}, TestMap::allocator_type(&upstream));
    upstream.deny = true;
    EXPECT_THROW(procedureMapTryEmplace(entries, uint64_t{1}), std::bad_alloc);
    EXPECT_EQ(budget.used(), 0U); EXPECT_TRUE(entries.empty());
    upstream.deny = false;
    EXPECT_TRUE(procedureMapTryEmplace(entries, uint64_t{1}).second);
    EXPECT_EQ(entries.at(1U), 0U); EXPECT_EQ(budget.used(), upstream.live);
    entries.clear(); credit.set(0U);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ExecutionBindingContainersTest, CompletenessSetFailureAndDuplicateInsertionPreserveExistingCharges) {
    using Set = ExecutionBindingSet<uint64_t, std::pmr::polymorphic_allocator<uint64_t>>;
    MaterializedByteBudget budget(4096U); BindingRequests upstream; upstream.observed = &budget;
    ProcedurePayloadCredit credit(&budget, 0U); ProcedurePayloadAdmissionScope scope(credit);
    Set entries(std::less<>{}, Set::allocator_type(&upstream));
    EXPECT_TRUE(procedureSetInsert(entries, uint64_t{7}).second);
    const auto before = budget.used(), calls = upstream.calls;
    EXPECT_FALSE(procedureSetInsert(entries, uint64_t{7}).second);
    EXPECT_EQ(upstream.calls, calls); EXPECT_EQ(budget.used(), before);
    upstream.deny = true;
    EXPECT_THROW(procedureSetInsert(entries, uint64_t{8}), std::bad_alloc);
    EXPECT_EQ(entries.size(), 1U); EXPECT_EQ(budget.used(), before);
    upstream.deny = false;
    EXPECT_TRUE(procedureSetInsert(entries, uint64_t{8}).second);
    EXPECT_EQ(budget.used(), upstream.live); EXPECT_EQ(upstream.late_admissions, 0U);
    entries.clear(); credit.set(0U);
    EXPECT_EQ(upstream.live, 0U); EXPECT_EQ(budget.used(), 0U);
}
} // namespace graph::query::execution::detail

TEST(ExecutionBindingContainersTest, ConvertingAssignmentKeepsTheDestinationAllocatorAndFailureIsAtomic) {
    using namespace graph::query::execution::detail;
    BindingRequests upstream;
    TestMap destination(std::less<>{}, TestMap::allocator_type(&upstream));
    destination.emplace(7U, 42U);
    const size_t retained = upstream.live;
    const std::map<uint64_t, uint64_t> source{{1U, 11U}, {2U, 22U}};
    upstream.deny = true;
    EXPECT_THROW(destination = source, std::bad_alloc);
    EXPECT_EQ(destination.size(), 1U);
    EXPECT_EQ(destination.at(7U), 42U);
    EXPECT_EQ(upstream.live, retained);
    upstream.deny = false;
    destination = source;
    EXPECT_EQ(destination.get_allocator().resource(), &upstream);
    EXPECT_EQ(destination.size(), 2U);
    EXPECT_EQ(destination.at(1U), 11U);
    EXPECT_EQ(destination.at(2U), 22U);
    EXPECT_EQ(upstream.live, executionBindingStructuralBytes(destination));
    destination.clear();
    EXPECT_EQ(upstream.live, 0U);
}

TEST(ExecutionBindingContainersTest, MissingKeysKeepStandardExceptionCategoriesWithoutInsertion) {
    graph::query::execution::detail::ExecutionBindingMap<int,int> bindings;
    EXPECT_THROW(static_cast<void>(bindings.at(7)),std::out_of_range);
    EXPECT_THROW(static_cast<void>(std::as_const(bindings).at(7)),std::out_of_range);
    EXPECT_TRUE(bindings.empty());
    bindings.emplace(7,42);
    EXPECT_EQ(bindings.at(7),42);
    EXPECT_EQ(std::as_const(bindings).at(7),42);
}
