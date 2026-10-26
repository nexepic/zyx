/**
 * @file test_MetadataPagePoolRetirement.cpp
 * @brief Publication retirement preserves snapshots and requires complete proof.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

TEST_F(RelocatableMetadataStoreTest, IncrementalRetirementKeepsOldSnapshotAcrossRepeatedPublicationAndReopen) {
    auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
    auto original = entry(40U, 1U, 2U);
    static_cast<void>(store->upsertCatalog(std::span(&original, 1U), LogSequenceNumber(2U)));
    static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
    auto old = store->openSnapshot();
    graph::debug::PerfTrace::setValueCollectionEnabled(true);
    graph::debug::PerfTrace::reset();
    for (uint64_t lsn = 4U; lsn <= 140U; ++lsn) {
        auto replacement = entry(40U + lsn, 1U, lsn);
        static_cast<void>(store->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(lsn)));
        EXPECT_EQ(store->findCatalog(old, original.segment_id).physical_slot, original.physical_slot);
    }
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    for (const auto &[name, value] : counters)
        if (name.find("retirement.") != std::string::npos || name.find("reclaim.") != std::string::npos)
            RecordProperty(name, std::to_string(value.totalValue));
    EXPECT_FALSE(counters.contains("native_metadata.reclaim.full_traversals"));
    ASSERT_TRUE(counters.contains("native_metadata.reclaim.retained_frames"));
    EXPECT_GT(counters.at("native_metadata.reclaim.retained_frames").totalValue, 0);
    old.release();
    auto finalEntry = entry(200U, 1U, 141U);
    static_cast<void>(store->upsertCatalog(std::span(&finalEntry, 1U), LogSequenceNumber(141U)));
    auto latest = store->openSnapshot();
    const auto current = latest.superblock();
    EXPECT_EQ(store->findCatalog(latest, original.segment_id).physical_slot, finalEntry.physical_slot);
    latest.release(); store.reset();
    MetadataPagePoolAllocator verifier(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
    EXPECT_NO_THROW(verifier.validateReachability(current));
    io->sync();
    auto reopened = RelocatableMetadataStore::open(io);
    auto snapshot = reopened->openSnapshot();
    EXPECT_EQ(reopened->findCatalog(snapshot, original.segment_id).physical_slot, finalEntry.physical_slot);
}

TEST_F(RelocatableMetadataStoreTest, RetirementQueueWaitsForOldestGenerationAndRejectsDuplicateRetirement) {
    const auto initial = superblock(1U, 1U);
    SuperblockFile bootstrap(io); bootstrap.initializeNew(initial);
    MetadataPagePoolAllocator pools(io, initial);
    auto second = initial; second.generation = Generation(2U); second.checkpoint_lsn = LogSequenceNumber(2U);
    pools.beginUnpublishedBatch(initial);
    const auto reference = pools.allocatePage(second.generation, second.checkpoint_lsn, {}).page;
    const std::array retired{reference};
    EXPECT_NO_THROW(pools.validateMetadataRetirement(initial, second, retired, true));
    pools.commitMetadataRetirement(initial, second, retired, true);
    pools.reclaimUnreachable(second, std::array{initial}, false);
    EXPECT_EQ(pools.stats().allocated_pages, 1U);
    auto third = second; third.generation = Generation(3U); third.checkpoint_lsn = LogSequenceNumber(3U);
    EXPECT_THROW(pools.validateMetadataRetirement(second, third, retired, true), FormatException);
    pools.reclaimUnreachable(second, {}, false);
    EXPECT_EQ(pools.stats().allocated_pages, 0U);
    pools.endUnpublishedBatch(initial);
}

TEST_F(RelocatableMetadataStoreTest, CapacityPreparationDoesNotVisitFullPools) {
    const auto initial = superblock(1U, 1U);
    SuperblockFile bootstrap(io); bootstrap.initializeNew(initial);
    MetadataPagePoolAllocator pools(io, initial);
    const auto pages = pools.allocatePages(Generation(2U), LogSequenceNumber(2U),
        RSS_PAGE_POOL_DATA_FRAME_COUNT * 100U + 1U, {}, {}, nullptr);
    ASSERT_EQ(pools.stats().pools, 101U);
    EXPECT_EQ(pools.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    EXPECT_TRUE(pools.ensureMetadataFrameCapacity(Generation(2U), LogSequenceNumber(2U), 1U, {}, {}, nullptr).empty());
    pools.reserveMetadataFrames(1U, {}, {}, nullptr);
    const auto next = pools.allocatePage(Generation(2U), LogSequenceNumber(2U), {});
    pools.releaseMetadataFrames();
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    EXPECT_EQ(next.page.physical_slot, pages.back().page.physical_slot);
    ASSERT_TRUE(counters.contains("native_metadata.capacity.visited_pools"));
    EXPECT_EQ(counters.at("native_metadata.capacity.visited_pools").totalValue, 3);
    EXPECT_EQ(pools.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 2U);
    pools.abandonPage(pages.front().page);
    EXPECT_EQ(pools.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
    pools.protectPoolUntilSnapshotsDrain(pages.front().page.physical_slot);
    EXPECT_EQ(pools.allocatableFreePages(), RSS_PAGE_POOL_DATA_FRAME_COUNT - 2U);
}

TEST_F(RelocatableMetadataStoreTest, MissingRetirementProofReconcilesInsteadOfTrustingFreeCapacity) {
    const auto initial = superblock(1U, 1U);
    SuperblockFile bootstrap(io); bootstrap.initializeNew(initial);
    MetadataPagePoolAllocator pools(io, initial);
    auto next = initial; next.generation = Generation(2U); next.checkpoint_lsn = LogSequenceNumber(2U);
    pools.beginUnpublishedBatch(initial);
    static_cast<void>(pools.allocatePages(next.generation, next.checkpoint_lsn,
        RSS_PAGE_POOL_DATA_FRAME_COUNT * 4U, {}, {}, nullptr));
    pools.commitMetadataRetirement(initial, next, {}, false);
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    EXPECT_NO_THROW(pools.reclaimUnreachable(next, {}, false));
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    EXPECT_EQ(pools.stats().allocated_pages, 0U);
    ASSERT_TRUE(counters.contains("native_metadata.reclaim.full_traversals"));
    EXPECT_EQ(counters.at("native_metadata.reclaim.full_traversals").totalValue, 1);
    pools.endUnpublishedBatch(initial);
}

namespace {
class IncrementalMetadataPublicationTest : public RelocatableMetadataStoreTest {
protected:
    void checkFixedChange(uint64_t leafCount) {
        auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
        auto entries = catalogEntriesForLeafCount(leafCount);
        static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(2U)));
        static_cast<void>(store->migratePageDirectoryToBootstrap(LogSequenceNumber(3U)));
        auto old = store->openSnapshot();
        auto replacement = entries.back(); replacement.physical_slot = PhysicalSlotId(10000U);
        replacement.committed_lsn = LogSequenceNumber(4U);
        graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
        static_cast<void>(store->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U)));
        const auto counters = graph::debug::PerfTrace::snapshotAndReset();
        graph::debug::PerfTrace::setValueCollectionEnabled(false);
        EXPECT_FALSE(counters.contains("native_metadata.reclaim.full_traversals"));
        EXPECT_FALSE(counters.contains("native_metadata.ownership.initial_pool_enumerations"));
        ASSERT_TRUE(counters.contains("native_metadata.ownership.visited_pools"));
        EXPECT_LE(counters.at("native_metadata.ownership.visited_pools").totalValue, 18);
        ASSERT_TRUE(counters.contains("native_graph.route.incremental_source_frames"));
        EXPECT_LE(counters.at("native_graph.route.incremental_source_frames").totalValue, 20);
        ASSERT_TRUE(counters.contains("native_metadata.capacity.visited_pools"));
        EXPECT_LE(counters.at("native_metadata.capacity.visited_pools").totalValue, 20);
        EXPECT_EQ(store->findCatalog(old, replacement.segment_id).physical_slot, entries.back().physical_slot);
        auto current = store->openSnapshot();
        EXPECT_EQ(store->findCatalog(current, replacement.segment_id).physical_slot, replacement.physical_slot);
        RecordProperty("catalog_leaves", std::to_string(leafCount));
        for (const auto &[name, value] : counters)
            if (name.find("native_graph.route.incremental_") == 0U || name.find("native_metadata.") == 0U)
                RecordProperty(name, std::to_string(value.totalValue));
    }
};
}
TEST_F(IncrementalMetadataPublicationTest, ThreeLeaves) { checkFixedChange(3U); }
TEST_F(IncrementalMetadataPublicationTest, ThreeHundredLeaves) { checkFixedChange(300U); }
TEST_F(IncrementalMetadataPublicationTest, NineHundredLeaves) { checkFixedChange(900U); }

TEST_F(RelocatableMetadataStoreTest, FullReconciliationRebuildsDelayedRetirementForPinnedRoots) {
    auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
    auto first = entry(40U, 1U, 3U);
    static_cast<void>(store->upsertCatalog(std::span(&first, 1U), LogSequenceNumber(3U)));
    auto old = store->openSnapshot();
    auto replacement = entry(41U, 1U, 4U);
    static_cast<void>(store->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4U)));
    auto latest = store->openSnapshot(); const auto current = latest.superblock(); latest.release();
    MetadataPagePoolAllocator verifier(io, current, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
    verifier.reclaimUnreachable(current, std::array{old.superblock()}, true);
    const auto pinned = verifier.stats().allocated_pages;
    EXPECT_EQ(store->findCatalog(old, first.segment_id).physical_slot, first.physical_slot);
    old.release();
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    verifier.reclaimUnreachable(current, {}, false);
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    EXPECT_LT(verifier.stats().allocated_pages, pinned);
    EXPECT_FALSE(counters.contains("native_metadata.reclaim.full_traversals"));
    EXPECT_NO_THROW(verifier.validateReachability(current));
}

TEST_F(RelocatableMetadataStoreTest, CompleteReconciliationChargesCallerAndDeniesBeforeWriting) {
    auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
    auto pin = store->openSnapshot(); const auto current = pin.superblock();
    MetadataPagePoolAllocator verifier(io, current, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
    const auto beforeBytes = io->fileSize();
    const auto beforeWrites = io->dataMutationEpoch();
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(sizeof(Superblock) + 1U);
    {
        graph::utils::OwnedAllocationScope owner(tiny);
        EXPECT_THROW(verifier.reclaimUnreachable(current, {}, true),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(tiny->used(), 0U);
    EXPECT_EQ(io->dataMutationEpoch(), beforeWrites);
    EXPECT_EQ(io->fileSize(), beforeBytes);
    MetadataPagePoolAllocator retry(io, current, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope owner(memory);
        EXPECT_NO_THROW(retry.reclaimUnreachable(current, {}, true));
        EXPECT_NO_THROW(retry.validateReachability(current));
    }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(store->openSnapshot().superblock(), current);
}

TEST_F(RelocatableMetadataStoreTest, CompleteRouteCollectorsRetainCallerCreditUntilResultsDie) {
    auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
    auto pin = store->openSnapshot(); const auto current = pin.superblock();
    MetadataPageFile pages(io, current);
    MetadataPageRouteDirectory routes(pages);
    const auto header = pages.readPageDirectoryBootstrap(current.page_directory_root);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        graph::utils::OwnedAllocationScope owner(memory);
        auto registered = routes.collectRoutesFromRouteBranch(current, header.metadata_page_directory_root);
        ASSERT_FALSE(registered.empty());
        EXPECT_GT(memory->used(), 0U);
        auto physical = current; physical.page_directory_root = header.data_page_directory_root;
        auto live = routes.collectRoutesFromPhysicalDirectoryRoots(physical);
        EXPECT_EQ(registered, live);
        auto frames = routes.collectRouteBranchFrames(current, header.metadata_page_directory_root);
        EXPECT_FALSE(frames.empty());
        EXPECT_GT(memory->used(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U);
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(1U);
    const auto before = io->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope owner(tiny);
        EXPECT_THROW(static_cast<void>(routes.collectRoutesFromRouteBranch(current, header.metadata_page_directory_root)),
            graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_THROW(static_cast<void>(routes.collectRouteBranchFrames(current, header.metadata_page_directory_root)),
            graph::query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(tiny->used(), 0U);
    EXPECT_EQ(io->dataMutationEpoch(), before);
}
