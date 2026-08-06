/**
 * @file test_BufferPool.cpp
 * @brief Unit tests for the native generation-aware 4 KiB Buffer Pool.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
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
	EXPECT_FALSE(readOnlyIo->isWritable());
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
