/**
 * @file test_BufferPool.cpp
 * @brief Unit tests for the native generation-aware 4 KiB Buffer Pool.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include "graph/debug/PerfTrace.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/cache/BufferPool.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"

namespace graph::storage {

	class BufferPoolTestAccess final {
	public:
		static void bytesForPin(const BufferPool &pool, BufferPoolPageKey key) {
			static_cast<void>(pool.bytesForPin(key));
		}

		static void mutableBytesForPin(BufferPool &pool, BufferPoolPageKey key) {
			static_cast<void>(pool.mutableBytesForPin(key));
		}

		static void markDirtyForPin(BufferPool &pool, BufferPoolPageKey key) { pool.markDirtyForPin(key); }

		static void setPinCount(BufferPool &pool, BufferPoolPageKey key, size_t pinCount) {
			std::lock_guard lock(pool.mutex_);
			const auto entry = pool.entries_.find(key);
			if (entry == pool.entries_.end()) {
				throw std::logic_error("buffer pool test access requires a resident page");
			}
			entry->second->pin_count = pinCount;
		}

		static void evictWithStaleRecentKey(BufferPool &pool, BufferPoolPageKey key) {
			std::lock_guard lock(pool.mutex_);
			pool.recent_.push_back(key);
			static_cast<void>(pool.evictOneLocked());
		}
	};

} // namespace graph::storage

using namespace graph::storage;

namespace {

	class BufferPoolTest : public ::testing::Test {
	protected:
		void SetUp() override {
			const uint64_t suffix = next_file_id_.fetch_add(1U, std::memory_order_relaxed);
			path_ = std::filesystem::temp_directory_path() /
					std::string("zyx_native_buffer_pool_").append(std::to_string(suffix)).append(".db");
		}

		void TearDown() override {
			pool_.reset();
			io_.reset();
			stream_.reset();
			static_cast<void>(portable_close_rw(write_fd_));
			static_cast<void>(portable_close(read_fd_));
			std::error_code ignored;
			static_cast<void>(std::filesystem::remove(path_, ignored));
		}

		void open(size_t pages, bool native_handles = true) {
			ASSERT_GT(pages, 0U);
			std::vector<std::byte> bytes(pages * RSS_BUFFER_POOL_PAGE_BYTES);
			for (size_t page = 0U; page < pages; ++page) {
				std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(page * RSS_BUFFER_POOL_PAGE_BYTES),
							RSS_BUFFER_POOL_PAGE_BYTES, static_cast<std::byte>(page + 1U));
			}
			std::ofstream initial(path_, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(initial.is_open());
			initial.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			ASSERT_TRUE(initial.good());
			initial.close();

			stream_ = std::make_shared<std::fstream>(path_, std::ios::binary | std::ios::in | std::ios::out);
			ASSERT_TRUE(stream_->is_open());
			if (native_handles) {
				write_fd_ = portable_open_rw(path_.string().c_str());
				read_fd_ = portable_open(path_.string().c_str(), O_RDONLY);
				ASSERT_NE(write_fd_, INVALID_FILE_HANDLE);
				ASSERT_NE(read_fd_, INVALID_FILE_HANDLE);
			}
			io_ = std::make_shared<StorageIO>(stream_, write_fd_, read_fd_);
		}

		[[nodiscard]] std::unique_ptr<BufferPool> createPool(size_t capacity_pages) {
			return std::make_unique<BufferPool>(*io_, capacity_pages * RSS_BUFFER_POOL_PAGE_BYTES);
		}

		[[nodiscard]] std::byte diskByte(uint64_t offset) const {
			std::ifstream input(path_, std::ios::binary);
			EXPECT_TRUE(input.is_open());
			input.seekg(static_cast<std::streamoff>(offset));
			char byte = 0;
			input.read(&byte, 1);
			EXPECT_EQ(input.gcount(), 1);
			return static_cast<std::byte>(static_cast<unsigned char>(byte));
		}

		static std::atomic<uint64_t> next_file_id_;
		std::filesystem::path path_;
		std::shared_ptr<std::fstream> stream_;
		std::shared_ptr<StorageIO> io_;
		std::unique_ptr<BufferPool> pool_;
		file_handle_t write_fd_ = INVALID_FILE_HANDLE;
		file_handle_t read_fd_ = INVALID_FILE_HANDLE;
	};

	std::atomic<uint64_t> BufferPoolTest::next_file_id_{1U};

} // namespace

TEST_F(BufferPoolTest, KeepsDifferentGenerationsForOnePhysicalPageResident) {
	open(2U);
	pool_ = createPool(2U);
	{
		auto first = pool_->pin({0U, 7U});
		EXPECT_EQ(first.bytes().front(), std::byte{1U});
	}
	{
		auto second = pool_->pin({0U, 8U});
		EXPECT_EQ(second.bytes().front(), std::byte{1U});
	}
	{
		auto firstAgain = pool_->pin({0U, 7U});
		EXPECT_EQ(firstAgain.bytes().front(), std::byte{1U});
	}

	const BufferPoolStats stats = pool_->stats();
	EXPECT_EQ(stats.resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(stats.misses, 2U);
	EXPECT_EQ(stats.hits, 1U);
	EXPECT_EQ(stats.evictions, 0U);
}

TEST_F(BufferPoolTest, ValidatesCapacityAndPinHandleState) {
	open(2U);
	EXPECT_THROW(static_cast<void>(BufferPool(*io_, RSS_BUFFER_POOL_PAGE_BYTES - 1U)), std::invalid_argument);
	pool_ = createPool(2U);

	auto first = pool_->pin({0U, 0U});
	BufferPool::Pin moved(std::move(first));
	EXPECT_FALSE(first.isValid());
	EXPECT_THROW(static_cast<void>(first.bytes()), std::logic_error);
	EXPECT_THROW(static_cast<void>(first.validBytes()), std::logic_error);
	EXPECT_THROW(static_cast<void>(first.mutableBytes()), std::logic_error);
	EXPECT_THROW(first.markDirty(), std::logic_error);
	EXPECT_TRUE(moved.isValid());
	EXPECT_EQ(moved.validBytes(), RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_THROW(static_cast<void>(moved.mutableBytes()), std::logic_error);
	EXPECT_THROW(moved.markDirty(), std::logic_error);

	BufferPool::Pin empty;
	EXPECT_THROW(static_cast<void>(empty.bytes()), std::logic_error);
	EXPECT_THROW(static_cast<void>(empty.validBytes()), std::logic_error);
	EXPECT_THROW(static_cast<void>(empty.mutableBytes()), std::logic_error);
	EXPECT_THROW(empty.markDirty(), std::logic_error);
	const auto selfMove = [](BufferPool::Pin &pin) -> BufferPool::Pin && { return std::move(pin); };
	moved = selfMove(moved);
	EXPECT_TRUE(moved.isValid());
	const uint64_t alignedOverflowOffset = std::numeric_limits<uint64_t>::max() - RSS_BUFFER_POOL_PAGE_BYTES + 1U;
	EXPECT_THROW(static_cast<void>(pool_->pin({alignedOverflowOffset, 0U})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(pool_->allocatePrivate({1U, 1U})), std::invalid_argument);
}

TEST_F(BufferPoolTest, TypedPageWritesRejectPinnedAndPrivatePagesAndReadBackByGeneration) {
	open(2U);
	pool_ = createPool(2U);
	const BufferPoolPageKey key{0U, 17U};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> written{};
	written.fill(std::byte{0xB7U});

	EXPECT_THROW(static_cast<void>(pool_->readPage({0U, 0U}, written)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(pool_->readPage({1U, 17U}, written)), std::invalid_argument);
	EXPECT_EQ(pool_->readPage({2U * RSS_BUFFER_POOL_PAGE_BYTES, 17U}, written), 0U);
	EXPECT_THROW(pool_->writePage({0U, 0U}, written), std::invalid_argument);
	EXPECT_THROW(pool_->writePage({1U, 17U}, written), std::invalid_argument);

	pool_->writePage(key, written);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> observed{};
	EXPECT_EQ(pool_->readPage(key, observed), RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(observed, written);
	// A repeated typed write retains the resident generation and does not
	// double-account the dirty page.
	pool_->writePage(key, written);
	{
		auto pinned = pool_->pin(key);
		EXPECT_THROW(pool_->writePage({0U, 18U}, written), std::logic_error);
	}

	const BufferPoolPageKey privateKey{0U, 19U};
	auto privatePage = pool_->allocatePrivate(privateKey);
	EXPECT_THROW(pool_->writePage(privateKey, written), std::logic_error);
	EXPECT_THROW(pool_->writePage({0U, 20U}, written), std::logic_error);
}

TEST_F(BufferPoolTest, TypedPageRangesPreserveGenerationAndPinSafety) {
	open(4U);
	pool_ = createPool(4U);
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> first{};
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> second{};
	first.fill(std::byte{0xA1U});
	second.fill(std::byte{0xB2U});

	EXPECT_THROW(pool_->writePages({0U, 0U}, first), std::invalid_argument);
	EXPECT_THROW(pool_->writePages({1U, 7U}, first), std::invalid_argument);
	EXPECT_THROW(pool_->writePages({0U, 7U}, std::span(first.data(), RSS_BUFFER_POOL_PAGE_BYTES + 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(pool_->readPages({0U, 7U}, std::span<std::byte>{})), std::invalid_argument);

	pool_->writePages({0U, 7U}, first);
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> observed{};
	EXPECT_EQ(pool_->readPages({0U, 7U}, observed, BufferPoolReadHint::BPRH_SEQUENTIAL), observed.size());
	EXPECT_EQ(observed, first);

	{
		auto pinned = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 7U});
		EXPECT_THROW(pool_->writePages({0U, 7U}, second), std::logic_error);
		EXPECT_THROW(pool_->writePages({0U, 8U}, second), std::logic_error);
	}
	pool_->writePages({0U, 8U}, second);
	EXPECT_EQ(pool_->stats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(pool_->readPages({0U, 8U}, observed, BufferPoolReadHint::BPRH_SEQUENTIAL), observed.size());
	EXPECT_EQ(observed, second);
}

TEST_F(BufferPoolTest, TypedPageRangesCoalesceSequentialMissesAndStopAtFileEnd) {
	open(1U);
	pool_ = createPool(2U);
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> observed{};

	EXPECT_EQ(pool_->readPages({0U, 9U}, observed, BufferPoolReadHint::BPRH_SEQUENTIAL),
			  RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(observed.front(), std::byte{1U});
	EXPECT_EQ(pool_->stats().resident_bytes, 0U);
	EXPECT_EQ(pool_->stats().disk_read_bytes, RSS_BUFFER_POOL_PAGE_BYTES);

	// Normal typed reads remain cache-admitted and must also stop at the exact
	// file boundary rather than returning a fabricated complete second page.
	EXPECT_EQ(pool_->readPages({0U, 10U}, observed, BufferPoolReadHint::BPRH_NORMAL),
			  RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
}

TEST_F(BufferPoolTest, TypedPageRangesRejectPrivateCowPagesAndReturnZeroPastEndOfFile) {
	open(1U);
	pool_ = createPool(2U);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> bytes{};

	EXPECT_EQ(pool_->readPages({RSS_BUFFER_POOL_PAGE_BYTES, 7U}, bytes), 0U);
	{
		auto privatePage = pool_->allocatePrivate({0U, 7U});
		privatePage.mutableBytes().front() = std::byte{0xC4U};
	}

	// A private COW frame remains unpublished after its writer releases it.
	EXPECT_THROW(static_cast<void>(pool_->readPages({0U, 7U}, bytes)), std::logic_error);
	EXPECT_THROW(pool_->writePages({0U, 8U}, bytes), std::logic_error);
}

TEST_F(BufferPoolTest, TypedPageWriteThroughFlushesAndDropsOverlappingCacheViews) {
	open(2U);
	pool_ = createPool(2U);
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> first{};
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> second{};
	first.fill(std::byte{0x51U});
	second.fill(std::byte{0xA2U});
	pool_->writePages({0U, 7U}, first);
	{
		auto pinned = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 7U});
		EXPECT_THROW(pool_->writePagesThrough({0U, 8U}, second), std::logic_error);
	}

	pool_->writePagesThrough({0U, 8U}, second);
	EXPECT_EQ(pool_->stats().resident_bytes, 0U);
	EXPECT_EQ(pool_->stats().dirty_bytes, 0U);
	EXPECT_EQ(diskByte(0U), std::byte{0xA2U});
	EXPECT_EQ(diskByte(RSS_BUFFER_POOL_PAGE_BYTES), std::byte{0xA2U});
}

TEST_F(BufferPoolTest, RefusesToEvictPinnedPageThenEvictsAfterUnpin) {
	open(2U);
	pool_ = createPool(1U);
	auto first = pool_->pin({0U, 0U});
	EXPECT_THROW(static_cast<void>(pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U})), std::runtime_error);

	first = {};
	auto second = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U});
	EXPECT_EQ(second.bytes().front(), std::byte{2U});
	const BufferPoolStats stats = pool_->stats();
	EXPECT_EQ(stats.resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(stats.pinned_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(stats.evictions, 1U);
}

TEST_F(BufferPoolTest, PrivateCowPageIsInvisibleUntilFlushThenDurablyWritten) {
	open(3U);
	pool_ = createPool(2U);
	const uint64_t pageOffset = 2U * RSS_BUFFER_POOL_PAGE_BYTES;
	EXPECT_EQ(diskByte(pageOffset), std::byte{3U});
	{
		auto page = pool_->allocatePrivate({pageOffset, 17U});
		auto bytes = page.mutableBytes();
		std::fill(bytes.begin(), bytes.end(), std::byte{0xA5U});
		page.markDirty();
		EXPECT_THROW(static_cast<void>(pool_->pin({pageOffset, 17U})), std::logic_error);
	}
	EXPECT_EQ(diskByte(pageOffset), std::byte{3U});
	EXPECT_EQ(pool_->stats().dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);

	pool_->flush();
	io_->sync();
	EXPECT_EQ(diskByte(pageOffset), std::byte{0xA5U});
	const BufferPoolStats stats = pool_->stats();
	EXPECT_EQ(stats.dirty_bytes, 0U);
	EXPECT_EQ(stats.disk_write_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
}

TEST_F(BufferPoolTest, FullPageOverwriteAvoidsReadBeforeWrite) {
	open(2U);
	pool_ = createPool(2U);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> replacement{};
	replacement.fill(std::byte{0xBEU});
	pool_->write(RSS_BUFFER_POOL_PAGE_BYTES, replacement);
	const BufferPoolStats beforeFlush = pool_->stats();
	EXPECT_EQ(beforeFlush.misses, 1U);
	EXPECT_EQ(beforeFlush.disk_read_bytes, 0U);
	EXPECT_EQ(beforeFlush.dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);

	pool_->flush();
	EXPECT_EQ(diskByte(RSS_BUFFER_POOL_PAGE_BYTES), std::byte{0xBEU});
}

TEST_F(BufferPoolTest, WritesReuseResidentPagesAndConsumePrefetchWithoutDroppingPinnedViews) {
	open(3U);
	pool_ = createPool(3U);
	const std::array<std::byte, 3U> firstWrite{{std::byte{0x31U}, std::byte{0x32U}, std::byte{0x33U}}};
	pool_->write(9U, firstWrite);
	pool_->flush();
	pool_->write(13U, firstWrite);

	auto held = pool_->pin({0U, 0U});
	EXPECT_THROW(pool_->write(17U, firstWrite), std::logic_error);
	held = {};
	pool_->write(17U, firstWrite);

	const std::array<BufferPoolPageKey, 1U> prefetched{{{RSS_BUFFER_POOL_PAGE_BYTES, 0U}}};
	pool_->prefetch(prefetched);
	pool_->write(RSS_BUFFER_POOL_PAGE_BYTES + 3U, firstWrite);
	// An uncached partial write at a page boundary must retain the old page
	// contents, unlike the full-page overwrite fast path.
	pool_->write(2U * RSS_BUFFER_POOL_PAGE_BYTES, firstWrite);
	const BufferPoolStats stats = pool_->stats();
	EXPECT_GE(stats.hits, 2U);
	EXPECT_EQ(stats.prefetch_hits, 1U);
	EXPECT_EQ(pool_->read(0U, std::span<std::byte>{}), 0U);
	pool_->write(0U, std::span<const std::byte>{});
}

TEST_F(BufferPoolTest, InvalidationFlushesDirtyPagesButRefusesPinnedOrOverlappingStaleViews) {
	open(3U);
	pool_ = createPool(3U);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> replacement{};
	replacement.fill(std::byte{0x77U});
	pool_->write(0U, replacement);
	{
		auto clean = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U});
		EXPECT_THROW(pool_->invalidateRange(RSS_BUFFER_POOL_PAGE_BYTES, 1U), std::logic_error);
	}
	pool_->invalidateRange(RSS_BUFFER_POOL_PAGE_BYTES, 1U);
	{
		auto fartherClean = pool_->pin({2U * RSS_BUFFER_POOL_PAGE_BYTES, 0U});
		EXPECT_EQ(fartherClean.bytes().front(), std::byte{3U});
	}
	// This disjoint range has one cached page below it and one above it.
	pool_->invalidateRange(RSS_BUFFER_POOL_PAGE_BYTES, 1U);
	pool_->invalidateRange(2U * RSS_BUFFER_POOL_PAGE_BYTES, 1U);
	pool_->invalidateRange(0U, 0U);
	pool_->invalidateRange(0U, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(diskByte(0U), std::byte{0x77U});
	EXPECT_EQ(pool_->stats().dirty_bytes, 0U);
}

TEST_F(BufferPoolTest, FrequentAndTransientPagesFollowTheirDistinctEvictionAndResetRules) {
	open(3U);
	pool_ = createPool(1U);
	{
		auto first = pool_->pin({0U, 0U});
		{
			auto second = pool_->pin({0U, 0U});
		}
	}
	{
		auto replacement = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U});
	}
	EXPECT_EQ(pool_->stats().evictions, 1U);

	pool_->resetStats();
	const BufferPoolStats reset = pool_->stats();
	EXPECT_EQ(reset.hits, 0U);
	EXPECT_EQ(reset.misses, 0U);
	EXPECT_EQ(reset.evictions, 0U);
	EXPECT_EQ(reset.prefetch_requests, 0U);
	EXPECT_EQ(reset.prefetch_hits, 0U);
	EXPECT_EQ(reset.disk_read_bytes, 0U);
	EXPECT_EQ(reset.disk_write_bytes, 0U);

	pool_.reset();
	pool_ = createPool(1U);
	{
		auto transient = pool_->pin({2U * RSS_BUFFER_POOL_PAGE_BYTES, 0U}, BufferPoolReadHint::BPRH_SEQUENTIAL);
		EXPECT_EQ(transient.bytes().front(), std::byte{3U});
	}
	EXPECT_EQ(pool_->stats().resident_bytes, 0U);
}

TEST_F(BufferPoolTest, EvictionFlushesDirtyPagesBeforeReusingTheirFrames) {
	open(2U);
	pool_ = createPool(1U);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> first{};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> second{};
	first.fill(std::byte{0x4AU});
	second.fill(std::byte{0x5BU});

	pool_->write(0U, first);
	pool_->write(RSS_BUFFER_POOL_PAGE_BYTES, second);

	EXPECT_EQ(diskByte(0U), std::byte{0x4AU});
	const BufferPoolStats stats = pool_->stats();
	EXPECT_EQ(stats.evictions, 1U);
	EXPECT_EQ(stats.disk_write_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(stats.dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
}

TEST_F(BufferPoolTest, PrivateCowPageRejectsDuplicatesAndCanBeRedirtiedAfterFlush) {
	open(2U);
	pool_ = createPool(2U);
	auto page = pool_->allocatePrivate({RSS_BUFFER_POOL_PAGE_BYTES, 41U});
	EXPECT_THROW(static_cast<void>(pool_->allocatePrivate({RSS_BUFFER_POOL_PAGE_BYTES, 41U})), std::logic_error);
	pool_->flush();
	EXPECT_THROW(static_cast<void>(page.mutableBytes()), std::logic_error);
	page.markDirty();
	EXPECT_EQ(pool_->stats().dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	pool_->flush();
}

TEST_F(BufferPoolTest, SequentialReadsBypassWorkingSetAndPrefetchIsConsumedByDemandRead) {
	open(4U);
	pool_ = createPool(2U);
	{
		auto first = pool_->pin({0U, 0U});
		auto second = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U});
	}
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> scan{};
	EXPECT_EQ(pool_->read(2U * RSS_BUFFER_POOL_PAGE_BYTES, scan, BufferPoolReadHint::BPRH_SEQUENTIAL),
			  RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(scan.front(), std::byte{3U});
	EXPECT_EQ(pool_->stats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(pool_->stats().evictions, 0U);
	{
		auto firstAgain = pool_->pin({0U, 0U});
		auto secondAgain = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U});
	}
	EXPECT_GE(pool_->stats().hits, 2U);

	pool_.reset();
	pool_ = createPool(2U);
	const std::array<BufferPoolPageKey, 2U> keys{{{0U, 0U}, {RSS_BUFFER_POOL_PAGE_BYTES, 0U}}};
	pool_->prefetch(keys);
	pool_->prefetch(std::array<BufferPoolPageKey, 1U>{{keys[0]}});
	{
		auto first = pool_->pin(keys[0]);
		auto second = pool_->pin(keys[1]);
	}
	const BufferPoolStats prefetched = pool_->stats();
	EXPECT_EQ(prefetched.prefetch_requests, 3U);
	EXPECT_EQ(prefetched.prefetch_hits, 2U);
	EXPECT_EQ(prefetched.hits, 2U);

	pool_.reset();
	pool_ = createPool(2U);
	const std::array<BufferPoolPageKey, 1U> sequentialKey{{{2U * RSS_BUFFER_POOL_PAGE_BYTES, 0U}}};
	pool_->prefetch(sequentialKey, BufferPoolReadHint::BPRH_SEQUENTIAL);
	EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	{
		auto consumed = pool_->pin(sequentialKey.front(), BufferPoolReadHint::BPRH_SEQUENTIAL);
		EXPECT_EQ(consumed.bytes().front(), std::byte{3U});
	}
	const BufferPoolStats sequentialPrefetch = pool_->stats();
	EXPECT_EQ(sequentialPrefetch.prefetch_requests, 1U);
	EXPECT_EQ(sequentialPrefetch.prefetch_hits, 1U);
	EXPECT_EQ(sequentialPrefetch.hits, 1U);
	EXPECT_EQ(sequentialPrefetch.resident_bytes, 0U);
}

TEST_F(BufferPoolTest, TransparentStorageIoWritesAreCachedUntilSyncOnFstreamFallback) {
	open(2U, /*native_handles=*/false);
	io_->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);
	const std::array<std::byte, 3U> written{{std::byte{0xC1U}, std::byte{0xC2U}, std::byte{0xC3U}}};
	io_->writeAt(31U, written.data(), written.size());
	EXPECT_EQ(diskByte(31U), std::byte{1U});

	std::array<std::byte, 3U> observed{};
	EXPECT_EQ(io_->readAt(31U, observed.data(), observed.size()), observed.size());
	EXPECT_EQ(observed, written);
	EXPECT_EQ(io_->bufferPoolStats().dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(io_->stats().write_bytes, written.size());

	io_->sync();
	EXPECT_EQ(diskByte(31U), written.front());
	EXPECT_EQ(io_->bufferPoolStats().dirty_bytes, 0U);
	EXPECT_EQ(io_->stats().sync_calls, 1U);
}

