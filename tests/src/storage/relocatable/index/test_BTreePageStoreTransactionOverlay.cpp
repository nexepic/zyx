/**
 * @file test_BTreePageStoreTransactionOverlay.cpp
 * @brief Transaction-overlay publication tests for relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <limits>

#include "RelocatableBTreePageStoreTestUtils.hpp"

namespace {

	using namespace relocatable_btree_test;

	TEST_F(RelocatableBTreePageStoreTest, PublishesOneFinalStableRootLeafStateForAnOrderedTransactionOverlay) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

		const std::array<std::byte, 1> firstKey = {std::byte{'a'}};
		const std::array<std::byte, 1> erasedKey = {std::byte{'b'}};
		const std::array<std::byte, 1> updatedKey = {std::byte{'c'}};
		const std::array<std::byte, 1> finalKey = {std::byte{'d'}};
		static_cast<void>(store.upsertRootLeaf(17, firstKey, 1, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17, erasedKey, 2, LogSequenceNumber(5)));

		MetadataSnapshotPin before = metadata->openSnapshot();
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexUpsert(store, 17, updatedKey, 3);
		transaction.stageScalarIndexUpsert(store, 17, updatedKey, 33);
		transaction.stageScalarIndexErase(store, 17, erasedKey);
		transaction.stageScalarIndexUpsert(store, 17, finalKey, 4);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

		BTreeIndex index(17, store, *metadata);
		EXPECT_EQ(index.find(before, firstKey), std::optional<uint64_t>(1));
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2));
		EXPECT_EQ(index.find(before, updatedKey), std::nullopt);
		EXPECT_EQ(index.find(before, finalKey), std::nullopt);
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17), root);
		EXPECT_EQ(index.find(current, firstKey), std::optional<uint64_t>(1));
		EXPECT_EQ(index.find(current, erasedKey), std::nullopt);
		EXPECT_EQ(index.find(current, updatedKey), std::optional<uint64_t>(33));
		EXPECT_EQ(index.find(current, finalKey), std::optional<uint64_t>(4));
		EXPECT_EQ(current.superblock().checkpoint_lsn, LogSequenceNumber(6));
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, ErasesTheFinalKeyThroughAFreshEmptyRootWhenTheSourcePoolIsFull) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		static_cast<void>(store.upsertRootLeaf(17, key, 1, LogSequenceNumber(4)));

		LogSequenceNumber nextLsn(5);
		uint64_t nextPageId = 10'000U;
		while (true) {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint8_t availableFrames = pages.availableFrameCount(snapshot, request.segment_id);
			snapshot.release();
			if (availableFrames == 0U) {
				break;
			}
			SlottedPagePoolAppendRequest append;
			append.segment_id = request.segment_id;
			append.generation = request.generation;
			append.page_id = PageId(nextPageId++);
			append.page_owner_id = request.page_owner_id;
			append.page_owner_kind = PageOwnerKind::POK_INDEX;
			static_cast<void>(store.append(append, appendedPage(append.page_id, nextLsn), nextLsn));
			nextLsn = LogSequenceNumber(nextLsn.value() + 1U);
		}

		MetadataSnapshotPin before = metadata->openSnapshot();
		const IndexRootLocation sourceRoot = metadata->findIndexRoot(before, 17);
		const PageLocation sourceLocation = metadata->findPageLocation(before, sourceRoot.root_page_id);
		BTreeIndex index(17, store, *metadata);
		ASSERT_EQ(index.find(before, key), std::optional<uint64_t>(1));

		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17, key);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

		// The pinned source generation remains readable after its root is retired.
		EXPECT_EQ(index.find(before, key), std::optional<uint64_t>(1));
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		const IndexRootLocation emptyRoot = metadata->findIndexRoot(current, 17);
		const PageLocation emptyRootLocation = metadata->findPageLocation(current, emptyRoot.root_page_id);
		EXPECT_NE(emptyRoot.root_page_id, sourceRoot.root_page_id);
		EXPECT_NE(emptyRootLocation.segment_id, sourceLocation.segment_id);
		EXPECT_EQ(store.read(current, emptyRoot.root_page_id).entries().size(), 0U);
		EXPECT_EQ(index.find(current, key), std::nullopt);
		EXPECT_EQ(current.superblock().checkpoint_lsn, nextLsn);
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, CollapsesATransactionLocalSplitBackToOneEmptyStableRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> firstKey(1800U, std::byte{'a'});
		const std::vector<std::byte> secondKey(1800U, std::byte{'b'});
		const std::vector<std::byte> thirdKey(1800U, std::byte{'c'});
		static_cast<void>(store.upsertRootLeaf(17, firstKey, 1U, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17, secondKey, 2U, LogSequenceNumber(5)));

		MetadataSnapshotPin before = metadata->openSnapshot();
		BTreeIndex index(17, store, *metadata);
		ASSERT_EQ(index.find(before, firstKey), std::optional<uint64_t>(1U));
		ASSERT_EQ(index.find(before, secondKey), std::optional<uint64_t>(2U));

		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexUpsert(store, 17, thirdKey, 3U);
		// Erase the appended right leaf first so the overlay must retire its
		// virtual leaf and virtual root before it reaches the empty root state.
		transaction.stageScalarIndexErase(store, 17, thirdKey);
		transaction.stageScalarIndexErase(store, 17, firstKey);
		transaction.stageScalarIndexErase(store, 17, secondKey);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

		EXPECT_EQ(index.find(before, firstKey), std::optional<uint64_t>(1U));
		EXPECT_EQ(index.find(before, secondKey), std::optional<uint64_t>(2U));
		EXPECT_EQ(index.find(before, thirdKey), std::nullopt);
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17), initialRoot);
		const BTreePage root = store.read(current, initialRoot.root_page_id);
		EXPECT_EQ(root.header().kind, BTreeNodeKind::BPN_LEAF);
		EXPECT_TRUE(root.entries().empty());
		EXPECT_EQ(index.find(current, firstKey), std::nullopt);
		EXPECT_EQ(index.find(current, secondKey), std::nullopt);
		EXPECT_EQ(index.find(current, thirdKey), std::nullopt);
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsBrokenSuccessorLinkWhileSplittingAnOverlayLeaf) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4), true, false, true);
				}));

		const std::vector<std::byte> splitKey = packedLargeKey(0x0201U);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, splitKey, 99U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsBrokenSiblingLinksWhileRebalancingAnOverlayLeaf) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 3U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({largeKey(std::byte{'b'}), pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = pool.generation;
					leftHeader.right_sibling = PageId(999U);
					BTreePage left = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftHeader);
					left.insert({largeKey(std::byte{'a'}), 1U});

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = pool.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right =
							BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({largeKey(std::byte{'b'}), 2U});

					std::vector<BTreePage> tree;
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, rightKey, 0U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsTamperedVirtualOverlayPlacementsBeforePreparingFrames) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> firstKey(1800U, std::byte{'a'});
		const std::vector<std::byte> secondKey(1800U, std::byte{'b'});
		const std::vector<std::byte> splitKey(1800U, std::byte{'c'});
		static_cast<void>(store.upsertRootLeaf(17, firstKey, 1U, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17, secondKey, 2U, LogSequenceNumber(5)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, splitKey, 3U},
		};
		const BTreeStructuralMutationPlan plan = store.planScalarIndexTransactionMutation(snapshot, 17, operations);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		ASSERT_FALSE(plan.new_pool_mutation.has_value());
		ASSERT_TRUE(plan.replacement_root.has_value());
		ASSERT_EQ(plan.pool_requests.front().appended_page_count, 2U);

		IndexTreeMutationPoolBatch tampered;
		tampered.pool = metadata->findCatalog(snapshot, request.segment_id);
		tampered.expected_replacements = plan.pool_requests.front().expected_replacements;
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(6));
				},
				FormatError::FERR_INVALID_STATE);

		tampered.expected_replacements.clear();
		tampered.appended_page_ids = {PageId(900U), PageId(901U)};
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(6));
				},
				FormatError::FERR_INVALID_STATE);

		tampered.expected_replacements = plan.pool_requests.front().expected_replacements;
		tampered.appended_page_ids = {PageId{}, PageId(901U)};
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(6));
			},
			FormatError::FERR_INVALID_STATE);

		// The immutable plan assigns the replacement root to the second appended
		// Page ID. A truncated callback batch must fail before any frame is written.
		tampered.appended_page_ids = {PageId(900U)};
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(6));
				},
				FormatError::FERR_INVALID_STATE);

		tampered.appended_page_ids = {PageId(900U), PageId(901U)};
		tampered.pool.segment_id = SegmentId(999U);
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(6));
				},
				FormatError::FERR_INVALID_STATE);

		tampered.pool = metadata->findCatalog(snapshot, request.segment_id);
		tampered.expected_retirements = {IndexTreePageRetirement{{}, {}}};
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(6));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsASiblingLinkedLeafAsTheOverlayRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 1U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					BTreePageHeader header;
					header.index_id = 17U;
					header.page_id = pageIds.front();
					header.generation = pool.generation;
					header.kind = BTreeNodeKind::BPN_LEAF;
					header.right_sibling = PageId(999U);
					std::vector<BTreePage> tree;
					tree.push_back(BTreePage::create(treeOwner(pool, header.page_id, LogSequenceNumber(4)), header));
					return tree;
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, {std::byte{'a'}}, 1U},
		};
		expectFormatError([&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
						  FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsACyclicTreeWithAnInconsistentRevisitedLevel) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 2U;
					rootHeader.leftmost_child = pageIds[1];

					BTreePageHeader childHeader;
					childHeader.index_id = 17U;
					childHeader.page_id = pageIds[1];
					childHeader.generation = pool.generation;
					childHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					childHeader.level = 1U;
					childHeader.leftmost_child = pageIds[0];

					std::vector<BTreePage> tree;
					tree.push_back(BTreePage::create(treeOwner(pool, rootHeader.page_id, LogSequenceNumber(4)), rootHeader));
					tree.push_back(BTreePage::create(treeOwner(pool, childHeader.page_id, LogSequenceNumber(4)), childHeader));
					return tree;
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, {std::byte{'a'}}, 1U},
		};
		expectFormatError([&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
						  FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnOverlayKeyThatCannotFitInAnyLeafPage) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT,
								  std::vector<std::byte>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{'x'}), 1U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, DenselyRebuildsALargeTransactionOverlayBeforePoolAssignment) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

		std::vector<BTreeScalarIndexOperation> operations;
		operations.reserve(150U);
		for (uint16_t ordinal = 0U; ordinal < 150U; ++ordinal) {
			std::vector<std::byte> key(1800U, std::byte{'k'});
			key[0] = static_cast<std::byte>(ordinal >> 8U);
			key[1] = static_cast<std::byte>(ordinal);
			operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT, std::move(key), ordinal + 1U});
		}

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const BTreeStructuralMutationPlan plan = store.planScalarIndexTransactionMutation(snapshot, 17, operations);
		ASSERT_TRUE(plan.replacement_root.has_value());
		EXPECT_FALSE(plan.stable_replacement_root.has_value());
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().segment_id, request.segment_id);
		EXPECT_TRUE(plan.pool_requests.front().expected_replacements.empty());
		ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, root.root_page_id);
		EXPECT_GT(plan.pool_requests.front().appended_page_count, 0U);
		ASSERT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_GT(plan.new_pool_mutation->appended_page_count, 0U);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RebuildsAnOversizedSparseOverlayAcrossSeveralFreshPools) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

		std::vector<BTreeScalarIndexOperation> operations;
		operations.reserve(100U);
		for (uint16_t ordinal = 0U; ordinal < 100U; ++ordinal) {
			std::vector<std::byte> key(2400U, std::byte{'k'});
			key[0] = static_cast<std::byte>(ordinal >> 8U);
			key[1] = static_cast<std::byte>(ordinal);
			operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT, std::move(key), ordinal + 1U});
		}

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const BTreeStructuralMutationPlan plan = store.planScalarIndexTransactionMutation(snapshot, 17, operations);
		ASSERT_TRUE(plan.replacement_root.has_value());
		EXPECT_FALSE(plan.stable_replacement_root.has_value());
		ASSERT_FALSE(plan.pool_requests.empty());
		EXPECT_TRUE(plan.pool_requests.front().expected_replacements.empty());
		EXPECT_FALSE(plan.pool_requests.front().expected_retirements.empty());
		ASSERT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_FALSE(plan.additional_new_pool_mutations.empty());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, PlansADeepStructuralSplitWithoutPublishingAndRejectsAPlainReplacement) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4), false, false, true);
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation sourceRoot = metadata->findIndexRoot(snapshot, 17);
		const std::vector<std::byte> existingKey = packedLargeKey(0x0200U);
		expectFormatError(
				[&] { static_cast<void>(store.planDeepStructuralUpsert(snapshot, 17, existingKey, 99U)); },
				FormatError::FERR_INVALID_STATE);

		const std::vector<std::byte> splitKey = packedLargeKey(0x0201U);
		const BTreeStructuralMutationPlan plan = store.planDeepStructuralUpsert(snapshot, 17, splitKey, 100U);
		EXPECT_EQ(plan.expected_root, sourceRoot);
		EXPECT_FALSE(plan.pool_requests.empty());
		EXPECT_FALSE(plan.new_pool_mutation.has_value());
		// Prefix-compressed leaf fixtures still need full separators in their
		// ancestors. The packed structural plan therefore legitimately grows a
		// new root while remaining strictly plan-only.
		EXPECT_TRUE(plan.replacement_root.has_value());
		EXPECT_FALSE(plan.stable_replacement_root.has_value());
		EXPECT_EQ(snapshot.superblock().checkpoint_lsn, LogSequenceNumber(4));
		snapshot.release();

		MetadataSnapshotPin after = metadata->openSnapshot();
		EXPECT_EQ(after.superblock().checkpoint_lsn, LogSequenceNumber(4));
		EXPECT_EQ(metadata->findIndexRoot(after, 17), sourceRoot);
		after.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RebuildsAnEmptySplitOverlayIntoOneFreshRootPage) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> firstKey(1800U, std::byte{'a'});
		const std::vector<std::byte> secondKey(1800U, std::byte{'b'});
		const std::vector<std::byte> thirdKey(1800U, std::byte{'c'});
		static_cast<void>(store.upsertRootLeaf(17, firstKey, 1U, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17, secondKey, 2U, LogSequenceNumber(5)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, thirdKey, 3U},
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, thirdKey, 0U},
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, firstKey, 0U},
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, secondKey, 0U},
		};
		const BTreeStructuralMutationPlan plan =
				store.planScalarIndexTransactionMutation(snapshot, 17, operations, false, true);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_TRUE(plan.pool_requests.front().expected_replacements.empty());
		ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, root.root_page_id);
		ASSERT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_EQ(plan.new_pool_mutation->appended_page_count, 1U);
		ASSERT_TRUE(plan.replacement_root.has_value());
		EXPECT_FALSE(plan.stable_replacement_root.has_value());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnEmptyNonRootLeafDuringForcedOverlayRebuild) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4));
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17);
		const BTreePage rootPage = store.read(snapshot, root.root_page_id);
		const BTreePage parent = store.read(snapshot, rootPage.header().leftmost_child);
		const PageId emptyLeafId = parent.header().leftmost_child;
		const BTreePage sourceLeaf = store.read(snapshot, emptyLeafId);
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		PageOwnerHeader owner;
		owner.page_id = sourceLeaf.slottedPage().pageId();
		owner.owner_id = sourceLeaf.slottedPage().ownerId();
		owner.kind = sourceLeaf.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = sourceLeaf.slottedPage().pageGeneration();
		owner.committed_lsn = sourceLeaf.slottedPage().ownerCommittedLsn();
		BTreePageHeader header = sourceLeaf.header();
		snapshot.release();
		const std::array<BTreePageEntry, 0> noEntries{};
		const BTreePage emptyLeaf = rebuildPage(pool, header, owner.committed_lsn, noEntries);
		overwriteCurrentBTreePage(*io, *metadata, emptyLeafId, owner, emptyLeaf);

		snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, largeKey(std::byte{'d'}), 9U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, operations, false, true)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsASharedChildDuringForcedOverlayRebuild) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4));
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17);
		const BTreePage sourceRoot = store.read(snapshot, root.root_page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		PageOwnerHeader owner;
		owner.page_id = sourceRoot.slottedPage().pageId();
		owner.owner_id = sourceRoot.slottedPage().ownerId();
		owner.kind = sourceRoot.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = sourceRoot.slottedPage().pageGeneration();
		owner.committed_lsn = sourceRoot.slottedPage().ownerCommittedLsn();
		BTreePageHeader header = sourceRoot.header();
		std::vector<BTreePageEntry> entries = sourceRoot.entries();
		ASSERT_EQ(entries.size(), 1U);
		entries.front().value_or_child = header.leftmost_child.value();
		snapshot.release();
		const BTreePage sharedChildRoot = rebuildPage(pool, header, owner.committed_lsn, entries);
		overwriteCurrentBTreePage(*io, *metadata, root.root_page_id, owner, sharedChildRoot);

		snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, largeKey(std::byte{'a'}), 9U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, operations, false, true)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAZeroLeafPayloadDuringForcedOverlayRebuild) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4));
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17);
		const BTreePage rootPage = store.read(snapshot, root.root_page_id);
		const BTreePage parent = store.read(snapshot, rootPage.header().leftmost_child);
		const PageId leafId = parent.header().leftmost_child;
		const BTreePage sourceLeaf = store.read(snapshot, leafId);
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		PageOwnerHeader owner;
		owner.page_id = sourceLeaf.slottedPage().pageId();
		owner.owner_id = sourceLeaf.slottedPage().ownerId();
		owner.kind = sourceLeaf.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = sourceLeaf.slottedPage().pageGeneration();
		owner.committed_lsn = sourceLeaf.slottedPage().ownerCommittedLsn();
		BTreePageHeader header = sourceLeaf.header();
		std::vector<BTreePageEntry> entries = sourceLeaf.entries();
		ASSERT_EQ(entries.size(), 1U);
		entries.front().value_or_child = 0U;
		snapshot.release();
		const BTreePage invalidLeaf = rebuildPage(pool, header, owner.committed_lsn, entries);
		overwriteCurrentBTreePage(*io, *metadata, leafId, owner, invalidLeaf);

		snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, largeKey(std::byte{'d'}), 9U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, operations, false, true)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RepairsTheNextLeafWhenMergingAnEmptyRightOverlayLeaf) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4));
				}));

		const std::vector<std::byte> firstKey = largeKey(std::byte{'a'});
		const std::vector<std::byte> erasedKey = largeKey(std::byte{'b'});
		const std::vector<std::byte> successorKey = largeKey(std::byte{'c'});
		const std::vector<std::byte> finalKey = largeKey(std::byte{'d'});
		MetadataSnapshotPin before = metadata->openSnapshot();
		BTreeIndex index(17, store, *metadata);
		ASSERT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));

		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17, erasedKey);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));
		before.release();
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(index.find(current, firstKey), std::optional<uint64_t>(1U));
		EXPECT_EQ(index.find(current, erasedKey), std::nullopt);
		EXPECT_EQ(index.find(current, successorKey), std::optional<uint64_t>(3U));
		EXPECT_EQ(index.find(current, finalKey), std::optional<uint64_t>(4U));
		const std::vector<BTreePageEntry> entries = index.scan(current, std::nullopt, 4U);
		ASSERT_EQ(entries.size(), 3U);
		EXPECT_EQ(entries[0].key, firstKey);
		EXPECT_EQ(entries[1].key, successorKey);
		EXPECT_EQ(entries[2].key, finalKey);
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RetainsANonemptyUnderfullLeafWhenNoValidOverlayMergeExists) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

		const std::vector<std::byte> firstKey(1800U, std::byte{'a'});
		const std::vector<std::byte> erasedKey(1800U, std::byte{'b'});
		const std::vector<std::byte> thirdKey(1800U, std::byte{'c'});
		const std::vector<std::byte> fourthKey(1800U, std::byte{'d'});
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 3U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({thirdKey, pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17U;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = pool.generation;
					leftHeader.right_sibling = pageIds[2];
					BTreePage left = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftHeader);
					left.insert({firstKey, 1U});
					left.insert({erasedKey, 2U});

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17U;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = pool.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({thirdKey, 3U});
					right.insert({fourthKey, 4U});
					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation sourceRoot = metadata->findIndexRoot(snapshot, 17U);
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, erasedKey, 0U},
		};
		const BTreeStructuralMutationPlan plan = store.planScalarIndexTransactionMutation(snapshot, 17U, operations);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_EQ(plan.stable_replacement_root, sourceRoot.root_page_id);
		EXPECT_FALSE(plan.replacement_root.has_value());
		EXPECT_FALSE(plan.new_pool_mutation.has_value());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, MergesAnEmptyRightLeafThroughTheTransactionOverlay) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

		const std::vector<std::byte> survivingKey = largeKey(std::byte{'a'});
		const std::vector<std::byte> erasedKey = largeKey(std::byte{'b'});
		PageId leftLeafId;
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 3U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					leftLeafId = pageIds[1];
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({erasedKey, pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17U;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = pool.generation;
					leftHeader.right_sibling = pageIds[2];
					BTreePage left = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftHeader);
					left.insert({survivingKey, 1U});

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17U;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = pool.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({erasedKey, 2U});
					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		MetadataSnapshotPin before = metadata->openSnapshot();
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17U, erasedKey);
		ASSERT_TRUE(transaction.commit().committed());
		BTreeIndex index(17U, store, *metadata);
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17U).root_page_id, leftLeafId);
		EXPECT_EQ(index.find(current, survivingKey), std::optional<uint64_t>(1U));
		EXPECT_EQ(index.find(current, erasedKey), std::nullopt);
		EXPECT_EQ(store.read(current, leftLeafId).header().kind, BTreeNodeKind::BPN_LEAF);
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RetainsRightLeafAndRepairsBothLinksWhenDeletingAnEmptyMiddleLeaf) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
					.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

		const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
		const std::vector<std::byte> erasedKey = largeKey(std::byte{'b'});
		const std::vector<std::byte> rightKey = largeKey(std::byte{'c'});
		const std::vector<std::byte> middleSeparator{std::byte{'b'}};
		const std::vector<std::byte> rightSeparator{std::byte{'c'}};
		PageId leftLeafId;
		PageId middleLeafId;
		PageId rightLeafId;
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 4U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					leftLeafId = pageIds[1];
					middleLeafId = pageIds[2];
					rightLeafId = pageIds[3];
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = leftLeafId;
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({middleSeparator, middleLeafId.value()});
					root.insert({rightSeparator, rightLeafId.value()});

					auto makeLeaf = [&](PageId pageId, PageId leftSibling, PageId rightSibling,
										const std::vector<std::byte> &key, uint64_t value) {
						BTreePageHeader header;
						header.index_id = 17U;
						header.page_id = pageId;
						header.generation = pool.generation;
						header.left_sibling = leftSibling;
						header.right_sibling = rightSibling;
						BTreePage leaf = BTreePage::create(treeOwner(pool, pageId, LogSequenceNumber(4)), header);
						leaf.insert({key, value});
						return leaf;
					};
					std::vector<BTreePage> tree;
					tree.reserve(4U);
					tree.push_back(std::move(root));
					tree.push_back(makeLeaf(leftLeafId, {}, middleLeafId, leftKey, 1U));
					tree.push_back(makeLeaf(middleLeafId, leftLeafId, rightLeafId, erasedKey, 2U));
					tree.push_back(makeLeaf(rightLeafId, middleLeafId, {}, rightKey, 3U));
					return tree;
				}));

		BTreeIndex index(17U, store, *metadata);
		MetadataSnapshotPin before = metadata->openSnapshot();
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17U, erasedKey);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));
		EXPECT_EQ(store.read(before, leftLeafId).header().right_sibling, middleLeafId);
		EXPECT_EQ(store.read(before, rightLeafId).header().left_sibling, middleLeafId);

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findPageLocationOptional(current, middleLeafId).has_value());
		EXPECT_EQ(store.read(current, leftLeafId).header().right_sibling, rightLeafId);
		EXPECT_EQ(store.read(current, rightLeafId).header().left_sibling, leftLeafId);
		const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17U).root_page_id);
		ASSERT_EQ(root.entries().size(), 1U);
		EXPECT_EQ(root.entries().front().key, rightSeparator);
		EXPECT_EQ(root.entries().front().value_or_child, rightLeafId.value());
		EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1U));
		EXPECT_FALSE(index.find(current, erasedKey).has_value());
		EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(3U));
		before.release();
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAMissingSuccessorPredecessorLinkWhileSplittingAnOverlayLeaf) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 7U, 0U, LogSequenceNumber(4),
				[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					return successorRepairTree(pool, pageIds, LogSequenceNumber(4), false, false, true);
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17U);
		const BTreePage rootPage = store.read(snapshot, root.root_page_id);
		const BTreePage leftParent = store.read(snapshot, rootPage.header().leftmost_child);
		const BTreePage splitLeaf = store.read(snapshot, PageId(leftParent.entries().front().value_or_child));
		const PageId successorId = splitLeaf.header().right_sibling;
		const BTreePage successor = store.read(snapshot, successorId);
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		PageOwnerHeader owner;
		owner.page_id = successor.slottedPage().pageId();
		owner.owner_id = successor.slottedPage().ownerId();
		owner.kind = successor.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = successor.slottedPage().pageGeneration();
		owner.committed_lsn = successor.slottedPage().ownerCommittedLsn();
		BTreePageHeader header = successor.header();
		const std::vector<BTreePageEntry> entries = successor.entries();
		header.left_sibling = {};
		snapshot.release();
		overwriteCurrentBTreePage(*io, *metadata, successorId, owner,
								  rebuildPage(pool, header, owner.committed_lsn, entries));

		snapshot = metadata->openSnapshot();
		const std::vector<std::byte> splitKey = packedLargeKey(0x0201U);
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, splitKey, 99U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, CollapsesAUnaryTransactionOverlayPathAfterTheFinalErase) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		PageId leafId;
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 3U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					leafId = pageIds[2];
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 2U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);

					BTreePageHeader parentHeader;
					parentHeader.index_id = 17U;
					parentHeader.page_id = pageIds[1];
					parentHeader.generation = pool.generation;
					parentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					parentHeader.level = 1U;
					parentHeader.leftmost_child = pageIds[2];
					BTreePage parent = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), parentHeader);

					BTreePageHeader leafHeader;
					leafHeader.index_id = 17U;
					leafHeader.page_id = pageIds[2];
					leafHeader.generation = pool.generation;
					BTreePage leaf = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), leafHeader);
					leaf.insert({std::vector<std::byte>(key.begin(), key.end()), 1U});
					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(parent));
					tree.push_back(std::move(leaf));
					return tree;
				}));

		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17U, key);
		ASSERT_TRUE(transaction.commit().committed());
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17U).root_page_id, leafId);
		EXPECT_TRUE(store.read(current, leafId).entries().empty());
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsACollapsedTransactionRootLeafThatRetainsASiblingLink) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
						.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
		const std::vector<std::byte> erasedKey = largeKey(std::byte{'b'});
		PageId leftLeafId;
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 3U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					leftLeafId = pageIds[1];
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({erasedKey, pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17U;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = pool.generation;
					leftHeader.right_sibling = pageIds[2];
					BTreePage left = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftHeader);
					left.insert({leftKey, 1U});

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17U;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = pool.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({erasedKey, 2U});
					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const BTreePage source = store.read(snapshot, leftLeafId);
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		PageOwnerHeader owner;
		owner.page_id = source.slottedPage().pageId();
		owner.owner_id = source.slottedPage().ownerId();
		owner.kind = source.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = source.slottedPage().pageGeneration();
		owner.committed_lsn = source.slottedPage().ownerCommittedLsn();
		BTreePageHeader header = source.header();
		const std::vector<BTreePageEntry> entries = source.entries();
		header.left_sibling = PageId(999U);
		snapshot.release();
		overwriteCurrentBTreePage(*io, *metadata, leftLeafId, owner,
								  rebuildPage(pool, header, owner.committed_lsn, entries));

		snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, erasedKey, 0U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RecursivelyCollapsesAUnaryDeepTreeAfterItsFinalErase) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		PageId leafId;
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 3U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					leafId = pageIds[2];
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 2U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);

					BTreePageHeader parentHeader;
					parentHeader.index_id = 17;
					parentHeader.page_id = pageIds[1];
					parentHeader.generation = pool.generation;
					parentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					parentHeader.level = 1U;
					parentHeader.leftmost_child = pageIds[2];
					BTreePage parent =
							BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), parentHeader);

					BTreePageHeader leafHeader;
					leafHeader.index_id = 17;
					leafHeader.page_id = pageIds[2];
					leafHeader.generation = pool.generation;
					BTreePage leaf = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), leafHeader);
					leaf.insert({std::vector<std::byte>(key.begin(), key.end()), 1U});

					std::vector<BTreePage> tree;
					tree.push_back(std::move(root));
					tree.push_back(std::move(parent));
					tree.push_back(std::move(leaf));
					return tree;
				}));

		ASSERT_TRUE(store.erase(17, key, LogSequenceNumber(5)));
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17).root_page_id, leafId);
		const BTreePage root = store.read(current, leafId);
		EXPECT_EQ(root.header().kind, BTreeNodeKind::BPN_LEAF);
		EXPECT_TRUE(root.entries().empty());
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAStaleTransactionOverlayPlanWithoutPublishingItsFinalState) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

		const std::array<std::byte, 1> firstKey = {std::byte{'a'}};
		const std::array<std::byte, 1> plannedKey = {std::byte{'b'}};
		const std::array<std::byte, 1> concurrentKey = {std::byte{'c'}};
		static_cast<void>(store.upsertRootLeaf(17, firstKey, 1, LogSequenceNumber(4)));

		MetadataSnapshotPin before = metadata->openSnapshot();
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexUpsert(store, 17, plannedKey, 2);
		static_cast<void>(store.upsertRootLeaf(17, concurrentKey, 3, LogSequenceNumber(5)));

		const NativeGraphWriteCommitResult conflicted = transaction.commit();
		EXPECT_FALSE(conflicted.committed());
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_CONFLICTED);

		BTreeIndex index(17, store, *metadata);
		EXPECT_EQ(index.find(before, firstKey), std::optional<uint64_t>(1));
		EXPECT_EQ(index.find(before, plannedKey), std::nullopt);
		EXPECT_EQ(index.find(before, concurrentKey), std::nullopt);
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(index.find(current, firstKey), std::optional<uint64_t>(1));
		EXPECT_EQ(index.find(current, plannedKey), std::nullopt);
		EXPECT_EQ(index.find(current, concurrentKey), std::optional<uint64_t>(3));
		EXPECT_EQ(current.superblock().checkpoint_lsn, LogSequenceNumber(5));
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, PlansAndPreparesAStableRootLeafReplacementForAnExistingKey) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		const std::array<std::byte, 1> insertedKey = {std::byte{'b'}};
		static_cast<void>(store.upsertRootLeaf(17, key, 1, LogSequenceNumber(4)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const BTreeRootLeafMutationPlan plan = store.planRootLeafUpsert(snapshot, 17, key, 2);
		const BTreeRootLeafMutationPlan insertionPlan = store.planRootLeafUpsert(snapshot, 17, insertedKey, 3);
		EXPECT_EQ(plan.expected_root, root);
		EXPECT_EQ(plan.expected_location.page_id, root.root_page_id);
		EXPECT_EQ(plan.replacement_payload.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		PageOwnerHeader insertionOwner;
		insertionOwner.page_id = root.root_page_id;
		insertionOwner.owner_id = root.index_id;
		insertionOwner.kind = PageOwnerKind::POK_INDEX;
		insertionOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		insertionOwner.generation = root.root_generation;
		insertionOwner.committed_lsn = insertionPlan.expected_owner_lsn;
		const BTreePage inserted = BTreePage::open(insertionOwner, insertionPlan.replacement_payload);
		ASSERT_EQ(inserted.entries().size(), 2U);
		EXPECT_EQ(inserted.entries().back().key, std::vector<std::byte>(insertedKey.begin(), insertedKey.end()));
		const CatalogEntry pool = metadata->findCatalog(snapshot, plan.expected_location.segment_id);
		PageLocation replacement;
		store.prepareRootLeafMutation(plan, pool, plan.expected_location, replacement, LogSequenceNumber(5));
		EXPECT_EQ(replacement.page_id, root.root_page_id);
		EXPECT_EQ(replacement.segment_id, plan.expected_location.segment_id);
		EXPECT_EQ(replacement.generation, plan.expected_location.generation);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, ReportsBoundedReplacementCapacityAndRejectsInvalidOverlayPlans) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		EXPECT_TRUE(store.hasReplacementFrameCapacity(pool, 0U));
		EXPECT_FALSE(
				store.hasReplacementFrameCapacity(pool, static_cast<size_t>(std::numeric_limits<uint8_t>::max()) + 1U));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		const std::vector<BTreeScalarIndexOperation> noOperations;
		const std::array invalidOperations = {
				BTreeScalarIndexOperation{static_cast<BTreeScalarIndexOperationKind>(0xffU),
										  std::vector<std::byte>(key.begin(), key.end()), 1U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, noOperations)); },
				FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, invalidOperations)); },
				FormatError::FERR_INVALID_HEADER);
		expectFormatError([&] { static_cast<void>(store.planDeepStructuralUpsert(snapshot, 0, key, 1)); },
						  FormatError::FERR_INVALID_HEADER);
		expectFormatError([&] { static_cast<void>(store.planDeepStructuralUpsert(snapshot, 17, key, 1)); },
						  FormatError::FERR_INVALID_STATE);
		snapshot.release();

		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17, noOperations)); },
				FormatError::FERR_INVALID_HEADER);
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidPublicBTreePlansAndTamperedPreparationInputs) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT,
										  std::vector<std::byte>(key.begin(), key.end()), 1U},
		};

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		expectFormatError([&] { static_cast<void>(store.planRootLeafUpsert(snapshot, 0U, key, 1U)); },
						  FormatError::FERR_INVALID_HEADER);
		expectFormatError([&] { static_cast<void>(store.planDeepStructuralUpsert(snapshot, 0U, key, 1U)); },
						  FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 0U, operations)); },
				FormatError::FERR_INVALID_HEADER);

		const BTreeRootLeafMutationPlan plan = store.planRootLeafUpsert(snapshot, 17U, key, 1U);
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		BTreeRootLeafMutationPlan invalidPlan = plan;
		invalidPlan.expected_root.index_id = 0U;
		PageLocation replacement;
		expectFormatError(
				[&] {
					store.prepareRootLeafMutation(invalidPlan, pool, plan.expected_location, replacement,
												  LogSequenceNumber(4));
				},
				FormatError::FERR_INVALID_STATE);

		BTreeRootLeafMutationPlan invalidPayload = plan;
		ASSERT_LT(invalidPayload.replacement_free_bytes,
				  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES);
		++invalidPayload.replacement_free_bytes;
		expectFormatError(
				[&] {
					store.prepareRootLeafMutation(invalidPayload, pool, plan.expected_location, replacement,
												  LogSequenceNumber(4));
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] {
					const std::array<BTreePage, 0> noPages{};
					std::array<PageLocation, 0> noLocations{};
					store.prepareNewPoolInitialPages(CatalogEntry{}, noPages, noLocations, LogSequenceNumber{});
				},
				FormatError::FERR_INVALID_STATE);
		expectFormatError([&] { static_cast<void>(store.hasReplacementFrameCapacity(CatalogEntry{}, 0U)); },
						  FormatError::FERR_INVALID_STATE);
		CatalogEntry buildingPool = pool;
		buildingPool.state = SegmentState::SGS_BUILDING;
		expectFormatError([&] { static_cast<void>(store.hasReplacementFrameCapacity(buildingPool, 0U)); },
						  FormatError::FERR_INVALID_STATE);
		const std::array materializationPools = {
				IndexTreeMaterializationPoolOptions{PhysicalSlotId(41), PhysicalSlotId(41), 17U, 1U},
		};
		expectFormatError(
				[&] {
					static_cast<void>(store.materializeAndPublishTreeAcrossPools(
							materializationPools, 17U, {0U, 1U}, LogSequenceNumber(4),
							[&](std::span<const IndexTreeMaterializedPool>, size_t, size_t) {
								return emptyPage(LogSequenceNumber(4));
							},
							{}));
				},
				FormatError::FERR_INVALID_HEADER);
		snapshot.release();

		expectFormatError([&] { static_cast<void>(store.planRootLeafUpsert(snapshot, 17U, key, 1U)); },
						  FormatError::FERR_INVALID_HEADER);
		expectFormatError([&] { static_cast<void>(store.planDeepStructuralUpsert(snapshot, 17U, key, 1U)); },
						  FormatError::FERR_INVALID_HEADER);
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_HEADER);
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnOverlayPlanWhenTheRootLeafHasSiblingLinks) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> existingKey = {std::byte{'a'}};
		const std::array<std::byte, 1> insertedKey = {std::byte{'b'}};
		static_cast<void>(store.upsertRootLeaf(17U, existingKey, 1U, LogSequenceNumber(4)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const BTreePage current = store.read(snapshot, root.root_page_id);
		PageOwnerHeader owner;
		owner.page_id = root.root_page_id;
		owner.owner_id = root.index_id;
		owner.kind = PageOwnerKind::POK_INDEX;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = root.root_generation;
		owner.committed_lsn = current.slottedPage().ownerCommittedLsn();
		BTreePageHeader header = current.header();
		header.entry_count = 0U;
		header.right_sibling = PageId(103);
		BTreePage corrupted = BTreePage::create(owner, header);
		for (const BTreePageEntry &entry: current.entries()) {
			corrupted.insert(entry);
		}
		snapshot.release();
		overwriteCurrentBTreePage(*io, *metadata, root.root_page_id, owner, corrupted);

		snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT,
										  std::vector<std::byte>(insertedKey.begin(), insertedKey.end()), 2U},
		};
		expectFormatError([&] { static_cast<void>(store.planRootLeafUpsert(snapshot, 17U, insertedKey, 2U)); },
						  FormatError::FERR_INVALID_STATE);
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnOverlayPlanWhenAnInternalChildHasTheWrongLevel) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		static_cast<void>(
				store.materializeAndPublishTree(request.segment_id, 17U, 7U, 0U, LogSequenceNumber(4),
												[](const CatalogEntry &pool, std::span<const PageId> pageIds) {
													return successorRepairTree(pool, pageIds, LogSequenceNumber(4));
												}));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17U);
		BTreePage malformedRoot = store.read(snapshot, root.root_page_id);
		const BTreePage parent = store.read(snapshot, malformedRoot.header().leftmost_child);
		const std::vector<BTreePageEntry> entries = malformedRoot.entries();
		snapshot.release();
		BTreePageHeader header = malformedRoot.header();
		header.leftmost_child = parent.header().leftmost_child;
		malformedRoot = rebuildPage(pool, header, LogSequenceNumber(4), entries);
		static_cast<void>(store.replace(malformedRoot, LogSequenceNumber(5)));

		snapshot = metadata->openSnapshot();
		const std::vector<std::byte> key = largeKey(std::byte{'a'});
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, key, 1U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnOverlayEraseForAnAbsentScalarKey) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> existingKey = {std::byte{'a'}};
		const std::array<std::byte, 1> missingKey = {std::byte{'b'}};
		static_cast<void>(store.upsertRootLeaf(17U, existingKey, 1U, LogSequenceNumber(4)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE,
										  std::vector<std::byte>(missingKey.begin(), missingKey.end()), 0U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsTamperedTransactionOverlayPreparerBatchesBeforeWritingFrames) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		static_cast<void>(store.upsertRootLeaf(17U, key, 1U, LogSequenceNumber(4)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT,
										  std::vector<std::byte>(key.begin(), key.end()), 2U},
		};
		const BTreeStructuralMutationPlan plan = store.planScalarIndexTransactionMutation(snapshot, 17U, operations);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		ASSERT_TRUE(plan.preparer);
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> noBatches;
					plan.preparer(noBatches, LogSequenceNumber(5));
				},
				FormatError::FERR_INVALID_STATE);

		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		IndexTreeMutationPoolBatch tampered;
		tampered.pool = pool;
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(5));
				},
				FormatError::FERR_INVALID_STATE);

		tampered.expected_replacements = plan.pool_requests.front().expected_replacements;
		tampered.appended_page_ids.push_back(PageId(999));
		expectFormatError(
				[&] {
					std::vector<IndexTreeMutationPoolBatch> batches{tampered};
					plan.preparer(batches, LogSequenceNumber(5));
				},
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RematerializesAForcedTransactionOverlayIntoAFreshImmutablePool) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> existingKey = {std::byte{'a'}};
		const std::array<std::byte, 1> insertedKey = {std::byte{'b'}};
		static_cast<void>(store.upsertRootLeaf(17, existingKey, 1U, LogSequenceNumber(4)));

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT,
										  std::vector<std::byte>(insertedKey.begin(), insertedKey.end()), 2U},
		};
		const BTreeStructuralMutationPlan plan =
				store.planScalarIndexTransactionMutation(snapshot, 17, operations, false, true);

		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().segment_id, request.segment_id);
		EXPECT_TRUE(plan.pool_requests.front().expected_replacements.empty());
		ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, root.root_page_id);
		ASSERT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_EQ(plan.new_pool_mutation->owner_id, 17U);
		EXPECT_EQ(plan.new_pool_mutation->appended_page_count, 1U);
		EXPECT_TRUE(plan.additional_new_pool_mutations.empty());
		ASSERT_TRUE(plan.replacement_root.has_value());
		EXPECT_EQ(plan.replacement_root->pool_ordinal, plan.pool_requests.size());
		EXPECT_EQ(plan.replacement_root->appended_page_ordinal, 0U);
		EXPECT_FALSE(plan.stable_replacement_root.has_value());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RebuildsOneSharedPoolOverlayBeforePublishingTwoIndexUpdates) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pages);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());

		SlottedPagePoolAppendRequest secondRequest;
		secondRequest.segment_id = request.segment_id;
		secondRequest.generation = request.generation;
		secondRequest.page_id = PageId(102);
		secondRequest.page_owner_id = 18U;
		secondRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader secondOwner;
		secondOwner.page_id = secondRequest.page_id;
		secondOwner.owner_id = secondRequest.page_owner_id;
		secondOwner.kind = secondRequest.page_owner_kind;
		secondOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		secondOwner.generation = secondRequest.generation;
		secondOwner.committed_lsn = LogSequenceNumber(3);
		BTreePageHeader secondHeader;
		secondHeader.index_id = secondRequest.page_owner_id;
		secondHeader.page_id = secondRequest.page_id;
		secondHeader.generation = secondRequest.generation;
		ASSERT_EQ(store.append(secondRequest, BTreePage::create(secondOwner, secondHeader), LogSequenceNumber(3))
						  .location.page_id,
				  secondRequest.page_id);

		const std::array roots = {
				IndexRootLocation{17U, request.page_id, request.generation},
				IndexRootLocation{18U, secondRequest.page_id, secondRequest.generation},
		};
		static_cast<void>(store.publishRoots(roots, {}, LogSequenceNumber(4)));
		const std::array<std::byte, 1> firstKey = {std::byte{'a'}};
		const std::array<std::byte, 1> firstInsertedKey = {std::byte{'b'}};
		const std::array<std::byte, 1> secondKey = {std::byte{'c'}};
		const std::array<std::byte, 1> secondInsertedKey = {std::byte{'d'}};
		static_cast<void>(store.upsertRootLeaf(17U, firstKey, 1U, LogSequenceNumber(5)));
		static_cast<void>(store.upsertRootLeaf(18U, secondKey, 3U, LogSequenceNumber(6)));
		store.reclaimRetiredFrames(request.segment_id);

		LogSequenceNumber nextLsn(7);
		uint64_t nextPageId = 10'000U;
		while (true) {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint8_t availableFrames = pages.availableFrameCount(snapshot, request.segment_id);
			snapshot.release();
			if (availableFrames <= 1U) {
				break;
			}
			SlottedPagePoolAppendRequest filler;
			filler.segment_id = request.segment_id;
			filler.generation = request.generation;
			filler.page_id = PageId(nextPageId++);
			filler.page_owner_id = request.page_owner_id;
			filler.page_owner_kind = PageOwnerKind::POK_INDEX;
			static_cast<void>(store.append(filler, appendedPage(filler.page_id, nextLsn), nextLsn));
			nextLsn = LogSequenceNumber(nextLsn.value() + 1U);
		}

		MetadataSnapshotPin before = metadata->openSnapshot();
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexUpsert(store, 17U, firstInsertedKey, 2U);
		transaction.stageScalarIndexUpsert(store, 18U, secondInsertedKey, 4U);
		ASSERT_TRUE(transaction.commit().committed());

		BTreeIndex firstIndex(17U, store, *metadata);
		BTreeIndex secondIndex(18U, store, *metadata);
		EXPECT_EQ(firstIndex.find(before, firstKey), std::optional<uint64_t>(1U));
		EXPECT_EQ(firstIndex.find(before, firstInsertedKey), std::nullopt);
		EXPECT_EQ(secondIndex.find(before, secondKey), std::optional<uint64_t>(3U));
		EXPECT_EQ(secondIndex.find(before, secondInsertedKey), std::nullopt);
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		// The shared-pool capacity planner selects the largest demand and then the
		// earliest staged Index ID as its deterministic fresh-rebuild candidate.
		// Index 18 retains the sole current-pool frame; Index 17 receives a new root.
		EXPECT_NE(metadata->findIndexRoot(current, 17U), roots.front());
		EXPECT_EQ(metadata->findIndexRoot(current, 18U), roots.back());
		EXPECT_EQ(firstIndex.find(current, firstKey), std::optional<uint64_t>(1U));
		EXPECT_EQ(firstIndex.find(current, firstInsertedKey), std::optional<uint64_t>(2U));
		EXPECT_EQ(secondIndex.find(current, secondKey), std::optional<uint64_t>(3U));
		EXPECT_EQ(secondIndex.find(current, secondInsertedKey), std::optional<uint64_t>(4U));
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RedistributesInternalOverlayPagesAfterALeafMergeInOneTransaction) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pool(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pool);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const auto separator = [](std::byte marker, size_t bytes) { return std::vector<std::byte>(bytes, marker); };
		const std::vector<std::byte> erasedKey = separator(std::byte{'a'}, 500U);
		const std::vector<std::byte> middleKey = separator(std::byte{'b'}, 1000U);
		const std::vector<std::byte> lastLeftKey = separator(std::byte{'c'}, 1500U);
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
					BTreePage root =
							BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({separator(std::byte{'d'}, 900U), rightParentId.value()});

					BTreePageHeader leftParentHeader;
					leftParentHeader.index_id = 17;
					leftParentHeader.page_id = leftParentId;
					leftParentHeader.generation = poolEntry.generation;
					leftParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					leftParentHeader.level = 1U;
					leftParentHeader.leftmost_child = leftmostLeaf;
					BTreePage leftParent = BTreePage::create(treeOwner(poolEntry, leftParentId, LogSequenceNumber(4)),
															 leftParentHeader);
					leftParent.insert({separator(std::byte{'b'}, 900U), middleLeaf.value()});
					leftParent.insert({separator(std::byte{'c'}, 1250U), lastLeftLeaf.value()});

					BTreePageHeader rightParentHeader;
					rightParentHeader.index_id = 17;
					rightParentHeader.page_id = rightParentId;
					rightParentHeader.generation = poolEntry.generation;
					rightParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rightParentHeader.level = 1U;
					rightParentHeader.leftmost_child = pageIds[6];
					BTreePage rightParent = BTreePage::create(treeOwner(poolEntry, rightParentId, LogSequenceNumber(4)),
															  rightParentHeader);
					rightParent.insert({separator(std::byte{'e'}, 1050U), pageIds[7].value()});
					rightParent.insert({separator(std::byte{'f'}, 1050U), pageIds[8].value()});
					rightParent.insert({separator(std::byte{'g'}, 1050U), pageIds[9].value()});

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

		MetadataSnapshotPin before = metadata->openSnapshot();
		BTreeIndex index(17, store, *metadata);
		const std::optional<uint64_t> erasedValue = index.find(before, erasedKey);
		const std::optional<uint64_t> middleValue = index.find(before, middleKey);
		const std::optional<uint64_t> lastLeftValue = index.find(before, lastLeftKey);
		ASSERT_TRUE(erasedValue.has_value());
		ASSERT_TRUE(middleValue.has_value());
		ASSERT_TRUE(lastLeftValue.has_value());
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17, erasedKey);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

		EXPECT_EQ(index.find(before, erasedKey), erasedValue);
		EXPECT_EQ(index.find(before, middleKey), middleValue);
		EXPECT_EQ(index.find(before, lastLeftKey), lastLeftValue);
		before.release();
		MetadataSnapshotPin current = metadata->openSnapshot();
		const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17).root_page_id);
		EXPECT_EQ(root.header().level, 2U);
		EXPECT_EQ(store.read(current, root.header().leftmost_child).header().level, 1U);
		EXPECT_EQ(index.find(current, erasedKey), std::nullopt);
		EXPECT_EQ(index.find(current, middleKey), middleValue);
		EXPECT_EQ(index.find(current, lastLeftKey), lastLeftValue);
		EXPECT_EQ(current.superblock().checkpoint_lsn, LogSequenceNumber(5));
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, UpdatesTheParentSeparatorAfterErasingANonLeftmostLeafMinimumInATransaction) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pool(io, *metadata);
		NativeBTreeWriteContext writes(io, *metadata, pool);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> leftKey(2200U, std::byte{'a'});
		const std::vector<std::byte> erasedKey(100U, std::byte{'b'});
		const std::vector<std::byte> survivingKey(2200U, std::byte{'b'});
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17, 3U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = poolEntry.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 1U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root =
							BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({erasedKey, pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = poolEntry.generation;
					leftHeader.right_sibling = pageIds[2];
					BTreePage left =
							BTreePage::create(treeOwner(poolEntry, pageIds[1], LogSequenceNumber(4)), leftHeader);
					left.insert({leftKey, 1U});

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = poolEntry.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right =
							BTreePage::create(treeOwner(poolEntry, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({erasedKey, 2U});
					right.insert({survivingKey, 3U});

					std::vector<BTreePage> pages;
					pages.reserve(3U);
					pages.push_back(std::move(root));
					pages.push_back(std::move(left));
					pages.push_back(std::move(right));
					return pages;
				}));

		MetadataSnapshotPin before = metadata->openSnapshot();
		BTreeIndex index(17, store, *metadata);
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));
		EXPECT_EQ(index.find(before, survivingKey), std::optional<uint64_t>(3U));
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17, erasedKey);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(2U));
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17).root_page_id);
		ASSERT_EQ(root.entries().size(), 1U);
		EXPECT_EQ(root.entries().front().key, survivingKey);
		EXPECT_EQ(index.find(current, erasedKey), std::nullopt);
		EXPECT_EQ(index.find(current, survivingKey), std::optional<uint64_t>(3U));
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, PlansAFreshPoolForTransactionSplitPagesWhenTheSourcePoolIsFull) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation root{17, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
		const std::vector<std::byte> firstKey(1800U, std::byte{'a'});
		const std::vector<std::byte> secondKey(1800U, std::byte{'b'});
		const std::vector<std::byte> splitKey(1800U, std::byte{'c'});
		static_cast<void>(store.upsertRootLeaf(17, firstKey, 1U, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17, secondKey, 2U, LogSequenceNumber(5)));

		LogSequenceNumber nextLsn(6);
		uint64_t nextPageId = 10'000U;
		while (true) {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint8_t availableFrames = pages.availableFrameCount(snapshot, request.segment_id);
			snapshot.release();
			if (availableFrames <= 1U) {
				break;
			}
			SlottedPagePoolAppendRequest append;
			append.segment_id = request.segment_id;
			append.generation = request.generation;
			append.page_id = PageId(nextPageId++);
			append.page_owner_id = request.page_owner_id;
			append.page_owner_kind = PageOwnerKind::POK_INDEX;
			static_cast<void>(store.append(append, appendedPage(append.page_id, nextLsn), nextLsn));
			nextLsn = LogSequenceNumber(nextLsn.value() + 1U);
		}

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, splitKey, 3U},
		};
		const BTreeStructuralMutationPlan plan =
				store.planScalarIndexTransactionMutation(snapshot, 17, operations, true);
		ASSERT_TRUE(plan.new_pool_mutation.has_value());
		ASSERT_TRUE(plan.replacement_root.has_value());
		EXPECT_GT(plan.new_pool_mutation->appended_page_count, 0U);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().segment_id, request.segment_id);
		// Preserve the root-leaf replacement in its existing pool and reserve a
		// fresh typed pool only for the split pages that no current pool can hold.
		EXPECT_FALSE(plan.pool_requests.front().expected_replacements.empty());
		EXPECT_TRUE(plan.pool_requests.front().expected_retirements.empty());
		snapshot.release();
	}

} // namespace
