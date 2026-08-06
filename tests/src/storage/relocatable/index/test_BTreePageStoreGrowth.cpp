/**
 * @file test_BTreePageStoreGrowth.cpp
 * @brief Capacity-growth tests for relocatable B+Tree page-store mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <string_view>

#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

TEST_F(RelocatableBTreePageStoreTest, GrowsAFullDeepTreeIntoANewPoolThroughTheGenericUpsertOverload) {
	PageId appendedLeafId;
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pool(io, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

		uint64_t nextLsn = 4;
		for (uint16_t key = 100; key <= 120; key += 2U) {
			static_cast<void>(store.upsert(17, wideKey(key), key, LogSequenceNumber(nextLsn++)));
			pool.reclaimRetiredFrames(request.segment_id);
		}
		MetadataSnapshotPin established = metadata->openSnapshot();
		const IndexRootLocation establishedRoot = metadata->findIndexRoot(established, 17);
		ASSERT_EQ(store.read(established, establishedRoot.root_page_id).header().level, 2U);
		established.release();

		uint64_t fillerPageId = 300;
		while (true) {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint8_t availableFrames = pool.availableFrameCount(snapshot, request.segment_id);
			snapshot.release();
			if (availableFrames <= 2U) {
				break;
			}
			SlottedPagePoolAppendRequest filler;
			filler.segment_id = request.segment_id;
			filler.generation = request.generation;
			filler.page_id = PageId(fillerPageId++);
			filler.page_owner_id = request.page_owner_id;
			filler.page_owner_kind = PageOwnerKind::POK_INDEX;
			const LogSequenceNumber fillerLsn(nextLsn++);
			static_cast<void>(store.append(filler, appendedPage(filler.page_id, fillerLsn), fillerLsn));
		}

		BTreeIndex index(17, store, *metadata);
		MetadataSnapshotPin beforeGrowth = metadata->openSnapshot();
		EXPECT_EQ(index.find(beforeGrowth, wideKey(120)), std::optional<uint64_t>(120));
		EXPECT_FALSE(index.find(beforeGrowth, wideKey(122)).has_value());
		const IndexPagePoolGrowthOptions growth{PhysicalSlotId(41), PhysicalSlotId(41), 17};
		static_cast<void>(store.upsert(17, wideKey(122), 122, growth, LogSequenceNumber(nextLsn++)));
		EXPECT_FALSE(index.find(beforeGrowth, wideKey(122)).has_value());

		MetadataSnapshotPin current = metadata->openSnapshot();
		const IndexRootLocation currentRoot = metadata->findIndexRoot(current, 17);
		const BTreePage root = store.read(current, currentRoot.root_page_id);
		ASSERT_EQ(root.header().level, 2U);
		const BTreePage rightParent = store.read(current, PageId(root.entries().back().value_or_child));
		ASSERT_FALSE(rightParent.entries().empty());
		appendedLeafId = PageId(rightParent.entries().back().value_or_child);
		EXPECT_EQ(metadata->findPageLocation(current, appendedLeafId).segment_id, SegmentId(72));
		const CatalogEntry createdPool = metadata->findCatalog(current, SegmentId(72));
		EXPECT_EQ(createdPool.physical_slot, PhysicalSlotId(41));
		EXPECT_EQ(createdPool.kind, SegmentKind::SGK_INDEX_PAGE_POOL);
		EXPECT_EQ(createdPool.owner_id, 17U);
		EXPECT_EQ(current.superblock().next_logical_segment_id, 73U);
		for (uint16_t key = 100; key <= 122; key += 2U) {
			EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
		}
		beforeGrowth.release();
		current.release();
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findPageLocation(reopened, appendedLeafId).segment_id, SegmentId(72));
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(reopenedIndex.find(reopened, wideKey(key)), std::optional<uint64_t>(key));
	}
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest, UsesTheTightestSharedPoolForStructuralAppendPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest primary = allocationRequest();
	BTreePage fullRoot = emptyPage(LogSequenceNumber(2U));
	std::vector<std::byte> splitKey;
	for (uint16_t ordinal = 100U;; ordinal += 2U) {
		const std::vector<std::byte> key = wideKey(ordinal);
		try {
			fullRoot.insert({key, ordinal});
		} catch (const FormatException &error) {
			ASSERT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
			splitKey = key;
			break;
		}
	}
	ASSERT_FALSE(splitKey.empty());
	ASSERT_TRUE(store.allocate(primary, fullRoot, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation initialRoot{17U, primary.page_id, primary.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1U), {}, LogSequenceNumber(3U)));

	SlottedPagePoolAllocationRequest tightPool = primary;
	tightPool.segment_id = SegmentId(72U);
	tightPool.page_id = PageId(200U);
	ASSERT_TRUE(store.allocate(tightPool, appendedPage(tightPool.page_id, LogSequenceNumber(4U)), PhysicalSlotId(41U),
							   PhysicalSlotId(41U), LogSequenceNumber(4U))
					.has_value());

	uint64_t nextPageId = 201U;
	uint64_t nextLsn = 5U;
	while (true) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const uint8_t available = pool.availableFrameCount(snapshot, tightPool.segment_id);
		snapshot.release();
		if (available == 2U) {
			break;
		}
		ASSERT_GT(available, 2U);
		SlottedPagePoolAppendRequest filler;
		filler.segment_id = tightPool.segment_id;
		filler.generation = tightPool.generation;
		filler.page_id = PageId(nextPageId++);
		filler.page_owner_id = tightPool.page_owner_id;
		filler.page_owner_kind = PageOwnerKind::POK_INDEX;
		const LogSequenceNumber lsn(nextLsn++);
		static_cast<void>(store.append(filler, appendedPage(filler.page_id, lsn), lsn));
	}

	static_cast<void>(store.upsert(17U, splitKey, 999U, LogSequenceNumber(nextLsn++)));

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17U);
	EXPECT_EQ(metadata->findPageLocation(current, root.root_page_id).segment_id, tightPool.segment_id);
	BTreeIndex index(17U, store, *metadata);
	EXPECT_EQ(index.find(current, splitKey), std::optional<uint64_t>(999U));
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsGenericGrowthRequestsWithoutPublishingAPartialDeepSplit) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const std::vector<std::byte> key = wideKey(100);
	expectFormatError([&] { static_cast<void>(store.upsert(0, key, 100, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.erase(0, key, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.erase(17, key, LogSequenceNumber{})); },
					  FormatError::FERR_INVALID_HEADER);

	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

	uint64_t nextLsn = 4;
	for (uint16_t value = 100; value <= 120; value += 2U) {
		static_cast<void>(store.upsert(17, wideKey(value), value, LogSequenceNumber(nextLsn++)));
		pool.reclaimRetiredFrames(request.segment_id);
	}
	MetadataSnapshotPin established = metadata->openSnapshot();
	ASSERT_EQ(store.read(established, metadata->findIndexRoot(established, 17).root_page_id).header().level, 2U);
	established.release();

	uint64_t fillerPageId = 400;
	while (true) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const uint8_t availableFrames = pool.availableFrameCount(snapshot, request.segment_id);
		snapshot.release();
		if (availableFrames <= 2U) {
			break;
		}
		SlottedPagePoolAppendRequest filler;
		filler.segment_id = request.segment_id;
		filler.generation = request.generation;
		filler.page_id = PageId(fillerPageId++);
		filler.page_owner_id = request.page_owner_id;
		filler.page_owner_kind = PageOwnerKind::POK_INDEX;
		const LogSequenceNumber fillerLsn(nextLsn++);
		static_cast<void>(store.append(filler, appendedPage(filler.page_id, fillerLsn), fillerLsn));
	}

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeRejectedGrowth = metadata->openSnapshot();
	const IndexRootLocation beforeRoot = metadata->findIndexRoot(beforeRejectedGrowth, 17);
	expectFormatError([&] { static_cast<void>(store.upsert(17, wideKey(122), 122, LogSequenceNumber(nextLsn++))); },
					  FormatError::FERR_INVALID_STATE);
	const IndexPagePoolGrowthOptions invalidGrowth{PhysicalSlotId{}, PhysicalSlotId(41), 17};
	expectFormatError(
			[&] {
				static_cast<void>(store.upsert(17, wideKey(122), 122, invalidGrowth, LogSequenceNumber(nextLsn++)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(metadata->findIndexRoot(beforeRejectedGrowth, 17), beforeRoot);
	EXPECT_FALSE(index.find(beforeRejectedGrowth, wideKey(122)).has_value());
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, 17), beforeRoot);
	for (uint16_t value = 100; value <= 120; value += 2U) {
		EXPECT_EQ(index.find(current, wideKey(value)), std::optional<uint64_t>(value));
	}
	EXPECT_FALSE(index.find(current, wideKey(122)).has_value());
	beforeRejectedGrowth.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RepairsPredecessorLeafLinkWhenMergingAnEmptyMiddleLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> middleKey = largeKey(std::byte{'b'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'c'});
	static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
	static_cast<void>(store.upsertRootLeaf(17, middleKey, 2, LogSequenceNumber(5)));
	static_cast<void>(store.upsert(17, rightKey, 3, LogSequenceNumber(6)));

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeErase = metadata->openSnapshot();
	const BTreePage root = store.read(beforeErase, metadata->findIndexRoot(beforeErase, 17).root_page_id);
	ASSERT_EQ(root.header().kind, BTreeNodeKind::BPN_INTERNAL);
	ASSERT_EQ(root.header().level, 2U);
	ASSERT_FALSE(root.entries().empty());
	const BTreePage leftParent = store.read(beforeErase, root.header().leftmost_child);
	const BTreePage rightParent = store.read(beforeErase, PageId(root.entries().front().value_or_child));
	const PageId leftLeafId = leftParent.header().leftmost_child;
	const PageId middleLeafId = rightParent.header().leftmost_child;
	ASSERT_FALSE(rightParent.entries().empty());
	const PageId rightLeafId(rightParent.entries().front().value_or_child);
	EXPECT_EQ(store.read(beforeErase, middleLeafId).header().left_sibling, leftLeafId);
	EXPECT_EQ(store.read(beforeErase, middleLeafId).header().right_sibling, rightLeafId);
	EXPECT_EQ(index.find(beforeErase, middleKey), std::optional<uint64_t>(2));

	ASSERT_TRUE(store.erase(17, middleKey, LogSequenceNumber(7)));
	EXPECT_EQ(index.find(beforeErase, middleKey), std::optional<uint64_t>(2));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(current, middleKey).has_value());
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(3));
	EXPECT_FALSE(metadata->findPageLocationOptional(current, middleLeafId).has_value());
	EXPECT_EQ(store.read(current, leftLeafId).header().right_sibling, rightLeafId);
	EXPECT_EQ(store.read(current, rightLeafId).header().left_sibling, leftLeafId);
	beforeErase.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RepairsSuccessorLeafLinkWhenMergingAnEmptyNonFirstLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

	const std::vector<std::byte> aKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> bKey = largeKey(std::byte{'b'});
	const std::vector<std::byte> cKey = largeKey(std::byte{'c'});
	const std::vector<std::byte> dKey = largeKey(std::byte{'d'});
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
																	return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
			}));

	MetadataSnapshotPin beforeErase = metadata->openSnapshot();
	BTreeIndex index(17, store, *metadata);
	const PageId predecessorId(105);
	const PageId removedLeafId(106);
	const PageId successorId(107);
	EXPECT_EQ(index.find(beforeErase, bKey), std::optional<uint64_t>(2));
	ASSERT_TRUE(store.erase(17, bKey, LogSequenceNumber(5)));
	EXPECT_EQ(index.find(beforeErase, bKey), std::optional<uint64_t>(2));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, removedLeafId).has_value());
	EXPECT_FALSE(index.find(current, bKey).has_value());
	EXPECT_EQ(store.read(current, predecessorId).header().right_sibling, successorId);
	EXPECT_EQ(store.read(current, successorId).header().left_sibling, predecessorId);
	EXPECT_EQ(index.find(current, aKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(current, cKey), std::optional<uint64_t>(3));
	EXPECT_EQ(index.find(current, dKey), std::optional<uint64_t>(4));
	beforeErase.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RoutesAStableSingleLeafThroughTheGenericUpsertEntryPoint) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

	const std::vector<std::byte> key = smallKey(101);
	const SharedPagePoolAllocation published = store.upsert(17, key, 9, LogSequenceNumber(4));
	EXPECT_EQ(published.location.page_id, request.page_id);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17), initialRoot);
	EXPECT_EQ(store.read(snapshot, request.page_id).header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_EQ(index.find(snapshot, key), std::optional<uint64_t>(9));
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsAGenericUpsertAgainstALeafRootWithSiblingLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	BTreePageHeader invalidRootHeader;
	invalidRootHeader.index_id = request.page_owner_id;
	invalidRootHeader.page_id = request.page_id;
	invalidRootHeader.generation = request.generation;
	invalidRootHeader.right_sibling = PageId(102);
	ASSERT_TRUE(store.allocate(request, BTreePage::create(pageOwner(LogSequenceNumber(2)), invalidRootHeader),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation root{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	expectFormatError([&] { static_cast<void>(store.upsert(17, smallKey(101), 9, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(store.read(snapshot, request.page_id).entries().empty());
}

TEST_F(RelocatableBTreePageStoreTest, RejectsEraseWhenTheIndexRootTargetsANonRootLeafWithSiblingLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
													return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
	}));

	const IndexRootLocation malformedRoot{17, PageId(106), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&malformedRoot, 1), {}, LogSequenceNumber(5)));
	MetadataSnapshotPin beforeRejectedErase = metadata->openSnapshot();
	EXPECT_EQ(store.read(beforeRejectedErase, malformedRoot.root_page_id).header().left_sibling, PageId(105));
	expectFormatError([&] { static_cast<void>(store.erase(17, largeKey(std::byte{'a'}), LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store.read(beforeRejectedErase, malformedRoot.root_page_id).entries().size(), 1U);
	beforeRejectedErase.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, 17), malformedRoot);
	EXPECT_EQ(store.read(current, malformedRoot.root_page_id).entries().size(), 1U);
	current.release();

	const IndexRootLocation rightLinkedRoot{17, PageId(105), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&rightLinkedRoot, 1), {}, LogSequenceNumber(7)));
	expectFormatError([&] { static_cast<void>(store.erase(17, largeKey(std::byte{'a'}), LogSequenceNumber(8))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsEraseWhenTheParentReferencesAChildAtTheWrongLevel) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
																							return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
			}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	BTreePage malformedRoot = store.read(snapshot, PageId(102));
	const std::vector<BTreePageEntry> entries = malformedRoot.entries();
	snapshot.release();
	BTreePageHeader header = malformedRoot.header();
	header.leftmost_child = PageId(105); // Leaf, while this level-two root requires a level-one child.
	malformedRoot = rebuildPage(poolEntry, header, LogSequenceNumber(4), entries);
	static_cast<void>(store.replace(malformedRoot, LogSequenceNumber(5)));

	expectFormatError([&] { static_cast<void>(store.erase(17, largeKey(std::byte{'b'}), LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.read(current, PageId(106)).entries().size(), 1U);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsLeafMergeWhenAdjacentLinksDisagreeWithParentOrder) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
			}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	BTreePage predecessor = store.read(snapshot, PageId(105));
	const std::vector<BTreePageEntry> entries = predecessor.entries();
	snapshot.release();
	BTreePageHeader header = predecessor.header();
	header.right_sibling = PageId(999);
	predecessor = rebuildPage(poolEntry, header, LogSequenceNumber(4), entries);
	static_cast<void>(store.replace(predecessor, LogSequenceNumber(5)));

	expectFormatError([&] { static_cast<void>(store.erase(17, largeKey(std::byte{'b'}), LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.read(current, PageId(106)).entries().size(), 1U);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsDeepLeafSplitWhenTheRightSiblingIsNotReciprocal) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4), true, false, true);
			}));

	const std::vector<std::byte> splitKey = packedLargeKey(0x0201U);
	expectFormatError([&] { static_cast<void>(store.upsert(17, splitKey, 9, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.read(current, PageId(106)).entries().size(), 1U);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsDeepUpsertWhenAnInternalChildHasTheWrongLevel) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
			}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	BTreePage malformedParent = store.read(snapshot, PageId(103));
	const std::vector<BTreePageEntry> entries = malformedParent.entries();
	snapshot.release();
	BTreePageHeader header = malformedParent.header();
	header.level = 2;
	malformedParent = rebuildPage(poolEntry, header, LogSequenceNumber(4), entries);
	static_cast<void>(store.replace(malformedParent, LogSequenceNumber(5)));

	expectFormatError([&] { static_cast<void>(store.upsert(17, largeKey(std::byte{'b'}), 9, LogSequenceNumber(6))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsDeepUpsertWhenAReachableChildBelongsToAnotherIndex) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
			}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	const BTreePage child = store.read(snapshot, PageId(106));
	const std::vector<BTreePageEntry> entries = child.entries();
	snapshot.release();
	BTreePageHeader header = child.header();
	header.index_id = 18;
	header.entry_count = 0;
	PageOwnerHeader owner = treeOwner(poolEntry, header.page_id, LogSequenceNumber(4));
	owner.owner_id = header.index_id;
	BTreePage foreign = BTreePage::create(owner, header);
	for (const BTreePageEntry &entry: entries) {
		foreign.insert(entry);
	}
	PageOwnerHeader outerOwner = owner;
	outerOwner.payload_bytes = static_cast<uint32_t>(foreign.serialize().size());
	overwriteCurrentBTreePage(*io, *metadata, header.page_id, outerOwner, foreign);

	expectFormatError([&] { static_cast<void>(store.upsert(17, largeKey(std::byte{'b'}), 9, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsDeepLeafSplitWhenRightSiblingHasWrongTypeOrIndexOwner) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(
			store.materializeAndPublishTree(request.segment_id, 17, 7, 0, LogSequenceNumber(4),
											[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
													return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4), false, false, true);
											}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	const BTreePage sibling = store.read(snapshot, PageId(106));
	const std::vector<BTreePageEntry> entries = sibling.entries();
	snapshot.release();
	const BTreePageHeader originalHeader = sibling.header();
	const std::vector<std::byte> splitKey = packedLargeKey(0x0101U);
	for (const uint8_t scenario: {0U, 1U}) {
		BTreePageHeader header = originalHeader;
		PageOwnerHeader owner = treeOwner(poolEntry, header.page_id, LogSequenceNumber(4));
		if (scenario == 0U) {
			header.kind = BTreeNodeKind::BPN_INTERNAL;
			header.level = 1;
			header.leftmost_child = PageId(105);
		} else {
			header.index_id = 18;
			owner.owner_id = header.index_id;
		}
		header.entry_count = 0;
		BTreePage malformed = BTreePage::create(owner, header);
		for (const BTreePageEntry &entry: entries) {
			malformed.insert(entry);
		}
		owner.payload_bytes = static_cast<uint32_t>(malformed.serialize().size());
		overwriteCurrentBTreePage(*io, *metadata, header.page_id, owner, malformed);

		try {
			static_cast<void>(store.upsert(17, splitKey, 9, LogSequenceNumber(5)));
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
			EXPECT_NE(std::string_view(error.what()).find("right sibling"), std::string_view::npos);
		}

		BTreePage restored = rebuildPage(poolEntry, originalHeader, LogSequenceNumber(4), entries);
		owner = treeOwner(poolEntry, originalHeader.page_id, LogSequenceNumber(4));
		owner.payload_bytes = static_cast<uint32_t>(restored.serialize().size());
		overwriteCurrentBTreePage(*io, *metadata, originalHeader.page_id, owner, restored);
	}
}

TEST_F(RelocatableBTreePageStoreTest, RejectsEraseWhenALeafPositionContainsAnInternalPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(
			store.materializeAndPublishTree(request.segment_id, 17, 7, 0, LogSequenceNumber(4),
											[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
												return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
											}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	const BTreePage leaf = store.read(snapshot, PageId(105));
	const std::vector<BTreePageEntry> entries = leaf.entries();
	snapshot.release();
	BTreePageHeader header = leaf.header();
	header.kind = BTreeNodeKind::BPN_INTERNAL;
	header.level = 1;
	header.leftmost_child = PageId(106);
	header.entry_count = 0;
	PageOwnerHeader owner = treeOwner(poolEntry, header.page_id, LogSequenceNumber(4));
	BTreePage malformed = BTreePage::create(owner, header);
	for (const BTreePageEntry &entry: entries) {
		malformed.insert(entry);
	}
	owner.payload_bytes = static_cast<uint32_t>(malformed.serialize().size());
	overwriteCurrentBTreePage(*io, *metadata, header.page_id, owner, malformed);

	expectFormatError([&] { static_cast<void>(store.erase(17, largeKey(std::byte{'a'}), LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsLeafMergeWhenSiblingRangesOverlap) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> first = mediumKey(std::byte{'b'}, std::byte{'a'});
	const std::vector<std::byte> second = mediumKey(std::byte{'b'}, std::byte{'c'});
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[&first, &second](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				std::vector<BTreePage> pages = successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
				const std::vector<BTreePageEntry> leftEntries = {{first, 1}};
				const std::vector<BTreePageEntry> rightEntries = {{first, 2}, {second, 3}};
				pages[3] = rebuildPage(poolEntry, pages[3].header(), LogSequenceNumber(4), leftEntries);
				pages[4] = rebuildPage(poolEntry, pages[4].header(), LogSequenceNumber(4), rightEntries);
				return pages;
			}));

	expectFormatError([&] { static_cast<void>(store.erase(17, second, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, PageId(106)).entries().size(), 2U);
}

TEST_F(RelocatableBTreePageStoreTest, CollapsesAUnaryInternalAncestorAfterDeletingItsOnlyLeafEntry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				std::vector<BTreePage> pages = successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
				BTreePageHeader unary = pages[1].header();
				unary.leftmost_child = PageId(106);
				pages[1] = rebuildPage(poolEntry, unary, LogSequenceNumber(4), {});
				return pages;
			}));

	ASSERT_TRUE(store.erase(17, largeKey(std::byte{'b'}), LogSequenceNumber(5)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(root.root_page_id, PageId(103));
	EXPECT_EQ(store.read(current, root.root_page_id).header().level, 1U);
}

TEST_F(RelocatableBTreePageStoreTest, MergesAnEmptyRightLeafIntoItsPredecessor) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
	static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
	static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(5)));

	ASSERT_TRUE(store.erase(17, rightKey, LogSequenceNumber(6)));
	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	const BTreePage rootPage = store.read(current, root.root_page_id);
	EXPECT_EQ(rootPage.header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_FALSE(rootPage.header().left_sibling.isValid());
	EXPECT_FALSE(rootPage.header().right_sibling.isValid());
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(current, rightKey).has_value());
}

TEST_F(RelocatableBTreePageStoreTest, RedistributesUnevenLeafPayloadsBeforeMerging) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const auto variableKey = [](std::byte marker, uint8_t suffix, size_t bytes) {
		std::vector<std::byte> key(bytes, marker);
		key[0] = marker;
		key[1] = static_cast<std::byte>(suffix);
		return key;
	};
	const std::vector<std::byte> erasedKey = variableKey(std::byte{'a'}, 0U, 500U);
	std::vector<BTreePageEntry> leftEntries;
	for (uint8_t suffix = 0; suffix < 4U; ++suffix) {
		leftEntries.push_back({variableKey(std::byte{'a'}, suffix, 500U), suffix + 1U});
	}
	std::vector<BTreePageEntry> rightEntries;
	for (uint8_t suffix = 0; suffix < 7U; ++suffix) {
		rightEntries.push_back({variableKey(std::byte{'b'}, suffix, 480U), suffix + 11U});
	}
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 3, 0, LogSequenceNumber(4),
			[&](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				BTreePageHeader rootHeader;
				rootHeader.index_id = 17;
				rootHeader.page_id = pageIds[0];
				rootHeader.generation = poolEntry.generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = 1U;
				rootHeader.leftmost_child = pageIds[1];
				BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4)), rootHeader);
				root.insert({std::vector<std::byte>{std::byte{'b'}}, pageIds[2].value()});

				BTreePageHeader leftHeader;
				leftHeader.index_id = 17;
				leftHeader.page_id = pageIds[1];
				leftHeader.generation = poolEntry.generation;
				leftHeader.right_sibling = pageIds[2];
				BTreePage left = BTreePage::create(treeOwner(poolEntry, pageIds[1], LogSequenceNumber(4)), leftHeader);
				for (const BTreePageEntry &entry: leftEntries) {
					left.insert(entry);
				}

				BTreePageHeader rightHeader;
				rightHeader.index_id = 17;
				rightHeader.page_id = pageIds[2];
				rightHeader.generation = poolEntry.generation;
				rightHeader.left_sibling = pageIds[1];
				BTreePage right = BTreePage::create(treeOwner(poolEntry, pageIds[2], LogSequenceNumber(4)), rightHeader);
				for (const BTreePageEntry &entry: rightEntries) {
					right.insert(entry);
				}
				std::vector<BTreePage> pages;
				pages.reserve(3U);
				pages.push_back(std::move(root));
				pages.push_back(std::move(left));
				pages.push_back(std::move(right));
				return pages;
			}));

	ASSERT_TRUE(store.erase(17, erasedKey, LogSequenceNumber(5)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	const BTreePage rootPage = store.read(current, root.root_page_id);
	const BTreePage left = store.read(current, rootPage.header().leftmost_child);
	const BTreePage right = store.read(current, PageId(rootPage.entries().front().value_or_child));
	EXPECT_EQ(left.entries().size(), 5U);
	EXPECT_EQ(right.entries().size(), 5U);
	EXPECT_EQ(rootPage.entries().front().key, right.entries().front().key);
	EXPECT_FALSE(BTreeIndex(17, store, *metadata).find(current, erasedKey).has_value());
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RetainsTheRightLeafWhenDeletingAnEmptyMiddleLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> middleKey = largeKey(std::byte{'b'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'c'});
	PageId leftLeaf;
	PageId middleLeaf;
	PageId rightLeaf;
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 4, 0, LogSequenceNumber(4),
			[&](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				leftLeaf = pageIds[1];
				middleLeaf = pageIds[2];
				rightLeaf = pageIds[3];
				BTreePageHeader rootHeader;
				rootHeader.index_id = 17;
				rootHeader.page_id = pageIds[0];
				rootHeader.generation = poolEntry.generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = 1U;
				rootHeader.leftmost_child = leftLeaf;
				BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4)), rootHeader);
				root.insert({std::vector<std::byte>{std::byte{'b'}}, middleLeaf.value()});
				root.insert({std::vector<std::byte>{std::byte{'c'}}, rightLeaf.value()});

				auto makeLeaf = [&](PageId pageId, PageId leftSibling, PageId rightSibling,
									const std::vector<std::byte> &key, uint64_t value) {
					BTreePageHeader header;
					header.index_id = 17;
					header.page_id = pageId;
					header.generation = poolEntry.generation;
					header.left_sibling = leftSibling;
					header.right_sibling = rightSibling;
					BTreePage page = BTreePage::create(treeOwner(poolEntry, pageId, LogSequenceNumber(4)), header);
					page.insert({key, value});
					return page;
				};
				std::vector<BTreePage> pages;
				pages.reserve(4U);
				pages.push_back(std::move(root));
				pages.push_back(makeLeaf(leftLeaf, {}, middleLeaf, leftKey, 1));
				pages.push_back(makeLeaf(middleLeaf, leftLeaf, rightLeaf, middleKey, 2));
				pages.push_back(makeLeaf(rightLeaf, middleLeaf, {}, rightKey, 3));
				return pages;
			}));

	ASSERT_TRUE(store.erase(17, middleKey, LogSequenceNumber(5)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, middleLeaf).has_value());
	EXPECT_EQ(store.read(current, leftLeaf).header().right_sibling, rightLeaf);
	EXPECT_EQ(store.read(current, rightLeaf).header().left_sibling, leftLeaf);
	const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17).root_page_id);
	EXPECT_EQ(root.entries().size(), 1U);
	EXPECT_EQ(root.entries().front().value_or_child, rightLeaf.value());
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsRootCollapseWhenTheCandidateLeafKeepsSiblingLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> key = smallKey(101);
	const auto publishUnaryTree = [&](PageId leftSibling, PageId rightSibling, LogSequenceNumber checkpointLsn) {
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 2, 0, checkpointLsn,
				[&](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = poolEntry.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], checkpointLsn), rootHeader);

					BTreePageHeader leafHeader;
					leafHeader.index_id = 17;
					leafHeader.page_id = pageIds[1];
					leafHeader.generation = poolEntry.generation;
					leafHeader.left_sibling = leftSibling;
					leafHeader.right_sibling = rightSibling;
					BTreePage leaf = BTreePage::create(treeOwner(poolEntry, pageIds[1], checkpointLsn), leafHeader);
					leaf.insert({key, 1});
					std::vector<BTreePage> pages;
					pages.reserve(2U);
					pages.push_back(std::move(root));
					pages.push_back(std::move(leaf));
					return pages;
				}));
	};

	publishUnaryTree({}, PageId(900), LogSequenceNumber(4));
	expectFormatError([&] { static_cast<void>(store.erase(17, key, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
	publishUnaryTree(PageId(901), {}, LogSequenceNumber(6));
	expectFormatError([&] { static_cast<void>(store.erase(17, key, LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, MergesTwoEmptyLeafSiblingsAfterDeletingTheOnlyLeftEntry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> key = mediumKey(std::byte{'a'}, std::byte{1});
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 3, 0, LogSequenceNumber(4),
			[&](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				BTreePageHeader rootHeader;
				rootHeader.index_id = 17;
				rootHeader.page_id = pageIds[0];
				rootHeader.generation = poolEntry.generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = 1U;
				rootHeader.leftmost_child = pageIds[1];
				BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4)), rootHeader);
				root.insert({mediumKey(std::byte{'f'}, std::byte{1}), pageIds[2].value()});

				BTreePageHeader leftHeader;
				leftHeader.index_id = 17;
				leftHeader.page_id = pageIds[1];
				leftHeader.generation = poolEntry.generation;
				leftHeader.right_sibling = pageIds[2];
				BTreePage left = BTreePage::create(treeOwner(poolEntry, pageIds[1], LogSequenceNumber(4)), leftHeader);
				left.insert({key, 1});

				BTreePageHeader rightHeader;
				rightHeader.index_id = 17;
				rightHeader.page_id = pageIds[2];
				rightHeader.generation = poolEntry.generation;
				rightHeader.left_sibling = pageIds[1];
				BTreePage right = BTreePage::create(treeOwner(poolEntry, pageIds[2], LogSequenceNumber(4)), rightHeader);
				std::vector<BTreePage> pages;
				pages.reserve(3U);
				pages.push_back(std::move(root));
				pages.push_back(std::move(left));
				pages.push_back(std::move(right));
				return pages;
			}));

	ASSERT_TRUE(store.erase(17, key, LogSequenceNumber(5)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17).root_page_id);
	EXPECT_EQ(root.header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_TRUE(root.entries().empty());
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RedistributesAsymmetricInternalPagesAfterALeafMerge) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const auto separator = [](std::byte marker, size_t bytes) {
		return std::vector<std::byte>(bytes, marker);
	};
	const std::vector<std::byte> erasedKey = smallKey(101);
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 10, 0, LogSequenceNumber(4),
			[&](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				const auto makeLeaf = [&](PageId pageId, PageId leftSibling, PageId rightSibling,
										const std::vector<std::byte> *entryKey) {
					BTreePageHeader header;
					header.index_id = 17;
					header.page_id = pageId;
					header.generation = poolEntry.generation;
					header.left_sibling = leftSibling;
					header.right_sibling = rightSibling;
					BTreePage page = BTreePage::create(treeOwner(poolEntry, pageId, LogSequenceNumber(4)), header);
					if (entryKey != nullptr) {
						page.insert({*entryKey, pageId.value()});
					}
					return page;
				};
				const PageId leftParentId = pageIds[1];
				const PageId rightParentId = pageIds[2];
				const PageId leftmostLeaf = pageIds[3];
				const PageId middleLeaf = pageIds[4];
				const PageId lastLeftLeaf = pageIds[5];

				BTreePageHeader rootHeader;
				rootHeader.index_id = 17;
				rootHeader.page_id = pageIds[0];
				rootHeader.generation = poolEntry.generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = 2U;
				rootHeader.leftmost_child = leftParentId;
				BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4)), rootHeader);
				root.insert({separator(std::byte{'d'}, 900U), rightParentId.value()});

				BTreePageHeader leftParentHeader;
				leftParentHeader.index_id = 17;
				leftParentHeader.page_id = leftParentId;
				leftParentHeader.generation = poolEntry.generation;
				leftParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				leftParentHeader.level = 1U;
				leftParentHeader.leftmost_child = leftmostLeaf;
				BTreePage leftParent =
						BTreePage::create(treeOwner(poolEntry, leftParentId, LogSequenceNumber(4)), leftParentHeader);
				leftParent.insert({separator(std::byte{'b'}, 900U), middleLeaf.value()});
				leftParent.insert({separator(std::byte{'c'}, 1250U), lastLeftLeaf.value()});

				BTreePageHeader rightParentHeader;
				rightParentHeader.index_id = 17;
				rightParentHeader.page_id = rightParentId;
				rightParentHeader.generation = poolEntry.generation;
				rightParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rightParentHeader.level = 1U;
				rightParentHeader.leftmost_child = pageIds[6];
				BTreePage rightParent =
						BTreePage::create(treeOwner(poolEntry, rightParentId, LogSequenceNumber(4)), rightParentHeader);
				rightParent.insert({separator(std::byte{'e'}, 1050U), pageIds[7].value()});
				rightParent.insert({separator(std::byte{'f'}, 1050U), pageIds[8].value()});
				rightParent.insert({separator(std::byte{'g'}, 1050U), pageIds[9].value()});

				const std::vector<std::byte> middleKey = smallKey(201);
				const std::vector<std::byte> lastLeftKey = smallKey(301);
				std::vector<BTreePage> pages;
				pages.reserve(pageIds.size());
				pages.push_back(std::move(root));
				pages.push_back(std::move(leftParent));
				pages.push_back(std::move(rightParent));
				pages.push_back(makeLeaf(leftmostLeaf, {}, middleLeaf, &erasedKey));
				pages.push_back(makeLeaf(middleLeaf, leftmostLeaf, lastLeftLeaf, &middleKey));
				pages.push_back(makeLeaf(lastLeftLeaf, middleLeaf, {}, &lastLeftKey));
				for (size_t index = 6U; index < pageIds.size(); ++index) {
					pages.push_back(makeLeaf(pageIds[index], {}, {}, nullptr));
				}
				return pages;
			}));

	ASSERT_TRUE(store.erase(17, erasedKey, LogSequenceNumber(5)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17).root_page_id);
	EXPECT_EQ(root.header().level, 2U);
	EXPECT_EQ(store.read(current, root.header().leftmost_child).header().level, 1U);
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsADeepSplitWhenOneKeyCannotFitInAnyLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4));
			}));

	std::vector<std::byte> oversized(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{'b'});
	expectFormatError([&] { static_cast<void>(store.upsert(17, oversized, 9, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsLeafMergeWhenTheSuccessorDoesNotReferenceItsRetiredPredecessor) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4), true);
			}));

	const std::vector<std::byte> bKey = largeKey(std::byte{'b'});
	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeRejectedErase = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.erase(17, bKey, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(index.find(beforeRejectedErase, bKey), std::optional<uint64_t>(2));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(index.find(current, bKey), std::optional<uint64_t>(2));
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(106)).has_value());
	beforeRejectedErase.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsLeafMergeWhenThePredecessorDoesNotReferenceItsRetiredSuccessor) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, 17, 7, 0, LogSequenceNumber(4),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				return successorRepairTree(poolEntry, pageIds, LogSequenceNumber(4), false, true);
			}));

	const std::vector<std::byte> cKey = largeKey(std::byte{'c'});
	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeRejectedErase = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.erase(17, cKey, LogSequenceNumber(5))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(index.find(beforeRejectedErase, cKey), std::optional<uint64_t>(3));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(index.find(current, cKey), std::optional<uint64_t>(3));
	EXPECT_TRUE(metadata->findPageLocationOptional(current, PageId(107)).has_value());
	beforeRejectedErase.release();
	current.release();
}