TEST_F(BufferPoolTest, ReadOnlyIoRejectsCachedWritesBeforeTheyCanBecomeDirty) {
	open(1U);
	io_.reset();
	stream_.reset();
	static_cast<void>(portable_close_rw(write_fd_));
	static_cast<void>(portable_close(read_fd_));
	write_fd_ = INVALID_FILE_HANDLE;
	read_fd_ = INVALID_FILE_HANDLE;

	auto readOnlyStream = std::make_shared<std::fstream>(path_, std::ios::binary | std::ios::in);
	ASSERT_TRUE(readOnlyStream->is_open());
	auto readOnlyIo = std::make_shared<StorageIO>(readOnlyStream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE,
												  StorageIOAccessMode::SIOAM_READ_ONLY);
	readOnlyIo->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES);
	const std::array<std::byte, 1U> written{{std::byte{0xC1U}}};
	EXPECT_THROW(readOnlyIo->writeAt(0U, written.data(), written.size()), std::logic_error);
	EXPECT_THROW(readOnlyIo->append(written.data(), written.size()), std::logic_error);
	EXPECT_THROW(readOnlyIo->reserveAppendSpace(1U), std::logic_error);
	EXPECT_EQ(readOnlyIo->bufferPoolStats().dirty_bytes, 0U);

	BufferPool directPool(*readOnlyIo, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_THROW(directPool.write(0U, written), std::logic_error);
	EXPECT_THROW(static_cast<void>(directPool.allocatePrivate({0U, 1U})), std::logic_error);
	EXPECT_EQ(directPool.stats().dirty_bytes, 0U);
	readOnlyIo.reset();
	EXPECT_EQ(diskByte(0U), std::byte{1U});
}

