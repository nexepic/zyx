/**
 * @file test_NativeGraphWritePlanner_SharedPoolFallback.cpp
 * @brief Shared Index-pool capacity fallback coverage for native graph writes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrObject.hpp"

#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;

namespace {

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	IndexRootLocation allocateEmptyIndexRoot(BTreePageStore &indexes, uint64_t indexId, PageId pageId,
											  LogSequenceNumber checkpointLsn,
											  BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64) {
		SlottedPagePoolAllocationRequest allocation;
		allocation.segment_id = SegmentId(71U);
		allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		allocation.pool_owner_id = indexId;
		allocation.generation = Generation(1U);
		allocation.page_id = pageId;
		allocation.page_owner_id = indexId;
		allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = pageId;
		owner.owner_id = indexId;
		owner.kind = PageOwnerKind::POK_INDEX;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = Generation(1U);
		owner.committed_lsn = checkpointLsn;
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.leaf_value_layout = leafValueLayout;
		if (!indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
							checkpointLsn)
				 .has_value()) {
			throw std::runtime_error("test Index root allocation unexpectedly ran out of space");
		}
		const IndexRootLocation root{indexId, pageId, Generation(1U)};
		static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(checkpointLsn.value() + 1U)));
		return root;
	}

	IndexRootLocation appendEmptyIndexRoot(BTreePageStore &indexes, uint64_t indexId, PageId pageId,
											LogSequenceNumber checkpointLsn,
        BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64) {
		SlottedPagePoolAppendRequest request;
		request.segment_id = SegmentId(71U);
		request.generation = Generation(1U);
		request.page_id = pageId;
		request.page_owner_id = indexId;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = pageId;
		owner.owner_id = indexId;
		owner.kind = PageOwnerKind::POK_INDEX;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = Generation(1U);
		owner.committed_lsn = checkpointLsn;
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1U);
        header.leaf_value_layout = leafValueLayout;
		static_cast<void>(indexes.append(request, BTreePage::create(owner, header), checkpointLsn));
		const IndexRootLocation root{indexId, pageId, Generation(1U)};
		static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(checkpointLsn.value() + 1U)));
		return root;
	}

} // namespace

class NativeGraphWritePlannerSharedPoolFallbackTest
		: public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerSharedPoolFallbackTest,
		PublishesThreeScalarIndexesWhenTheirSharedPoolNeedsTwoFreshRebuilds) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation firstRoot = allocateEmptyIndexRoot(indexes, 17U, PageId(101U), LogSequenceNumber(2U));
	const IndexRootLocation secondRoot = appendEmptyIndexRoot(indexes, 18U, PageId(102U), LogSequenceNumber(4U));
	const IndexRootLocation thirdRoot = appendEmptyIndexRoot(indexes, 19U, PageId(103U), LogSequenceNumber(6U));
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());

	// Keep the original root frame pinned while exhausting every frame except one.
	MetadataSnapshotPin pinned = metadata_->openSnapshot();
	std::vector<MetadataSnapshotPin> generationPins;
	generationPins.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - 4U);
	const std::array<std::byte, 1U> firstKey = {std::byte{'f'}};
	for (size_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 4U; ++ordinal) {
		NativeGraphWriteTransaction update = planner.beginTransaction();
		update.stageScalarIndexUpsert(indexes, firstRoot.index_id, firstKey, ordinal + 1U);
		ASSERT_TRUE(update.commit().committed());
		generationPins.push_back(metadata_->openSnapshot());
	}

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const CatalogEntry originalPool = metadata_->findCatalog(before, SegmentId(71U));
	ASSERT_TRUE(indexes.hasReplacementFrameCapacity(originalPool, 1U));
	ASSERT_FALSE(indexes.hasReplacementFrameCapacity(originalPool, 2U));
	before.release();

	const std::array<std::byte, 1U> secondKey = {std::byte{'s'}};
	const std::array<std::byte, 1U> thirdKey = {std::byte{'t'}};
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.stageScalarIndexUpsert(indexes, thirdRoot.index_id, thirdKey, 3003U);
	transaction.stageScalarIndexUpsert(indexes, secondRoot.index_id, secondKey, 2002U);
	transaction.stageScalarIndexUpsert(indexes, firstRoot.index_id, firstKey, 1001U);
	ASSERT_TRUE(transaction.commit().committed());

	BTreeIndex first(firstRoot.index_id, indexes, *metadata_);
	BTreeIndex second(secondRoot.index_id, indexes, *metadata_);
	BTreeIndex third(thirdRoot.index_id, indexes, *metadata_);
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(first.find(current, firstKey), std::optional<uint64_t>(1001U));
	EXPECT_EQ(second.find(current, secondKey), std::optional<uint64_t>(2002U));
	EXPECT_EQ(third.find(current, thirdKey), std::optional<uint64_t>(3003U));
	const auto usesOriginalPool = [this, &current, &originalPool](const IndexRootLocation &root) {
		return metadata_->findPageLocation(current, metadata_->findIndexRoot(current, root.index_id).root_page_id)
				   .segment_id == originalPool.segment_id;
	};
	EXPECT_EQ(static_cast<uint8_t>(usesOriginalPool(firstRoot)) + static_cast<uint8_t>(usesOriginalPool(secondRoot)) +
				  static_cast<uint8_t>(usesOriginalPool(thirdRoot)),
			  1U);
	current.release();
	for (MetadataSnapshotPin &generation: generationPins) {
		generation.release();
	}
	pinned.release();

	// Reopen the single-file stores to verify that the published root bindings,
	// replacement pool pages, and their catalog metadata survive recovery rather
	// than only working through the in-memory publication path.
	reopenNativeStores();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	BTreeIndex reopenedFirst(firstRoot.index_id, reopenedIndexes, *metadata_);
	BTreeIndex reopenedSecond(secondRoot.index_id, reopenedIndexes, *metadata_);
	BTreeIndex reopenedThird(thirdRoot.index_id, reopenedIndexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(reopenedFirst.find(recovered, firstKey), std::optional<uint64_t>(1001U));
	EXPECT_EQ(reopenedSecond.find(recovered, secondKey), std::optional<uint64_t>(2002U));
	EXPECT_EQ(reopenedThird.find(recovered, thirdKey), std::optional<uint64_t>(3003U));
	recovered.release();
}

TEST_F(NativeGraphWritePlannerSharedPoolFallbackTest,
		RebuildsTheGlobalAdjacencyIndexFreshWhenItsCurrentPoolHasNoFrameCapacity) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	static_cast<void>(allocateEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U),
											 LogSequenceNumber(2U), BTreeLeafValueLayout::BLVL_EMPTY));
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t source = seed.createNode(noLabels);
	const uint64_t seedTarget = seed.createNode(noLabels);
	const uint64_t seedEdgeId = seed.createEdge(source, seedTarget, 29U);
	ASSERT_TRUE(seed.commit().committed());

	// Fill the pool only after the adjacency tree has a real leaf entry, so the
	// next mutation starts as a current-pool COW plan and exercises its fallback.
	size_t appendedRoots = 0U;
	while (true) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const CatalogEntry pool = metadata_->findCatalog(snapshot, SegmentId(71U));
		const bool hasCapacity = indexes.hasReplacementFrameCapacity(pool, 1U);
		const uint64_t checkpoint = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (!hasCapacity) {
			break;
		}
		ASSERT_LT(appendedRoots, RSS_PAGE_POOL_DATA_FRAME_COUNT);
		appendEmptyIndexRoot(indexes, 10'000U + appendedRoots, PageId(1'000U + appendedRoots),
							 LogSequenceNumber(checkpoint + 1U));
		++appendedRoots;
	}

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const CatalogEntry originalPool = metadata_->findCatalog(before, SegmentId(71U));
	ASSERT_FALSE(indexes.hasReplacementFrameCapacity(originalPool, 1U));
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId}));

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t target = transaction.createNode(noLabels);
	const uint64_t edgeId = transaction.createEdge(source, target, 31U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const IndexRootLocation rebuiltRoot = metadata_->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	EXPECT_EQ(RSS_GLOBAL_ADJACENCY_INDEX_ID, rebuiltRoot.index_id);
	EXPECT_NE(metadata_->findPageLocation(current, rebuiltRoot.root_page_id).segment_id, originalPool.segment_id);
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId, edgeId}));
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_IN, target, 4U).edge_ids,
			  (std::vector<uint64_t>{edgeId}));
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId}));
	current.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerSharedPoolFallbackTest, OwnedImagesPreserveUnrelatedIndexesInALegalSharedPool) {
    BTreePageStore indexes(*slotted_pages_, *metadata_);
    static_cast<void>(allocateEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U),
        LogSequenceNumber(2U), BTreeLeafValueLayout::BLVL_EMPTY));
    const auto foreign = appendEmptyIndexRoot(indexes, 17U, PageId(102U), LogSequenceNumber(4U));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
    const std::array<uint64_t, 0> noLabels{};
    auto seed = planner.beginTransaction(); const auto source = seed.createNode(noLabels), target = seed.createNode(noLabels);
    const auto initial = seed.createEdge(source, target, 29U); ASSERT_TRUE(seed.commit().committed());
    auto old = metadata_->openSnapshot(); const auto originalForeign = metadata_->findPageLocation(old, foreign.root_page_id);
    int64_t skipped = 0;
    graph::debug::PerfTrace::ScopedObserver observer(&skipped, [](void *owner, std::string_view key, int64_t value, bool) noexcept {
        if (key == "native_graph.write.index_placement.mixed_pool_skipped") *static_cast<int64_t *>(owner) += value;
    });
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope allocation(memory);
    auto transaction = planner.beginTransaction(); const auto added = transaction.createEdge(source, target, 31U);
    ASSERT_TRUE(transaction.commit().committed()); EXPECT_GT(skipped, 0);
    auto current = metadata_->openSnapshot(); GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
    EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
        (std::vector<uint64_t>{initial, added}));
    EXPECT_EQ(adjacency.scan(old, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
        (std::vector<uint64_t>{initial}));
    EXPECT_EQ(metadata_->findIndexRoot(current, foreign.index_id), foreign);
    EXPECT_EQ(metadata_->findPageLocation(current, foreign.root_page_id), originalForeign);
    current.release(); old.release();
}

TEST_F(NativeGraphWritePlannerSharedPoolFallbackTest, OwnedImagesAuthenticateSourcePagesInAnotherIndexFamilyPool) {
    BTreePageStore indexes(*slotted_pages_, *metadata_);
    const auto foreign = allocateEmptyIndexRoot(indexes, 17U, PageId(101U), LogSequenceNumber(2U));
    static_cast<void>(appendEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(102U),
        LogSequenceNumber(4U), BTreeLeafValueLayout::BLVL_EMPTY));
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
    const std::array<uint64_t, 0> noLabels{};
    auto seed = planner.beginTransaction(); const auto source = seed.createNode(noLabels), target = seed.createNode(noLabels);
    const auto initial = seed.createEdge(source, target, 29U); ASSERT_TRUE(seed.commit().committed());
    auto old = metadata_->openSnapshot(); const auto originalForeign = metadata_->findPageLocation(old, foreign.root_page_id);
    const auto root = metadata_->findIndexRoot(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
    const auto sourcePool = metadata_->findCatalog(old, metadata_->findPageLocation(old, root.root_page_id).segment_id);
    ASSERT_EQ(sourcePool.owner_id, foreign.index_id);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope allocation(memory);
    auto transaction = planner.beginTransaction(); const auto added = transaction.createEdge(source, target, 31U);
    ASSERT_TRUE(transaction.commit().committed());
    auto current = metadata_->openSnapshot(); GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
    EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
        (std::vector<uint64_t>{initial, added}));
    EXPECT_EQ(adjacency.scan(old, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
        (std::vector<uint64_t>{initial}));
    EXPECT_EQ(metadata_->findIndexRoot(current, foreign.index_id), foreign);
    EXPECT_EQ(metadata_->findPageLocation(current, foreign.root_page_id), originalForeign);
    const auto installed = metadata_->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
    EXPECT_NE(metadata_->findPageLocation(current, installed.root_page_id).segment_id, sourcePool.segment_id);
    current.release(); old.release();
}

TEST_F(NativeGraphWritePlannerSharedPoolFallbackTest, OwnedImagesReserveExplicitEntitySegmentsBeforeFreshIndexPools) {
    BTreePageStore indexes(*slotted_pages_, *metadata_);
    static_cast<void>(allocateEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U),
        LogSequenceNumber(2U), BTreeLeafValueLayout::BLVL_EMPTY));
    const auto foreign = appendEmptyIndexRoot(indexes, 17U, PageId(102U), LogSequenceNumber(4U));
    auto old = metadata_->openSnapshot();
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    graph::utils::OwnedAllocationScope allocation(memory);
    const std::array<uint64_t, 0> noLabels{};
    auto transaction = planner.beginTransaction();
    const auto source = transaction.createNode(noLabels), target = transaction.createNode(noLabels);
    const auto edge = transaction.createEdge(source, target, 29U);
    ASSERT_TRUE(transaction.commit().committed());
    auto current = metadata_->openSnapshot(); GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
    EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
        (std::vector<uint64_t>{edge}));
    EXPECT_TRUE(adjacency.scan(old, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids.empty());
    EXPECT_EQ(metadata_->findIndexRoot(current, foreign.index_id), foreign);
    current.release(); old.release();
}
