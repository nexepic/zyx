#include <gtest/gtest.h>
#include "graph/storage/relocatable/metadata/NativeRecoveryOwnerCache.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/UnmanagedAllocationResource.hpp"

using namespace graph::storage;
namespace {
class RecoveryCacheBudget final : public graph::utils::UnmanagedAllocationResource {
public:
    enum class Failure { none, admission, allocation } failure = Failure::none;
    size_t outstanding = 0; bool denied = false;
    void reserveUnmanaged(size_t bytes) override {
        if (bytes > 1024U && failure != Failure::none) {
            denied = true;
            if (failure == Failure::allocation) throw std::bad_alloc();
            throw graph::query::execution::detail::ResourceAdmissionDenied();
        }
        outstanding += bytes;
    }
    void releaseUnmanaged(size_t bytes) noexcept override { outstanding -= bytes; }
private:
    void *do_allocate(size_t n, size_t a) override { return std::pmr::new_delete_resource()->allocate(n, a); }
    void do_deallocate(void *p, size_t n, size_t a) override { std::pmr::new_delete_resource()->deallocate(p, n, a); }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}
TEST(NativeRecoveryOwnerCacheTest, IdentityAndCapacityBoundReuse) {
    auto budget = std::make_shared<RecoveryCacheBudget>();
    {
        graph::utils::OwnedAllocationScope scope(budget);
        detail::NativeRecoveryOwnerCache cache;
        PageOwnerHeader owner{}; owner.page_id = PageId(123U);
        for (size_t i = 0; i < detail::NativeRecoveryOwnerCache::capacity; ++i)
            cache.remember(SegmentId(i + 1U), Generation(3U), PhysicalSlotId(i + 11U), 1U, owner);
        ASSERT_TRUE(cache.find(SegmentId(1U), Generation(3U), PhysicalSlotId(11U), 1U));
        EXPECT_FALSE(cache.find(SegmentId(1U), Generation(4U), PhysicalSlotId(11U), 1U));
        EXPECT_FALSE(cache.find(SegmentId(1U), Generation(3U), PhysicalSlotId(12U), 1U));
        EXPECT_FALSE(cache.find(SegmentId(1U), Generation(3U), PhysicalSlotId(11U), 2U));
        cache.remember(SegmentId(5000U), Generation(3U), PhysicalSlotId(5000U), 1U, owner);
        EXPECT_FALSE(cache.find(SegmentId(5000U), Generation(3U), PhysicalSlotId(5000U), 1U));
        EXPECT_GT(budget->outstanding, 0U);
    }
    EXPECT_EQ(budget->outstanding, 0U);
}
TEST(NativeRecoveryOwnerCacheTest, DeniedAndAllocationFailureFallBackAndRefund) {
    for (auto failure : {RecoveryCacheBudget::Failure::admission, RecoveryCacheBudget::Failure::allocation}) {
        auto budget = std::make_shared<RecoveryCacheBudget>(); budget->failure = failure;
        {
            graph::utils::OwnedAllocationScope scope(budget);
            detail::NativeRecoveryOwnerCache cache;
            PageOwnerHeader owner{}; owner.page_id = PageId(7U);
            cache.remember(SegmentId(1U), Generation(2U), PhysicalSlotId(3U), 1U, owner);
            EXPECT_TRUE(budget->denied);
            EXPECT_FALSE(cache.find(SegmentId(1U), Generation(2U), PhysicalSlotId(3U), 1U));
        }
        EXPECT_EQ(budget->outstanding, 0U);
    }
}