TEST_F(BufferPoolTest, DirectFstreamAppendIsVisibleToCachedReadsWithNativeReadHandle) {
	open(2U, /*native_handles=*/false);
	read_fd_ = portable_open(path_.string().c_str(), O_RDONLY);
	ASSERT_NE(read_fd_, INVALID_FILE_HANDLE);
	io_ = std::make_shared<StorageIO>(stream_, INVALID_FILE_HANDLE, read_fd_);
	io_->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);

	const std::array<std::byte, 3U> appended{{std::byte{0xD1U}, std::byte{0xD2U}, std::byte{0xD3U}}};
	const uint64_t offset = io_->append(appended.data(), appended.size());
	EXPECT_EQ(offset, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	std::array<std::byte, 3U> observed{};
	EXPECT_EQ(io_->readAt(offset, observed.data(), observed.size()), observed.size());
	EXPECT_EQ(observed, appended);
}

TEST_F(BufferPoolTest, StorageIoTruncateDropsCachedTailPagesAndPreservesPrefix) {
	open(2U);
	io_->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);

	std::array<std::byte, 1U> prefix{};
	std::array<std::byte, 1U> tail{};
	ASSERT_EQ(io_->readAt(0U, prefix.data(), prefix.size()), prefix.size());
	ASSERT_EQ(io_->readAt(RSS_BUFFER_POOL_PAGE_BYTES, tail.data(), tail.size()), tail.size());
	EXPECT_EQ(io_->bufferPoolStats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);

	io_->truncate(RSS_BUFFER_POOL_PAGE_BYTES);

	EXPECT_EQ(io_->fileSize(), RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(std::filesystem::file_size(path_), RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(io_->bufferPoolStats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(io_->readAt(0U, prefix.data(), prefix.size()), prefix.size());
	EXPECT_EQ(prefix.front(), std::byte{1U});
	EXPECT_EQ(io_->readAt(RSS_BUFFER_POOL_PAGE_BYTES, tail.data(), tail.size()), 0U);
}

TEST_F(BufferPoolTest, StorageIoTruncateRejectsExtensionAndRequiresNativeWriteSupport) {
	open(1U);
	EXPECT_THROW(io_->truncate(RSS_BUFFER_POOL_PAGE_BYTES + 1U), std::invalid_argument);
	EXPECT_EQ(io_->fileSize(), RSS_BUFFER_POOL_PAGE_BYTES);

	auto fallbackIo = std::make_shared<StorageIO>(stream_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_THROW(fallbackIo->truncate(RSS_BUFFER_POOL_PAGE_BYTES - 1U), std::runtime_error);
	EXPECT_EQ(fallbackIo->fileSize(), RSS_BUFFER_POOL_PAGE_BYTES);
}

TEST_F(BufferPoolTest, StorageIoTruncateNoOpKeepsCachedPagesResident) {
	open(1U);
	io_->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES);
	std::array<std::byte, 1U> byte{};
	ASSERT_EQ(io_->readAt(0U, byte.data(), byte.size()), byte.size());
	const StorageIOStats before = io_->stats();

	EXPECT_NO_THROW(io_->truncate(RSS_BUFFER_POOL_PAGE_BYTES));

	EXPECT_EQ(io_->fileSize(), RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(io_->bufferPoolStats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(io_->stats(), before);
}

TEST_F(BufferPoolTest, ExternalRebaseDiscardDropsOnlyCleanUnpinnedPages) {
	open(2U);
	pool_ = createPool(2U);

	{
		auto pinned = pool_->pin({0U, 0U});
		EXPECT_THROW(pool_->discardForExternalRebase(), std::logic_error);
	}
	EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);

	const std::array<std::byte, 1U> dirtyByte{{std::byte{0xF2U}}};
	pool_->write(RSS_BUFFER_POOL_PAGE_BYTES, dirtyByte);
	EXPECT_THROW(pool_->discardForExternalRebase(), std::logic_error);
	EXPECT_EQ(pool_->stats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_EQ(pool_->stats().dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);

	pool_->flush();
	EXPECT_NO_THROW(pool_->discardForExternalRebase());
	EXPECT_EQ(pool_->stats().resident_bytes, 0U);
	EXPECT_EQ(pool_->stats().pinned_bytes, 0U);
	EXPECT_EQ(pool_->stats().dirty_bytes, 0U);
}

TEST_F(BufferPoolTest, StorageIoTruncateFailsClosedWhenNativeHandleCannotShrink) {
	open(1U);
	auto stream = std::make_shared<std::fstream>(path_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	file_handle_t readOnlyAsWrite = portable_open(path_.string().c_str(), O_RDONLY);
	ASSERT_NE(readOnlyAsWrite, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(stream, readOnlyAsWrite, INVALID_FILE_HANDLE);

	EXPECT_THROW(io->truncate(RSS_BUFFER_POOL_PAGE_BYTES - 1U), std::system_error);
	EXPECT_EQ(io->fileSize(), RSS_BUFFER_POOL_PAGE_BYTES);

	io.reset();
	EXPECT_EQ(portable_close(readOnlyAsWrite), 0);
}

TEST_F(BufferPoolTest, RejectsOverflowUnalignedPrivateAndEndOfFileKeysWithoutPartialPrefetch) {
	open(1U);
	pool_ = createPool(1U);
	std::array<std::byte, 16U> output{};
	EXPECT_EQ(pool_->read(RSS_BUFFER_POOL_PAGE_BYTES - 8U, output, BufferPoolReadHint::BPRH_NORMAL), 8U);
	EXPECT_EQ(pool_->read(RSS_BUFFER_POOL_PAGE_BYTES, output, BufferPoolReadHint::BPRH_NORMAL), 0U);
	EXPECT_THROW(static_cast<void>(pool_->read(std::numeric_limits<uint64_t>::max() - 4U, output)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(pool_->pin({1U, 0U})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U})), std::out_of_range);
	EXPECT_THROW(static_cast<void>(pool_->allocatePrivate({0U, 0U})), std::invalid_argument);
	EXPECT_THROW(pool_->prefetch(std::array<BufferPoolPageKey, 1U>{{{1U, 0U}}}), std::invalid_argument);

	const std::array<BufferPoolPageKey, 2U> invalidPrefetch{{{0U, 0U}, {RSS_BUFFER_POOL_PAGE_BYTES, 0U}}};
	EXPECT_THROW(pool_->prefetch(invalidPrefetch), std::out_of_range);
	const BufferPoolStats stats = pool_->stats();
	EXPECT_EQ(stats.prefetch_requests, 0U);
	EXPECT_EQ(stats.resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
}

TEST_F(BufferPoolTest, FailsClosedWhenInternalPinOrRecencyInvariantsAreCorrupted) {
	open(2U);
	pool_ = createPool(2U);
	const BufferPoolPageKey unknown{RSS_BUFFER_POOL_PAGE_BYTES, 99U};

	EXPECT_THROW(BufferPoolTestAccess::bytesForPin(*pool_, unknown), std::logic_error);
	{
		auto page = pool_->pin({0U, 0U});
	}
	EXPECT_THROW(BufferPoolTestAccess::bytesForPin(*pool_, {0U, 0U}), std::logic_error);

	{
		auto shared = pool_->pin({0U, 0U});
		EXPECT_THROW(BufferPoolTestAccess::mutableBytesForPin(*pool_, shared.key()), std::logic_error);
		EXPECT_THROW(BufferPoolTestAccess::markDirtyForPin(*pool_, shared.key()), std::logic_error);
	}
	EXPECT_THROW(BufferPoolTestAccess::mutableBytesForPin(*pool_, unknown), std::logic_error);
	EXPECT_THROW(BufferPoolTestAccess::markDirtyForPin(*pool_, unknown), std::logic_error);

	const BufferPoolPageKey privateKey{RSS_BUFFER_POOL_PAGE_BYTES, 7U};
	{
		auto privatePage = pool_->allocatePrivate(privateKey);
		BufferPoolTestAccess::setPinCount(*pool_, privateKey, 2U);
		EXPECT_THROW(BufferPoolTestAccess::mutableBytesForPin(*pool_, privateKey), std::logic_error);
		EXPECT_THROW(BufferPoolTestAccess::markDirtyForPin(*pool_, privateKey), std::logic_error);
		BufferPoolTestAccess::setPinCount(*pool_, privateKey, 1U);
	}

	EXPECT_THROW(BufferPoolTestAccess::evictWithStaleRecentKey(*pool_, {2U * RSS_BUFFER_POOL_PAGE_BYTES, 0U}),
				 std::logic_error);
}

namespace {
using namespace graph::query::execution::detail;
std::shared_ptr<PrepaidMemoryResource> pageCacheGrant(size_t bytes) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(bytes));
	auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, bytes);
	return std::make_shared<PrepaidMemoryResource>(account, bytes);
}
class FailingPageCacheResource final : public std::pmr::memory_resource {
public:
	explicit FailingPageCacheResource(std::shared_ptr<PrepaidMemoryResource> grant) : grant(std::move(grant)) {}
	std::shared_ptr<PrepaidMemoryResource> grant;
	size_t calls = 0U, fail_at = (std::numeric_limits<size_t>::max)();
private:
	void *do_allocate(size_t bytes, size_t alignment) override {
		if (++calls == fail_at) throw std::bad_alloc();
		return grant->allocate(bytes, alignment);
	}
	void do_deallocate(void *pointer, size_t bytes, size_t alignment) override { grant->deallocate(pointer, bytes, alignment); }
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}

TEST_F(BufferPoolTest, CoordinatedCacheIncludesEntryQueueAndHashMetadataCapacity) {
	open(3U); auto memory = pageCacheGrant(4U * RSS_BUFFER_POOL_PAGE_BYTES);
	pool_ = std::make_unique<BufferPool>(*io_, 2U * RSS_BUFFER_POOL_PAGE_BYTES, memory);
	{ auto first = pool_->pin({0U, 1U}); }
	EXPECT_GT(memory->used(), RSS_BUFFER_POOL_PAGE_BYTES);
	{ auto second = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 1U}); }
	EXPECT_GT(memory->used(), 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	{ auto third = pool_->pin({2U * RSS_BUFFER_POOL_PAGE_BYTES, 1U}); EXPECT_EQ(third.bytes()[0], std::byte{3}); }
	EXPECT_EQ(pool_->stats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_GT(pool_->stats().evictions, 0U);
	pool_.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BufferPoolTest, CacheQuotaIncludesMetadataInsteadOfAdmittingAPayloadOnlyPage) {
	open(1U); auto memory = pageCacheGrant(RSS_BUFFER_POOL_PAGE_BYTES);
	pool_ = std::make_unique<BufferPool>(*io_, RSS_BUFFER_POOL_PAGE_BYTES, memory);
	EXPECT_THROW((void)pool_->pin({0U, 1U}), ResourceAdmissionDenied);
	EXPECT_EQ(pool_->stats().resident_bytes, 0U); EXPECT_EQ(memory->used(), 0U);
	pool_.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BufferPoolTest, EveryFirstAdmissionAllocationFailureLeavesAUsableEmptyCache) {
	open(1U); auto memory = std::make_shared<FailingPageCacheResource>(pageCacheGrant(4U * RSS_BUFFER_POOL_PAGE_BYTES));
	pool_ = std::make_unique<BufferPool>(*io_, RSS_BUFFER_POOL_PAGE_BYTES, memory);
	{ auto first = pool_->pin({0U, 1U}); }
	const size_t allocations = memory->calls; ASSERT_GE(allocations, 3U);
	pool_.reset(); ASSERT_EQ(memory->grant->used(), 0U);
	for (size_t failure = 1U; failure <= allocations; ++failure) {
		SCOPED_TRACE(failure); memory->calls = 0U; memory->fail_at = failure;
		pool_ = std::make_unique<BufferPool>(*io_, RSS_BUFFER_POOL_PAGE_BYTES, memory);
		EXPECT_THROW((void)pool_->pin({0U, 1U}), std::bad_alloc);
		EXPECT_EQ(pool_->stats().resident_bytes, 0U);
		memory->fail_at = (std::numeric_limits<size_t>::max)();
		{ auto retry = pool_->pin({0U, 1U}); EXPECT_EQ(retry.bytes()[0], std::byte{1}); }
		EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
		pool_.reset(); EXPECT_EQ(memory->grant->used(), 0U);
	}
}

TEST_F(BufferPoolTest, PromotionReusesItsQueueNodeWhenFurtherAllocationsWouldFail) {
	open(1U); auto memory = std::make_shared<FailingPageCacheResource>(pageCacheGrant(4U * RSS_BUFFER_POOL_PAGE_BYTES));
	pool_ = std::make_unique<BufferPool>(*io_, RSS_BUFFER_POOL_PAGE_BYTES, memory);
	{ auto first = pool_->pin({0U, 1U}); }
	const size_t allocations = memory->calls; memory->fail_at = allocations + 1U;
	{ auto hit = pool_->pin({0U, 1U}); EXPECT_EQ(hit.bytes()[0], std::byte{1}); }
	EXPECT_EQ(memory->calls, allocations); EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	pool_.reset(); EXPECT_EQ(memory->grant->used(), 0U);
}

TEST_F(BufferPoolTest, CacheRetainsItsAllocatorUntilAllOwnedPagesAndMetadataAreDestroyed) {
	open(1U); auto memory = pageCacheGrant(4U * RSS_BUFFER_POOL_PAGE_BYTES);
	std::weak_ptr<PrepaidMemoryResource> lifetime = memory;
	pool_ = std::make_unique<BufferPool>(*io_, RSS_BUFFER_POOL_PAGE_BYTES, memory); memory.reset();
	{ auto first = pool_->pin({0U, 1U}); EXPECT_EQ(first.bytes()[0], std::byte{1}); }
	ASSERT_FALSE(lifetime.expired()); EXPECT_GT(lifetime.lock()->used(), RSS_BUFFER_POOL_PAGE_BYTES);
	pool_.reset(); EXPECT_TRUE(lifetime.expired());
}

TEST_F(BufferPoolTest, StorageIOForwardsAndRetainsTheCoordinatedCacheGrant) {
    open(1U);
    auto memory = pageCacheGrant(4U * RSS_BUFFER_POOL_PAGE_BYTES);
    std::weak_ptr<PrepaidMemoryResource> lifetime = memory;
    io_->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES, memory);
    memory.reset();
    {
        const auto pinned = io_->pinTypedPage({0U, 1U});
        ASSERT_TRUE(pinned.has_value());
        EXPECT_EQ(pinned->bytes()[0], std::byte{1});
        ASSERT_FALSE(lifetime.expired());
        EXPECT_GT(lifetime.lock()->used(), RSS_BUFFER_POOL_PAGE_BYTES);
    }
    io_->disableBufferPool();
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(BufferPoolTest, StorageIOCacheDenialLeavesNoPartiallyPublishedPage) {
    open(1U);
    auto memory = pageCacheGrant(RSS_BUFFER_POOL_PAGE_BYTES);
    io_->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES, memory);
    EXPECT_THROW(static_cast<void>(io_->pinTypedPage({0U, 1U})), ResourceAdmissionDenied);
    EXPECT_EQ(io_->bufferPoolStats().resident_bytes, 0U);
    EXPECT_EQ(memory->used(), 0U);
    io_->disableBufferPool();
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BufferPoolTest, EvictsForMetadataPressureBeforeThePayloadOnlyCeiling) {
    open(8U);
    auto memory = pageCacheGrant(RSS_BUFFER_POOL_PAGE_BYTES + 1024U);
    pool_ = std::make_unique<BufferPool>(*io_, 8U * RSS_BUFFER_POOL_PAGE_BYTES, memory);
    for (size_t page = 0U; page < 8U; ++page) {
        auto pin = pool_->pin({page * RSS_BUFFER_POOL_PAGE_BYTES, 1U});
        EXPECT_EQ(pin.bytes()[0], static_cast<std::byte>(page + 1U));
        EXPECT_LE(memory->used(), memory->capacity());
    }
    EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
    EXPECT_EQ(pool_->stats().evictions, 7U);
    pool_.reset();
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BufferPoolTest, SharedGrantNeverEvictsPinnedPagesOrBorrowsAnotherAllowance) {
    open(2U);
    auto memory = pageCacheGrant(RSS_BUFFER_POOL_PAGE_BYTES + 1024U);
    pool_ = std::make_unique<BufferPool>(*io_, 2U * RSS_BUFFER_POOL_PAGE_BYTES, memory);
    auto alias = std::make_unique<BufferPool>(*io_, 2U * RSS_BUFFER_POOL_PAGE_BYTES, memory);
    {
        auto pin = pool_->pin({0U, 1U});
        EXPECT_THROW(static_cast<void>(pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 1U})), ResourceAdmissionDenied);
        EXPECT_THROW(static_cast<void>(alias->pin({RSS_BUFFER_POOL_PAGE_BYTES, 1U})), ResourceAdmissionDenied);
        EXPECT_EQ(pin.bytes()[0], std::byte{1});
        EXPECT_EQ(pool_->stats().evictions, 0U);
        EXPECT_EQ(alias->stats().resident_bytes, 0U);
    }
    pool_.reset();
    {
        auto retry = alias->pin({RSS_BUFFER_POOL_PAGE_BYTES, 1U});
        EXPECT_EQ(retry.bytes()[0], std::byte{2});
    }
    alias.reset();
    EXPECT_EQ(memory->used(), 0U);
}

#include <barrier>
#include <thread>
TEST_F(BufferPoolTest, TypedCopyReadsSerializeWithInvalidationWithoutExposingTransientPins) {
    open(1U);
    pool_ = createPool(1U);
    std::barrier start(2);
    std::barrier iterationStart(2);
    std::atomic<size_t> failures{0U};
    constexpr size_t iterations = 10000U;
    std::thread reader([&] {
        std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> image{};
        start.arrive_and_wait();
        for (size_t index = 0U; index < iterations; ++index) {
            iterationStart.arrive_and_wait();
            try {
                if (pool_->readPage({0U, 7U}, image) != image.size() ||
                    !std::ranges::all_of(image, [](std::byte byte) { return byte == std::byte{1U}; })) ++failures;
            } catch (...) { ++failures; }
        }
    });
    start.arrive_and_wait();
    for (size_t index = 0U; index < iterations; ++index) {
        iterationStart.arrive_and_wait();
        try { pool_->invalidateRange(0U, RSS_BUFFER_POOL_PAGE_BYTES); }
        catch (...) { ++failures; }
    }
    reader.join();
    EXPECT_EQ(failures.load(), 0U);
    EXPECT_EQ(pool_->stats().pinned_bytes, 0U);
}

TEST_F(BufferPoolTest, TypedCopyReadsReleaseSequentialMissesAndPreserveExistingExternalPins) {
    open(2U);
    pool_ = createPool(2U);
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> image{};
    EXPECT_EQ(pool_->readPage({0U, 17U}, image), image.size());
    EXPECT_EQ(pool_->readPage({RSS_BUFFER_POOL_PAGE_BYTES, 17U}, image, BufferPoolReadHint::BPRH_SEQUENTIAL), image.size());
    EXPECT_EQ(image.front(), std::byte{2U});
    EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
    EXPECT_EQ(pool_->stats().pinned_bytes, 0U);
    {
        auto pin = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 17U}, BufferPoolReadHint::BPRH_SEQUENTIAL);
        EXPECT_EQ(pool_->readPage(pin.key(), image), image.size());
        EXPECT_EQ(pool_->stats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
        EXPECT_EQ(pool_->stats().pinned_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
        EXPECT_EQ(pin.bytes().front(), std::byte{2U});
    }
    EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
    EXPECT_EQ(pool_->stats().pinned_bytes, 0U);
}

TEST_F(BufferPoolTest, TypedSequentialCopyMissBypassesPinnedCapacityWithoutEvictionOrAllocation) {
    open(2U);
    auto memory = std::make_shared<FailingPageCacheResource>(pageCacheGrant(4U * RSS_BUFFER_POOL_PAGE_BYTES));
    pool_ = std::make_unique<BufferPool>(*io_, RSS_BUFFER_POOL_PAGE_BYTES, memory);
    auto pinned = pool_->pin({0U, 9U});
    const auto allocations = memory->calls;
    memory->fail_at = allocations + 1U;
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> image{};
    EXPECT_EQ(pool_->readPage({RSS_BUFFER_POOL_PAGE_BYTES, 9U}, image, BufferPoolReadHint::BPRH_SEQUENTIAL), image.size());
    EXPECT_EQ(image.front(), std::byte{2U});
    EXPECT_EQ(pinned.bytes().front(), std::byte{1U});
    EXPECT_EQ(memory->calls, allocations);
    EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
    EXPECT_EQ(pool_->stats().pinned_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
    EXPECT_EQ(pool_->stats().evictions, 0U);
}

TEST_F(BufferPoolTest, TypedSequentialCopyHitReadsDirtyMemoryAndRejectsUnpublishedPrivatePages) {
    open(2U); pool_ = createPool(2U);
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> changed{};
    changed.fill(std::byte{77U});
    pool_->writePage({0U, 11U}, changed);
    ASSERT_EQ(diskByte(0U), std::byte{1U});
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> image{};
    EXPECT_EQ(pool_->readPage({0U, 11U}, image, BufferPoolReadHint::BPRH_SEQUENTIAL), image.size());
    EXPECT_EQ(image, changed);
    auto unpublished = pool_->allocatePrivate({RSS_BUFFER_POOL_PAGE_BYTES, 12U});
    EXPECT_THROW(static_cast<void>(pool_->readPage(unpublished.key(), image, BufferPoolReadHint::BPRH_SEQUENTIAL)), std::logic_error);
    EXPECT_EQ(pool_->stats().dirty_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
}

TEST_F(BufferPoolTest, UntypedSequentialReadCoalescesMissesAndKeepsDirtyPartialHitsCoherent) {
    open(4U); pool_ = createPool(1U);
    const std::array<std::byte, 3U> patch{std::byte{71}, std::byte{72}, std::byte{73}};
    pool_->write(RSS_BUFFER_POOL_PAGE_BYTES + 7U, patch);
    ASSERT_EQ(diskByte(RSS_BUFFER_POOL_PAGE_BYTES + 7U), std::byte{2U});
    auto pin = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 0U});
    std::vector<std::byte> image(4U * RSS_BUFFER_POOL_PAGE_BYTES);
    const auto before = io_->stats();
    EXPECT_EQ(pool_->read(0U, image, BufferPoolReadHint::BPRH_SEQUENTIAL), image.size());
    EXPECT_EQ(image.front(), std::byte{1U});
    EXPECT_EQ(image.back(), std::byte{4U});
    EXPECT_EQ(image[RSS_BUFFER_POOL_PAGE_BYTES + 7U], patch[0]);
    EXPECT_EQ(image[RSS_BUFFER_POOL_PAGE_BYTES + 9U], patch[2]);
    EXPECT_EQ(pool_->stats().evictions, 0U);
    EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
    // Uncached low-level reads are reflected in BufferPool disk telemetry.
    EXPECT_EQ(pool_->stats().disk_read_bytes, 4U * RSS_BUFFER_POOL_PAGE_BYTES);
    std::array<std::byte, 2U> partial{};
    EXPECT_EQ(pool_->read(RSS_BUFFER_POOL_PAGE_BYTES + 8U, partial, BufferPoolReadHint::BPRH_SEQUENTIAL), partial.size());
    EXPECT_EQ(partial[0], patch[1]); EXPECT_EQ(partial[1], patch[2]);
    EXPECT_EQ(io_->stats().write_calls, before.write_calls);
}

TEST_F(BufferPoolTest, ScopedSequentialStorageReadsRestoreNormalAdmissionAndStayOnTheirThread) {
    open(2U); io_->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES);
    auto pin = io_->pinTypedPage({0U, 17U}); ASSERT_TRUE(pin.has_value());
    std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> image{};
    const BufferPoolPageKey missing{RSS_BUFFER_POOL_PAGE_BYTES, 17U};
    EXPECT_THROW(static_cast<void>(io_->readAt(missing, image.data(), image.size())), std::runtime_error);
    {
        StorageIO::SequentialReadScope scope(*io_);
        EXPECT_EQ(io_->readAt(missing, image.data(), image.size()), image.size());
        EXPECT_EQ(image.front(), std::byte{2U});
        bool otherThreadDenied = false;
        std::thread other([&] {
            std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> copy{};
            try { (void)io_->readAt(missing, copy.data(), copy.size()); }
            catch (const std::runtime_error &) { otherThreadDenied = true; }
        });
        other.join(); EXPECT_TRUE(otherThreadDenied);
        EXPECT_THROW(static_cast<void>(io_->pinTypedPage(missing)), std::runtime_error);
    }
    EXPECT_FALSE(StorageIO::SequentialReadScope::activeFor(*io_));
    EXPECT_THROW(static_cast<void>(io_->readAt(missing, image.data(), image.size())), std::runtime_error);
    pin.reset(); io_->disableBufferPool();
}

