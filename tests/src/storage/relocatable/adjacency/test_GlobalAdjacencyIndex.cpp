/**
 * @file test_GlobalAdjacencyIndex.cpp
 * @brief Snapshot range tests for the global key-only ordinary adjacency index.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

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

	PageOwnerHeader owner(PageId pageId, BTreeLeafValueLayout layout) {
		static_cast<void>(layout);
		return {pageId,
				RSS_GLOBAL_ADJACENCY_INDEX_ID,
				PageOwnerKind::POK_INDEX,
				0U,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1U),
				LogSequenceNumber(2U)};
	}

	BTreePage leaf(PageId pageId, BTreeLeafValueLayout layout) {
		BTreePageHeader header;
		header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.leaf_value_layout = layout;
		return BTreePage::create(owner(pageId, layout), header);
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
	EXPECT_EQ(first.continuation->last_edge_id, 9U);
	const GlobalAdjacencyRange second =
			index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 2U, first.continuation);
	EXPECT_EQ(second.edge_ids, (std::vector<uint64_t>{11U}));
	EXPECT_FALSE(second.continuation.has_value());
	EXPECT_EQ(index.scan(snapshot, GlobalAdjacencyDirection::GAD_IN, 7U, 8U).edge_ids,
			  (std::vector<uint64_t>{4U}));
	EXPECT_TRUE(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 99U, 8U).edge_ids.empty());
	expectFormatError(
			[&] {
				static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_IN, 7U, 2U, first.continuation));
			},
			FormatError::FERR_INVALID_STATE);
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
											 std::numeric_limits<uint64_t>::max()};
	EXPECT_TRUE(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U, exhausted).edge_ids.empty());
	const GlobalAdjacencyContinuation zeroEdge{snapshot.superblock().generation,
											 GlobalAdjacencyDirection::GAD_OUT, 7U, 0U};
	expectFormatError(
			[&] { static_cast<void>(index.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, 7U, 1U, zeroEdge)); },
			FormatError::FERR_INVALID_STATE);
}
