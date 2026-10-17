#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

TEST_F(RelocatableMetadataStoreTest, ReclaimsRoutedEmptyMetadataPoolsWithPublicationHeadroom) {
 auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
 prepareRoutedMetadataPoolForEvacuation(*store);
 const auto before = store->pagePoolStats();
 MetadataPagePoolReclamation result;
 ASSERT_NO_THROW(result = store->reclaimEmptyMetadataPagePools(LogSequenceNumber(10U)));
 ASSERT_GT(result.reclaimed_pool_slots, 0U);
 EXPECT_LT(store->pagePoolStats().pools, before.pools);
 auto current = store->openSnapshot();
 EXPECT_TRUE(current.superblock().usesPageDirectoryBootstrap());
 for (const auto slot : result.released_pool_slots) EXPECT_FALSE(store->isPhysicalSlotAllocated(current, slot));
 current.release(); store.reset();
 auto reopened = RelocatableMetadataStore::open(io);
 auto recovered = reopened->openSnapshot();
 for (const auto slot : result.released_pool_slots) EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, slot));
}
