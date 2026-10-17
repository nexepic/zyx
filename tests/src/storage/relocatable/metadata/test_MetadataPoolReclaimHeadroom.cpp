#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include <set>
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"

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

namespace {
std::set<uint64_t> routeIds(std::shared_ptr<StorageIO> io, const Superblock &superblock, bool liveOnly) {
 MetadataPageFile pages(io, superblock); MetadataPageRouteDirectory routes(pages);
 const auto header = pages.readPageDirectoryBootstrap(superblock.page_directory_root);
 auto physical = superblock; physical.page_directory_root = header.data_page_directory_root;
 const auto entries = liveOnly ? routes.collectRoutesFromPhysicalDirectoryRoots(physical) :
     routes.collectRoutesFromRouteBranch(superblock, header.metadata_page_directory_root);
 std::set<uint64_t> result; for (const auto &route : entries) result.insert(route.page_id.value());
 return result;
}
MetadataSnapshotPin seedDetachedNodeDirectory(RelocatableMetadataStore &store, std::shared_ptr<StorageIO> io) {
 const auto segment = store.allocateLogicalSegmentIds(1U, LogSequenceNumber(3U)).front();
 auto request = reservationRequest(segment.value(), 1U); request.require_absent = true;
 const auto reserved = store.reserveFirstFreeAndUpsertCatalog(std::span(&request, 1U),
     PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(4U));
 if (!reserved) throw std::logic_error("route-pruning fixture could not reserve its typed pool"); const auto pool = reserved->entries.front();
 auto initialized = store.openSnapshot(); const auto offset = initialized.superblock().data_region_offset; initialized.release();
 initializeTestSharedPagePool(*io, offset, pool);
 prepareTestSharedPageFrame(*io, offset, pool, PageId(90U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
     pool.owner_id, PageOwnerKind::POK_ENTITY, LogSequenceNumber(4U));
 const PageLocation page{PageId(90U), segment, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U)};
 static_cast<void>(store.upsertLocations(std::span(&page, 1U), {}, LogSequenceNumber(5U)));
 const EntityPageLocation node{1U, PageId(90U), 0U, 1U};
 static_cast<void>(store.upsertEntityPageLocations(std::span(&node, 1U), {}, {}, {}, LogSequenceNumber(6U)));
 auto old = store.openSnapshot(); const uint64_t erased = 1U;
 static_cast<void>(store.upsertEntityPageLocations({}, std::span(&erased, 1U), {}, {}, LogSequenceNumber(7U)));
 return old;
}

}

TEST_F(RelocatableMetadataStoreTest, MetadataMaintenanceDropsDetachedRoutesWhilePreservingPinnedDirectoryReads) {
 auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
 auto old = seedDetachedNodeDirectory(*store, io);
 const EntityPageLocation node{1U, PageId(90U), 0U, 1U};
 auto before = store->openSnapshot();
 const auto registered = routeIds(io, before.superblock(), false);
 const auto live = routeIds(io, before.superblock(), true);
 std::set<uint64_t> detached;
 for (const auto id : registered) if (!live.contains(id)) detached.insert(id);
 ASSERT_FALSE(detached.empty()); before.release();
 static_cast<void>(store->reclaimEmptyMetadataPagePools(LogSequenceNumber(8U)));
 auto after = store->openSnapshot(); const auto remaining = routeIds(io, after.superblock(), false);
 for (const auto id : detached) EXPECT_FALSE(remaining.contains(id));
 EXPECT_EQ(store->findNodePageLocation(old, 1U), node);
 EXPECT_FALSE(store->findNodePageLocationOptional(after, 1U).has_value());
 after.release(); old.release(); store.reset();
 auto reopened = RelocatableMetadataStore::open(io); auto recovered = reopened->openSnapshot();
 EXPECT_FALSE(reopened->findNodePageLocationOptional(recovered, 1U).has_value());
 const auto durable = routeIds(io, recovered.superblock(), false);
 for (const auto id : detached) EXPECT_FALSE(durable.contains(id));
}

namespace {
void checkPruningInterruption(std::shared_ptr<StorageIO> io, BootstrapPublishFailpoint failpoint, bool committed) {
 auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
 auto old = seedDetachedNodeDirectory(*store, io); auto before = store->openSnapshot();
 const auto registered = routeIds(io, before.superblock(), false);
 const auto live = routeIds(io, before.superblock(), true); const auto generation = before.superblock().generation;
 std::set<uint64_t> detached; for (const auto id : registered) if (!live.contains(id)) detached.insert(id);
 ASSERT_FALSE(detached.empty()); before.release();
 MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
 EXPECT_THROW(static_cast<void>(store->reclaimEmptyMetadataPagePools(LogSequenceNumber(8U), options)), BootstrapPublishInterrupted);
 const EntityPageLocation node{1U, PageId(90U), 0U, 1U};
 EXPECT_EQ(store->findNodePageLocation(old, 1U), node); old.release(); store.reset();
 auto reopened = RelocatableMetadataStore::open(io); auto recovered = reopened->openSnapshot();
 const auto remaining = routeIds(io, recovered.superblock(), false);
 for (const auto id : detached) EXPECT_EQ(remaining.contains(id), !committed);
 EXPECT_FALSE(reopened->findNodePageLocationOptional(recovered, 1U).has_value());
 if (committed) EXPECT_GT(recovered.superblock().generation, generation);
 else EXPECT_EQ(recovered.superblock().generation, generation);
}
}

TEST_F(RelocatableMetadataStoreTest, InterruptedPruningBeforeInactivePublicationKeepsTheOldRouteBranch) {
 checkPruningInterruption(io, BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE, false);
}
TEST_F(RelocatableMetadataStoreTest, InterruptedPruningAfterInactivePublicationRecoversThePrunedRouteBranch) {
 checkPruningInterruption(io, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE, true);
}
