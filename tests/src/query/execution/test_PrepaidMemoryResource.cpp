#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <thread>
#include <vector>
#include "graph/query/execution/PrepaidMemoryResource.hpp"

namespace graph::query::execution::detail {
namespace {
std::shared_ptr<ResourceAccount> prepaidAccount(size_t maximum = 1000U) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
	return std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, maximum);
}

TEST(PrepaidMemoryResourceTest, UnusedGrantStaysChargedAndContainersChargeActualCapacity) {
	auto account = prepaidAccount();
	{
		PrepaidMemoryResource memory(account, 800U);
		EXPECT_EQ(account->used(), 800U); EXPECT_EQ(memory.used(), 0U);
		{
			std::pmr::vector<uint64_t> values(&memory); values.reserve(20U);
			EXPECT_EQ(memory.used(), values.capacity() * sizeof(uint64_t));
			values.push_back(7U); values.clear();
			EXPECT_GT(memory.used(), 0U); EXPECT_EQ(account->used(), 800U);
		}
		EXPECT_EQ(memory.used(), 0U); EXPECT_EQ(account->used(), 800U);
	}
	EXPECT_EQ(account->used(), 0U);
}

TEST(PrepaidMemoryResourceTest, VectorGrowthOverlapIsAdmittedBeforeReplacingTheOldBuffer) {
	auto account = prepaidAccount(); PrepaidMemoryResource memory(account, 80U);
	std::pmr::vector<uint64_t> values(&memory); values.reserve(4U); values.push_back(42U);
	EXPECT_THROW(values.reserve(8U), ResourceAdmissionDenied);
	EXPECT_EQ(values.size(), 1U); EXPECT_EQ(values[0], 42U); EXPECT_EQ(values.capacity(), 4U);
	EXPECT_EQ(memory.used(), 32U); EXPECT_EQ(memory.denials(), 1U);
	std::pmr::vector<uint64_t>(&memory).swap(values);
	values.reserve(8U); EXPECT_EQ(memory.used(), 64U);
}

class PrepaidFailingUpstream final : public std::pmr::memory_resource {
public:
	size_t calls = 0U;
private:
	void *do_allocate(size_t, size_t) override { ++calls; throw std::bad_alloc(); }
	void do_deallocate(void *, size_t, size_t) override {}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(PrepaidMemoryResourceTest, UpstreamFailureRefundsLocalUsageWithoutReleasingTheSharedGrant) {
	auto account = prepaidAccount(); PrepaidFailingUpstream upstream;
	PrepaidMemoryResource memory(account, 100U, &upstream);
	EXPECT_THROW((void)memory.allocate(101U), ResourceAdmissionDenied); EXPECT_EQ(upstream.calls, 0U);
	EXPECT_THROW((void)memory.allocate(80U), std::bad_alloc); EXPECT_EQ(upstream.calls, 1U);
	EXPECT_EQ(memory.used(), 0U); EXPECT_EQ(memory.peakAdmitted(), 80U); EXPECT_EQ(account->used(), 100U);
}

TEST(PrepaidMemoryResourceTest, ConcurrentConsumersCannotSpendTheSameGrantTwice) {
	auto account = prepaidAccount(); PrepaidMemoryResource memory(account, 100U);
	std::barrier synchronization(2); std::atomic<size_t> admitted{0U}, denied{0U};
	auto run = [&] {
		void *pointer = nullptr; synchronization.arrive_and_wait();
		try { pointer = memory.allocate(80U); ++admitted; }
		catch (const ResourceAdmissionDenied &) { ++denied; }
		synchronization.arrive_and_wait();
		if (pointer) memory.deallocate(pointer, 80U);
	};
	std::thread first(run), second(run); first.join(); second.join();
	EXPECT_EQ(admitted.load(), 1U); EXPECT_EQ(denied.load(), 1U);
	EXPECT_EQ(memory.used(), 0U); EXPECT_EQ(account->used(), 100U);
}

TEST(PrepaidMemoryResourceTest, ParentLifetimeExtendsUntilEveryGrantedContainerIsDestroyed) {
	std::weak_ptr<ResourceAccount> lifetime;
	{
		auto account = prepaidAccount(); lifetime = account;
		PrepaidMemoryResource memory(account, 100U); account.reset();
		std::pmr::vector<uint64_t> values(&memory); values.push_back(42U);
		ASSERT_FALSE(lifetime.expired()); EXPECT_EQ(lifetime.lock()->used(), 100U);
	}
	EXPECT_TRUE(lifetime.expired());
}

TEST(PrepaidMemoryResourceTest, InvalidGrantCannotConsumeOrHideParentQuota) {
	auto account = prepaidAccount();
	EXPECT_THROW((void)PrepaidMemoryResource(account, 0U), std::invalid_argument);
	EXPECT_THROW((void)PrepaidMemoryResource(account, 100U, nullptr), std::invalid_argument);
	EXPECT_THROW((void)PrepaidMemoryResource(std::shared_ptr<ResourceAccount>{}, 100U), std::invalid_argument);
	EXPECT_THROW((void)PrepaidMemoryResource(account, 1001U), ResourceAdmissionDenied);
	EXPECT_EQ(account->used(), 0U);
}

TEST(PrepaidMemoryResourceTest, CachedGrantAndQueriesShareTheSameProcessCapacity) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(100U));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(100U));
	auto cache = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, 100U);
	ResourceAccount query(database, ResourceClass::RCK_QUERY, 100U); ResourceLease active(query);
	{
		PrepaidMemoryResource memory(cache, 60U); active.set(40U);
		EXPECT_THROW(active.set(41U), ResourceAdmissionDenied);
		EXPECT_EQ(process->usage().total, 100U);
	}
	active.set(100U); EXPECT_EQ(process->usage().total, 100U);
}
} // namespace
} // namespace graph::query::execution::detail

