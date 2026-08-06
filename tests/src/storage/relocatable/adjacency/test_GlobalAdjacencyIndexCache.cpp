/**
 * @file test_GlobalAdjacencyIndexCache.cpp
 * @brief Buffer Pool admission tests for global ordinary-graph adjacency scans.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	PageOwnerHeader owner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId,
				RSS_GLOBAL_ADJACENCY_INDEX_ID,
				PageOwnerKind::POK_INDEX,
				0U,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1U),
				lsn};
	}

	BTreePage leaf(PageId pageId, uint64_t edgeId, PageId left, PageId right, LogSequenceNumber lsn) {
		BTreePageHeader header;
		header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.left_sibling = left;
		header.right_sibling = right;
		header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
		BTreePage result = BTreePage::create(owner(pageId, lsn), header);
		result.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, edgeId}), 0U});
		return result;
	}

	BTreePage internal(PageId pageId, uint16_t level, PageId leftmost, uint64_t separatorEdgeId, PageId rightChild,
					   LogSequenceNumber lsn) {
		BTreePageHeader header;
		header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.kind = BTreeNodeKind::BPN_INTERNAL;
		header.level = level;
		header.leftmost_child = leftmost;
		BTreePage result = BTreePage::create(owner(pageId, lsn), header);
		result.insert(
				{AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 7U, separatorEdgeId}), rightChild.value()});
		return result;
	}

	SlottedPagePoolAllocationRequest allocationRequest(PageId pageId) {
		return {SegmentId(71U),
				SegmentKind::SGK_INDEX_PAGE_POOL,
				RSS_GLOBAL_ADJACENCY_INDEX_ID,
				Generation(1U),
				pageId,
				RSS_GLOBAL_ADJACENCY_INDEX_ID,
				PageOwnerKind::POK_INDEX};
	}

	SlottedPagePoolAppendRequest appendRequest(PageId pageId) {
		return {SegmentId(71U), Generation(1U), pageId, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageOwnerKind::POK_INDEX};
	}

} // namespace

class GlobalAdjacencyIndexCacheTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<SlottedPagePool> pool_;
	std::unique_ptr<BTreePageStore> pages_;
	std::unique_ptr<GlobalAdjacencyIndex> index_;
	MetadataSnapshotPin snapshot_;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("global_adjacency_cache_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".dat");
		file_ = std::make_shared<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		pool_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		pages_ = std::make_unique<BTreePageStore>(*pool_, *metadata_);

		BTreePage root = internal(PageId(100U), 2U, PageId(201U), 3U, PageId(202U), LogSequenceNumber(2U));
		ASSERT_TRUE(pages_->allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
									 LogSequenceNumber(2U))
						.has_value());
		static_cast<void>(pages_->append(appendRequest(PageId(201U)),
				internal(PageId(201U), 1U, PageId(101U), 2U, PageId(102U), LogSequenceNumber(3U)),
				LogSequenceNumber(3U)));
		static_cast<void>(pages_->append(appendRequest(PageId(202U)),
				internal(PageId(202U), 1U, PageId(103U), 4U, PageId(104U), LogSequenceNumber(4U)),
				LogSequenceNumber(4U)));

		const std::array<PageId, 4U> leaves{PageId(101U), PageId(102U), PageId(103U), PageId(104U)};
		for (size_t ordinal = 0U; ordinal < leaves.size(); ++ordinal) {
			const PageId left = ordinal == 0U ? PageId{} : leaves[ordinal - 1U];
			const PageId right = ordinal + 1U == leaves.size() ? PageId{} : leaves[ordinal + 1U];
			const LogSequenceNumber lsn(5U + ordinal);
			static_cast<void>(pages_->append(appendRequest(leaves[ordinal]),
					leaf(leaves[ordinal], ordinal + 1U, left, right, lsn), lsn));
		}
		const IndexRootLocation rootLocation{RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(100U), Generation(1U)};
		static_cast<void>(pages_->publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(9U)));
		io_->sync();
		io_->enableBufferPool(32U * RSS_PAGE_SIZE);
		index_ = std::make_unique<GlobalAdjacencyIndex>(*pages_, *metadata_);
		snapshot_ = metadata_->openSnapshot();
	}

	void TearDown() override {
		snapshot_.release();
		index_.reset();
		pages_.reset();
		pool_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST_F(GlobalAdjacencyIndexCacheTest, KeepsRoutingPagesResidentAndStreamsSupernodeLeavesTransiently) {
	io_->resetBufferPoolStats();
	const GlobalAdjacencyRange first = index_->scan(snapshot_, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U);
	EXPECT_EQ(first.edge_ids, (std::vector<uint64_t>{1U, 2U, 3U, 4U}));
	EXPECT_FALSE(first.continuation.has_value());
	const BufferPoolStats afterFirst = io_->bufferPoolStats();

	const GlobalAdjacencyRange second = index_->scan(snapshot_, GlobalAdjacencyDirection::GAD_OUT, 7U, 8U);
	EXPECT_EQ(second.edge_ids, first.edge_ids);
	EXPECT_FALSE(second.continuation.has_value());
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	// The starting leaf is admitted normally for sparse adjacency reuse. Every
	// later supernode leaf remains sequential and cannot pollute the working set.
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes + 3U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + 3U);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.hits, afterFirst.hits);
}
