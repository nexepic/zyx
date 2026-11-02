#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include <set>
#include <atomic>
#include <cerrno>
#include <system_error>
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/metadata/CatalogPage.hpp"
#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/storage/relocatable/metadata/PageDirectoryBootstrap.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
struct ScopedTestNativeHandle {
 file_handle_t handle;
 ~ScopedTestNativeHandle() { if (handle != INVALID_FILE_HANDLE) static_cast<void>(portable_close_rw(handle)); }
};
void checkCurrentFormatEvacuationFault(const std::shared_ptr<StorageIO> &io,
 BootstrapPublishFailpoint failpoint, bool committed, int nativeFault = 0) {
 auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
 std::vector<CatalogSlotReservationRequest> requests;
 for (uint64_t i = 0; i < 30U; ++i) {
  auto request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + i * RSS_CATALOG_PAGE_MAX_ENTRIES, 1U);
  request.require_absent = true;
  requests.push_back(request);
 }
 const auto reserved = store->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40U), PhysicalSlotId(69U),
  LogSequenceNumber(3U), [&](std::span<const CatalogEntry> entries) {
   for (const auto &pool : entries) initializeTestSharedPagePool(*io, RSS_FILE_BOOTSTRAP_BYTES, pool);
   const auto end = PhysicalSegmentAllocator::checkedSlotOffset(RSS_FILE_BOOTSTRAP_BYTES, PhysicalSlotId(69U)) + RSS_SEGMENT_SIZE;
   const std::byte zero{}; io->writeAt(end - 1U, &zero, 1U);
  });
 ASSERT_TRUE(reserved.has_value());
 ASSERT_EQ(reserved->entries.size(), requests.size());
 const auto expected = reserved->entries;
 std::vector<MetadataSnapshotPin> pinned;
 for (uint64_t lsn = 4U; lsn <= 7U; ++lsn) {
  pinned.emplace_back(store->openSnapshot());
  static_cast<void>(store->upsertCatalog(expected, LogSequenceNumber(lsn)));
 }
 for (auto &pin : pinned) pin.release();
 auto old = store->openSnapshot();
 const auto oldGeneration = old.superblock().generation.value();
 const auto oldRoute = old.superblock().page_directory_root;
 MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
 if (nativeFault == 3 || nativeFault == 11 || nativeFault == 10) {
  uint32_t syncCalls = 0U;
  {
   StorageIO::ScopedNativeSyncHookForTesting hook([&](file_handle_t fd) {
    if (syncCalls++ == (nativeFault == 10 ? 0U : 1U)) {
#ifdef _WIN32
     SetLastError(nativeFault == 11 ? ERROR_DISK_FULL : ERROR_WRITE_FAULT);
#else
     errno = nativeFault == 11 ? ENOSPC : EIO;
#endif
     return -1;
    }
    return portable_fsync(fd);
   });
   EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(LogSequenceNumber(8U),
    RSS_PAGE_POOL_DATA_FRAME_COUNT)), std::runtime_error);
  }
  EXPECT_GE(syncCalls, nativeFault == 10 ? 1U : 2U);
 } else if (nativeFault) {
  bool injected = false;
  {
   StorageIO::ScopedNativeWriteHookForTesting hook([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
    bool target = false;
    if (!injected && offset >= 0 && count == RSS_PAGE_SIZE) {
     if ((nativeFault == 8 || nativeFault == 9) && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES)
      target = true;
     else if (static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES) {
      const auto page = std::span(static_cast<const std::byte *>(bytes), count);
      try {
       if (nativeFault == 1 || nativeFault == 2) { static_cast<void>(CatalogLeafPageCodec::decode(page)); target = true; }
       else if (nativeFault == 4 || nativeFault == 5) {
        const auto header = PageDirectoryBootstrapCodec::decode(page);
        target = header.generation.value() == oldGeneration + 1U && header.metadata_page_directory_root.isValid();
       } else if (nativeFault == 6 || nativeFault == 7) {
        const auto radix = MetadataRadixPageCodec::decode(page);
        target = radix.directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE &&
         radix.generation.value() == oldGeneration + 1U && !radix.logical_children_enabled;
       }
      } catch (const FormatException &) {}
     }
    }
    if (target) {
     injected = true;
     if (nativeFault % 2 == 1) return portable_pwrite(fd, bytes,
      nativeFault == 9 ? RSS_SUPERBLOCK_HEADER_CRC_OFFSET - 1U : count - 1U, offset);
#ifdef _WIN32
     SetLastError(ERROR_DISK_FULL);
#else
     errno = ENOSPC;
#endif
     return static_cast<pwrite_ssize_t>(-1);
    }
    return portable_pwrite(fd, bytes, count, offset);
   });
   EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(LogSequenceNumber(8U),
    RSS_PAGE_POOL_DATA_FRAME_COUNT)), std::system_error);
  }
  EXPECT_TRUE(injected);
 } else {
  EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(LogSequenceNumber(8U),
   RSS_PAGE_POOL_DATA_FRAME_COUNT, options)), BootstrapPublishInterrupted);
 }
 EXPECT_EQ(store->findCatalog(old, expected.front().segment_id).physical_slot, expected.front().physical_slot);
 old.release(); store.reset();
 auto reopened = RelocatableMetadataStore::open(io);
 auto recovered = reopened->openSnapshot();
 if (nativeFault == 3 || nativeFault == 11) {
  const auto generation = recovered.superblock().generation.value();
  ASSERT_TRUE(generation == oldGeneration || generation == oldGeneration + 1U);
  committed = generation == oldGeneration + 1U;
 }
 EXPECT_EQ(recovered.superblock().generation.value(), oldGeneration + (committed ? 1U : 0U));
 EXPECT_TRUE(recovered.superblock().usesPageDirectoryBootstrap());
 if (committed) EXPECT_NE(recovered.superblock().page_directory_root, oldRoute);
 else EXPECT_EQ(recovered.superblock().page_directory_root, oldRoute);
 for (const auto &entry : expected) {
  const auto actual = reopened->findCatalog(recovered, entry.segment_id);
  EXPECT_EQ(actual.physical_slot, entry.physical_slot);
  EXPECT_EQ(actual.generation, entry.generation);
  EXPECT_EQ(actual.owner_id, entry.owner_id);
  EXPECT_EQ(actual.state, entry.state);
  EXPECT_EQ(actual.kind, entry.kind);
  EXPECT_EQ(actual.committed_lsn, entry.committed_lsn);
  EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, entry.physical_slot));
 }
 EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
 recovered.release();
 if (!committed) {
  const auto retried = reopened->evacuateSparseMetadataPagePool(LogSequenceNumber(8U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
  EXPECT_GT(retried.relocated_metadata_pages, 0U);
  EXPECT_NE(retried.superblock.page_directory_root, oldRoute);
  EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
 }
}
void checkNativeEvacuationFault(const std::filesystem::path &path,
 const std::shared_ptr<std::fstream> &file, int fault) {
 const auto handle = portable_open_rw(path.string().c_str()); ASSERT_NE(handle, INVALID_FILE_HANDLE);
 ScopedTestNativeHandle guard{handle};
 auto nativeIo = std::make_shared<StorageIO>(file, handle, handle);
 checkCurrentFormatEvacuationFault(nativeIo, BootstrapPublishFailpoint::BPF_NONE, false, fault);
 nativeIo.reset(); EXPECT_EQ(portable_close_rw(handle), 0); guard.handle = INVALID_FILE_HANDLE;
}
}

TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationRetainsPinnedCatalogAcrossPublicationFault) {
 checkCurrentFormatEvacuationFault(io, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE, true);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationBeforePublicationRetainsRootAndRetries) {
 checkCurrentFormatEvacuationFault(io, BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE, false);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationShortWriteRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 1);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationEnospcRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 2);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationFinalSyncReopensWholeRoute) {
 checkNativeEvacuationFault(testFilePath, file, 3);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationHeaderShortWriteRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 5);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationHeaderEnospcRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 4);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationRouteRadixShortWriteRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 7);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationRouteRadixEnospcRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 6);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationSuperblockShortWriteRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 9);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationSuperblockEnospcRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 8);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationDataSyncFailureRetainsRootAndRetries) {
 checkNativeEvacuationFault(testFilePath, file, 10);
}
TEST_F(RelocatableMetadataStoreTest, CurrentFormatEvacuationFinalSyncEnospcReopensWholeRoute) {
 checkNativeEvacuationFault(testFilePath, file, 11);
}

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

// This test was moved from the large metadata fixture. It uses real pools and
// accepts either complete root after the final sync error, never a mixed root.
TEST_F(RelocatableMetadataStoreTest, NativeSparseMetadataPoolEvacuationFinalFenceReportsUncertainDurabilityAndReopensTheCompleteRoot) {
 const auto handle = portable_open_rw(testFilePath.string().c_str()); ASSERT_NE(handle, INVALID_FILE_HANDLE);
 ScopedTestNativeHandle guard{handle};
 auto nativeIo = std::make_shared<StorageIO>(file, handle, handle);
 auto store = RelocatableMetadataStore::initializeNewComplete(nativeIo, superblock(1U, 1U));
 const auto entries = prepareCurrentEvacuationFixture(nativeIo, *store);
 const auto before = store->openSnapshot().superblock();
 std::atomic<uint32_t> syncCalls{0U};
 {
  StorageIO::ScopedNativeSyncHookForTesting failFinalFence([&](file_handle_t fd) {
   if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
    SetLastError(ERROR_WRITE_FAULT);
#else
    errno = EIO;
#endif
    return -1;
   }
   return portable_fsync(fd);
  });
  EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(LogSequenceNumber(8U), RSS_PAGE_POOL_DATA_FRAME_COUNT)), std::runtime_error);
 }
 EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
 store.reset();
 auto reopened = RelocatableMetadataStore::open(nativeIo);
 auto recovered = reopened->openSnapshot();
 const auto generation = recovered.superblock().generation.value();
 ASSERT_TRUE(generation == before.generation.value() || generation == before.generation.value() + 1U);
 if (generation == before.generation.value()) EXPECT_EQ(recovered.superblock().page_directory_root, before.page_directory_root);
 else EXPECT_NE(recovered.superblock().page_directory_root, before.page_directory_root);
 for (const auto &entry : entries) {
  EXPECT_EQ(reopened->findCatalog(recovered, entry.segment_id).physical_slot, entry.physical_slot);
  EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, entry.physical_slot));
 }
 EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(nativeIo, *reopened).scan()));
 recovered.release();
 if (generation == before.generation.value()) {
  const auto retried = reopened->evacuateSparseMetadataPagePool(LogSequenceNumber(8U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
  EXPECT_GT(retried.relocated_metadata_pages, 0U);
 }
 EXPECT_GT(reopened->reclaimEmptyMetadataPagePools(LogSequenceNumber(9U)).reclaimed_pool_slots, 0U);
 EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(nativeIo, *reopened).scan()));
 reopened.reset(); nativeIo.reset();
 EXPECT_EQ(portable_close_rw(handle), 0); guard.handle = INVALID_FILE_HANDLE;
}
