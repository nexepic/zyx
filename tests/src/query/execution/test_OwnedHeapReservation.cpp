#include <gtest/gtest.h>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"

namespace {
using namespace graph;
using namespace query::execution;
using namespace query::execution::detail;

class NoBackingAllocation final : public std::pmr::memory_resource {
public:
    size_t calls = 0U;
private:
    void *do_allocate(size_t, size_t) override { ++calls; throw std::bad_alloc(); }
    void do_deallocate(void *, size_t, size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(OwnedHeapReservationTest, RetainsAuthorityAndMoveOwnershipWithoutBackingAllocations) {
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(1024U));
    auto domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(1024U));
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_QUERY, 1024U);
    NoBackingAllocation backing;
    auto memory = std::make_shared<MaterializedByteBudget>(1024U, account, &backing);
    std::weak_ptr<MaterializedByteBudget> lifetime = memory;
    {
        utils::OwnedHeapReservation loan(memory, 256U);
        ASSERT_TRUE(loan.governed());
        EXPECT_EQ(account->used(), 256U);
        memory.reset();
        utils::OwnedHeapReservation moved(std::move(loan));
        EXPECT_EQ(loan.bytes(), 0U);
        EXPECT_FALSE(loan.governed());
        EXPECT_THROW(loan.set(1U), std::logic_error);
        EXPECT_FALSE(lifetime.expired());
        moved.set(512U);
        EXPECT_EQ(account->used(), 512U);
        EXPECT_THROW(moved.set(1025U), MaterializedByteLimitExceeded);
        EXPECT_EQ(moved.bytes(), 512U);
        EXPECT_EQ(account->used(), 512U);
        moved.set(128U);
        EXPECT_EQ(account->used(), 128U);
    }
    EXPECT_EQ(backing.calls, 0U);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(account->used(), 0U);
}

TEST(OwnedHeapReservationTest, PrepaidDenialAndCrossResourceSwapPreserveTheOriginalCharges) {
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(2048U));
    auto domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(2048U));
    auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_CACHE, 1024U);
    NoBackingAllocation backing;
    auto prepaid = std::make_shared<PrepaidMemoryResource>(account, 1024U, &backing);
    auto local = std::make_shared<MaterializedByteBudget>(1024U);
    {
        utils::OwnedHeapReservation first(prepaid, 256U), second(local, 512U);
        EXPECT_THROW(first.set(1025U), ResourceAdmissionDenied);
        EXPECT_EQ(first.bytes(), 256U);
        EXPECT_EQ(prepaid->used(), 256U);
        first.swap(second);
        EXPECT_EQ(first.owner(), local);
        EXPECT_EQ(second.owner(), prepaid);
        EXPECT_EQ(prepaid->used(), 256U);
        EXPECT_EQ(local->used(), 512U);
    }
    EXPECT_EQ(prepaid->used(), 0U);
    EXPECT_EQ(local->used(), 0U);
    EXPECT_EQ(backing.calls, 0U);
}

TEST(OwnedHeapReservationTest, LegacyResourcesDoNotTurnAdmissionIntoDummyAllocations) {
    auto resource = std::make_shared<NoBackingAllocation>();
    utils::OwnedHeapReservation loan(resource, 256U);
    EXPECT_FALSE(loan.governed());
    loan.set(512U);
    EXPECT_EQ(resource->calls, 0U);
}
} // namespace