TEST_F(BufferPoolTest, ControlRecordWritesValidateRangesAndProtectPinnedImages) {
 open(2U); pool_ = createPool(2U);
 const BufferPoolPageKey key{0U, 1U};
 const std::array<std::byte, 1U> replacement{std::byte{0x5A}};
 EXPECT_THROW(pool_->writeControlRecord({0U, 0U}, 0U, replacement), std::invalid_argument);
 EXPECT_THROW(pool_->writeControlRecord({1U, 1U}, 0U, replacement), std::invalid_argument);
 EXPECT_THROW(pool_->writeControlRecord(key, RSS_BUFFER_POOL_PAGE_BYTES, replacement), std::invalid_argument);
 EXPECT_THROW(pool_->writeControlRecord(key, 0U, {}), std::invalid_argument);
 {
  auto pinned = pool_->pin(key);
  EXPECT_THROW(pool_->writeControlRecord(key, 64U, replacement), std::logic_error);
  EXPECT_EQ(pinned.bytes()[64U], std::byte{1U});
 }
 pool_->writeControlRecord(key, 64U, replacement);
 EXPECT_EQ(pool_->stats().resident_bytes, 0U);
 EXPECT_EQ(diskByte(64U), replacement.front());
 EXPECT_EQ(diskByte(63U), std::byte{1U});
}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST_F(BufferPoolTest, FailedControlRecordWriteLeavesNoDirtyCachedImageToOverwriteTheSurvivor) {
 open(2U); pool_ = createPool(2U);
 const BufferPoolPageKey key{0U, 1U};
 BufferPool::PageBytes dirty{}; dirty.fill(std::byte{0x11});
 pool_->writePage(key, dirty);
 std::array<std::byte, 16U> replacement{}; replacement.fill(std::byte{0x5A});
 {
  StorageIO::ScopedNativeWriteHookForTesting shortWrite(
   [](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) -> pwrite_ssize_t {
    return portable_pwrite(fd, bytes, count == 16U ? 8U : count, offset);
   });
  EXPECT_THROW(pool_->writeControlRecord(key, 64U, replacement), std::system_error);
 }
 EXPECT_EQ(pool_->stats().resident_bytes, 0U);
 pool_->flush();
 BufferPool::PageBytes observed{};
 ASSERT_EQ(pool_->readPage(key, observed), observed.size());
 for (size_t index = 0U; index < observed.size(); ++index)
  EXPECT_EQ(observed[index], index >= 64U && index < 72U ? std::byte{0x5A} : std::byte{0x11});
}
#endif

