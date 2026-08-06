/**
 * @file test_BTreePageStore.cpp
 * @brief Durable COW materialization tests for relocatable B+Tree index pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

TEST_F(RelocatableBTreePageStoreTest, AtomicallyMaterializesAndCowReplacesAStableIndexPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin old = metadata->openSnapshot();
	BTreePage updated = store.read(old, request.page_id);
	updated.insert({{std::byte{'a'}}, 100});
	static_cast<void>(store.replace(updated, LogSequenceNumber(3)));
	EXPECT_TRUE(store.read(old, request.page_id).entries().empty());
	MetadataSnapshotPin current = metadata->openSnapshot();
	ASSERT_EQ(store.read(current, request.page_id).entries().size(), 1U);
	EXPECT_EQ(store.read(current, request.page_id).entries().front().value_or_child, 100U);
	old.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	ASSERT_EQ(reopenedStore.read(reopened, request.page_id).entries().size(), 1U);
	EXPECT_EQ(reopenedStore.read(reopened, request.page_id).entries().front().key,
			  std::vector<std::byte>({std::byte{'a'}}));
}

TEST_F(RelocatableBTreePageStoreTest, EvacuatesARootPageAndRebindsItsGenerationAtomically) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = allocationRequest();
	ASSERT_TRUE(store.allocate(sourceRequest, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation sourceRoot{17, sourceRequest.page_id, sourceRequest.generation};
	static_cast<void>(store.publishRoots(std::span(&sourceRoot, 1), {}, LogSequenceNumber(3)));

	SlottedPagePoolAllocationRequest destinationRequest = sourceRequest;
	destinationRequest.segment_id = SegmentId(72);
	destinationRequest.pool_owner_id = 78;
	destinationRequest.generation = Generation(2);
	destinationRequest.page_id = PageId(102);
	PageOwnerHeader destinationOwner = pageOwner(LogSequenceNumber(4));
	destinationOwner.page_id = destinationRequest.page_id;
	destinationOwner.generation = destinationRequest.generation;
	BTreePageHeader destinationHeader;
	destinationHeader.index_id = destinationOwner.owner_id;
	destinationHeader.page_id = destinationOwner.page_id;
	destinationHeader.generation = destinationOwner.generation;
	ASSERT_TRUE(store.allocate(destinationRequest, BTreePage::create(destinationOwner, destinationHeader),
							   PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const BTreePage root = store.read(oldSnapshot, sourceRequest.page_id);
	const SharedPagePoolAllocation moved = store.evacuate(root, destinationRequest.segment_id, LogSequenceNumber(5));
	EXPECT_EQ(moved.location.segment_id, destinationRequest.segment_id);
	EXPECT_EQ(moved.location.generation, destinationRequest.generation);
	EXPECT_EQ(store.read(oldSnapshot, sourceRequest.page_id).header().generation, Generation(1));
	EXPECT_EQ(metadata->findIndexRoot(oldSnapshot, 17), sourceRoot);

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation reboundRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(reboundRoot.root_page_id, sourceRequest.page_id);
	EXPECT_EQ(reboundRoot.root_generation, destinationRequest.generation);
	EXPECT_EQ(metadata->findPageLocation(current, sourceRequest.page_id).segment_id, destinationRequest.segment_id);
	EXPECT_EQ(store.read(current, sourceRequest.page_id).header().generation, destinationRequest.generation);
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_EQ(scanner.scan().page_locations, 2U);
	expectFormatError(
			[&] { static_cast<void>(store.evacuate(root, destinationRequest.segment_id, LogSequenceNumber(6))); },
			FormatError::FERR_INVALID_STATE);
	oldSnapshot.release();
	current.release();
	metadata.reset();

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17).root_generation, destinationRequest.generation);
	EXPECT_EQ(reopenedStore.read(reopened, sourceRequest.page_id).header().generation, destinationRequest.generation);
}

TEST_F(RelocatableBTreePageStoreTest, EvacuatesANonRootPageWithoutChangingTheIndexRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = allocationRequest();
	ASSERT_TRUE(store.allocate(sourceRequest, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{17, sourceRequest.page_id, sourceRequest.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	SlottedPagePoolAppendRequest appendRequest;
	appendRequest.segment_id = sourceRequest.segment_id;
	appendRequest.generation = sourceRequest.generation;
	appendRequest.page_id = PageId(102);
	appendRequest.page_owner_id = 17;
	appendRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	ASSERT_NO_THROW(
			static_cast<void>(store.append(appendRequest, appendedPage(LogSequenceNumber(4)), LogSequenceNumber(4))));

	SlottedPagePoolAllocationRequest destinationRequest = sourceRequest;
	destinationRequest.segment_id = SegmentId(72);
	destinationRequest.pool_owner_id = 78;
	destinationRequest.generation = Generation(2);
	destinationRequest.page_id = PageId(200);
	PageOwnerHeader destinationOwner = pageOwner(LogSequenceNumber(5));
	destinationOwner.page_id = destinationRequest.page_id;
	destinationOwner.generation = destinationRequest.generation;
	BTreePageHeader destinationHeader;
	destinationHeader.index_id = destinationOwner.owner_id;
	destinationHeader.page_id = destinationOwner.page_id;
	destinationHeader.generation = destinationOwner.generation;
	ASSERT_TRUE(store.allocate(destinationRequest, BTreePage::create(destinationOwner, destinationHeader),
							   PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(5))
						.has_value());

	MetadataSnapshotPin before = metadata->openSnapshot();
	const BTreePage leaf = store.read(before, appendRequest.page_id);
	static_cast<void>(store.evacuate(leaf, destinationRequest.segment_id, LogSequenceNumber(6)));
	EXPECT_EQ(metadata->findIndexRoot(before, 17), root);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, 17), root);
	EXPECT_EQ(metadata->findPageLocation(current, appendRequest.page_id).segment_id, destinationRequest.segment_id);
	EXPECT_EQ(store.read(current, appendRequest.page_id).header().generation, destinationRequest.generation);
	expectFormatError([&] { static_cast<void>(store.evacuate(leaf, {}, LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreePageStoreTest, EvacuatesAnUnrootedPageAndRejectsAnInvalidCheckpointLsn) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = allocationRequest();
	ASSERT_TRUE(store.allocate(sourceRequest, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());

	SlottedPagePoolAllocationRequest destinationRequest = sourceRequest;
	destinationRequest.segment_id = SegmentId(72);
	destinationRequest.pool_owner_id = 78;
	destinationRequest.generation = Generation(2);
	destinationRequest.page_id = PageId(102);
	PageOwnerHeader destinationOwner = pageOwner(LogSequenceNumber(3));
	destinationOwner.page_id = destinationRequest.page_id;
	destinationOwner.generation = destinationRequest.generation;
	BTreePageHeader destinationHeader;
	destinationHeader.index_id = destinationOwner.owner_id;
	destinationHeader.page_id = destinationOwner.page_id;
	destinationHeader.generation = destinationOwner.generation;
	ASSERT_TRUE(store.allocate(destinationRequest, BTreePage::create(destinationOwner, destinationHeader),
							   PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());

	MetadataSnapshotPin sourceSnapshot = metadata->openSnapshot();
	const BTreePage source = store.read(sourceSnapshot, sourceRequest.page_id);
	expectFormatError(
			[&] { static_cast<void>(store.evacuate(source, destinationRequest.segment_id, LogSequenceNumber{})); },
			FormatError::FERR_INVALID_HEADER);
	static_cast<void>(store.evacuate(source, destinationRequest.segment_id, LogSequenceNumber(4)));
	EXPECT_FALSE(metadata->findIndexRootOptional(sourceSnapshot, source.header().index_id).has_value());
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, sourceRequest.page_id).segment_id, destinationRequest.segment_id);
}

TEST_F(RelocatableBTreePageStoreTest, UpsertsAnUnsplitRootLeafThroughSnapshotSafeFrameCow) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	MetadataSnapshotPin before = metadata->openSnapshot();
	static_cast<void>(store.upsertRootLeaf(17, std::vector<std::byte>{std::byte{'a'}}, 1, LogSequenceNumber(4)));
	EXPECT_TRUE(store.read(before, request.page_id).entries().empty());
	MetadataSnapshotPin current = metadata->openSnapshot();
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
	EXPECT_EQ(metadata->findIndexRoot(current, 17), root);
	before.release();
	current.release();

	static_cast<void>(store.upsertRootLeaf(17, std::vector<std::byte>{std::byte{'b'}}, 2, LogSequenceNumber(5)));
	static_cast<void>(store.upsertRootLeaf(17, std::vector<std::byte>{std::byte{'a'}}, 3, LogSequenceNumber(6)));
	MetadataSnapshotPin updated = metadata->openSnapshot();
	EXPECT_EQ(index.find(updated, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(3));
	EXPECT_EQ(index.find(updated, std::vector<std::byte>{std::byte{'b'}}), std::optional<uint64_t>(2));
	EXPECT_EQ(store.read(updated, request.page_id).entries().size(), 2U);
}

TEST_F(RelocatableBTreePageStoreTest, PlansAnUnsplitRootLeafUpsertWithoutPublishingIt) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{17U, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeRootLeafMutationPlan plan =
			store.planRootLeafUpsert(snapshot, 17U, std::vector<std::byte>{std::byte{'a'}}, 1U);
	EXPECT_EQ(plan.expected_root, root);
	EXPECT_EQ(plan.expected_location.page_id, request.page_id);
	EXPECT_EQ(plan.expected_pool.segment_id, request.segment_id);
	EXPECT_EQ(plan.expected_owner_lsn, LogSequenceNumber(2));
	EXPECT_FALSE(plan.replacement_payload.empty());
	EXPECT_LT(plan.replacement_free_bytes, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	EXPECT_TRUE(store.read(snapshot, request.page_id).entries().empty());
	snapshot.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_TRUE(store.read(current, request.page_id).entries().empty());
	EXPECT_EQ(metadata->findIndexRoot(current, 17U), root);
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, PlansAnUnsplitRootLeafEraseWithoutPublishingIt) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation root{17U, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	static_cast<void>(store.upsertRootLeaf(17U, std::vector<std::byte>{std::byte{'a'}}, 1U, LogSequenceNumber(4)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeRootLeafMutationPlan plan =
			store.planRootLeafErase(snapshot, 17U, std::vector<std::byte>{std::byte{'a'}});
	EXPECT_EQ(plan.expected_root, root);
	EXPECT_EQ(plan.expected_location.page_id, request.page_id);
	EXPECT_EQ(plan.expected_pool.segment_id, request.segment_id);
	EXPECT_EQ(plan.expected_owner_lsn, LogSequenceNumber(4));
	EXPECT_FALSE(plan.replacement_payload.empty());
	EXPECT_EQ(store.read(snapshot, request.page_id).entries().size(), 1U);
	expectFormatError(
			[&] { static_cast<void>(store.planRootLeafErase(snapshot, 17U, std::vector<std::byte>{std::byte{'b'}})); },
			FormatError::FERR_INVALID_STATE);
	snapshot.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	ASSERT_EQ(store.read(current, request.page_id).entries().size(), 1U);
	EXPECT_EQ(metadata->findIndexRoot(current, 17U), root);
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, ErasesRootLeafEntriesThroughSnapshotSafeFrameCow) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> firstKey = {std::byte{'a'}};
	const std::vector<std::byte> secondKey = {std::byte{'b'}};
	static_cast<void>(store.upsertRootLeaf(17, firstKey, 1, LogSequenceNumber(4)));
	static_cast<void>(store.upsertRootLeaf(17, secondKey, 2, LogSequenceNumber(5)));

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeErase = metadata->openSnapshot();
	ASSERT_TRUE(store.erase(17, firstKey, LogSequenceNumber(6)));
	EXPECT_EQ(index.find(beforeErase, firstKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(beforeErase, secondKey), std::optional<uint64_t>(2));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(index.find(current, firstKey).has_value());
	EXPECT_EQ(index.find(current, secondKey), std::optional<uint64_t>(2));
	EXPECT_FALSE(store.erase(17, firstKey, LogSequenceNumber(7)));
	beforeErase.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, MergesAnEmptyLeafAndCollapsesTheRootWithoutBreakingSnapshots) {
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

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeErase = metadata->openSnapshot();
	ASSERT_TRUE(store.erase(17, leftKey, LogSequenceNumber(6)));
	EXPECT_EQ(metadata->findIndexRoot(beforeErase, 17).root_page_id, PageId(103));
	EXPECT_EQ(index.find(beforeErase, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(beforeErase, rightKey), std::optional<uint64_t>(2));

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation collapsedRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(collapsedRoot.root_page_id, PageId(102));
	EXPECT_EQ(store.read(current, collapsedRoot.root_page_id).header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(101)).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(103)).has_value());
	EXPECT_FALSE(index.find(current, leftKey).has_value());
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(2));
	beforeErase.release();
	current.release();
	pool.reclaimRetiredFrames(request.segment_id);
}

TEST_F(RelocatableBTreePageStoreTest, RecoversLeafMergeAndRootCollapseAfterDurableWalHeader) {
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
		const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
		const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
		static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
		static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(5)));

		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.erase(17, leftKey, LogSequenceNumber(6), options)),
					 BootstrapPublishInterrupted);
	}

	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17);
	EXPECT_EQ(root.root_page_id, PageId(102));
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(101)).has_value());
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(103)).has_value());
	EXPECT_FALSE(index.find(snapshot, largeKey(std::byte{'a'})).has_value());
	EXPECT_EQ(index.find(snapshot, largeKey(std::byte{'b'})), std::optional<uint64_t>(2));
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidRootLeafUpsertsBeforePublishingAFrame) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.upsertRootLeaf(0, std::vector<std::byte>{std::byte{'a'}}, 1, LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);

	const SlottedPagePoolAllocationRequest request = allocationRequest();
	BTreePageHeader splitRootHeader;
	splitRootHeader.index_id = request.page_owner_id;
	splitRootHeader.page_id = request.page_id;
	splitRootHeader.generation = request.generation;
	splitRootHeader.right_sibling = PageId(102);
	ASSERT_TRUE(store.allocate(request, BTreePage::create(pageOwner(LogSequenceNumber(2)), splitRootHeader),
							   PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	expectFormatError(
			[&] {
				static_cast<void>(
						store.upsertRootLeaf(17, std::vector<std::byte>{std::byte{'a'}}, 1, LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(store.read(snapshot, request.page_id).entries().empty());
}

TEST_F(RelocatableBTreePageStoreTest, RejectsARootLeafSplitWhenOneKeyCannotFitInEitherLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation root{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> oversizedKey(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{'x'});
	expectFormatError(
			[&] { static_cast<void>(store.upsertRootLeaf(17, oversizedKey, 1, LogSequenceNumber(4))); },
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, 17), root);
	EXPECT_TRUE(store.read(current, request.page_id).entries().empty());
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, SplitsAnOverfullRootLeafThroughOneSnapshotSafePublication) {
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
	MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(index.find(beforeSplit, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(beforeSplit, rightKey).has_value());

	const SharedPagePoolAllocation published = store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(5));
	EXPECT_EQ(published.location.page_id, PageId(103));
	EXPECT_EQ(published.owner.committed_lsn, LogSequenceNumber(5));
	EXPECT_EQ(metadata->findIndexRoot(beforeSplit, 17), initialRoot);
	EXPECT_EQ(index.find(beforeSplit, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(beforeSplit, rightKey).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation currentRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(currentRoot.root_page_id, PageId(103));
	EXPECT_EQ(currentRoot.root_generation, request.generation);
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(2));
	const BTreePage root = store.read(current, currentRoot.root_page_id);
	EXPECT_EQ(root.header().kind, BTreeNodeKind::BPN_INTERNAL);
	EXPECT_EQ(root.header().level, 1U);
	EXPECT_EQ(root.header().leftmost_child, request.page_id);
	ASSERT_EQ(root.entries().size(), 1U);
	EXPECT_EQ(root.entries().front().key, rightKey);
	EXPECT_EQ(root.entries().front().value_or_child, PageId(102).value());
	const BTreePage left = store.read(current, request.page_id);
	const BTreePage right = store.read(current, PageId(102));
	EXPECT_EQ(left.header().right_sibling, PageId(102));
	EXPECT_EQ(right.header().left_sibling, request.page_id);
	EXPECT_EQ(left.entries().front().key, leftKey);
	EXPECT_EQ(right.entries().front().key, rightKey);
	beforeSplit.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, SplitsARootLeafIntoABestFitExternalIndexPool) {
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
	pool.reclaimRetiredFrames(request.segment_id);

	SlottedPagePoolAllocationRequest destination = allocationRequest();
	destination.segment_id = SegmentId(72);
	destination.pool_owner_id = 78;
	destination.page_id = PageId(200);
	ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, LogSequenceNumber(5)), PhysicalSlotId(41),
							   PhysicalSlotId(41), LogSequenceNumber(5))
						.has_value());
	uint64_t nextLsn = 6;
	uint64_t fillerPageId = 201;
	while (true) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const uint8_t availableFrames = pool.availableFrameCount(snapshot, request.segment_id);
		snapshot.release();
		if (availableFrames <= 1U) {
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
	MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
	EXPECT_EQ(index.find(beforeSplit, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(beforeSplit, rightKey).has_value());
	static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(nextLsn++)));
	EXPECT_EQ(metadata->findIndexRoot(beforeSplit, 17), initialRoot);
	EXPECT_FALSE(index.find(beforeSplit, rightKey).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(metadata->findPageLocation(current, root.root_page_id).segment_id, destination.segment_id);
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(2));
	beforeSplit.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, ChoosesTheLowestSurplusEligiblePoolForARootLeafSplit) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest source = allocationRequest();
	ASSERT_TRUE(store.allocate(source, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
						   LogSequenceNumber(2))
					.has_value());
	const IndexRootLocation initialRoot{17, source.page_id, source.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));
	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
	static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
	pool.reclaimRetiredFrames(source.segment_id);

	SlottedPagePoolAllocationRequest destination = allocationRequest();
	destination.segment_id = SegmentId(72);
	destination.pool_owner_id = 78;
	destination.page_id = PageId(200);
	ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, LogSequenceNumber(5)), PhysicalSlotId(41),
						   PhysicalSlotId(41), LogSequenceNumber(5))
					.has_value());
	uint64_t nextLsn = 6;
	uint64_t fillerPageId = 201;
	while (true) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const uint8_t availableFrames = pool.availableFrameCount(snapshot, destination.segment_id);
		snapshot.release();
		if (availableFrames <= 2U) {
			break;
		}
		SlottedPagePoolAppendRequest filler;
		filler.segment_id = destination.segment_id;
		filler.generation = destination.generation;
		filler.page_id = PageId(fillerPageId++);
		filler.page_owner_id = destination.page_owner_id;
		filler.page_owner_kind = destination.page_owner_kind;
		const LogSequenceNumber fillerLsn(nextLsn++);
		static_cast<void>(store.append(filler, appendedPage(filler.page_id, fillerLsn), fillerLsn));
	}

	static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(nextLsn++)));
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(metadata->findPageLocation(current, root.root_page_id).segment_id, destination.segment_id);
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(2));
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, SplitsARootLeafIntoANewAtomicallyReservedIndexPool) {
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
	pool.reclaimRetiredFrames(request.segment_id);

	uint64_t nextLsn = 5;
	uint64_t fillerPageId = 200;
	while (true) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const uint8_t availableFrames = pool.availableFrameCount(snapshot, request.segment_id);
		snapshot.release();
		if (availableFrames <= 1U) {
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
	MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
	EXPECT_EQ(index.find(beforeSplit, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(beforeSplit, rightKey).has_value());
	const IndexPagePoolGrowthOptions growth{PhysicalSlotId(41), PhysicalSlotId(41), 17};
	static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, growth, LogSequenceNumber(nextLsn++)));
	EXPECT_EQ(metadata->findIndexRoot(beforeSplit, 17), initialRoot);
	EXPECT_FALSE(index.find(beforeSplit, rightKey).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	const PageLocation rootLocation = metadata->findPageLocation(current, root.root_page_id);
	EXPECT_EQ(rootLocation.segment_id, SegmentId(72));
	EXPECT_EQ(rootLocation.generation, Generation(1));
	const CatalogEntry createdPool = metadata->findCatalog(current, rootLocation.segment_id);
	EXPECT_EQ(createdPool.physical_slot, PhysicalSlotId(41));
	EXPECT_EQ(createdPool.kind, SegmentKind::SGK_INDEX_PAGE_POOL);
	EXPECT_EQ(createdPool.owner_id, 17U);
	EXPECT_EQ(current.superblock().next_logical_segment_id, 73U);
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(2));
	beforeSplit.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17).root_page_id, root.root_page_id);
	EXPECT_EQ(reopenedIndex.find(reopened, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(reopenedIndex.find(reopened, rightKey), std::optional<uint64_t>(2));
}

TEST_F(RelocatableBTreePageStoreTest, ReclaimsAnUnpublishedNewIndexPoolAfterAnUndurableWalFailure) {
	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
	const IndexPagePoolGrowthOptions growth{PhysicalSlotId(41), PhysicalSlotId(41), 17};
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
		static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
		pool.reclaimRetiredFrames(request.segment_id);

		uint64_t fillerPageId = 200;
		uint64_t nextLsn = 5;
		while (true) {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint8_t availableFrames = pool.availableFrameCount(snapshot, request.segment_id);
			snapshot.release();
			if (availableFrames <= 1U) {
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

		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(
				static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, growth, LogSequenceNumber(nextLsn), options)),
				BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin beforeRetry = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(beforeRetry, 17).root_page_id, PageId(101));
	EXPECT_EQ(beforeRetry.superblock().next_logical_segment_id, 72U);
	EXPECT_EQ(reopenedIndex.find(beforeRetry, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(reopenedIndex.find(beforeRetry, rightKey).has_value());
	beforeRetry.release();

	static_cast<void>(reopenedStore.upsertRootLeaf(17, rightKey, 2, growth, LogSequenceNumber(1'000)));
	MetadataSnapshotPin current = reopenedMetadata->openSnapshot();
	const IndexRootLocation root = reopenedMetadata->findIndexRoot(current, 17);
	EXPECT_EQ(reopenedMetadata->findPageLocation(current, root.root_page_id).segment_id, SegmentId(72));
	EXPECT_EQ(current.superblock().next_logical_segment_id, 73U);
	EXPECT_EQ(reopenedIndex.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(reopenedIndex.find(current, rightKey), std::optional<uint64_t>(2));
}

TEST_F(RelocatableBTreePageStoreTest, RecoversANewIndexPoolAfterTheWalHeaderIsDurable) {
	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
	const IndexPagePoolGrowthOptions growth{PhysicalSlotId(41), PhysicalSlotId(41), 17};
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
		static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
		pool.reclaimRetiredFrames(request.segment_id);

		uint64_t fillerPageId = 200;
		uint64_t nextLsn = 5;
		while (true) {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint8_t availableFrames = pool.availableFrameCount(snapshot, request.segment_id);
			snapshot.release();
			if (availableFrames <= 1U) {
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

		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(
				static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, growth, LogSequenceNumber(nextLsn), options)),
				BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	const IndexRootLocation root = reopenedMetadata->findIndexRoot(reopened, 17);
	EXPECT_EQ(reopenedMetadata->findPageLocation(reopened, root.root_page_id).segment_id, SegmentId(72));
	EXPECT_EQ(reopened.superblock().next_logical_segment_id, 73U);
	EXPECT_EQ(reopenedIndex.find(reopened, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(reopenedIndex.find(reopened, rightKey), std::optional<uint64_t>(2));
}

TEST_F(RelocatableBTreePageStoreTest, ElevatesAnOverfullLevelOneRootWithoutChangingExistingPageIds) {
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
	static_cast<void>(store.upsertRootLeaf(17, aKey, 1, LogSequenceNumber(4)));
	static_cast<void>(store.upsertRootLeaf(17, bKey, 2, LogSequenceNumber(5)));
	MetadataSnapshotPin beforeElevation = metadata->openSnapshot();
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(metadata->findIndexRoot(beforeElevation, 17).root_page_id, PageId(103));
	EXPECT_EQ(index.find(beforeElevation, aKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(beforeElevation, bKey), std::optional<uint64_t>(2));
	EXPECT_FALSE(index.find(beforeElevation, cKey).has_value());

	const SharedPagePoolAllocation published = store.upsert(17, cKey, 3, LogSequenceNumber(6));
	EXPECT_EQ(published.location.page_id, PageId(106));
	EXPECT_EQ(metadata->findIndexRoot(beforeElevation, 17).root_page_id, PageId(103));
	EXPECT_FALSE(index.find(beforeElevation, cKey).has_value());
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(root.root_page_id, PageId(106));
	EXPECT_EQ(index.find(current, aKey), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(current, bKey), std::optional<uint64_t>(2));
	EXPECT_EQ(index.find(current, cKey), std::optional<uint64_t>(3));
	const BTreePage newRoot = store.read(current, root.root_page_id);
	EXPECT_EQ(newRoot.header().kind, BTreeNodeKind::BPN_INTERNAL);
	EXPECT_EQ(newRoot.header().level, 2U);
	EXPECT_EQ(newRoot.header().leftmost_child, PageId(103));
	ASSERT_EQ(newRoot.entries().size(), 1U);
	EXPECT_EQ(newRoot.entries().front().key, bKey);
	EXPECT_EQ(newRoot.entries().front().value_or_child, PageId(105).value());
	const BTreePage leftInternal = store.read(current, PageId(103));
	const BTreePage rightInternal = store.read(current, PageId(105));
	EXPECT_EQ(leftInternal.header().level, 1U);
	EXPECT_EQ(leftInternal.header().leftmost_child, request.page_id);
	EXPECT_TRUE(leftInternal.entries().empty());
	EXPECT_EQ(rightInternal.header().level, 1U);
	EXPECT_EQ(rightInternal.header().leftmost_child, PageId(102));
	ASSERT_EQ(rightInternal.entries().size(), 1U);
	EXPECT_EQ(rightInternal.entries().front().key, cKey);
	EXPECT_EQ(rightInternal.entries().front().value_or_child, PageId(104).value());
	static_cast<void>(store.upsert(17, aKey, 11, LogSequenceNumber(7)));
	EXPECT_EQ(index.find(current, aKey), std::optional<uint64_t>(1));
	MetadataSnapshotPin updated = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(updated, 17).root_page_id, PageId(106));
	EXPECT_EQ(index.find(updated, aKey), std::optional<uint64_t>(11));
	EXPECT_EQ(index.find(updated, bKey), std::optional<uint64_t>(2));
	EXPECT_EQ(index.find(updated, cKey), std::optional<uint64_t>(3));
	beforeElevation.release();
	current.release();
	updated.release();
}

TEST_F(RelocatableBTreePageStoreTest, SplitsALeafBelowALevelTwoRootWithOneSnapshotSafePublication) {
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
	const auto upsertAndReclaim = [&](uint16_t key, uint64_t value) {
		static_cast<void>(store.upsert(17, wideKey(key), value, LogSequenceNumber(nextLsn++)));
		pool.reclaimRetiredFrames(request.segment_id);
	};
	for (uint16_t key = 100; key <= 120; key += 2U) {
		upsertAndReclaim(key, key);
	}

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
	const IndexRootLocation beforeRoot = metadata->findIndexRoot(beforeSplit, 17);
	const BTreePage beforeRootPage = store.read(beforeSplit, beforeRoot.root_page_id);
	ASSERT_EQ(beforeRootPage.header().kind, BTreeNodeKind::BPN_INTERNAL);
	ASSERT_EQ(beforeRootPage.header().level, 2U);
	EXPECT_EQ(index.find(beforeSplit, wideKey(120)), std::optional<uint64_t>(120));
	EXPECT_FALSE(index.find(beforeSplit, wideKey(122)).has_value());

	static_cast<void>(store.upsert(17, wideKey(122), 122, LogSequenceNumber(nextLsn++)));
	EXPECT_EQ(metadata->findIndexRoot(beforeSplit, 17), beforeRoot);
	EXPECT_EQ(index.find(beforeSplit, wideKey(120)), std::optional<uint64_t>(120));
	EXPECT_FALSE(index.find(beforeSplit, wideKey(122)).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation currentRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(currentRoot.root_page_id, beforeRoot.root_page_id);
	EXPECT_EQ(currentRoot.root_generation, beforeRoot.root_generation);
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	const BTreePage root = store.read(current, currentRoot.root_page_id);
	ASSERT_EQ(root.header().level, 2U);
	ASSERT_FALSE(root.entries().empty());
	const PageId rightParentId(root.entries().back().value_or_child);
	const BTreePage rightParent = store.read(current, rightParentId);
	ASSERT_EQ(rightParent.header().level, 1U);
	ASSERT_EQ(rightParent.entries().size(), 3U);
	const PageId newRightLeafId(rightParent.entries().back().value_or_child);
	const BTreePage newRightLeaf = store.read(current, newRightLeafId);
	EXPECT_EQ(rightParent.entries().back().key, newRightLeaf.entries().front().key);
	EXPECT_EQ(newRightLeaf.header().left_sibling, PageId(rightParent.entries()[1].value_or_child));
	EXPECT_FALSE(newRightLeaf.header().right_sibling.isValid());
	EXPECT_EQ(newRightLeaf.entries().back().key, wideKey(122));
	beforeSplit.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RecoversALevelTwoLeafSplitAfterTheWalHeaderIsDurable) {
	PageId stableRootId;
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
		MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
		stableRootId = metadata->findIndexRoot(beforeSplit, 17).root_page_id;
		ASSERT_EQ(store.read(beforeSplit, stableRootId).header().level, 2U);
		beforeSplit.release();

		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.upsert(17, wideKey(122), 122, LogSequenceNumber(nextLsn), options)),
					 BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	const IndexRootLocation reopenedRoot = reopenedMetadata->findIndexRoot(reopened, 17);
	EXPECT_EQ(reopenedRoot.root_page_id, stableRootId);
	EXPECT_EQ(reopenedStore.read(reopened, reopenedRoot.root_page_id).header().level, 2U);
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(reopenedIndex.find(reopened, wideKey(key)), std::optional<uint64_t>(key));
	}
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest, PropagatesALevelTwoLeafSplitThroughAnOverfullParentAtomically) {
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
	const auto upsertAndReclaim = [&](uint16_t key) {
		static_cast<void>(store.upsert(17, wideKey(key), key, LogSequenceNumber(nextLsn++)));
		pool.reclaimRetiredFrames(request.segment_id);
	};
	for (uint16_t key = 100; key <= 124; key += 2U) {
		upsertAndReclaim(key);
	}

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforePropagation = metadata->openSnapshot();
	const IndexRootLocation beforeRoot = metadata->findIndexRoot(beforePropagation, 17);
	ASSERT_EQ(store.read(beforePropagation, beforeRoot.root_page_id).header().level, 2U);
	EXPECT_EQ(index.find(beforePropagation, wideKey(124)), std::optional<uint64_t>(124));
	EXPECT_FALSE(index.find(beforePropagation, wideKey(126)).has_value());

	static_cast<void>(store.upsert(17, wideKey(126), 126, LogSequenceNumber(nextLsn++)));
	EXPECT_EQ(metadata->findIndexRoot(beforePropagation, 17), beforeRoot);
	EXPECT_FALSE(index.find(beforePropagation, wideKey(126)).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation currentRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(currentRoot.root_page_id, beforeRoot.root_page_id);
	EXPECT_EQ(currentRoot.root_generation, beforeRoot.root_generation);
	const BTreePage root = store.read(current, currentRoot.root_page_id);
	ASSERT_EQ(root.header().level, 2U);
	ASSERT_EQ(root.entries().size(), 2U);
	for (uint16_t key = 100; key <= 126; key += 2U) {
		EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	const BTreePage rightInternal = store.read(current, PageId(root.entries().back().value_or_child));
	EXPECT_EQ(rightInternal.header().level, 1U);
	EXPECT_TRUE(rightInternal.header().leftmost_child.isValid());
	ASSERT_FALSE(rightInternal.entries().empty());
	EXPECT_TRUE(PageId(rightInternal.entries().back().value_or_child).isValid());
	beforePropagation.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, ElevatesALevelTwoRootThroughOneAtomicAncestorPathMutation) {
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
	uint16_t lastKey = 0;
	bool elevated = false;
	MetadataSnapshotPin beforeElevation;
	IndexRootLocation beforeRoot;
	uint16_t keyAtSnapshot = 0;
	for (uint16_t key = 100; key <= 250; key += 2U) {
		static_cast<void>(store.upsert(17, wideKey(key), key, LogSequenceNumber(nextLsn++)));
		pool.reclaimRetiredFrames(request.segment_id);
		MetadataSnapshotPin current = metadata->openSnapshot();
		const IndexRootLocation rootLocation = metadata->findIndexRoot(current, 17);
		const BTreePage root = store.read(current, rootLocation.root_page_id);
		if (!beforeElevation.isValid() && root.header().level == 2U && root.entries().size() == 3U) {
			beforeElevation = std::move(current);
			beforeRoot = rootLocation;
			keyAtSnapshot = key;
		} else {
			current.release();
		}
		lastKey = key;
		if (root.header().level == 3U) {
			elevated = true;
			break;
		}
	}
	ASSERT_TRUE(elevated);
	ASSERT_TRUE(beforeElevation.isValid());

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation rootLocation = metadata->findIndexRoot(current, 17);
	const BTreePage root = store.read(current, rootLocation.root_page_id);
	EXPECT_EQ(root.header().kind, BTreeNodeKind::BPN_INTERNAL);
	EXPECT_EQ(root.header().level, 3U);
	EXPECT_TRUE(root.header().leftmost_child.isValid());
	ASSERT_FALSE(root.entries().empty());
	EXPECT_EQ(metadata->findIndexRoot(beforeElevation, 17), beforeRoot);
	EXPECT_EQ(index.find(beforeElevation, wideKey(keyAtSnapshot)), std::optional<uint64_t>(keyAtSnapshot));
	EXPECT_FALSE(index.find(beforeElevation, wideKey(lastKey)).has_value());
	for (uint16_t key = 100; key <= lastKey; key += 2U) {
		EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	static_cast<void>(store.upsert(17, wideKey(100), 10'100, LogSequenceNumber(nextLsn++)));
	EXPECT_EQ(index.find(current, wideKey(100)), std::optional<uint64_t>(100));
	beforeElevation.release();
	current.release();
	MetadataSnapshotPin updated = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(updated, 17).root_page_id, rootLocation.root_page_id);
	EXPECT_EQ(index.find(updated, wideKey(100)), std::optional<uint64_t>(10'100));
	updated.release();
}

TEST_F(RelocatableBTreePageStoreTest, MatchesAnOrderedReferenceModelAcrossRandomDeepInsertAndUpdateSequences) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation initialRoot{17, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1), {}, LogSequenceNumber(3)));

	std::vector<uint16_t> keys;
	for (uint16_t key = 100; key < 180; key += 2U) {
		keys.push_back(key);
	}
	std::mt19937 random(0x5A17U);
	std::shuffle(keys.begin(), keys.end(), random);
	std::map<uint16_t, uint64_t> reference;
	uint64_t nextLsn = 4;
	for (uint16_t key: keys) {
		static_cast<void>(store.upsert(17, wideKey(key), key, LogSequenceNumber(nextLsn++)));
		reference.emplace(key, key);
		pool.reclaimRetiredFrames(request.segment_id);
	}
	std::shuffle(keys.begin(), keys.end(), random);
	for (size_t index = 0; index < keys.size(); index += 3U) {
		const uint16_t key = keys[index];
		const uint64_t value = 10'000U + key;
		static_cast<void>(store.upsert(17, wideKey(key), value, LogSequenceNumber(nextLsn++)));
		reference[key] = value;
		pool.reclaimRetiredFrames(request.segment_id);
	}
	std::shuffle(keys.begin(), keys.end(), random);
	for (size_t index = 0; index < keys.size(); index += 2U) {
		const uint16_t key = keys[index];
		ASSERT_TRUE(store.erase(17, wideKey(key), LogSequenceNumber(nextLsn++)));
		reference.erase(key);
		pool.reclaimRetiredFrames(request.segment_id);
	}

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const auto &[key, value]: reference) {
		EXPECT_EQ(index.find(snapshot, wideKey(key)), std::optional<uint64_t>(value));
	}
	const std::vector<BTreePageEntry> scanned = index.scan(snapshot, std::nullopt, reference.size());
	ASSERT_EQ(scanned.size(), reference.size());
	size_t offset = 0;
	for (const auto &[key, value]: reference) {
		EXPECT_EQ(scanned[offset].key, wideKey(key));
		EXPECT_EQ(scanned[offset].value_or_child, value);
		++offset;
	}
	snapshot.release();
	while (!reference.empty()) {
		const uint16_t key = reference.begin()->first;
		ASSERT_TRUE(store.erase(17, wideKey(key), LogSequenceNumber(nextLsn++)));
		reference.erase(key);
		pool.reclaimRetiredFrames(request.segment_id);
	}
	MetadataSnapshotPin collapsed = metadata->openSnapshot();
	const IndexRootLocation collapsedRoot = metadata->findIndexRoot(collapsed, 17);
	EXPECT_EQ(store.read(collapsed, collapsedRoot.root_page_id).header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_TRUE(index.scan(collapsed, std::nullopt, 1U).empty());
	collapsed.release();
}

TEST_F(RelocatableBTreePageStoreTest, PublishesDeepSplitAcrossIndexPoolsThroughOneSnapshotSafeMutation) {
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
	SlottedPagePoolAllocationRequest destination = allocationRequest();
	destination.segment_id = SegmentId(72);
	destination.pool_owner_id = 78;
	destination.page_id = PageId(200);
	const LogSequenceNumber destinationLsn(nextLsn++);
	ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, destinationLsn), PhysicalSlotId(41),
							   PhysicalSlotId(41), destinationLsn)
						.has_value());

	MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(beforeEvacuation, 17);
	const BTreePage rootPage = store.read(beforeEvacuation, root.root_page_id);
	ASSERT_EQ(rootPage.header().level, 2U);
	const BTreePage rightParent = store.read(beforeEvacuation, PageId(rootPage.entries().back().value_or_child));
	const PageId targetLeafId = rightParent.entries().empty() ? rightParent.header().leftmost_child
															  : PageId(rightParent.entries().back().value_or_child);
	const BTreePage targetLeaf = store.read(beforeEvacuation, targetLeafId);
	ASSERT_EQ(targetLeaf.header().kind, BTreeNodeKind::BPN_LEAF);
	beforeEvacuation.release();
	static_cast<void>(
			pool.evacuatePage(targetLeaf.slottedPage(), destination.segment_id, LogSequenceNumber(nextLsn++)));

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(beforeSplit, targetLeafId).segment_id, destination.segment_id);
	EXPECT_EQ(index.find(beforeSplit, wideKey(120)), std::optional<uint64_t>(120));
	EXPECT_FALSE(index.find(beforeSplit, wideKey(122)).has_value());
	static_cast<void>(store.upsert(17, wideKey(122), 122, LogSequenceNumber(nextLsn++)));
	EXPECT_EQ(metadata->findIndexRoot(beforeSplit, 17), root);
	EXPECT_FALSE(index.find(beforeSplit, wideKey(122)).has_value());
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, 17).root_page_id, root.root_page_id);
	EXPECT_EQ(metadata->findPageLocation(current, targetLeafId).segment_id, destination.segment_id);
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	beforeSplit.release();
	current.release();
	pool.reclaimRetiredFrames(request.segment_id);
	pool.reclaimRetiredFrames(destination.segment_id);
	ASSERT_TRUE(store.erase(17, wideKey(120), LogSequenceNumber(nextLsn++)));
	MetadataSnapshotPin afterErase = metadata->openSnapshot();
	EXPECT_FALSE(index.find(afterErase, wideKey(120)).has_value());
	for (uint16_t key = 100; key <= 118; key += 2U) {
		EXPECT_EQ(index.find(afterErase, wideKey(key)), std::optional<uint64_t>(key));
	}
	EXPECT_EQ(index.find(afterErase, wideKey(122)), std::optional<uint64_t>(122));
	afterErase.release();
}

TEST_F(RelocatableBTreePageStoreTest, AppendsDeepSplitPagesToABestFitPoolWhenThePathPoolIsCapacityBound) {
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
	SlottedPagePoolAllocationRequest destination = allocationRequest();
	destination.segment_id = SegmentId(72);
	destination.pool_owner_id = 78;
	destination.page_id = PageId(200);
	const LogSequenceNumber destinationLsn(nextLsn++);
	ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, destinationLsn), PhysicalSlotId(41),
							   PhysicalSlotId(41), destinationLsn)
						.has_value());

	uint64_t fillerPageId = 201;
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
	MetadataSnapshotPin beforeSplit = metadata->openSnapshot();
	const BTreePage rootPage =
			store.read(beforeSplit, metadata->findIndexRoot(beforeSplit, request.page_owner_id).root_page_id);
	const BTreePage rightParent = store.read(beforeSplit, PageId(rootPage.entries().back().value_or_child));
	const PageId targetLeafId = rightParent.entries().empty() ? rightParent.header().leftmost_child
															  : PageId(rightParent.entries().back().value_or_child);
	EXPECT_FALSE(index.find(beforeSplit, wideKey(122)).has_value());
	static_cast<void>(store.upsert(17, wideKey(122), 122, LogSequenceNumber(nextLsn++)));
	EXPECT_FALSE(index.find(beforeSplit, wideKey(122)).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const BTreePage currentRoot =
			store.read(current, metadata->findIndexRoot(current, request.page_owner_id).root_page_id);
	const BTreePage currentRightParent = store.read(current, PageId(currentRoot.entries().back().value_or_child));
	const PageId appendedLeafId(currentRightParent.entries().back().value_or_child);
	EXPECT_EQ(metadata->findPageLocation(current, targetLeafId).segment_id, request.segment_id);
	EXPECT_EQ(metadata->findPageLocation(current, appendedLeafId).segment_id, destination.segment_id);
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	beforeSplit.release();
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RecoversMultiPoolDeepSplitAfterTheWalHeaderIsDurable) {
	PageId stableRootId;
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
		SlottedPagePoolAllocationRequest destination = allocationRequest();
		destination.segment_id = SegmentId(72);
		destination.pool_owner_id = 78;
		destination.page_id = PageId(200);
		const LogSequenceNumber destinationLsn(nextLsn++);
		ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, destinationLsn), PhysicalSlotId(41),
								   PhysicalSlotId(41), destinationLsn)
							.has_value());

		MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
		stableRootId = metadata->findIndexRoot(beforeEvacuation, 17).root_page_id;
		const BTreePage rootPage = store.read(beforeEvacuation, stableRootId);
		const BTreePage rightParent = store.read(beforeEvacuation, PageId(rootPage.entries().back().value_or_child));
		const PageId targetLeafId = rightParent.entries().empty() ? rightParent.header().leftmost_child
																  : PageId(rightParent.entries().back().value_or_child);
		const BTreePage targetLeaf = store.read(beforeEvacuation, targetLeafId);
		beforeEvacuation.release();
		static_cast<void>(
				pool.evacuatePage(targetLeaf.slottedPage(), destination.segment_id, LogSequenceNumber(nextLsn++)));

		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.upsert(17, wideKey(122), 122, LogSequenceNumber(nextLsn), options)),
					 BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17).root_page_id, stableRootId);
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(reopenedIndex.find(reopened, wideKey(key)), std::optional<uint64_t>(key));
	}
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest, ReclaimsUnpublishedMultiPoolDeepSplitFramesAfterAnUndurableWalFailure) {
	LogSequenceNumber retryLsn;
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
		SlottedPagePoolAllocationRequest destination = allocationRequest();
		destination.segment_id = SegmentId(72);
		destination.pool_owner_id = 78;
		destination.page_id = PageId(200);
		const LogSequenceNumber destinationLsn(nextLsn++);
		ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, destinationLsn), PhysicalSlotId(41),
								   PhysicalSlotId(41), destinationLsn)
							.has_value());

		MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
		const BTreePage rootPage =
				store.read(beforeEvacuation, metadata->findIndexRoot(beforeEvacuation, 17).root_page_id);
		const BTreePage rightParent = store.read(beforeEvacuation, PageId(rootPage.entries().back().value_or_child));
		const PageId targetLeafId = rightParent.entries().empty() ? rightParent.header().leftmost_child
																  : PageId(rightParent.entries().back().value_or_child);
		const BTreePage targetLeaf = store.read(beforeEvacuation, targetLeafId);
		beforeEvacuation.release();
		static_cast<void>(
				pool.evacuatePage(targetLeaf.slottedPage(), destination.segment_id, LogSequenceNumber(nextLsn++)));

		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		retryLsn = LogSequenceNumber(nextLsn);
		EXPECT_THROW(static_cast<void>(store.upsert(17, wideKey(122), 122, retryLsn, options)), BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin beforeRetry = reopenedMetadata->openSnapshot();
	EXPECT_FALSE(reopenedIndex.find(beforeRetry, wideKey(122)).has_value());
	beforeRetry.release();
	static_cast<void>(reopenedStore.upsert(17, wideKey(122), 122, retryLsn));
	MetadataSnapshotPin current = reopenedMetadata->openSnapshot();
	for (uint16_t key = 100; key <= 122; key += 2U) {
		EXPECT_EQ(reopenedIndex.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, ElevatesARootAcrossPoolsAndUsesTheBestFitAppendPool) {
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
	SlottedPagePoolAllocationRequest destination = allocationRequest();
	destination.segment_id = SegmentId(72);
	destination.pool_owner_id = 78;
	destination.page_id = PageId(200);
	const LogSequenceNumber destinationLsn(nextLsn++);
	ASSERT_TRUE(store.allocate(destination, appendedPage(destination.page_id, destinationLsn), PhysicalSlotId(41),
							   PhysicalSlotId(41), destinationLsn)
						.has_value());

	MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
	const BTreePage initialLevelTwoRoot =
			store.read(beforeEvacuation, metadata->findIndexRoot(beforeEvacuation, 17).root_page_id);
	ASSERT_EQ(initialLevelTwoRoot.header().level, 2U);
	const BTreePage initialRightParent =
			store.read(beforeEvacuation, PageId(initialLevelTwoRoot.entries().back().value_or_child));
	const PageId initialRightLeafId = initialRightParent.entries().empty()
											  ? initialRightParent.header().leftmost_child
											  : PageId(initialRightParent.entries().back().value_or_child);
	const BTreePage initialRightLeaf = store.read(beforeEvacuation, initialRightLeafId);
	beforeEvacuation.release();
	static_cast<void>(
			pool.evacuatePage(initialRightLeaf.slottedPage(), destination.segment_id, LogSequenceNumber(nextLsn++)));

	uint16_t lastKey = 120;
	bool elevated = false;
	for (uint16_t key = 122; key <= 250; key += 2U) {
		static_cast<void>(store.upsert(17, wideKey(key), key, LogSequenceNumber(nextLsn++)));
		pool.reclaimRetiredFrames(request.segment_id);
		pool.reclaimRetiredFrames(destination.segment_id);
		MetadataSnapshotPin current = metadata->openSnapshot();
		const BTreePage root = store.read(current, metadata->findIndexRoot(current, 17).root_page_id);
		current.release();
		lastKey = key;
		if (root.header().level == 3U) {
			elevated = true;
			break;
		}
	}
	ASSERT_TRUE(elevated);

	BTreeIndex index(17, store, *metadata);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(metadata->findPageLocation(current, root.root_page_id).segment_id, request.segment_id);
	EXPECT_EQ(store.read(current, root.root_page_id).header().level, 3U);
	for (uint16_t key = 100; key <= lastKey; key += 2U) {
		EXPECT_EQ(index.find(current, wideKey(key)), std::optional<uint64_t>(key));
	}
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RecoversRootLeafSplitAfterTheWalHeaderIsDurable) {
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
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(5), options)),
				 BootstrapPublishInterrupted);

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17).root_page_id, PageId(103));
	EXPECT_EQ(reopenedIndex.find(reopened, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(reopenedIndex.find(reopened, rightKey), std::optional<uint64_t>(2));
}

TEST_F(RelocatableBTreePageStoreTest, UpsertsAndSplitsLeavesBelowALevelOneRootWithoutBreakingSnapshots) {
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
	const auto splitTestKey = [](uint16_t value) {
		std::vector<std::byte> key(77U, std::byte{'k'});
		key[0] = static_cast<std::byte>(value >> 8U);
		key[1] = static_cast<std::byte>(value);
		return key;
	};
	std::map<uint16_t, uint64_t> expected;
	const auto rootSeparatorCount = [&] {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17);
		const BTreePage page = store.read(snapshot, root.root_page_id);
		const size_t count = page.entries().size();
		snapshot.release();
		return count;
	};
	const auto rootLevel = [&] {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const IndexRootLocation root = metadata->findIndexRoot(snapshot, 17);
		const uint16_t level = store.read(snapshot, root.root_page_id).header().level;
		snapshot.release();
		return level;
	};
	const auto upsertAndReclaim = [&](uint16_t key, uint64_t value) {
		static_cast<void>(store.upsert(17, splitTestKey(key), value, LogSequenceNumber(nextLsn++)));
		pool.reclaimRetiredFrames(request.segment_id);
	};

	uint16_t nextHighKey = 100U;
	for (size_t attempts = 0U; rootLevel() == 0U; ++attempts) {
		ASSERT_LT(attempts, 512U);
		static_cast<void>(store.upsertRootLeaf(17, splitTestKey(nextHighKey), nextHighKey, LogSequenceNumber(nextLsn++)));
		expected.emplace(nextHighKey, nextHighKey);
		++nextHighKey;
		pool.reclaimRetiredFrames(request.segment_id);
	}
	ASSERT_EQ(rootLevel(), 1U);
	ASSERT_EQ(rootSeparatorCount(), 1U);

	BTreeIndex index(17, store, *metadata);
	const uint16_t updatedKey = 100U;
	upsertAndReclaim(updatedKey, 10'100U);
	expected[updatedKey] = 10'100U;

	const size_t separatorsBeforeRightSplit = rootSeparatorCount();
	bool sawRightSplit = false;
	for (size_t attempts = 0U; !sawRightSplit; ++attempts) {
		ASSERT_LT(attempts, 512U);
		const uint16_t candidate = nextHighKey++;
		MetadataSnapshotPin before = metadata->openSnapshot();
		EXPECT_FALSE(index.find(before, splitTestKey(candidate)).has_value());
		upsertAndReclaim(candidate, candidate);
		expected.emplace(candidate, candidate);
		if (rootSeparatorCount() > separatorsBeforeRightSplit) {
			EXPECT_FALSE(index.find(before, splitTestKey(candidate)).has_value());
			EXPECT_EQ(index.find(before, splitTestKey(updatedKey)), std::optional<uint64_t>(10'100U));
			sawRightSplit = true;
		}
		before.release();
	}
	ASSERT_EQ(rootLevel(), 1U);
	ASSERT_GT(rootSeparatorCount(), separatorsBeforeRightSplit);

	const size_t separatorsBeforeLeftSplit = rootSeparatorCount();
	uint16_t nextLowKey = 99U;
	bool sawLeftSplit = false;
	for (size_t attempts = 0U; !sawLeftSplit; ++attempts) {
		ASSERT_LT(attempts, 512U);
		ASSERT_NE(nextLowKey, 0U);
		const uint16_t candidate = nextLowKey--;
		MetadataSnapshotPin before = metadata->openSnapshot();
		EXPECT_FALSE(index.find(before, splitTestKey(candidate)).has_value());
		upsertAndReclaim(candidate, candidate);
		expected.emplace(candidate, candidate);
		if (rootSeparatorCount() > separatorsBeforeLeftSplit) {
			EXPECT_FALSE(index.find(before, splitTestKey(candidate)).has_value());
			EXPECT_EQ(index.find(before, splitTestKey(updatedKey)), std::optional<uint64_t>(10'100U));
			sawLeftSplit = true;
		}
		before.release();
	}

	MetadataSnapshotPin current = metadata->openSnapshot();
	for (const auto &[key, value]: expected) {
		EXPECT_EQ(index.find(current, splitTestKey(key)), std::optional<uint64_t>(value));
	}
	const IndexRootLocation root = metadata->findIndexRoot(current, 17);
	const BTreePage currentRoot = store.read(current, root.root_page_id);
	ASSERT_EQ(currentRoot.header().kind, BTreeNodeKind::BPN_INTERNAL);
	ASSERT_EQ(currentRoot.header().level, 1U);
	ASSERT_GT(currentRoot.entries().size(), separatorsBeforeLeftSplit);
	std::vector<PageId> leaves{currentRoot.header().leftmost_child};
	for (const BTreePageEntry &entry: currentRoot.entries()) {
		leaves.push_back(PageId(entry.value_or_child));
	}
	for (size_t index = 0U; index < leaves.size(); ++index) {
		const BTreePage leaf = store.read(current, leaves[index]);
		ASSERT_EQ(leaf.header().kind, BTreeNodeKind::BPN_LEAF);
		ASSERT_FALSE(leaf.entries().empty());
		if (index > 0U) {
			EXPECT_EQ(currentRoot.entries()[index - 1U].key, leaf.entries().front().key);
			EXPECT_EQ(leaf.header().left_sibling, leaves[index - 1U]);
		}
		if (index + 1U < leaves.size()) {
			EXPECT_EQ(leaf.header().right_sibling, leaves[index + 1U]);
		}
	}
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, ReclaimsUnpublishedRootLeafSplitFramesAfterAnUndurableWalFailure) {
	const std::vector<std::byte> leftKey = largeKey(std::byte{'a'});
	const std::vector<std::byte> rightKey = largeKey(std::byte{'b'});
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
		static_cast<void>(store.upsertRootLeaf(17, leftKey, 1, LogSequenceNumber(4)));
		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(5), options)),
					 BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin old = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(old, 17).root_page_id, PageId(101));
	EXPECT_EQ(reopenedIndex.find(old, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(reopenedIndex.find(old, rightKey).has_value());
	old.release();

	static_cast<void>(reopenedStore.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(5)));
	MetadataSnapshotPin current = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(current, 17).root_page_id, PageId(103));
	EXPECT_EQ(reopenedIndex.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_EQ(reopenedIndex.find(current, rightKey), std::optional<uint64_t>(2));
}

TEST_F(RelocatableBTreePageStoreTest, RejectsRootLeafSplitWhenItsPoolCannotReserveEveryCowFrame) {
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
	for (uint64_t rawPageId = 102; rawPageId <= 129; ++rawPageId) {
		SlottedPagePoolAppendRequest append;
		append.segment_id = request.segment_id;
		append.generation = request.generation;
		append.page_id = PageId(rawPageId);
		append.page_owner_id = request.page_owner_id;
		append.page_owner_kind = PageOwnerKind::POK_INDEX;
		static_cast<void>(store.append(append, appendedPage(PageId(rawPageId), LogSequenceNumber(rawPageId - 97U)),
									   LogSequenceNumber(rawPageId - 97U)));
	}
	expectFormatError([&] { static_cast<void>(store.upsertRootLeaf(17, rightKey, 2, LogSequenceNumber(33))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, 17), initialRoot);
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(index.find(current, leftKey), std::optional<uint64_t>(1));
	EXPECT_FALSE(index.find(current, rightKey).has_value());
}

TEST_F(RelocatableBTreePageStoreTest, AppendsIndependentIndexPagesToAnExistingPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest first = allocationRequest();
	ASSERT_TRUE(store.allocate(first, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	SlottedPagePoolAppendRequest second;
	second.segment_id = first.segment_id;
	second.generation = first.generation;
	second.page_id = PageId(102);
	second.page_owner_id = first.page_owner_id;
	second.page_owner_kind = PageOwnerKind::POK_INDEX;
	SlottedPagePoolAppendRequest invalidAppend = second;
	invalidAppend.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	expectFormatError(
			[&] {
				static_cast<void>(
						store.append(invalidAppend, appendedPage(LogSequenceNumber(3)), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	ASSERT_EQ(store.append(second, appendedPage(LogSequenceNumber(3)), LogSequenceNumber(3)).location.page_id,
			  second.page_id);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, first.page_id).header().index_id, first.page_owner_id);
	EXPECT_EQ(store.read(snapshot, second.page_id).header().page_id, second.page_id);
}

TEST_F(RelocatableBTreePageStoreTest, PublishesSnapshotStableIndexRootsAndPreservesTheLogicalPageHighWater) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest first = allocationRequest();
	ASSERT_TRUE(store.allocate(first, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin beforeRootPublication = metadata->openSnapshot();
	const IndexRootLocation firstRoot{first.page_owner_id, first.page_id, first.generation};
	static_cast<void>(store.publishRoots(std::span(&firstRoot, 1), {}, LogSequenceNumber(3)));
	EXPECT_FALSE(metadata->findIndexRootOptional(beforeRootPublication, first.page_owner_id).has_value());
	MetadataSnapshotPin firstRootSnapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(firstRootSnapshot, first.page_owner_id), firstRoot);

	SlottedPagePoolAppendRequest second;
	second.segment_id = first.segment_id;
	second.generation = first.generation;
	second.page_id = PageId(102);
	second.page_owner_id = first.page_owner_id;
	second.page_owner_kind = PageOwnerKind::POK_INDEX;
	ASSERT_EQ(store.append(second, appendedPage(LogSequenceNumber(4)), LogSequenceNumber(4)).location.page_id,
			  second.page_id);
	const IndexRootLocation secondRoot{second.page_owner_id, second.page_id, second.generation};
	static_cast<void>(store.publishRoots(std::span(&secondRoot, 1), {}, LogSequenceNumber(5)));
	EXPECT_EQ(metadata->findIndexRoot(firstRootSnapshot, first.page_owner_id), firstRoot);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(current, first.page_owner_id), secondRoot);
	beforeRootPublication.release();
	firstRootSnapshot.release();
	current.release();

	// Both explicit page IDs advance the durable high-water mark. A fresh
	// allocation therefore never reuses either a live page or an old root.
	EXPECT_EQ(metadata->allocateLogicalPageId(LogSequenceNumber(6)), PageId(103));
	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, first.page_owner_id), secondRoot);
	reopened.release();
	EXPECT_EQ(reopenedMetadata->allocateLogicalPageId(LogSequenceNumber(7)), PageId(104));
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidOrUnavailableIndexRootTargets) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	auto foreignMetadata = RelocatableMetadataStore::open(io);
	EXPECT_THROW(static_cast<void>(BTreePageStore(pool, *foreignMetadata)), std::invalid_argument);
	expectFormatError([&] { static_cast<void>(store.publishRoots({}, {}, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation malformed{request.page_owner_id, PageId{}, request.generation};
	expectFormatError(
			[&] { static_cast<void>(store.publishRoots(std::span(&malformed, 1), {}, LogSequenceNumber(3))); },
			FormatError::FERR_INVALID_HEADER);
	const IndexRootLocation staleGeneration{request.page_owner_id, request.page_id, Generation(2)};
	expectFormatError(
			[&] { static_cast<void>(store.publishRoots(std::span(&staleGeneration, 1), {}, LogSequenceNumber(3))); },
			FormatError::FERR_INVALID_STATE);
	const IndexRootLocation absentPage{request.page_owner_id, PageId(102), request.generation};
	expectFormatError(
			[&] { static_cast<void>(store.publishRoots(std::span(&absentPage, 1), {}, LogSequenceNumber(3))); },
			FormatError::FERR_INVALID_STATE);
	SlottedPagePoolAppendRequest foreign;
	foreign.segment_id = request.segment_id;
	foreign.generation = request.generation;
	foreign.page_id = PageId(102);
	foreign.page_owner_id = 18;
	foreign.page_owner_kind = PageOwnerKind::POK_INDEX;
	static_cast<void>(store.append(foreign, foreignIndexPage(LogSequenceNumber(3)), LogSequenceNumber(3)));
	const IndexRootLocation wrongOwner{request.page_owner_id, foreign.page_id, foreign.generation};
	expectFormatError(
			[&] { static_cast<void>(store.publishRoots(std::span(&wrongOwner, 1), {}, LogSequenceNumber(4))); },
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, request.page_owner_id).has_value());
	snapshot.release();
	expectFormatError([&] { static_cast<void>(metadata->findIndexRoot(snapshot, request.page_owner_id)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(metadata->findIndexRootOptional(snapshot, request.page_owner_id)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RemovesIndexRootsWithoutMakingOldSnapshotsDangling) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(3)));
	MetadataSnapshotPin beforeDelete = metadata->openSnapshot();
	const std::array erased = {request.page_owner_id};
	static_cast<void>(store.publishRoots({}, erased, LogSequenceNumber(4)));
	EXPECT_EQ(metadata->findIndexRoot(beforeDelete, request.page_owner_id), root);
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(current, request.page_owner_id).has_value());
	expectFormatError([&] { static_cast<void>(metadata->findIndexRoot(current, request.page_owner_id)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsIncompatibleOrStalePageMaterializationBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	SlottedPagePoolAllocationRequest request = allocationRequest();
	request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	expectFormatError(
			[&] {
				static_cast<void>(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40),
												 PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	request = allocationRequest();
	expectFormatError(
			[&] {
				static_cast<void>(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40),
												 PhysicalSlotId(40), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
}