#include "graph/utils/OwnedPmrAllocator.hpp"
namespace graph::query::execution::detail {
TEST(PrepaidMemoryResourceTest, OwningAllocatorRetainsTheGrantThroughTheLastSharedControlBlock) {
    auto account = prepaidAccount();
    auto memory = std::make_shared<PrepaidMemoryResource>(account, 800U);
    std::weak_ptr<PrepaidMemoryResource> lifetime = memory;
    auto value = std::allocate_shared<uint64_t>(utils::OwnedPmrAllocator<uint64_t>(memory), 42U);
    std::weak_ptr<uint64_t> weak = value;
    EXPECT_GT(memory->used(), sizeof(uint64_t));
    memory.reset();
    EXPECT_EQ(*value, 42U);
    value.reset();
    EXPECT_FALSE(lifetime.expired());
    EXPECT_EQ(account->used(), 800U);
    weak.reset();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(account->used(), 0U);
}

TEST(PrepaidMemoryResourceTest, OwningAllocatorCopiesAndAssignmentsPreserveActualAllocationOwnership) {
    auto firstAccount = prepaidAccount(), secondAccount = prepaidAccount();
    auto first = std::make_shared<PrepaidMemoryResource>(firstAccount, 800U);
    auto second = std::make_shared<PrepaidMemoryResource>(secondAccount, 800U);
    using Values = std::vector<uint64_t, utils::OwnedPmrAllocator<uint64_t>>;
    {
        Values original({1U, 2U, 3U}, utils::OwnedPmrAllocator<uint64_t>(first));
        Values copy = original;
        EXPECT_EQ(copy, original);
        EXPECT_EQ(first->used(), (original.capacity() + copy.capacity()) * sizeof(uint64_t));
        Values assigned({7U}, utils::OwnedPmrAllocator<uint64_t>(second));
        assigned = copy;
        EXPECT_EQ(second->used(), 0U);
        EXPECT_EQ(assigned.get_allocator().owner(), first);
        Values moved(utils::OwnedPmrAllocator<uint64_t>{second});
        moved = std::move(assigned);
        EXPECT_EQ(moved, original);
        EXPECT_EQ(moved.get_allocator().owner(), first);
        EXPECT_EQ(second->used(), 0U);
        Values other({9U}, utils::OwnedPmrAllocator<uint64_t>(second));
        moved.swap(other);
        EXPECT_EQ(moved.get_allocator().owner(), second);
        EXPECT_EQ(other.get_allocator().owner(), first);
    }
    EXPECT_EQ(first->used(), 0U);
    EXPECT_EQ(second->used(), 0U);
}
}