TEST_F(BufferPoolTest, NormalReadsBypassDeniedResidencyAndPreservePinnedDirtyHits) {
    open(2U);
    auto memory = pageCacheGrant(RSS_BUFFER_POOL_PAGE_BYTES + 1024U);
    pool_ = std::make_unique<BufferPool>(*io_, 2U * RSS_BUFFER_POOL_PAGE_BYTES, memory);
    std::array<std::byte, 1U> first{};
    ASSERT_EQ(pool_->read(0U, first), first.size());
    const std::array<std::byte, 1U> changed{std::byte{0x5A}};
    pool_->write(0U, changed);
    {
        auto pin = pool_->pin({0U, 0U});
        const auto used = memory->used();
        std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> observed{};
        ASSERT_EQ(pool_->read(0U, observed), observed.size());
        EXPECT_EQ(observed.front(), changed.front());
        EXPECT_EQ(observed[RSS_BUFFER_POOL_PAGE_BYTES], std::byte{2});
        EXPECT_EQ(pin.bytes().front(), changed.front());
        BufferPool::PageBytes typed{};
        ASSERT_EQ(pool_->readPage({RSS_BUFFER_POOL_PAGE_BYTES, 7U}, typed), typed.size());
        EXPECT_EQ(typed.front(), std::byte{2});
        ASSERT_EQ(pool_->readPages({RSS_BUFFER_POOL_PAGE_BYTES, 7U}, typed), typed.size());
        EXPECT_EQ(typed.front(), std::byte{2});
        EXPECT_EQ(memory->used(), used);
        EXPECT_EQ(pool_->stats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
        EXPECT_EQ(pool_->stats().evictions, 0U);
    }
    pool_.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BufferPoolTest, SiblingCacheReclamationNeverFlushesDirtyPrivateOrPinnedPages) {
 open(4U); pool_ = createPool(4U);
 {
  auto page = pool_->allocatePrivate({0U, 17U});
  page.mutableBytes()[0] = std::byte{0xA1}; page.markDirty();
 }
 std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> dirty{}; dirty.fill(std::byte{0xA2});
 pool_->writePage({RSS_BUFFER_POOL_PAGE_BYTES, 19U}, dirty);
 auto pinned = pool_->pin({2U * RSS_BUFFER_POOL_PAGE_BYTES, 1U});
 { auto clean = pool_->pin({3U * RSS_BUFFER_POOL_PAGE_BYTES, 1U}); }
 const auto writes = io_->stats().write_bytes;
 EXPECT_FALSE(pool_->tryReleaseCachedPage(nullptr));
 EXPECT_TRUE(pool_->tryReleaseCachedPage(std::pmr::get_default_resource()));
 EXPECT_FALSE(pool_->tryReleaseCachedPage(std::pmr::get_default_resource()));
 EXPECT_EQ(pool_->stats().evictions, 1U);
 EXPECT_EQ(pool_->stats().dirty_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
 EXPECT_EQ(io_->stats().write_bytes, writes);
 EXPECT_EQ(diskByte(0U), std::byte{1U});
 EXPECT_EQ(diskByte(RSS_BUFFER_POOL_PAGE_BYTES), std::byte{2U});
 EXPECT_EQ(pinned.bytes()[0], std::byte{3U});
}

TEST_F(BufferPoolTest, LocalWriteAndFlushWorkStayBoundedAsCleanResidencyGrows) {
    open(2048U);
    const bool collected = graph::debug::PerfTrace::areValuesEnabled();
    struct Restore {
        bool enabled;
        ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); }
    } restore{collected};
    graph::debug::PerfTrace::setValueCollectionEnabled(true);
    for (const size_t pages : {32U, 512U, 2048U}) {
        SCOPED_TRACE(pages);
        pool_ = createPool(pages);
        for (size_t page = 0U; page < pages; ++page) {
            auto pin = pool_->pin({page * RSS_BUFFER_POOL_PAGE_BYTES, 1U});
        }
        graph::debug::PerfTrace::reset();
        BufferPool::PageBytes replacement{}; replacement.fill(std::byte{0x57});
        pool_->writePage({0U, 2U}, replacement);
        pool_->flush();
        const std::array control{std::byte{0x19}};
        pool_->writeControlRecord({0U, 2U}, 8U, control);
        const auto counters = graph::debug::PerfTrace::snapshotAndReset();
        ASSERT_TRUE(counters.contains("storage.buffer_pool.generation_checked_entries"));
        EXPECT_EQ(counters.at("storage.buffer_pool.generation_checked_entries").totalValue, 1);
        EXPECT_EQ(counters.at("storage.buffer_pool.range_erase_visited_entries").totalValue, 2);
        EXPECT_EQ(counters.at("storage.buffer_pool.range_checked_entries").totalValue, 1);
        EXPECT_EQ(counters.at("storage.buffer_pool.flush_visited_entries").totalValue, 1);
        EXPECT_EQ(pool_->stats().resident_bytes, (pages - 1U) * RSS_BUFFER_POOL_PAGE_BYTES);
        EXPECT_EQ(diskByte(0U), std::byte{0x57});
        EXPECT_EQ(diskByte(8U), std::byte{0x19});
        RecordProperty("largest_clean_residency", static_cast<int>(pages));
        const auto suffix = std::to_string(pages);
        RecordProperty("generation_checks_" + suffix, std::to_string(counters.at("storage.buffer_pool.generation_checked_entries").totalValue));
        RecordProperty("erase_visits_" + suffix, std::to_string(counters.at("storage.buffer_pool.range_erase_visited_entries").totalValue));
        RecordProperty("range_checks_" + suffix, std::to_string(counters.at("storage.buffer_pool.range_checked_entries").totalValue));
        RecordProperty("flush_visits_" + suffix, std::to_string(counters.at("storage.buffer_pool.flush_visited_entries").totalValue));
    }
}

