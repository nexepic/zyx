/**
 * @file test_MetadataPageRouteDirectoryReuse.cpp
 * @brief Immutable route-branch reuse, snapshot, corruption and admission checks.
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <vector>
#include <gtest/gtest.h>
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

using namespace graph::storage;
namespace {
class ReuseTestAllocator final : public MetadataPageAllocator {
public:
    MetadataPageId allocateMetadataPageId() override { return MetadataPageId(nextId++); }
    MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
                                       std::span<const PhysicalSlotId>) override {
        const auto index = nextFrame++;
        return {{PhysicalSlotId(index / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U),
                 static_cast<uint8_t>(index % RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U), generation}, std::nullopt};
    }
    void abandonPage(PhysicalPageRef) noexcept override {}
private:
    uint64_t nextId = 1U, nextFrame = 128U;
};
Superblock reuseSuperblock(uint64_t generation) {
    Superblock s; s.database_id = DatabaseId(0x1020304050607080ULL);
    s.generation = Generation(generation); s.checkpoint_lsn = LogSequenceNumber(generation); return s;
}
std::shared_ptr<graph::query::execution::detail::PrepaidMemoryResource> reuseWorkGrant(size_t bytes) {
    using namespace graph::query::execution::detail;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
    auto domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(bytes));
    return std::make_shared<PrepaidMemoryResource>(
        std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, bytes), bytes);
}
}
class MetadataRouteBranchReuseTest : public graph::storage::test_support::RelocatableMetadataPagePoolTest {
protected:
    ReuseTestAllocator allocator;
    Superblock published;
    PhysicalPageRef oldRoot;
    std::vector<MetadataPageRoute> expected;
    void seed(size_t count) {
        MetadataWriteBatch first(allocator, reuseSuperblock(1U), LogSequenceNumber(2U));
        MetadataPageFile pages(io, first.candidate()); MetadataPageRouteDirectory directory(pages);
        for (size_t i = 0; i < count; ++i)
            expected.push_back({MetadataPageId(i * RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U),
                                {PhysicalSlotId(10000U + i), 1U, Generation(1U)}});
        oldRoot = directory.buildFromCompleteRouteSet(first, expected);
        PageDirectoryBootstrap header; header.generation = first.generation();
        header.committed_lsn = first.checkpointLsn(); header.metadata_page_directory_root = oldRoot;
        const auto reference = first.allocatePage(); pages.writePageDirectoryBootstrap(reference, header);
        published = first.candidate(); published.page_directory_root = reference;
        published.flags |= RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
    }
    std::map<uint64_t, PhysicalPageRef> leaves(MetadataPageFile &pages, PhysicalPageRef root) {
        std::map<uint64_t, PhysicalPageRef> result;
        const auto visit = [&](auto &&self, PhysicalPageRef reference) -> void {
            const auto node = pages.readMetadataRadix(reference);
            for (const auto &child : node.children) if (child.isValid()) {
                if (node.level != 0U) self(self, child);
                else {
                    const auto leaf = pages.readMetadataPageRouteLeafView(child);
                    result.emplace((leaf->routes.front().page_id.value() - 1U) / RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES, child);
                }
            }
        };
        visit(visit, root); return result;
    }
    void TearDown() override {
        graph::debug::PerfTrace::setValueCollectionEnabled(false); graph::debug::PerfTrace::reset();
        RelocatableMetadataPagePoolTest::TearDown();
    }
};
TEST_F(MetadataRouteBranchReuseTest, UnchangedOverlayReusesEveryPlumbingFrame) {
    seed(3U); MetadataWriteBatch batch(allocator, published, LogSequenceNumber(3U));
    MetadataPageFile pages(io, batch.candidate()); MetadataPageRouteDirectory directory(pages);
    const auto before = batch.allocatedPageCount();
    const auto root = directory.buildNextRouteBranch(batch, oldRoot);
    EXPECT_EQ(root, oldRoot); EXPECT_EQ(batch.allocatedPageCount(), before);
    EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), root), expected);
}
TEST_F(MetadataRouteBranchReuseTest, ChangedLeafCopiesItsPathAndPreservesUnchangedLeavesAndOldSnapshot) {
    seed(3U); MetadataWriteBatch batch(allocator, published, LogSequenceNumber(3U));
    MetadataPageFile pages(io, batch.candidate()); MetadataPageRouteDirectory directory(pages);
    const auto oldLeaves = leaves(pages, oldRoot);
    const auto replacement = batch.allocatePage(); batch.registerMetadataPageRoute(expected.back().page_id, replacement);
    const auto capacity = directory.nextBootstrapFrameCount(batch); const auto before = batch.allocatedPageCount();
    const auto root = directory.buildNextRouteBranch(batch, oldRoot); const auto newLeaves = leaves(pages, root);
    EXPECT_EQ(newLeaves.at(0U), oldLeaves.at(0U)); EXPECT_EQ(newLeaves.at(1U), oldLeaves.at(1U));
    EXPECT_NE(newLeaves.at(2U), oldLeaves.at(2U));
    EXPECT_LT(batch.allocatedPageCount() - before + 1U, capacity);
    EXPECT_EQ(directory.find(published, oldRoot, expected.back().page_id), expected.back().physical_reference);
    auto wanted = expected; wanted.back().physical_reference = replacement;
    EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), root), wanted);
    io->sync(); reopen(); MetadataPageFile reopened(io, batch.candidate()); MetadataPageRouteDirectory restored(reopened);
    EXPECT_EQ(restored.collectRoutesFromRouteBranch(published, oldRoot), expected);
    EXPECT_EQ(restored.collectRoutesFromRouteBranch(batch.candidate(), root), wanted);
}
TEST_F(MetadataRouteBranchReuseTest, ForcedRefreshRewritesAllPlumbingEvenWithIdenticalTargets) {
    seed(3U); MetadataWriteBatch batch(allocator, published, LogSequenceNumber(3U));
    MetadataPageFile pages(io, batch.candidate()); MetadataPageRouteDirectory directory(pages);
    const auto oldLeaves = leaves(pages, oldRoot); batch.requestBootstrapRouteRefresh();
    const auto capacity = directory.nextBootstrapFrameCount(batch); const auto before = batch.allocatedPageCount();
    const auto root = directory.buildNextRouteBranch(batch, oldRoot); const auto newLeaves = leaves(pages, root);
    EXPECT_NE(root, oldRoot); for (const auto &[ordinal, reference] : oldLeaves) EXPECT_NE(newLeaves.at(ordinal), reference);
    EXPECT_EQ(batch.allocatedPageCount() - before + 1U, capacity);
    EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), root), expected);
}
TEST_F(MetadataRouteBranchReuseTest, UnmodifiedCorruptLeafIsRejectedBeforeAnyPublicationFrameAllocation) {
    seed(3U); MetadataWriteBatch batch(allocator, published, LogSequenceNumber(3U));
    MetadataPageFile pages(io, batch.candidate()); MetadataPageRouteDirectory directory(pages);
    const auto oldLeaves = leaves(pages, oldRoot); const auto replacement = batch.allocatePage();
    batch.registerMetadataPageRoute(expected.front().page_id, replacement);
    const std::byte corrupt{0U}; io->writeAt(pages.pageOffset(oldLeaves.at(2U)), &corrupt, 1U);
    const auto before = batch.allocatedPageCount();
    EXPECT_THROW((void)directory.buildNextRouteBranch(batch, oldRoot), FormatException);
    EXPECT_EQ(batch.allocatedPageCount(), before);
}
TEST_F(MetadataRouteBranchReuseTest, RootGrowthRetainsTheUnchangedOldLeaf) {
    seed(1U); MetadataWriteBatch batch(allocator, published, LogSequenceNumber(3U));
    MetadataPageFile pages(io, batch.candidate()); MetadataPageRouteDirectory directory(pages);
    const auto oldLeaves = leaves(pages, oldRoot); const auto replacement = batch.allocatePage();
    const MetadataPageId farId(static_cast<uint64_t>(RSS_CATALOG_RADIX_FANOUT) * RSS_CATALOG_RADIX_FANOUT *
                               RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES + 1U);
    batch.registerMetadataPageRoute(farId, replacement); const auto capacity = directory.nextBootstrapFrameCount(batch);
    const auto before = batch.allocatedPageCount(); const auto root = directory.buildNextRouteBranch(batch, oldRoot);
    EXPECT_GT(pages.readMetadataRadix(root).level, pages.readMetadataRadix(oldRoot).level);
    EXPECT_EQ(leaves(pages, root).at(0U), oldLeaves.at(0U));
    EXPECT_LE(batch.allocatedPageCount() - before + 1U, capacity);
    EXPECT_EQ(directory.find(batch.candidate(), root, farId), replacement);
    EXPECT_EQ(directory.find(published, oldRoot, expected.front().page_id), expected.front().physical_reference);
}
TEST_F(MetadataRouteBranchReuseTest, WorkAdmissionDenialRefundsOptionalIndexAndFallsBackToFullRewrite) {
    seed(300U); MetadataWriteBatch batch(allocator, published, LogSequenceNumber(3U));
    MetadataPageFile pages(io, batch.candidate()); MetadataPageRouteDirectory directory(pages);
    auto work = reuseWorkGrant(16384U); PhysicalPageRef root;
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    {
        graph::utils::OwnedAllocationScope scope(work);
        EXPECT_NO_THROW(root = directory.buildNextRouteBranch(batch, oldRoot));
        EXPECT_LE(work->used(), work->capacity());
    }
    EXPECT_EQ(work->used(), 0U); const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    ASSERT_TRUE(counters.contains("native_graph.route.reuse_admission_fallback"));
    EXPECT_NE(root, oldRoot);
    EXPECT_EQ(directory.collectRoutesFromRouteBranch(batch.candidate(), root), expected);
}
