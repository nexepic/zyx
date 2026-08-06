/**
 * @file test_BTreeIndex.cpp
 * @brief Snapshot-pinned traversal tests for isolated relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

using namespace graph::storage;

namespace {

	Superblock superblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	std::vector<std::byte> key(std::string_view text) {
		std::vector<std::byte> result;
		result.reserve(text.size());
		for (const char value: text) {
			result.push_back(std::byte{static_cast<unsigned char>(value)});
		}
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

	PageOwnerHeader owner(PageId pageId, uint64_t indexId, LogSequenceNumber lsn) {
		PageOwnerHeader result;
		result.page_id = pageId;
		result.owner_id = indexId;
		result.kind = PageOwnerKind::POK_INDEX;
		result.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		result.generation = Generation(1);
		result.committed_lsn = lsn;
		return result;
	}

	BTreePage leaf(PageId pageId, uint64_t indexId, LogSequenceNumber lsn, PageId left = {}, PageId right = {}) {
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1);
		header.left_sibling = left;
		header.right_sibling = right;
		return BTreePage::create(owner(pageId, indexId, lsn), header);
	}

	BTreePage internal(PageId pageId, uint64_t indexId, LogSequenceNumber lsn, PageId leftmost,
					   std::vector<std::byte> separator, PageId rightChild, uint16_t level = 1) {
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1);
		header.kind = BTreeNodeKind::BPN_INTERNAL;
		header.level = level;
		header.leftmost_child = leftmost;
		BTreePage page = BTreePage::create(owner(pageId, indexId, lsn), header);
		page.insert({std::move(separator), rightChild.value()});
		return page;
	}

	SlottedPagePoolAllocationRequest allocationRequest(PageId pageId, uint64_t indexId = 17) {
		SlottedPagePoolAllocationRequest request;
		request.segment_id = SegmentId(71);
		request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		request.pool_owner_id = 77;
		request.generation = Generation(1);
		request.page_id = pageId;
		request.page_owner_id = indexId;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		return request;
	}

	SlottedPagePoolAppendRequest appendRequest(PageId pageId, uint64_t indexId = 17) {
		SlottedPagePoolAppendRequest request;
		request.segment_id = SegmentId(71);
		request.generation = Generation(1);
		request.page_id = pageId;
		request.page_owner_id = indexId;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		return request;
	}

} // namespace

class RelocatableBTreeIndexTest : public ::testing::Test {
protected:
	std::filesystem::path data_path;
	std::filesystem::path wal_path;
	std::shared_ptr<std::fstream> data_file;
	std::shared_ptr<std::fstream> wal_file;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> wal_io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		data_path = std::filesystem::temp_directory_path() / ("relocatable_btree_index_" + to_string(uuid) + ".dat");
		wal_path = data_path.string() + ".foreign";
		data_file = std::make_shared<std::fstream>(data_path,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file = std::make_shared<std::fstream>(wal_path,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data_file->is_open());
		ASSERT_TRUE(wal_file->is_open());
		io = std::make_shared<StorageIO>(data_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io = std::make_shared<StorageIO>(wal_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		wal_io.reset();
		if (data_file && data_file->is_open()) {
			data_file->close();
		}
		if (wal_file && wal_file->is_open()) {
			wal_file->close();
		}
		data_file.reset();
		wal_file.reset();
		std::error_code error;
		std::filesystem::remove(data_path, error);
		std::filesystem::remove(wal_path, error);
	}
};

TEST_F(RelocatableBTreeIndexTest, FindsAndScansOneSnapshotPinnedLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = leaf(PageId(101), 17, LogSequenceNumber(2));
	root.insert({key("a"), 10});
	root.insert({key("c"), 30});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation rootLocation{17, PageId(101), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(3)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<std::byte> a = key("a");
	const std::vector<std::byte> b = key("b");
	const std::vector<std::byte> c = key("c");
	EXPECT_EQ(index.find(snapshot, a), std::optional<uint64_t>(10));
	EXPECT_FALSE(index.find(snapshot, b).has_value());
	EXPECT_EQ(index.find(snapshot, c), std::optional<uint64_t>(30));
	EXPECT_FALSE(BTreeIndex(18, pages, *metadata).find(snapshot, a).has_value());
	EXPECT_TRUE(index.scan(snapshot, std::nullopt, 0).empty());
	const auto all = index.scan(snapshot, std::nullopt, 8);
	ASSERT_EQ(all.size(), 2U);
	EXPECT_EQ(all[0].key, a);
	EXPECT_EQ(all[1].key, c);
	const auto fromB = index.scan(snapshot, std::span<const std::byte>(b), 8);
	ASSERT_EQ(fromB.size(), 1U);
	EXPECT_EQ(fromB.front().key, c);
	EXPECT_EQ(index.scan(snapshot, std::nullopt, 1).front().value_or_child, 10U);
	EXPECT_THROW(static_cast<void>(BTreeIndex(0, pages, *metadata)), std::invalid_argument);
}

TEST_F(RelocatableBTreeIndexTest, TraversesInternalSeparatorsAndLeafSiblingLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3), {}, PageId(102));
	left.insert({key("a"), 1});
	BTreePage right = leaf(PageId(102), 17, LogSequenceNumber(4), PageId(101));
	right.insert({key("m"), 2});
	right.insert({key("z"), 3});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	static_cast<void>(pages.append(appendRequest(PageId(102)), right, LogSequenceNumber(4)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(5)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<std::byte> a = key("a");
	const std::vector<std::byte> m = key("m");
	const std::vector<std::byte> z = key("z");
	EXPECT_EQ(index.find(snapshot, a), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(snapshot, m), std::optional<uint64_t>(2));
	EXPECT_EQ(index.find(snapshot, z), std::optional<uint64_t>(3));
	const auto fromM = index.scan(snapshot, std::span<const std::byte>(m), 8);
	ASSERT_EQ(fromM.size(), 2U);
	EXPECT_EQ(fromM[0].key, m);
	EXPECT_EQ(fromM[1].key, z);
}

TEST_F(RelocatableBTreeIndexTest, RejectsMissingChildrenAndBrokenLeafLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3), {}, PageId(102));
	left.insert({key("a"), 1});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(4)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.find(snapshot, key("m"))); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(index.scan(snapshot, std::nullopt, 8)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreeIndexTest, RejectsForeignMetadataAndReturnsEmptyForAnUnpublishedIndex) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	auto foreignMetadata = RelocatableMetadataStore::open(io);
	EXPECT_THROW(static_cast<void>(BTreeIndex(17, pages, *foreignMetadata)), std::invalid_argument);
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(index.find(snapshot, key("a")).has_value());
	EXPECT_TRUE(index.scan(snapshot, std::nullopt, 8).empty());
}

TEST_F(RelocatableBTreeIndexTest, DescendsMultipleInternalLevelsWithinSeparatorRanges) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102), 2);
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = internal(PageId(101), 17, LogSequenceNumber(3), PageId(103), key("f"), PageId(104));
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	BTreePage firstLeaf = leaf(PageId(103), 17, LogSequenceNumber(4));
	firstLeaf.insert({key("a"), 1});
	static_cast<void>(pages.append(appendRequest(PageId(103)), firstLeaf, LogSequenceNumber(4)));
	BTreePage secondLeaf = leaf(PageId(104), 17, LogSequenceNumber(5));
	secondLeaf.insert({key("g"), 7});
	static_cast<void>(pages.append(appendRequest(PageId(104)), secondLeaf, LogSequenceNumber(5)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(6)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(index.find(snapshot, key("a")), std::optional<uint64_t>(1));
	EXPECT_EQ(index.find(snapshot, key("g")), std::optional<uint64_t>(7));
}

TEST_F(RelocatableBTreeIndexTest, RejectsAChildOwnedByAnotherIndex) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage foreignLeaf = leaf(PageId(101), 18, LogSequenceNumber(3));
	foreignLeaf.insert({key("a"), 1});
	static_cast<void>(pages.append(appendRequest(PageId(101), 18), foreignLeaf, LogSequenceNumber(3)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(4)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.find(snapshot, key("a"))); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreeIndexTest, RejectsAChildAtTheWrongLevel) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage invalidLevel = internal(PageId(101), 17, LogSequenceNumber(3), PageId(103), key("f"), PageId(104));
	static_cast<void>(pages.append(appendRequest(PageId(101)), invalidLevel, LogSequenceNumber(3)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(4)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.find(snapshot, key("a"))); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsRootLeafSiblingLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage rootLeaf = leaf(PageId(101), 17, LogSequenceNumber(2), {}, PageId(102));
	rootLeaf.insert({key("a"), 1});
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(101)), rootLeaf, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage sibling = leaf(PageId(102), 17, LogSequenceNumber(3), PageId(101));
	sibling.insert({key("b"), 2});
	static_cast<void>(pages.append(appendRequest(PageId(102)), sibling, LogSequenceNumber(3)));
	IndexRootLocation rootLocation{17, PageId(101), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(4)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.scan(snapshot, std::nullopt, 8)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsNonreciprocalLeafLinks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3), {}, PageId(102));
	left.insert({key("a"), 1});
	BTreePage right = leaf(PageId(102), 17, LogSequenceNumber(4));
	right.insert({key("m"), 2});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	static_cast<void>(pages.append(appendRequest(PageId(102)), right, LogSequenceNumber(4)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(5)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.scan(snapshot, std::nullopt, 8)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsCyclicLeafChains) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3), PageId(102), PageId(102));
	left.insert({key("a"), 1});
	BTreePage right = leaf(PageId(102), 17, LogSequenceNumber(4), PageId(101), PageId(101));
	right.insert({key("m"), 2});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	static_cast<void>(pages.append(appendRequest(PageId(102)), right, LogSequenceNumber(4)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(5)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.scan(snapshot, std::nullopt, 8)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsUnorderedLeafChains) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3), {}, PageId(102));
	left.insert({key("l"), 1});
	BTreePage right = leaf(PageId(102), 17, LogSequenceNumber(4), PageId(101));
	right.insert({key("a"), 2});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	static_cast<void>(pages.append(appendRequest(PageId(102)), right, LogSequenceNumber(4)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(5)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.scan(snapshot, std::nullopt, 8)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsLeafKeysOutsideTheirSeparatorRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3));
	left.insert({key("a"), 1});
	BTreePage right = leaf(PageId(102), 17, LogSequenceNumber(4));
	right.insert({key("b"), 2});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	static_cast<void>(pages.append(appendRequest(PageId(102)), right, LogSequenceNumber(4)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(5)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.find(snapshot, key("m"))); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsALeftLeafKeyAtItsExclusiveUpperSeparator) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102));
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage left = leaf(PageId(101), 17, LogSequenceNumber(3));
	left.insert({key("m"), 1});
	static_cast<void>(pages.append(appendRequest(PageId(101)), left, LogSequenceNumber(3)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(4)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.find(snapshot, key("a"))); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreeIndexTest, RejectsAnInternalSeparatorOutsideItsParentRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore pages(pool, *metadata);
	BTreePage root = internal(PageId(100), 17, LogSequenceNumber(2), PageId(101), key("m"), PageId(102), 2);
	ASSERT_TRUE(pages.allocate(allocationRequest(PageId(100)), root, PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	BTreePage child = internal(PageId(101), 17, LogSequenceNumber(3), PageId(103), key("z"), PageId(104));
	static_cast<void>(pages.append(appendRequest(PageId(101)), child, LogSequenceNumber(3)));
	const IndexRootLocation rootLocation{17, PageId(100), Generation(1)};
	static_cast<void>(pages.publishRoots(std::span(&rootLocation, 1), {}, LogSequenceNumber(4)));
	BTreeIndex index(17, pages, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(index.find(snapshot, key("a"))); }, FormatError::FERR_INVALID_HEADER);
}
