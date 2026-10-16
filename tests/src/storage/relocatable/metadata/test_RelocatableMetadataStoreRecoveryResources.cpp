#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace {
class DeniedRecoveryCache final : public std::pmr::memory_resource {
    void *do_allocate(size_t, size_t) override {
        throw graph::query::execution::detail::ResourceAdmissionDenied();
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST_F(RelocatableMetadataStoreTest, RecoveryAndReadonlyDiscoveryUseProtectedControlScratchWhenCacheIsFull) {
    auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
    const ExtentLocation original{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, Generation(1U)};
    (void)store->upsertLocations({}, std::span(&original, 1U), LogSequenceNumber(2U));
    store.reset();
    auto cache = std::make_shared<DeniedRecoveryCache>();
    auto control = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    for (bool readonly : {false, true}) {
        SCOPED_TRACE(readonly);
        auto reopened = readonly ? RelocatableMetadataStore::openReadOnly(io, cache, control)
                                 : RelocatableMetadataStore::open(io, cache, control);
        {
            graph::utils::OwnedAllocationScope privateRead(control);
            auto snapshot = reopened->openSnapshot();
            EXPECT_EQ(reopened->findExtentLocation(snapshot, original.extent_id), original);
        }
        reopened.reset();
        EXPECT_EQ(control->used(), 0U);
    }
}
