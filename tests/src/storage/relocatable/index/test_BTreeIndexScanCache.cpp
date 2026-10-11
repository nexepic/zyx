/**
 * @file test_BTreeIndexScanCache.cpp
 * @brief Buffer Pool admission tests for snapshot-pinned B+Tree scans.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t INDEX_ID = 17U;
	constexpr uint64_t POOL_OWNER_ID = 77U;
	constexpr uint64_t POOL_SEGMENT_ID = 71U;

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	std::vector<std::byte> key(std::string_view text) {
		std::vector<std::byte> result;
		result.reserve(text.size());
		for (char value: text) {
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

	class ThrowOnCheck final : public CancellationCheckpoint {
	public:
		void check() const override {
			++checks;
			throw std::runtime_error("range cursor cancelled");
		}

		mutable size_t checks = 0U;
	};

	PageOwnerHeader owner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId, INDEX_ID, PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), lsn};
	}

	BTreePage leaf(PageId pageId, std::string_view value, LogSequenceNumber lsn, PageId left, PageId right) {
		BTreePageHeader header;
		header.index_id = INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.left_sibling = left;
		header.right_sibling = right;
		BTreePage result = BTreePage::create(owner(pageId, lsn), header);
		result.insert({key(value), static_cast<uint64_t>(value.front())});
		return result;
	}

	BTreePage internal(PageId pageId, uint16_t level, PageId leftmost, std::string_view separator, PageId rightChild,
					   LogSequenceNumber lsn) {
		BTreePageHeader header;
		header.index_id = INDEX_ID;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.kind = BTreeNodeKind::BPN_INTERNAL;
		header.level = level;
		header.leftmost_child = leftmost;
		BTreePage result = BTreePage::create(owner(pageId, lsn), header);
		result.insert({key(separator), rightChild.value()});
		return result;
	}

	SlottedPagePoolAllocationRequest allocationRequest(PageId pageId) {
		return {SegmentId(POOL_SEGMENT_ID),
				SegmentKind::SGK_INDEX_PAGE_POOL,
				POOL_OWNER_ID,
				Generation(1U),
				pageId,
				INDEX_ID,
				PageOwnerKind::POK_INDEX};
	}

	SlottedPagePoolAppendRequest appendRequest(PageId pageId) {
		return {SegmentId(POOL_SEGMENT_ID), Generation(1U), pageId, INDEX_ID, PageOwnerKind::POK_INDEX};
	}

} // namespace

class BTreeIndexScanCacheTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<SlottedPagePool> pool_;
	std::unique_ptr<BTreePageStore> pages_;
	std::unique_ptr<BTreeIndex> index_;
	MetadataSnapshotPin snapshot_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("btree_scan_cache_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		pool_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		pages_ = std::make_unique<BTreePageStore>(*pool_, *metadata_);

		BTreePage root = internal(PageId(100U), 2U, PageId(201U), "c", PageId(202U), LogSequenceNumber(2U));
		ASSERT_TRUE(pages_->allocate(allocationRequest(PageId(100U)), root, PhysicalSlotId(40U), PhysicalSlotId(40U),
									 LogSequenceNumber(2U))
							.has_value());

		BTreePage leftInternal = internal(PageId(201U), 1U, PageId(101U), "b", PageId(102U), LogSequenceNumber(3U));
		static_cast<void>(pages_->append(appendRequest(PageId(201U)), leftInternal, LogSequenceNumber(3U)));
		BTreePage rightInternal = internal(PageId(202U), 1U, PageId(103U), "d", PageId(104U), LogSequenceNumber(4U));
		static_cast<void>(pages_->append(appendRequest(PageId(202U)), rightInternal, LogSequenceNumber(4U)));

		const std::array<PageId, 4U> leafIds{PageId(101U), PageId(102U), PageId(103U), PageId(104U)};
		const std::array<std::string_view, 4U> leafKeys{"a", "b", "c", "d"};
		for (size_t ordinal = 0U; ordinal < leafIds.size(); ++ordinal) {
			const PageId left = ordinal == 0U ? PageId{} : leafIds[ordinal - 1U];
			const PageId right = ordinal + 1U == leafIds.size() ? PageId{} : leafIds[ordinal + 1U];
			const LogSequenceNumber lsn(5U + ordinal);
			BTreePage page = leaf(leafIds[ordinal], leafKeys[ordinal], lsn, left, right);
			static_cast<void>(pages_->append(appendRequest(leafIds[ordinal]), page, lsn));
		}

		const IndexRootLocation rootLocation{INDEX_ID, PageId(100U), Generation(1U)};
		static_cast<void>(pages_->publishRoots(std::span(&rootLocation, 1U), {}, LogSequenceNumber(9U)));
		io_->sync();
		io_->enableBufferPool(32U * RSS_PAGE_SIZE);
		index_ = std::make_unique<BTreeIndex>(INDEX_ID, *pages_, *metadata_);
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

TEST_F(BTreeIndexScanCacheTest, KeepsRoutingPagesResidentAndStreamsLeafChainTransiently) {
	io_->resetBufferPoolStats();
	const std::vector<BTreePageEntry> first = index_->scan(snapshot_, std::nullopt, 8U);
	ASSERT_EQ(first.size(), 4U);
	EXPECT_EQ(first[0].key, key("a"));
	EXPECT_EQ(first[1].key, key("b"));
	EXPECT_EQ(first[2].key, key("c"));
	EXPECT_EQ(first[3].key, key("d"));
	const BufferPoolStats afterFirst = io_->bufferPoolStats();

	const std::vector<BTreePageEntry> second = index_->scan(snapshot_, std::nullopt, 8U);
	EXPECT_EQ(second, first);
	const BufferPoolStats afterSecond = io_->bufferPoolStats();
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes + 4U * RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + 4U);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.hits, afterFirst.hits);

	const std::vector<std::byte> b = key("b");
	EXPECT_EQ(index_->find(snapshot_, b), std::optional<uint64_t>(static_cast<uint64_t>('b')));
	const BufferPoolStats afterFirstPoint = io_->bufferPoolStats();
	EXPECT_EQ(afterFirstPoint.disk_read_bytes, afterSecond.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirstPoint.misses, afterSecond.misses + 1U);
	EXPECT_EQ(afterFirstPoint.resident_bytes, afterSecond.resident_bytes + RSS_PAGE_SIZE);

	EXPECT_EQ(index_->find(snapshot_, b), std::optional<uint64_t>(static_cast<uint64_t>('b')));
	const BufferPoolStats afterSecondPoint = io_->bufferPoolStats();
	EXPECT_EQ(afterSecondPoint.disk_read_bytes, afterFirstPoint.disk_read_bytes);
	EXPECT_EQ(afterSecondPoint.misses, afterFirstPoint.misses);
	EXPECT_GT(afterSecondPoint.hits, afterFirstPoint.hits);
}

TEST_F(BTreeIndexScanCacheTest, RejectsAnInvalidReadHintBeforeResolvingThePageRoute) {
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();
	expectFormatError(
			[&] {
				static_cast<void>(pages_->read(snapshot_, PageId(999999U), static_cast<BufferPoolReadHint>(0xFFU)));
			},
			FormatError::FERR_INVALID_ENUM);
	const BufferPoolStats stats = io_->bufferPoolStats();
	EXPECT_EQ(stats.disk_read_bytes, 0U);
	EXPECT_EQ(stats.misses, 0U);
	EXPECT_EQ(stats.resident_bytes, before.resident_bytes);
}

TEST_F(BTreeIndexScanCacheTest, RejectsAnInvalidInitialLeafHintBeforeResolvingTheRoot) {
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();
	expectFormatError(
			[&] {
				static_cast<void>(index_->scan(snapshot_, std::nullopt, 1U, static_cast<BufferPoolReadHint>(0xFFU)));
			},
			FormatError::FERR_INVALID_ENUM);
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes);
	EXPECT_EQ(after.misses, before.misses);
	EXPECT_EQ(after.resident_bytes, before.resident_bytes);
}

TEST_F(BTreeIndexScanCacheTest, RangeCursorDescendsOnceAndResumesAcrossSiblingLeaves) {
	const std::vector<std::byte> lower = key("b");
	BTreeIndex::RangeCursor cursor = index_->openRangeCursor(snapshot_, std::span<const std::byte>(lower), 3U);

	const std::optional<BTreePageEntry> first = cursor.next();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->key, key("b"));
	const std::optional<BTreePageEntry> second = cursor.next();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->key, key("c"));
	const std::optional<BTreePageEntry> third = cursor.next();
	ASSERT_TRUE(third.has_value());
	EXPECT_EQ(third->key, key("d"));
	EXPECT_FALSE(cursor.next().has_value());
}

TEST_F(BTreeIndexScanCacheTest, RangeCursorChecksCancellationBeforeOpeningStorage) {
	io_->resetBufferPoolStats();
	const BufferPoolStats before = io_->bufferPoolStats();
	ThrowOnCheck cancellation;
	EXPECT_THROW(static_cast<void>(index_->openRangeCursor(snapshot_, std::nullopt, 3U,
																	 BufferPoolReadHint::BPRH_SEQUENTIAL, false, &cancellation)),
				 std::runtime_error);
	EXPECT_EQ(cancellation.checks, 1U);
	const BufferPoolStats after = io_->bufferPoolStats();
	EXPECT_EQ(after.disk_read_bytes, before.disk_read_bytes);
	EXPECT_EQ(after.misses, before.misses);
}

TEST_F(BTreeIndexScanCacheTest, RangeBoundaryAdmissionRejectsWithoutConsumingAndReportsRelease) {
	auto cursor = index_->openRangeCursor(snapshot_, std::nullopt, 4U);
	const auto reject = [](size_t bytes) {
		if (bytes != 0U) throw std::length_error("boundary key rejected");
	};
	for (size_t retry = 0U; retry < 2U; ++retry) {
		EXPECT_THROW(static_cast<void>(cursor.next(reject)), std::length_error);
	}
	size_t retained = 0U;
	const auto admit = [&](size_t bytes) { retained = bytes; };
	for (const auto text: {"a", "b", "c", "d"}) {
		const auto entry = cursor.next(admit);
		ASSERT_TRUE(entry.has_value());
		EXPECT_EQ(entry->key, key(text));
		if (std::string_view(text) == "d") EXPECT_EQ(retained, 0U);
		else EXPECT_GE(retained, entry->key.size());
	}
	EXPECT_FALSE(cursor.next(admit).has_value());
	EXPECT_EQ(retained, 0U);
}

TEST_F(BTreeIndexScanCacheTest, ScanAndCursorReuseValidatedLeafDecodesAcrossSubtrees) {
	for (bool stream: {false, true}) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		std::vector<BTreePageEntry> actual;
		EXPECT_NO_THROW({
			if (stream) {
				auto cursor = index_->openRangeCursor(snapshot_, std::nullopt, 4U);
				while (auto entry = cursor.next()) actual.push_back(std::move(*entry));
			} else {
				actual = index_->scan(snapshot_, std::nullopt, 4U);
			}
		});
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(actual.size(), 4U);
		for (size_t index = 0U; index < actual.size(); ++index) EXPECT_EQ(actual[index].key, key(std::string(1U, char('a' + index))));
		ASSERT_TRUE(trace.contains("native_btree.read.packed_page_decodes"));
		// Three internal pages plus four leaves each authenticate and transfer
		// their canonical entries once; route/range checks still cover every entry.
		EXPECT_EQ(trace.at("native_btree.read.packed_page_decodes").totalValue, 7);
		EXPECT_EQ(trace.at("native_graph.read.root_descents").totalValue, 1);
	}
}

TEST_F(BTreeIndexScanCacheTest, PointLookupReusesAuthenticatedLeafEntries) {
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 const auto found = index_->find(snapshot_, key("c"));
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 ASSERT_EQ(found, std::optional<uint64_t>(static_cast<uint64_t>('c')));
 ASSERT_TRUE(trace.contains("native_btree.read.packed_page_decodes"));
 // Root open plus root entries; the child routing page and leaf each
 // authenticate once and transfer their decoded entries.
 EXPECT_EQ(trace.at("native_btree.read.packed_page_decodes").totalValue, 4);
 EXPECT_FALSE(index_->find(snapshot_, key("cc")).has_value());
}

TEST_F(BTreeIndexScanCacheTest, SortedExactBatchReusesAuthenticatedRangesIncludingMissingKeys) {
 const std::array keys{key("a"), key("aa"), key("ab"), key("b"), key("bb"), key("c"), key("cc"), key("d"), key("dd")};
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 const auto found = index_->findSorted(snapshot_, keys);
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 ASSERT_EQ(found.size(), keys.size());
 for (size_t position = 0; position < keys.size(); ++position)
  EXPECT_EQ(found[position], index_->find(snapshot_, keys[position]));
 EXPECT_EQ(found[0], static_cast<uint64_t>('a')); EXPECT_FALSE(found[1]); EXPECT_FALSE(found[2]);
 EXPECT_EQ(found[3], static_cast<uint64_t>('b')); EXPECT_FALSE(found[4]);
 EXPECT_EQ(found[5], static_cast<uint64_t>('c')); EXPECT_FALSE(found[6]);
 EXPECT_EQ(found[7], static_cast<uint64_t>('d')); EXPECT_FALSE(found[8]);
 ASSERT_TRUE(trace.contains("native_btree.read.packed_page_decodes"));
 // One root open plus four descents: borrowed root entries and one
 // complete decode for each owned child routing page and leaf.
 EXPECT_EQ(trace.at("native_btree.read.packed_page_decodes").totalValue, 13);
}

TEST_F(BTreeIndexScanCacheTest, SortedExactBatchRejectsOrderBeforeIoAndHandlesEmptyAndUnpublishedRoots) {
 const auto before = io_->bufferPoolStats();
 const std::array repeated{key("a"), key("a")};
 const std::array reversed{key("b"), key("a")};
 EXPECT_THROW(static_cast<void>(index_->findSorted(snapshot_, repeated)), FormatException);
 EXPECT_THROW(static_cast<void>(index_->findSorted(snapshot_, reversed)), FormatException);
 EXPECT_TRUE(index_->findSorted(snapshot_, {}).empty());
 EXPECT_EQ(io_->bufferPoolStats().disk_read_bytes, before.disk_read_bytes);
 EXPECT_EQ(io_->bufferPoolStats().hits, before.hits);
 const std::array keys{key("a"), key("b")};
 BTreeIndex missing(18, *pages_, *metadata_);
 const auto absent = missing.findSorted(snapshot_, keys);
 ASSERT_EQ(absent.size(), 2U); EXPECT_FALSE(absent[0]); EXPECT_FALSE(absent[1]);
}