TEST_F(BufferPoolTest, OffsetInvalidationPrechecksTheWholeOverlappingRangeBeforeWritingDirtyData) {
    open(3U); pool_ = createPool(3U);
    BufferPool::PageBytes replacement{}; replacement.fill(std::byte{0x42});
    pool_->writePage({0U, 1U}, replacement);
    auto pin = pool_->pin({RSS_BUFFER_POOL_PAGE_BYTES, 1U});
    const auto before = pool_->stats();
    EXPECT_THROW(pool_->invalidateRange(RSS_BUFFER_POOL_PAGE_BYTES - 4U, 5U), std::logic_error);
    EXPECT_EQ(pool_->stats().dirty_bytes, before.dirty_bytes);
    EXPECT_EQ(pool_->stats().disk_write_bytes, before.disk_write_bytes);
    EXPECT_EQ(diskByte(0U), std::byte{1});
    // The exclusive end at the next page must not visit its pinned image.
    EXPECT_NO_THROW(pool_->invalidateRange(RSS_BUFFER_POOL_PAGE_BYTES - 4U, 4U));
    EXPECT_EQ(diskByte(0U), std::byte{0x42});
    EXPECT_EQ(pin.bytes()[0U], std::byte{2});
}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST_F(BufferPoolTest, FailedFlushRetainsDirtyChainForAnExactRetry) {
    open(3U); pool_ = createPool(3U);
    BufferPool::PageBytes first{}, second{}; first.fill(std::byte{0x31}); second.fill(std::byte{0x63});
    pool_->writePage({0U, 1U}, first);
    pool_->writePage({RSS_BUFFER_POOL_PAGE_BYTES, 1U}, second);
    bool failed = false;
    {
        StorageIO::ScopedNativeWriteHookForTesting shortWrite([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
            if (!failed) { failed = true; return portable_pwrite(fd, bytes, 8U, offset); }
            return portable_pwrite(fd, bytes, count, offset);
        });
        EXPECT_THROW(pool_->flush(), std::system_error);
    }
    ASSERT_TRUE(failed);
    EXPECT_EQ(pool_->stats().dirty_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);
    EXPECT_NO_THROW(pool_->flush());
    EXPECT_EQ(pool_->stats().dirty_bytes, 0U);
    EXPECT_EQ(diskByte(0U), std::byte{0x31});
    EXPECT_EQ(diskByte(RSS_BUFFER_POOL_PAGE_BYTES), std::byte{0x63});
    const auto written = pool_->stats().disk_write_bytes;
    EXPECT_NO_THROW(pool_->flush());
    EXPECT_EQ(pool_->stats().disk_write_bytes, written);
}
#endif