#include "graph/utils/OwnedVector.hpp"
namespace graph::query::execution::detail {
TEST(PrepaidMemoryResourceTest, ScopedConstructionAndCopyRebindingRestoreThePreviousOwner) {
    auto first = std::make_shared<PrepaidMemoryResource>(prepaidAccount(), 800U);
    auto second = std::make_shared<PrepaidMemoryResource>(prepaidAccount(), 800U);
    graph::utils::OwnedVector<uint64_t> retained;
    {
        graph::utils::OwnedAllocationScope outer(first);
        graph::utils::OwnedVector<uint64_t> original{1U, 2U};
        EXPECT_EQ(original.get_allocator().owner(), first);
        {
            graph::utils::OwnedAllocationScope inner(second);
            const auto copy = original;
            EXPECT_EQ(copy, original);
            EXPECT_EQ(copy.get_allocator().owner(), second);
            EXPECT_EQ(second->used(), copy.capacity() * sizeof(uint64_t));
        }
        EXPECT_EQ(second->used(), 0U);
        retained = original;
        graph::utils::OwnedVector<uint64_t> restored{3U};
        EXPECT_EQ(restored.get_allocator().owner(), first);
    }
    EXPECT_EQ(retained.get_allocator().owner(), first);
    const auto copy = retained;
    EXPECT_EQ(copy.get_allocator().owner(), first);
    EXPECT_FALSE(graph::utils::OwnedAllocationScope::active());
}

TEST(PrepaidMemoryResourceTest, MovedFromOwningVectorKeepsAReusableAllocatorAfterDestinationDestruction) {
    auto memory = std::make_shared<PrepaidMemoryResource>(prepaidAccount(), 800U);
    std::weak_ptr<PrepaidMemoryResource> lifetime = memory;
    graph::utils::OwnedVector<uint64_t> source{utils::OwnedPmrAllocator<uint64_t>(memory)};
    source.push_back(42U);
    {
        auto destination = std::move(source);
        memory.reset();
        EXPECT_EQ(destination[0], 42U);
    }
    ASSERT_FALSE(lifetime.expired());
    EXPECT_EQ(lifetime.lock()->used(), 0U);
    source.push_back(43U);
    EXPECT_EQ(source[0], 43U);
    source = graph::utils::OwnedVector<uint64_t>{};
    EXPECT_TRUE(lifetime.expired());
}

TEST(PrepaidMemoryResourceTest, AllocationScopeIsThreadLocalAcrossIndependentDomains) {
    auto first = std::make_shared<PrepaidMemoryResource>(prepaidAccount(), 800U);
    auto second = std::make_shared<PrepaidMemoryResource>(prepaidAccount(), 800U);
    std::barrier ready(2);
    std::array<std::shared_ptr<std::pmr::memory_resource>, 2U> observed;
    auto run = [&](size_t index, std::shared_ptr<std::pmr::memory_resource> memory) {
        utils::OwnedAllocationScope scope(memory);
        ready.arrive_and_wait();
        utils::OwnedVector<uint64_t> values{7U};
        observed[index] = values.get_allocator().owner();
    };
    std::thread left(run, 0U, first), right(run, 1U, second);
    left.join(); right.join();
    EXPECT_EQ(observed[0], first); EXPECT_EQ(observed[1], second);
    EXPECT_EQ(first->used(), 0U); EXPECT_EQ(second->used(), 0U);
    EXPECT_FALSE(utils::OwnedAllocationScope::active());
}

class OwnedAllocatorAmbientResource final : public std::pmr::memory_resource {
public:
    size_t allocations = 0U, deallocations = 0U;
private:
    void *do_allocate(size_t bytes, size_t alignment) override {
        ++allocations; return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
        ++deallocations; std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return &other == this; }
};
TEST(PrepaidMemoryResourceTest, BorrowedDefaultAllocatorKeepsItsCapturedUpstreamAfterTheDefaultChanges) {
    OwnedAllocatorAmbientResource upstream;
    auto *previous = std::pmr::set_default_resource(&upstream);
    utils::OwnedPmrAllocator<uint64_t> allocator;
    std::pmr::set_default_resource(previous);
    auto *values = allocator.allocate(2U);
    EXPECT_EQ(upstream.allocations, 1U);
    allocator.deallocate(values, 2U);
    EXPECT_EQ(upstream.deallocations, 1U);
}
}

