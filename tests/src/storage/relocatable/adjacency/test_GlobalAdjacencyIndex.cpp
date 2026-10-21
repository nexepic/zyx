/**
 * @file test_GlobalAdjacencyIndex.cpp
 * @brief Snapshot range tests for the global key-only ordinary adjacency index.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <stdexcept>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"

using namespace graph::storage;

namespace {

	Superblock superblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	PageOwnerHeader owner(PageId pageId, BTreeLeafValueLayout layout, LogSequenceNumber lsn = LogSequenceNumber(2U)) {
		static_cast<void>(layout);
		return {pageId,
				RSS_GLOBAL_ADJACENCY_INDEX_ID,
				PageOwnerKind::POK_INDEX,
				0U,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1U),
				lsn};
	}

	BTreePage leaf(PageId pageId, BTreeLeafValueLayout layout, PageId leftSibling = {}, PageId rightSibling = {},
				   LogSequenceNumber lsn = LogSequenceNumber(2U)) {
		BTreePageHeader header;
		header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.left_sibling = leftSibling;
		header.right_sibling = rightSibling;
		header.leaf_value_layout = layout;
		return BTreePage::create(owner(pageId, layout, lsn), header);
	}

	BTreePage internal(PageId pageId, PageId leftmostChild, const GlobalAdjacencyKey &separator, PageId rightChild,
					   LogSequenceNumber lsn = LogSequenceNumber(2U)) {
		BTreePageHeader header;
		header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.kind = BTreeNodeKind::BPN_INTERNAL;
		header.level = 1U;
		header.leftmost_child = leftmostChild;
		BTreePage result = BTreePage::create(owner(pageId, BTreeLeafValueLayout::BLVL_EMPTY, lsn), header);
		result.insert({AdjacencyKeyCodec::encode(separator), rightChild.value()});
		return result;
	}

	SlottedPagePoolAllocationRequest allocationRequest(PageId pageId) {
		SlottedPagePoolAllocationRequest request;
		request.segment_id = SegmentId(71U);
		request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		request.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		request.generation = Generation(1U);
		request.page_id = pageId;
		request.page_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		return request;
	}

	SlottedPagePoolAllocationRequest allocationRequestForIndex(PageId pageId, uint64_t indexId) {
		SlottedPagePoolAllocationRequest request = allocationRequest(pageId);
		request.pool_owner_id = indexId;
		request.page_owner_id = indexId;
		return request;
	}

	BTreePage leafForIndex(PageId pageId, uint64_t indexId, BTreeLeafValueLayout layout) {
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.leaf_value_layout = layout;
		const PageOwnerHeader pageOwner{pageId, indexId, PageOwnerKind::POK_INDEX, 0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)};
		return BTreePage::create(pageOwner, header);
	}

} // namespace

class GlobalAdjacencyIndexTest : public ::testing::Test {
protected:
	std::filesystem::path data_path;
	std::shared_ptr<std::fstream> data_file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		data_path = std::filesystem::temp_directory_path() /
					("global_adjacency_index_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".dat");
		data_file = std::make_shared<std::fstream>(data_path,
																			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data_file->is_open());
		io = std::make_shared<StorageIO>(data_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (data_file && data_file->is_open()) {
			data_file->close();
		}
		data_file.reset();
		std::error_code error;
		std::filesystem::remove(data_path, error);
	}
};

TEST_F(GlobalAdjacencyIndexTest, StreamsOneLogicalRangeAndResumesWithoutPhysicalState) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY);
	for (const GlobalAdjacencyKey key : {
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 7U, 3U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 7U, 9U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 7U, 11U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 8U, 1U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_IN, 7U, 4U},
		 }) {
		root.insert({AdjacencyKeyCodec::encode(key), 0U});
	}
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
									   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(3U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const GlobalAdjacencyRange first = index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U);
	EXPECT_EQ(first.edge_ids, (std::vector<uint64_t>{3U, 9U}));
	ASSERT_TRUE(first.continuation.has_value());
	EXPECT_EQ(first.continuation->generation, snapshot.superblock().generation);
	EXPECT_EQ(first.continuation->database_id, snapshot.superblock().database_id);
	EXPECT_EQ(first.continuation->last_edge_id, 9U);
	const GlobalAdjacencyRange second =
			index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U, first.continuation);
	EXPECT_EQ(second.edge_ids, (std::vector<uint64_t>{11U}));
	EXPECT_FALSE(second.continuation.has_value());
	EXPECT_EQ(index.scan(snapshot, GlobalAdjacencyDirection::GAD_IN, 7U, 8U).edge_ids,
			  (std::vector<uint64_t>{4U}));
	EXPECT_TRUE(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 99U, 8U).edge_ids.empty());

    auto foreign = *first.continuation;
    foreign.database_id = DatabaseId(snapshot.superblock().database_id.value() + 1U);
    expectFormatError([&] {
        static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U, foreign));
    }, FormatError::FERR_INVALID_STATE);
    foreign.database_id = {};
    expectFormatError([&] {
        static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U, foreign));
    }, FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_IN, 7U, 2U, first.continuation));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(GlobalAdjacencyIndexTest, VisitsSeveralSourceRangesWithOneCursor) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY);
	for (const GlobalAdjacencyKey key : {
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 7U, 3U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 7U, 9U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 8U, 4U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_OUT, 9U, 5U},
				 GlobalAdjacencyKey{GlobalAdjacencyDirection::GAD_IN, 8U, 6U},
			}) {
		root.insert({AdjacencyKeyCodec::encode(key), 0U});
	}
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
									 LogSequenceNumber(2U)).has_value());
		const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
		static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(3U)));
		GlobalAdjacencyIndex index(pages, *metadata);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::array<uint64_t, 2U> nodes{7U, 9U};
		std::vector<std::pair<uint64_t, uint64_t>> observed;
		EXPECT_TRUE(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U,
				[&observed](const uint64_t nodeId, const uint64_t edgeId) {
					observed.emplace_back(nodeId, edgeId);
					return true;
				}));
		EXPECT_EQ(observed, (std::vector<std::pair<uint64_t, uint64_t>>{{7U, 3U}, {7U, 9U}, {9U, 5U}}));
		EXPECT_FALSE(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 2U,
				[](uint64_t, uint64_t) { return true; }));
		size_t stopped = 0U;
		EXPECT_FALSE(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U,
				[&stopped](uint64_t, uint64_t) {
					++stopped;
					return false;
				}));
		EXPECT_EQ(stopped, 1U);
		const std::array<uint64_t, 2U> unsorted{9U, 7U};
		const std::array<uint64_t, 2U> duplicate{7U, 7U};
		const std::array<uint64_t, 2U> zero{0U, 7U};
		MetadataSnapshotPin invalidSnapshot;
		expectFormatError([&] {
			static_cast<void>(index.visitForNodes(invalidSnapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U,
					[](uint64_t, uint64_t) { return true; }));
		}, FormatError::FERR_INVALID_HEADER);
		for (const std::span<const uint64_t> invalidNodes: {
				std::span<const uint64_t>(unsorted), std::span<const uint64_t>(duplicate),
				std::span<const uint64_t>(zero), std::span<const uint64_t>{}}) {
			expectFormatError([&] {
				static_cast<void>(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, invalidNodes, 8U,
						[](uint64_t, uint64_t) { return true; }));
			}, FormatError::FERR_INVALID_HEADER);
		}
		expectFormatError([&] {
			static_cast<void>(index.visitForNodes(snapshot, static_cast<GlobalAdjacencyDirection>(99U), nodes, 8U,
					[](uint64_t, uint64_t) { return true; }));
		}, FormatError::FERR_INVALID_HEADER);
		expectFormatError([&] {
			static_cast<void>(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 0U,
					[](uint64_t, uint64_t) { return true; }));
		}, FormatError::FERR_INVALID_HEADER);
		expectFormatError([&] {
			static_cast<void>(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U, {}));
		}, FormatError::FERR_INVALID_HEADER);
	}

TEST_F(GlobalAdjacencyIndexTest, RejectsAValuedLeafForTheReservedAdjacencyRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_U64);
	root.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 1U});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
									   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(3U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	MetadataSnapshotPin invalidSnapshot;
	expectFormatError(
			[&] { static_cast<void>(index.scan(invalidSnapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U)); },
			FormatError::FERR_INVALID_STATE);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsForeignOwnerBeforePublishingTheReservedRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	constexpr uint64_t FOREIGN_INDEX_ID = 73U;
	BTreePage root = leafForIndex(PageId(101U), FOREIGN_INDEX_ID, BTreeLeafValueLayout::BLVL_EMPTY);
	root.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U});
	ASSERT_TRUE(pages.allocate(allocationRequestForIndex(PageId(101U), FOREIGN_INDEX_ID), root, PhysicalSlotId(40U),
								   PhysicalSlotId(40U), LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation forgedRoot{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	expectFormatError(
			[&] { static_cast<void>(pages.publishRoots(std::span(&forgedRoot, 1U), {}, LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsMalformedKeysInTheReservedRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY);
	// This is a valid range lower bound but not a complete adjacency key because
	// its encoded Edge ID is zero. It must fail after the B+Tree returns it.
	root.insert({AdjacencyKeyCodec::lowerBound(GlobalAdjacencyDirection::GAD_OUT, 7U), 0U});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
								   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(3U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsAReservedAdjacencyLeafOutsideItsSeparatorRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100U), PageId(101U),
						  {GlobalAdjacencyDirection::GAD_OUT, 7U, 5U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	BTreePage left =
			leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, {}, PageId(102U), LogSequenceNumber(3U));
	// The key routes to the right child, but the corrupted left leaf claims it.
	left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 7U}), 0U});
	BTreePage right = leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), {}, LogSequenceNumber(4U));
	right.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 9U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 right, LogSequenceNumber(4U)));
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(5U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsNonreciprocalLeafLinksOnThePublicAdjacencyReadPath) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100U), PageId(101U),
						  {GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	BTreePage left =
			leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, {}, PageId(102U), LogSequenceNumber(3U));
	left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 1U}), 0U});
	BTreePage right = leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, {}, {}, LogSequenceNumber(4U));
	right.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 right, LogSequenceNumber(4U)));
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(5U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsCyclicLeafLinksOnThePublicAdjacencyReadPath) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100U), PageId(101U),
						  {GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	BTreePage left = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(102U), PageId(102U),
							LogSequenceNumber(3U));
	left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 1U}), 0U});
	BTreePage right = leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), PageId(101U),
							 LogSequenceNumber(4U));
	right.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 right, LogSequenceNumber(4U)));
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(5U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsSiblingThatIsReciprocalButOutsideTheRoutedLeafSequence) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100U), PageId(101U),
						  {GlobalAdjacencyDirection::GAD_OUT, 7U, 4U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	// Page 103 is owned by the same Index and its key is correctly ordered, but
	// it is not a child reachable from the root. A leaf scan must not trust the
	// sibling chain as an authority independent of the routed tree.
	BTreePage left =
			leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, {}, PageId(103U), LogSequenceNumber(3U));
	left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 1U}), 0U});
	BTreePage routedRight =
			leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), {}, LogSequenceNumber(4U));
	routedRight.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 4U}), 0U});
	BTreePage foreign =
			leaf(PageId(103U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), {}, LogSequenceNumber(5U));
	foreign.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 routedRight, LogSequenceNumber(4U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(103U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 foreign, LogSequenceNumber(5U)));
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(6U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, RejectsReciprocalSiblingOutsideTheRoutedSeparatorRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100U), PageId(101U),
						  {GlobalAdjacencyDirection::GAD_OUT, 7U, 4U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	BTreePage left =
			leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, {}, PageId(102U), LogSequenceNumber(3U));
	left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 1U}), 0U});
	// Page 102 is the real right child, but its key violates the separator that
	// routes it. The scan path must validate that leaf before returning its key.
	BTreePage right =
			leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), {}, LogSequenceNumber(4U));
	right.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
								  RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX},
							 right, LogSequenceNumber(4U)));
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(5U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, PlansKeyOnlyCowMutationsAndRejectsValuedOperations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY);
	root.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
									   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(3U)));

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array operations = {
			BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_UPSERT,
								  AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 9U}), 0U},
			BTreeScalarIndexOperation{BTreeScalarIndexOperationKind::BSIOP_ERASE,
								  AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U},
	};
	const BTreeStructuralMutationPlan plan = pages.planScalarIndexTransactionMutation(
			snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, operations, false, false, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_EQ(plan.expected_root, rootLocation);
	EXPECT_TRUE(plan.stable_replacement_root.has_value());
	expectFormatError(
			[&] {
				const std::array invalid = {BTreeScalarIndexOperation{
						BTreeScalarIndexOperationKind::BSIOP_UPSERT,
						AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 9U}), 1U}};
				static_cast<void>(pages.planScalarIndexTransactionMutation(
						snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, invalid, false, false,
						BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(GlobalAdjacencyIndexTest, BoundsAndValidatesLogicalRangeCursorsBeforeReadingTheTree) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY);
	root.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
								   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(3U)));

	GlobalAdjacencyIndex index(pages, *metadata);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_TRUE(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 0U).edge_ids.empty());
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 0U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(index.scan(snapshot, static_cast<GlobalAdjacencyDirection>(99U), 7U, 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, std::numeric_limits<size_t>::max()));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);

	const GlobalAdjacencyContinuation exhausted{snapshot.superblock().generation,
											 GlobalAdjacencyDirection::GAD_OUT, 7U,
											 std::numeric_limits<uint64_t>::max(), snapshot.superblock().database_id};
	EXPECT_TRUE(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U, exhausted).edge_ids.empty());
	const GlobalAdjacencyContinuation zeroEdge{snapshot.superblock().generation,
											 GlobalAdjacencyDirection::GAD_OUT, 7U, 0U};
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U, zeroEdge)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(GlobalAdjacencyIndexTest, AdmitsIdBufferGrowthPeaksAndRetriesWithoutMaterializingForeignRanges) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY);
	for (uint64_t edgeId = 1U; edgeId <= 40U; ++edgeId) {
		root.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, edgeId}), 0U});
	}
	root.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 8U, 1U}), 0U});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
		LogSequenceNumber(2U)).has_value());
	const IndexRootLocation location{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(101U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&location, 1U), {}, LogSequenceNumber(3U)));
	GlobalAdjacencyIndex index(pages, *metadata);
	auto snapshot = metadata->openSnapshot();
	size_t peakBytes = 0U;
	EXPECT_THROW(static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 40U, {},
		[&](size_t bytes) {
			peakBytes = std::max(peakBytes, bytes);
			if (bytes > 383U) throw std::runtime_error("ID allocation peak rejected");
		})), std::runtime_error);
	// Growing from 16 to 32 IDs must admit both buffers before allocation.
	EXPECT_EQ(peakBytes, 48U * sizeof(uint64_t));
	peakBytes = 0U;
	size_t retainedBytes = 0U;
	const auto result = index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 40U, {},
		[&](size_t bytes) { peakBytes = std::max(peakBytes, bytes); retainedBytes = bytes; });
	ASSERT_EQ(result.edge_ids.size(), 40U);
	for (size_t ordinal = 0U; ordinal < 40U; ++ordinal) EXPECT_EQ(result.edge_ids[ordinal], ordinal + 1U);
	EXPECT_FALSE(result.continuation.has_value());
	EXPECT_EQ(peakBytes, 72U * sizeof(uint64_t));
	EXPECT_EQ(retainedBytes, result.edge_ids.capacity() * sizeof(uint64_t));
	// A huge logical limit must neither reserve its full size nor collect Node 8.
	const auto sparse = index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 8U, 1'000'000U);
	ASSERT_EQ(sparse.edge_ids, (std::vector<uint64_t>{1U}));
	EXPECT_LE(sparse.edge_ids.capacity(), 16U);
}

TEST_F(GlobalAdjacencyIndexTest, AdmitsLeafBoundaryKeyAlongsideIdsAndReleasesItBeforeReturn) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	auto root = internal(PageId(100U), PageId(101U),
		{GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
		LogSequenceNumber(2U)).has_value());
	auto left = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, {}, PageId(102U), LogSequenceNumber(3U));
	for (uint64_t id: {1U, 2U})
		left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, id}), 0U});
	auto right = leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), {}, LogSequenceNumber(4U));
	right.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
		RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX}, left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
		RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX}, right, LogSequenceNumber(4U)));
	const IndexRootLocation location{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&location, 1U), {}, LogSequenceNumber(5U)));
	GlobalAdjacencyIndex index(pages, *metadata);
	auto snapshot = metadata->openSnapshot();
	size_t peak = 0U;
	EXPECT_THROW(static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 3U, {},
		[&](size_t bytes) { peak = std::max(peak, bytes); if (bytes > 3U * sizeof(uint64_t))
			throw std::runtime_error("leaf boundary budget rejected"); })), std::runtime_error);
	const size_t keyBytes = AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}).size();
	EXPECT_EQ(peak, 3U * sizeof(uint64_t) + keyBytes);
	size_t retained = 0U;
	const auto admit = [&](size_t bytes) { peak = std::max(peak, bytes); retained = bytes; };
	const auto all = index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 3U, {}, admit);
	EXPECT_EQ(all.edge_ids, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_FALSE(all.continuation.has_value());
	EXPECT_EQ(retained, all.edge_ids.capacity() * sizeof(uint64_t));
	const auto first = index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U, {}, admit);
	EXPECT_EQ(first.edge_ids, (std::vector<uint64_t>{1U, 2U}));
	ASSERT_TRUE(first.continuation.has_value());
	EXPECT_EQ(retained, first.edge_ids.capacity() * sizeof(uint64_t));
	const auto last = index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U, first.continuation, admit);
	EXPECT_EQ(last.edge_ids, (std::vector<uint64_t>{3U}));
	EXPECT_FALSE(last.continuation.has_value());
	EXPECT_EQ(retained, last.edge_ids.capacity() * sizeof(uint64_t));
}

TEST_F(GlobalAdjacencyIndexTest, MultiNodeTraversalAdmitsBoundaryKeyAndRetriesAfterRejection) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	auto root = internal(PageId(100U), PageId(101U),
		{GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}, PageId(102U));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
		LogSequenceNumber(2U)).has_value());
	auto left = leaf(PageId(101U), BTreeLeafValueLayout::BLVL_EMPTY, {}, PageId(102U), LogSequenceNumber(3U));
	for (uint64_t id: {1U, 2U})
		left.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, id}), 0U});
	auto right = leaf(PageId(102U), BTreeLeafValueLayout::BLVL_EMPTY, PageId(101U), {}, LogSequenceNumber(4U));
	right.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 3U}), 0U});
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(101U),
		RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX}, left, LogSequenceNumber(3U)));
	static_cast<void>(pages.append({SegmentId(71U), Generation(1U), PageId(102U),
		RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX}, right, LogSequenceNumber(4U)));
	const IndexRootLocation location{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
	static_cast<void>(pages.publishRoots(std::span(&location, 1U), {}, LogSequenceNumber(5U)));
	GlobalAdjacencyIndex index(pages, *metadata);
	auto snapshot = metadata->openSnapshot();
	const std::array<uint64_t, 1U> nodes{7U};
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U,
		[&](uint64_t, uint64_t) { ++delivered; return true; },
		[](size_t bytes) { if (bytes != 0U) throw std::runtime_error("boundary rejected"); })), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	// A throwing checkpoint must cancel root descent before any key delivery.
	delivered = 0U;
	size_t checkpoints = 0U;
	EXPECT_THROW(static_cast<void>(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U,
		[&](uint64_t, uint64_t) { ++delivered; return true; },
		[&](size_t bytes) { ++checkpoints; EXPECT_EQ(bytes, 0U); throw std::runtime_error("cancel descent"); })),
		std::runtime_error);
	EXPECT_EQ(checkpoints, 1U);
	EXPECT_EQ(delivered, 0U);

	std::vector<uint64_t> ids;
	size_t peak = 0U;
	EXPECT_TRUE(index.visitForNodes(snapshot, GlobalAdjacencyDirection::GAD_OUT, nodes, 8U,
		[&](uint64_t node, uint64_t edge) { EXPECT_EQ(node, 7U); ids.push_back(edge); return true; },
		[&](size_t bytes) { peak = std::max(peak, bytes); }));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_GE(peak, AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, 2U}).size());
}
