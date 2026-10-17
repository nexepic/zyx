#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <stdexcept>

namespace {
LogSequenceNumber historyLsn(RelocatableMetadataStore &store) {
 auto snapshot = store.openSnapshot();
 return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
}
CatalogEntry reserveHistorySegment(RelocatableMetadataStore &store, PhysicalSlotId slot) {
 const auto id = store.allocateLogicalSegmentIds(1U, historyLsn(store)).front();
 auto request = reservationRequest(id.value(), 1U); request.require_absent = true;
 const auto reserved = store.reserveFirstFreeAndUpsertCatalog(std::span(&request, 1U), slot, slot, historyLsn(store));
 if (!reserved) throw std::runtime_error("history fixture could not reserve its exact reusable slot");
 return reserved->entries.front();
}
void retireHistorySegment(RelocatableMetadataStore &store, const CatalogEntry &entry) {
 const CatalogSegmentRetirementRequest request{entry.segment_id, entry.generation};
 static_cast<void>(store.retireCatalogSegments(std::span(&request, 1U), historyLsn(store)));
}
bool releaseHistorySegment(RelocatableMetadataStore &store, const CatalogEntry &entry) {
 const CatalogSegmentRetirementRequest request{entry.segment_id, entry.generation};
 return store.tryReleaseRetiredCatalogSegments(std::span(&request, 1U), historyLsn(store)).has_value();
}
}

TEST_F(RelocatableMetadataStoreTest, CatalogSlotReuseKeepsOnlyTheNewestObsoleteOwnerAndPreservesOtherSlots) {
 auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
 const auto unrelated = reserveHistorySegment(*store, PhysicalSlotId(41U));
 retireHistorySegment(*store, unrelated); ASSERT_TRUE(releaseHistorySegment(*store, unrelated));
 for (size_t round = 0U; round < 8U; ++round) {
  const auto entry = reserveHistorySegment(*store, PhysicalSlotId(40U));
  retireHistorySegment(*store, entry); ASSERT_TRUE(releaseHistorySegment(*store, entry));
  auto snapshot = store->openSnapshot();
  size_t obsoleteOwners = 0U;
  store->visitCatalogEntries(snapshot, [&](const CatalogEntry &candidate) {
   if (candidate.physical_slot == PhysicalSlotId(40U) && candidate.state == SegmentState::SGS_OBSOLETE)
    ++obsoleteOwners;
  });
  EXPECT_EQ(obsoleteOwners, 1U);
  EXPECT_EQ(store->findCatalog(snapshot, entry.segment_id).state, SegmentState::SGS_OBSOLETE);
  EXPECT_EQ(store->findCatalog(snapshot, unrelated.segment_id).state, SegmentState::SGS_OBSOLETE);
 }
}

TEST_F(RelocatableMetadataStoreTest, CatalogHistoryCleanupWaitsForTheSameSnapshotFenceAsSlotRelease) {
 auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
 const auto first = reserveHistorySegment(*store, PhysicalSlotId(40U));
 retireHistorySegment(*store, first); ASSERT_TRUE(releaseHistorySegment(*store, first));
 const auto second = reserveHistorySegment(*store, PhysicalSlotId(40U));
 auto pin = store->openSnapshot();
 retireHistorySegment(*store, second);
 auto before = store->openSnapshot(); const auto generation = before.superblock().generation; before.release();
 EXPECT_FALSE(releaseHistorySegment(*store, second));
 auto blocked = store->openSnapshot();
 EXPECT_EQ(blocked.superblock().generation, generation);
 EXPECT_EQ(store->findCatalog(blocked, first.segment_id).state, SegmentState::SGS_OBSOLETE);
 EXPECT_EQ(store->findCatalog(blocked, second.segment_id).state, SegmentState::SGS_RETIRED);
 blocked.release(); pin.release();
 ASSERT_TRUE(releaseHistorySegment(*store, second));
 auto current = store->openSnapshot(); bool foundOld = false;
 store->visitCatalogEntries(current, [&](const CatalogEntry &entry) { foundOld |= entry.segment_id == first.segment_id; });
 EXPECT_FALSE(foundOld);
 EXPECT_EQ(store->findCatalog(current, second.segment_id).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogHistoryCleanupRejectsAStillMappedObsoleteOwnerBeforeSlotRelease) {
 auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
 const auto first = reserveHistorySegment(*store, PhysicalSlotId(40U));
 retireHistorySegment(*store, first); ASSERT_TRUE(releaseHistorySegment(*store, first));
 const auto second = reserveHistorySegment(*store, PhysicalSlotId(40U));
 retireHistorySegment(*store, second);
 const auto page = store->allocateLogicalPageId(historyLsn(*store));
 const PageLocation corrupt{page, first.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, first.generation, 0U};
 static_cast<void>(store->upsertLocations(std::span(&corrupt, 1U), {}, historyLsn(*store)));
 auto before = store->openSnapshot(); const auto generation = before.superblock().generation; before.release();
 EXPECT_THROW(static_cast<void>(releaseHistorySegment(*store, second)), FormatException);
 auto after = store->openSnapshot();
 EXPECT_EQ(after.superblock().generation, generation);
 EXPECT_EQ(store->findCatalog(after, second.segment_id).state, SegmentState::SGS_RETIRED);
 EXPECT_TRUE(store->isPhysicalSlotAllocated(after, PhysicalSlotId(40U)));
}

TEST_F(RelocatableMetadataStoreTest, BatchedSlotReleaseGraduallyDrainsHistoryCreatedByLegacySingleReleases) {
 auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
 for (size_t i = 0U; i < 8U; ++i) {
  const auto entry = reserveHistorySegment(*store, PhysicalSlotId(40U));
  retireHistorySegment(*store, entry);
  ASSERT_TRUE(store->tryReleaseRetiredCatalogSegment(entry.segment_id, entry.generation, historyLsn(*store)).has_value());
 }
 const auto countHistory = [&] {
  auto snapshot = store->openSnapshot(); size_t count = 0U;
  store->visitCatalogEntries(snapshot, [&](const CatalogEntry &entry) {
   if (entry.physical_slot == PhysicalSlotId(40U) && entry.state == SegmentState::SGS_OBSOLETE) ++count;
  });
  return count;
 };
 size_t previous = countHistory(); ASSERT_EQ(previous, 8U);
 for (size_t i = 0U; i < 8U; ++i) {
  const auto entry = reserveHistorySegment(*store, PhysicalSlotId(40U));
  retireHistorySegment(*store, entry); ASSERT_TRUE(releaseHistorySegment(*store, entry));
  const size_t current = countHistory();
  if (previous > 1U) EXPECT_LT(current, previous); else EXPECT_EQ(current, 1U);
  previous = current;
 }
 EXPECT_EQ(previous, 1U);
}