namespace graph::query::execution::detail {
TEST(PrepaidMemoryResourceTest, ExternalBuffersShareTheGrantWithoutAllocatingDuplicateStorage) {
 auto account = prepaidAccount();
 PrepaidFailingUpstream upstream;
 auto memory = std::make_shared<PrepaidMemoryResource>(account, 100U, &upstream);
 {
  PrepaidMemoryResource::ExternalReservation buffer(memory, 80U);
  EXPECT_EQ(buffer.bytes(), 80U);
  EXPECT_EQ(memory->used(), 80U);
  EXPECT_EQ(upstream.calls, 0U);
  EXPECT_THROW((void)memory->allocate(21U), ResourceAdmissionDenied);
  EXPECT_THROW((void)PrepaidMemoryResource::ExternalReservation(memory, 21U), ResourceAdmissionDenied);
  EXPECT_EQ(memory->used(), 80U);
  auto moved = std::move(buffer);
  EXPECT_EQ(moved.bytes(), 80U);
  EXPECT_EQ(buffer.bytes(), 0U);
 }
 EXPECT_EQ(memory->used(), 0U);
 EXPECT_EQ(account->used(), 100U);
 EXPECT_EQ(upstream.calls, 0U);
 EXPECT_THROW((void)PrepaidMemoryResource::ExternalReservation({}, 1U), std::invalid_argument);
}

TEST(PrepaidMemoryResourceTest, ExternalBufferReservationKeepsItsGrantAliveUntilReleased) {
 auto account = prepaidAccount();
 auto memory = std::make_shared<PrepaidMemoryResource>(account, 100U);
 std::weak_ptr<PrepaidMemoryResource> weak = memory;
 {
  PrepaidMemoryResource::ExternalReservation buffer(memory, 60U);
  memory.reset();
  ASSERT_FALSE(weak.expired());
  EXPECT_EQ(weak.lock()->used(), 60U);
  EXPECT_EQ(account->used(), 100U);
 }
 EXPECT_TRUE(weak.expired());
 EXPECT_EQ(account->used(), 0U);
}
}

namespace graph::query::execution::detail {
TEST(PrepaidMemoryResourceTest, ExternalResizeRejectsBeforeChangingCreditAndSupportsRetry) {
 auto account = prepaidAccount();
 auto memory = std::make_shared<PrepaidMemoryResource>(account, 800U);
 {
  PrepaidMemoryResource::ExternalReservation first(memory, 300U);
  PrepaidMemoryResource::ExternalReservation second(memory, 400U);
  EXPECT_THROW(first.set(401U), ResourceAdmissionDenied);
  EXPECT_EQ(first.bytes(), 300U); EXPECT_EQ(memory->used(), 700U);
  second.set(0U); first.set(800U);
  EXPECT_EQ(memory->used(), 800U); EXPECT_EQ(first.bytes(), 800U);
  auto moved = std::move(first);
  EXPECT_THROW(first.set(0U), std::logic_error);
  EXPECT_EQ(moved.bytes(), 800U);
 }
 EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(account->used(), 800U);
 memory.reset(); EXPECT_EQ(account->used(), 0U);
}
}
