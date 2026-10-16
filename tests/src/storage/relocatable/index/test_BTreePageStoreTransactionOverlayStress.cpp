/**
 * @file test_BTreePageStoreTransactionOverlayStress.cpp
 * @brief Mixed structural transaction-overlay coverage for relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <map>
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

#include "RelocatableBTreePageStoreTestUtils.hpp"

namespace {

	using namespace relocatable_btree_test;

 TEST_F(RelocatableBTreePageStoreTest, DenseRebuildPublishesEveryKeyAndPreservesOldSnapshotWithinWriteGrant) {
  using namespace graph::query::execution::detail;
  auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
  SlottedPagePool pages(io, *metadata);
  BTreePageStore store(pages, *metadata);
  EntityPageStore entities(io, *metadata);
  EntityLabelSetStore labels(io, *metadata, pages);
  PropertyCollectionRootStore properties(pages, *metadata);
  auto plannerOptions = nativeBTreeWriteOptions();
  plannerOptions.last_slot = PhysicalSlotId(64U);
  NativeGraphWritePlanner writes(*metadata, entities, labels, properties, store, plannerOptions);
  const auto request = allocationRequest();
  ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)).has_value());
  const IndexRootLocation root{17U, request.page_id, request.generation};
  static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
  auto before = metadata->openSnapshot();
  constexpr size_t maximum = 32U * 1024U * 1024U;
  auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
  auto domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
  auto account = std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_WRITE, maximum);
  auto memory = std::make_shared<PrepaidMemoryResource>(account, maximum);
  {
   graph::utils::OwnedAllocationScope scope(memory);
   auto transaction = writes.beginTransaction();
   for (uint16_t ordinal = 0U; ordinal < 500U; ++ordinal)
    transaction.stageScalarIndexUpsert(store, 17U, wideKey(ordinal), uint64_t{ordinal} + 1U);
   ASSERT_TRUE(transaction.commit().committed());
  }
  EXPECT_EQ(memory->used(), 0U);
  BTreeIndex index(17U, store, *metadata);
  EXPECT_TRUE(index.scan(before, std::nullopt, 501U).empty());
  auto current = metadata->openSnapshot();
  const auto rows = index.scan(current, std::nullopt, 501U);
  ASSERT_EQ(rows.size(), 500U);
  for (uint16_t ordinal = 0U; ordinal < 500U; ++ordinal) {
   EXPECT_EQ(rows[ordinal].key, wideKey(ordinal));
   EXPECT_EQ(rows[ordinal].value_or_child, uint64_t{ordinal} + 1U);
  }
  current.release(); before.release();
 }

	TEST_F(RelocatableBTreePageStoreTest, AppliesMixedWideKeyOverlayMutationsAsOneDurableTreeGeneration) {
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

		std::map<uint16_t, uint64_t> expected;
		uint64_t nextLsn = 4U;
		for (uint16_t ordinal = 0U; ordinal < 24U; ++ordinal) {
			const uint64_t value = static_cast<uint64_t>(ordinal) + 1U;
			static_cast<void>(store.upsert(17U, wideKey(ordinal), value, LogSequenceNumber(nextLsn++)));
			expected.emplace(ordinal, value);
		}

		auto transaction = writes.beginTransaction();
		for (uint16_t ordinal = 1U; ordinal < 24U; ordinal += 2U) {
			transaction.stageScalarIndexErase(store, 17U, wideKey(ordinal));
			expected.erase(ordinal);
		}
		for (uint16_t ordinal = 30U; ordinal < 42U; ++ordinal) {
			const uint64_t value = 10'000U + ordinal;
			transaction.stageScalarIndexUpsert(store, 17U, wideKey(ordinal), value);
			expected.emplace(ordinal, value);
		}
		for (uint16_t ordinal = 0U; ordinal < 24U; ordinal += 4U) {
			const uint64_t replacement = 20'000U + ordinal;
			transaction.stageScalarIndexUpsert(store, 17U, wideKey(ordinal), replacement);
			expected[ordinal] = replacement;
		}
		ASSERT_TRUE(transaction.commit().committed());

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		BTreeIndex index(17U, store, *metadata);
		for (const auto &[ordinal, value]: expected) {
			EXPECT_EQ(index.find(snapshot, wideKey(ordinal)), std::optional<uint64_t>(value));
		}
		for (uint16_t ordinal = 1U; ordinal < 24U; ordinal += 2U) {
			EXPECT_EQ(index.find(snapshot, wideKey(ordinal)), std::nullopt);
		}
		const std::vector<BTreePageEntry> scanned = index.scan(snapshot, std::nullopt, expected.size() + 1U);
		ASSERT_EQ(scanned.size(), expected.size());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, DenselyRematerializesALargeTransactionOverlayBeforePublication) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

		std::vector<BTreeScalarIndexOperation> operations;
		operations.reserve(500U);
		for (uint16_t ordinal = 0U; ordinal < 500U; ++ordinal) {
			operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT, wideKey(ordinal),
								  static_cast<uint64_t>(ordinal) + 1U});
		}
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const BTreeStructuralMutationPlan plan = store.planScalarIndexTransactionMutation(snapshot, 17U, operations);
		EXPECT_TRUE(plan.replacement_root.has_value());
		EXPECT_FALSE(plan.pool_requests.empty());
		EXPECT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_FALSE(plan.additional_new_pool_mutations.empty());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RebuildsAnExhaustedOverlayIntoFreshPoolsBeforePublication) {
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
		const std::vector<std::byte> secondKey(1800U, std::byte{'b'});
		const std::vector<std::byte> splitKey(1800U, std::byte{'c'});
		static_cast<void>(store.upsertRootLeaf(17U, firstKey, 1U, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17U, secondKey, 2U, LogSequenceNumber(5)));

		LogSequenceNumber nextLsn(6U);
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

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT, splitKey, 3U},
		};
		const BTreeStructuralMutationPlan plan =
				store.planScalarIndexTransactionMutation(snapshot, 17U, operations, true);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_TRUE(plan.pool_requests.front().expected_replacements.empty());
		ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, initialRoot.root_page_id);
		EXPECT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_TRUE(plan.replacement_root.has_value());
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RebuildsAnExhaustedFinalEraseIntoOneFreshEmptyRoot) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const std::array<std::byte, 1> key = {std::byte{'a'}};
		static_cast<void>(store.upsertRootLeaf(17U, key, 1U, LogSequenceNumber(4)));

		LogSequenceNumber nextLsn(5U);
		uint64_t nextPageId = 20'000U;
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

		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE,
										  std::vector<std::byte>(key.begin(), key.end()), 0U},
		};
		const BTreeStructuralMutationPlan plan =
				store.planScalarIndexTransactionMutation(snapshot, 17U, operations, true);
		ASSERT_EQ(plan.pool_requests.size(), 1U);
		EXPECT_TRUE(plan.pool_requests.front().expected_replacements.empty());
		ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
		EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, initialRoot.root_page_id);
		ASSERT_TRUE(plan.new_pool_mutation.has_value());
		EXPECT_EQ(plan.new_pool_mutation->appended_page_count, 1U);
		ASSERT_TRUE(plan.replacement_root.has_value());
		EXPECT_EQ(plan.replacement_root->appended_page_ordinal, 0U);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RedistributesUnevenLeafPayloadsThroughTheTransactionOverlay) {
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
		const auto variableKey = [](std::byte marker, uint8_t suffix, size_t bytes) {
			std::vector<std::byte> key(bytes, marker);
			key[0] = marker;
			key[1] = static_cast<std::byte>(suffix);
			return key;
		};
		const std::vector<std::byte> erasedKey = variableKey(std::byte{'a'}, 0U, 500U);
		std::vector<BTreePageEntry> leftEntries;
		for (uint8_t suffix = 0U; suffix < 4U; ++suffix) {
			leftEntries.push_back({variableKey(std::byte{'a'}, suffix, 500U), static_cast<uint64_t>(suffix) + 1U});
		}
		std::vector<BTreePageEntry> rightEntries;
		for (uint8_t suffix = 0U; suffix < 7U; ++suffix) {
			rightEntries.push_back({variableKey(std::byte{'b'}, suffix, 480U), static_cast<uint64_t>(suffix) + 11U});
		}
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
					root.insert({std::vector<std::byte>{std::byte{'b'}}, pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17U;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = pool.generation;
					leftHeader.right_sibling = pageIds[2];
					BTreePage left = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftHeader);
					for (const BTreePageEntry &entry: leftEntries) {
						left.insert(entry);
					}

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17U;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = pool.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					for (const BTreePageEntry &entry: rightEntries) {
						right.insert(entry);
					}
					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		BTreeIndex index(17U, store, *metadata);
		MetadataSnapshotPin before = metadata->openSnapshot();
		ASSERT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(1U));
		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17U, erasedKey);
		ASSERT_TRUE(transaction.commit().committed());
		EXPECT_EQ(index.find(before, erasedKey), std::optional<uint64_t>(1U));
		before.release();

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(index.find(current, erasedKey), std::nullopt);
		for (uint8_t suffix = 1U; suffix < 4U; ++suffix) {
			EXPECT_EQ(index.find(current, variableKey(std::byte{'a'}, suffix, 500U)),
					  std::optional<uint64_t>(static_cast<uint64_t>(suffix) + 1U));
		}
		for (uint8_t suffix = 0U; suffix < 7U; ++suffix) {
			EXPECT_EQ(index.find(current, variableKey(std::byte{'b'}, suffix, 480U)),
					  std::optional<uint64_t>(static_cast<uint64_t>(suffix) + 11U));
		}
		const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17U).root_page_id);
		const BTreePage left = store.read(current, root.header().leftmost_child);
		const BTreePage right = store.read(current, PageId(root.entries().front().value_or_child));
		EXPECT_EQ(left.entries().size(), 5U);
		EXPECT_EQ(right.entries().size(), 5U);
		EXPECT_EQ(root.entries().front().key, right.entries().front().key);
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, MergesAnEmptyLeftLeafThroughTheTransactionOverlay) {
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
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
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
					root.insert({rightKey, pageIds[2].value()});

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
					right.insert({rightKey, 2U});

					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17U, leftKey);
		ASSERT_TRUE(transaction.commit().committed());
		MetadataSnapshotPin current = metadata->openSnapshot();
		const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17U).root_page_id);
		EXPECT_EQ(root.header().kind, BTreeNodeKind::BPN_LEAF);
		ASSERT_EQ(root.entries().size(), 1U);
		EXPECT_EQ(root.entries().front().key, rightKey);
		EXPECT_EQ(root.entries().front().value_or_child, 2U);
		EXPECT_FALSE(root.header().left_sibling.isValid());
		EXPECT_FALSE(root.header().right_sibling.isValid());
		current.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsInternalRootSiblingLinksBeforeOverlayOrDirectErase) {
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
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
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
					root.insert({rightKey, pageIds[2].value()});

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
					right.insert({rightKey, 2U});

					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17U);
		const BTreePage sourceRoot = store.read(snapshot, root.root_page_id);
		PageOwnerHeader owner;
		owner.page_id = sourceRoot.slottedPage().pageId();
		owner.owner_id = sourceRoot.slottedPage().ownerId();
		owner.kind = sourceRoot.slottedPage().ownerKind();
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = sourceRoot.slottedPage().pageGeneration();
		owner.committed_lsn = sourceRoot.slottedPage().ownerCommittedLsn();
		BTreePageHeader corruptedHeader = sourceRoot.header();
		const std::vector<BTreePageEntry> rootEntries = sourceRoot.entries();
		corruptedHeader.right_sibling = corruptedHeader.leftmost_child;
		corruptedHeader.entry_count = 0U;
		snapshot.release();
		BTreePage corruptedRoot = BTreePage::create(owner, corruptedHeader);
		for (const BTreePageEntry &entry: rootEntries) {
			corruptedRoot.insert(entry);
		}
		overwriteCurrentBTreePage(*io, *metadata, root.root_page_id, owner, corruptedRoot);
		snapshot = metadata->openSnapshot();
		EXPECT_TRUE(store.read(snapshot, root.root_page_id).header().right_sibling.isValid());
		snapshot.release();

		expectFormatError(
				[&] { static_cast<void>(store.erase(17U, leftKey, LogSequenceNumber(5))); },
				FormatError::FERR_INVALID_STATE);
		{
			auto transaction = writes.beginTransaction();
			transaction.stageScalarIndexErase(store, 17U, leftKey);
			expectFormatError(
					[&] {
						static_cast<void>(transaction.commit());
					},
					FormatError::FERR_INVALID_STATE);
		}
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsBrokenLeafSiblingLinksBeforeOverlayRebalance) {
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
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
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
					root.insert({rightKey, pageIds[2].value()});

					BTreePageHeader leftHeader;
					leftHeader.index_id = 17U;
					leftHeader.page_id = pageIds[1];
					leftHeader.generation = pool.generation;
					BTreePage left = BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftHeader);
					left.insert({leftKey, 1U});

					BTreePageHeader rightHeader;
					rightHeader.index_id = 17U;
					rightHeader.page_id = pageIds[2];
					rightHeader.generation = pool.generation;
					rightHeader.left_sibling = pageIds[1];
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({rightKey, 2U});

					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));

		auto transaction = writes.beginTransaction();
		transaction.stageScalarIndexErase(store, 17U, leftKey);
		expectFormatError(
				[&] {
					static_cast<void>(transaction.commit());
				},
				FormatError::FERR_INVALID_STATE);
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsLeafWhoseSuccessorOmitsItsReciprocalPredecessorLink) {
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
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
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
					root.insert({rightKey, pageIds[2].value()});

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
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({rightKey, 2U});

					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, leftKey, 0U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsLeafWhoseSuccessorPointsToTheWrongPredecessor) {
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
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
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
					root.insert({rightKey, pageIds[2].value()});

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
					rightHeader.left_sibling = PageId(999U);
					BTreePage right = BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightHeader);
					right.insert({rightKey, 2U});

					std::vector<BTreePage> tree;
					tree.reserve(3U);
					tree.push_back(std::move(root));
					tree.push_back(std::move(left));
					tree.push_back(std::move(right));
					return tree;
				}));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, leftKey, 0U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnOversizedInternalOverlayMerge) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation initialRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
		const auto separator = [](std::byte marker) { return std::vector<std::byte>(1500U, marker); };
		const std::vector<std::byte> erasedKey = smallKey(100U);
		static_cast<void>(store.materializeAndPublishTree(
				request.segment_id, 17U, 8U, 0U, LogSequenceNumber(4),
				[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
					BTreePageHeader rootHeader;
					rootHeader.index_id = 17U;
					rootHeader.page_id = pageIds[0];
					rootHeader.generation = pool.generation;
					rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rootHeader.level = 2U;
					rootHeader.leftmost_child = pageIds[1];
					BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], LogSequenceNumber(4)), rootHeader);
					root.insert({separator(std::byte{'d'}), pageIds[2].value()});

					BTreePageHeader leftParentHeader;
					leftParentHeader.index_id = 17U;
					leftParentHeader.page_id = pageIds[1];
					leftParentHeader.generation = pool.generation;
					leftParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					leftParentHeader.level = 1U;
					leftParentHeader.leftmost_child = pageIds[3];
					BTreePage leftParent =
							BTreePage::create(treeOwner(pool, pageIds[1], LogSequenceNumber(4)), leftParentHeader);
					leftParent.insert({separator(std::byte{'b'}), pageIds[4].value()});
					leftParent.insert({separator(std::byte{'c'}), pageIds[5].value()});

					BTreePageHeader rightParentHeader;
					rightParentHeader.index_id = 17U;
					rightParentHeader.page_id = pageIds[2];
					rightParentHeader.generation = pool.generation;
					rightParentHeader.kind = BTreeNodeKind::BPN_INTERNAL;
					rightParentHeader.level = 1U;
					rightParentHeader.leftmost_child = pageIds[6];
					BTreePage rightParent =
							BTreePage::create(treeOwner(pool, pageIds[2], LogSequenceNumber(4)), rightParentHeader);
					rightParent.insert({separator(std::byte{'e'}), pageIds[7].value()});

					const auto makeLeaf = [&](PageId pageId, PageId leftSibling, PageId rightSibling,
											  const std::optional<std::vector<std::byte>> &key) {
						BTreePageHeader header;
						header.index_id = 17U;
						header.page_id = pageId;
						header.generation = pool.generation;
						header.left_sibling = leftSibling;
						header.right_sibling = rightSibling;
						BTreePage page = BTreePage::create(treeOwner(pool, pageId, LogSequenceNumber(4)), header);
						if (key.has_value()) {
							page.insert({*key, pageId.value()});
						}
						return page;
					};

					std::vector<BTreePage> tree;
					tree.reserve(pageIds.size());
					tree.push_back(std::move(root));
					tree.push_back(std::move(leftParent));
					tree.push_back(std::move(rightParent));
					tree.push_back(makeLeaf(pageIds[3], {}, pageIds[4], erasedKey));
					tree.push_back(makeLeaf(pageIds[4], pageIds[3], pageIds[5], smallKey(101U)));
					tree.push_back(makeLeaf(pageIds[5], pageIds[4], pageIds[6], smallKey(102U)));
					tree.push_back(makeLeaf(pageIds[6], pageIds[5], pageIds[7], std::nullopt));
					tree.push_back(makeLeaf(pageIds[7], pageIds[6], {}, std::nullopt));
					return tree;
				}));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array operations = {
				BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE, erasedKey, 0U},
		};
		expectFormatError(
				[&] { static_cast<void>(store.planScalarIndexTransactionMutation(snapshot, 17U, operations)); },
				FormatError::FERR_INVALID_STATE);
		snapshot.release();
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsAnInvalidInitialPageLocationBeforePreparingANewPool) {
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const CatalogEntry pool = metadata->findCatalog(snapshot, request.segment_id);
		snapshot.release();
		const std::array<BTreePage, 1> initialPages = {emptyPage(LogSequenceNumber(2))};
		std::array<PageLocation, 1> invalidLocations{};
		expectFormatError(
				[&] { store.prepareNewPoolInitialPages(pool, initialPages, invalidLocations, LogSequenceNumber(2)); },
				FormatError::FERR_INVALID_STATE);
	}

	TEST_F(RelocatableBTreePageStoreTest, RejectsUnaryOverlayRootThatWouldPublishALeafWithSiblingLinks) {
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
		const std::vector<std::byte> key = smallKey(100U);
		const auto publishUnaryTree = [&](PageId leftSibling, PageId rightSibling, LogSequenceNumber checkpointLsn) {
			static_cast<void>(store.materializeAndPublishTree(
					request.segment_id, 17U, 2U, 0U, checkpointLsn,
					[&](const CatalogEntry &pool, std::span<const PageId> pageIds) {
						BTreePageHeader rootHeader;
						rootHeader.index_id = 17U;
						rootHeader.page_id = pageIds[0];
						rootHeader.generation = pool.generation;
						rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
						rootHeader.level = 1U;
						rootHeader.leftmost_child = pageIds[1];
						BTreePage root = BTreePage::create(treeOwner(pool, pageIds[0], checkpointLsn), rootHeader);

						BTreePageHeader leafHeader;
						leafHeader.index_id = 17U;
						leafHeader.page_id = pageIds[1];
						leafHeader.generation = pool.generation;
						leafHeader.left_sibling = leftSibling;
						leafHeader.right_sibling = rightSibling;
						BTreePage leaf = BTreePage::create(treeOwner(pool, pageIds[1], checkpointLsn), leafHeader);
						leaf.insert({key, 1U});

						std::vector<BTreePage> tree;
						tree.reserve(2U);
						tree.push_back(std::move(root));
						tree.push_back(std::move(leaf));
						return tree;
					}));
		};

		publishUnaryTree({}, PageId(900U), LogSequenceNumber(4));
		{
			auto transaction = writes.beginTransaction();
			transaction.stageScalarIndexErase(store, 17U, key);
			expectFormatError(
					[&] {
						static_cast<void>(
								transaction.commit());
					},
					FormatError::FERR_INVALID_STATE);
		}
		publishUnaryTree(PageId(901U), {}, LogSequenceNumber(6));
		{
			auto transaction = writes.beginTransaction();
			transaction.stageScalarIndexErase(store, 17U, key);
			expectFormatError(
					[&] {
						static_cast<void>(
								transaction.commit());
					},
					FormatError::FERR_INVALID_STATE);
		}
	}

} // namespace
