#include <gtest/gtest.h>
#include "graph/utils/OwnedSharedPtr.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace {
using namespace graph;
using namespace query::execution;
struct DestructionWitness {
    explicit DestructionWitness(size_t &destroyed) : destroyed(destroyed) {}
    ~DestructionWitness() { ++destroyed; }
    size_t &destroyed;
};
struct RejectConstruction {
    RejectConstruction() { throw std::runtime_error("construction failed"); }
};
class RejectAmbientResource final : public std::pmr::memory_resource {
public:
    size_t calls = 0U;
private:
    void *do_allocate(size_t, size_t) override { ++calls; throw std::bad_alloc(); }
    void do_deallocate(void *, size_t, size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(OwnedSharedPtrTest, InPlaceObjectAndControlBlockRetainTheResourceAfterTheirScopeEnds) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    std::weak_ptr<MaterializedByteBudget> lifetime = memory;
    size_t destroyed = 0U;
    std::shared_ptr<DestructionWitness> value;
    {
        utils::OwnedAllocationScope scope(memory);
        value = utils::makeSharedOwned<DestructionWitness>(destroyed);
    }
    EXPECT_GT(memory->used(), sizeof(DestructionWitness));
    memory.reset();
    EXPECT_FALSE(lifetime.expired());
    value.reset();
    EXPECT_EQ(destroyed, 1U);
    EXPECT_TRUE(lifetime.expired());
}

TEST(OwnedSharedPtrTest, ControlBlockDenialDeletesTheConsumedUniqueObjectExactlyOnce) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U);
    size_t destroyed = 0U;
    auto value = std::make_unique<DestructionWitness>(destroyed);
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(utils::shareOwned(std::move(value))), MaterializedByteLimitExceeded);
    }
    EXPECT_FALSE(value);
    EXPECT_EQ(destroyed, 1U);
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedSharedPtrTest, ConstructorFailureRefundsTheInPlaceAllocation) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(utils::makeSharedOwned<RejectConstruction>()), std::runtime_error);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedSharedPtrTest, LegacyFactoriesIgnoreUnrelatedAmbientPmrResources) {
    RejectAmbientResource resource;
    size_t destroyed = 0U;
    {
        struct Restore {
            std::pmr::memory_resource *old;
            ~Restore() { std::pmr::set_default_resource(old); }
        } restore{std::pmr::set_default_resource(&resource)};
        auto first = utils::makeSharedOwned<DestructionWitness>(destroyed);
        auto second = utils::shareOwned(std::make_unique<DestructionWitness>(destroyed));
    }
    EXPECT_EQ(resource.calls, 0U);
    EXPECT_EQ(destroyed, 2U);
}

TEST(OwnedSharedPtrTest, ExpressionClonesAndTheirControlBlocksSelectTheActiveOwner) {
    auto queryMemory = std::make_shared<MaterializedByteBudget>(65536U);
    auto cacheMemory = std::make_shared<MaterializedByteBudget>(65536U);
    const std::string text(8192U, 's');
    std::unique_ptr<query::expressions::Expression> original;
    std::shared_ptr<query::expressions::Expression> cached;
    {
        utils::OwnedAllocationScope scope(queryMemory);
        original = std::make_unique<query::expressions::LiteralExpression>(text);
    }
    EXPECT_GT(queryMemory->used(), text.size());
    {
        utils::OwnedAllocationScope scope(cacheMemory);
        cached = utils::shareOwned(original->clone());
    }
    EXPECT_GT(cacheMemory->used(), text.size());
    original.reset();
    EXPECT_EQ(queryMemory->used(), 0U);
    EXPECT_EQ(dynamic_cast<const query::expressions::LiteralExpression &>(*cached).getStringValue(), text);
    cached.reset();
    EXPECT_EQ(cacheMemory->used(), 0U);
}
} // namespace
