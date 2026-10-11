/**
 * @file test_StorageIO.cpp
 * @brief Unit tests for StorageIO unified I/O abstraction
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/debug/PerfTrace.hpp"

using namespace graph::storage;

namespace graph::storage {

	class StorageIOTestAccess final {
	public:
		static void saturatingAdd(std::atomic<uint64_t> &counter, uint64_t value) noexcept {
			StorageIO::saturatingAdd(counter, value);
		}

		static void writeAtUncached(StorageIO &io, uint64_t offset, const void *buffer, size_t size) {
			io.writeAtUncached(offset, buffer, size);
		}

		[[nodiscard]] static size_t readAtUncached(const StorageIO &io, uint64_t offset, void *buffer, size_t size) {
			return io.readAtUncached(offset, buffer, size);
		}

		[[nodiscard]] static bool isWritable(const StorageIO &io) { return io.isWritable(); }
		[[nodiscard]] static std::shared_ptr<std::fstream> stream(const StorageIO &io) { return io.getStream(); }
		[[nodiscard]] static file_handle_t writeHandle(const StorageIO &io) { return io.getWriteFd(); }
		static void setWriteHandle(StorageIO &io, file_handle_t handle) { io.setWriteFd(handle); }
	};

} // namespace graph::storage

class StorageIOTest : public ::testing::Test {
protected:
	std::string testFile_ = "test_storage_io.bin";
	const size_t testSize_ = 4096;

	void SetUp() override {
		// Create test file with zeros
		std::ofstream file(testFile_, std::ios::binary);
		std::vector<char> data(testSize_, 0);
		file.write(data.data(), static_cast<std::streamsize>(testSize_));
		file.close();
	}

	void TearDown() override {
		for (const file_handle_t readFd: read_handles_) {
			static_cast<void>(portable_close(readFd));
		}
		for (const file_handle_t writeFd: write_handles_) {
			static_cast<void>(portable_close_rw(writeFd));
		}
		std::error_code ec;
		std::filesystem::remove(testFile_, ec);
	}

	std::shared_ptr<StorageIO> createIO(bool withNativeFds = true) {
		auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
		EXPECT_TRUE(stream->good());

		file_handle_t wfd = INVALID_FILE_HANDLE;
		file_handle_t rfd = INVALID_FILE_HANDLE;
		if (withNativeFds) {
			wfd = portable_open_rw(testFile_.c_str());
			rfd = portable_open(testFile_.c_str(), O_RDONLY);
			EXPECT_NE(wfd, INVALID_FILE_HANDLE);
			EXPECT_NE(rfd, INVALID_FILE_HANDLE);
			if (wfd != INVALID_FILE_HANDLE) {
				write_handles_.push_back(wfd);
			}
			if (rfd != INVALID_FILE_HANDLE) {
				read_handles_.push_back(rfd);
			}
		}

		return std::make_shared<StorageIO>(stream, wfd, rfd);
	}

	std::vector<file_handle_t> write_handles_;
	std::vector<file_handle_t> read_handles_;
};

// ============================================================================
// writeAt / readAt roundtrip with native fds (pwrite/pread)
// ============================================================================

TEST_F(StorageIOTest, WriteAtReadAtRoundtrip_NativeFd) {
	auto io = createIO(/*withNativeFds=*/true);
	ASSERT_TRUE(io->hasPwriteSupport());
	ASSERT_TRUE(io->hasPreadSupport());

	const char data[] = "Hello StorageIO!";
	const bool profileEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	io->writeAt(100, data, sizeof(data));
	const auto writeTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(profileEnabled);
	ASSERT_TRUE(writeTrace.contains("storage_io.completed_write_bytes"));
	ASSERT_TRUE(writeTrace.contains("storage_io.completed_write_calls"));
	EXPECT_EQ(writeTrace.at("storage_io.completed_write_bytes").totalValue, static_cast<int64_t>(sizeof(data)));
	EXPECT_EQ(writeTrace.at("storage_io.completed_write_calls").totalValue, 1);

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(100, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
	const StorageIOStats stats = io->stats();
	EXPECT_EQ(stats.write_bytes, sizeof(data));
	EXPECT_EQ(stats.read_bytes, sizeof(data));
	EXPECT_EQ(stats.write_calls, 1U);
	EXPECT_EQ(stats.read_calls, 1U);
	io->resetStats();
	EXPECT_EQ(io->stats(), StorageIOStats{});
}

TEST_F(StorageIOTest, CounterSaturationNeverWraps) {
	std::atomic<uint64_t> counter{std::numeric_limits<uint64_t>::max()};
	StorageIOTestAccess::saturatingAdd(counter, 1U);
	EXPECT_EQ(counter.load(std::memory_order_relaxed), std::numeric_limits<uint64_t>::max());

	counter.store(std::numeric_limits<uint64_t>::max() - 1U, std::memory_order_relaxed);
	StorageIOTestAccess::saturatingAdd(counter, 2U);
	EXPECT_EQ(counter.load(std::memory_order_relaxed), std::numeric_limits<uint64_t>::max());
}

TEST_F(StorageIOTest, ConstructorRejectsUnknownAccessMode) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());

	EXPECT_THROW(static_cast<void>(std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE,
															   static_cast<StorageIOAccessMode>(0U))),
				 std::invalid_argument);
}

TEST_F(StorageIOTest, ExplicitAccessModeAccuratelyReportsWriteCapability) {
	auto writable = createIO(/*withNativeFds=*/false);
	EXPECT_TRUE(StorageIOTestAccess::isWritable(*writable));

	auto readOnlyStream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in);
	ASSERT_TRUE(readOnlyStream->is_open());
	StorageIO readOnly(std::move(readOnlyStream), INVALID_FILE_HANDLE, INVALID_FILE_HANDLE,
					   StorageIOAccessMode::SIOAM_READ_ONLY);
	EXPECT_FALSE(StorageIOTestAccess::isWritable(readOnly));
}

TEST_F(StorageIOTest, RetainsItsStreamAndNativeWriteHandleLifecycle) {
	auto io = createIO(/*withNativeFds=*/true);
	ASSERT_NE(StorageIOTestAccess::stream(*io), nullptr);
	EXPECT_TRUE(StorageIOTestAccess::stream(*io)->is_open());
	const file_handle_t handle = StorageIOTestAccess::writeHandle(*io);
	ASSERT_NE(handle, INVALID_FILE_HANDLE);

	// The backend can retain the same native descriptor after a stream rebase.
	StorageIOTestAccess::setWriteHandle(*io, handle);
	EXPECT_EQ(StorageIOTestAccess::writeHandle(*io), handle);
}

TEST_F(StorageIOTest, PositionedOperationsRejectUnaddressableRangesBeforeIo) {
	auto io = createIO(true);
	const char byte = 0;
	char output = 0;

	EXPECT_THROW(io->writeAt(std::numeric_limits<uint64_t>::max(), &byte, 1U), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(io->readAt(std::numeric_limits<uint64_t>::max(), &output, 1U)),
				 std::invalid_argument);
	const uint64_t unrepresentableOffset = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U;
	EXPECT_THROW(io->writeAt(unrepresentableOffset, &byte, 1U), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(io->readAt(unrepresentableOffset, &output, 1U)), std::invalid_argument);
	EXPECT_THROW(io->writeAt(0U, &byte, std::numeric_limits<size_t>::max()), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(io->readAt(0U, &output, std::numeric_limits<size_t>::max())), std::invalid_argument);
}

TEST_F(StorageIOTest, TypedPagePinsCountDistinctPhysicalIdentitiesOnlyAfterSuccessfulPin) {
	auto io = createIO(true);
	io->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> bytes{};
	io->writePages({0U, 3U}, bytes.data(), bytes.size());
	using graph::debug::PerfTrace;
	struct Cleanup { ~Cleanup() { PerfTrace::setEnabled(false); PerfTrace::reset(); } } cleanup;
	PerfTrace::setEnabled(false); PerfTrace::setValueCollectionEnabled(true); PerfTrace::reset();
	{
		auto first = io->pinTypedPage({0U, 3U});
		ASSERT_TRUE(first.has_value());
	}
	{
		auto repeated = io->pinTypedPage({0U, 3U});
		ASSERT_TRUE(repeated.has_value());
	}
	EXPECT_EQ(io->readPages({0U, 3U}, bytes.data(), bytes.size()), bytes.size());
	EXPECT_EQ(io->readAt(BufferPoolPageKey{RSS_BUFFER_POOL_PAGE_BYTES, 3U},
		bytes.data(), RSS_BUFFER_POOL_PAGE_BYTES), RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_THROW(static_cast<void>(io->pinTypedPage({1U, 3U})), std::invalid_argument);
	const auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_EQ(snapshot.at("native_graph.read.distinct_page_pins").totalValue, 2);
	EXPECT_EQ(snapshot.at("native_graph.read.distinct_page_pins_incomplete").totalValue, 0);
}

TEST_F(StorageIOTest, TypedPageRangesRejectMalformedInputsAndRoundTripThroughTheCache) {
	auto io = createIO(true);
	io->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> written{};
	std::array<std::byte, 2U * RSS_BUFFER_POOL_PAGE_BYTES> observed{};
	written.fill(std::byte{0xC3U});

	EXPECT_THROW(io->writePages({0U, 0U}, written.data(), written.size()), std::invalid_argument);
	EXPECT_THROW(io->writePages({1U, 3U}, written.data(), written.size()), std::invalid_argument);
	EXPECT_THROW(io->writePages({0U, 3U}, nullptr, written.size()), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(io->readPages({0U, 3U}, observed.data(), RSS_BUFFER_POOL_PAGE_BYTES - 1U)),
				 std::invalid_argument);

	io->writePages({0U, 3U}, written.data(), written.size());
	EXPECT_EQ(io->readPages({0U, 3U}, observed.data(), observed.size(), BufferPoolReadHint::BPRH_SEQUENTIAL),
			  observed.size());
	EXPECT_EQ(observed, written);
	io->sync();
}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST_F(StorageIOTest, TypedPageRangeShortNativeWriteFailsClosed) {
	auto io = createIO(true);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> page{};
	StorageIO::ScopedNativeWriteHookForTesting shortWrite(
			[](file_handle_t, const void *, size_t size, pwrite_off_t) -> pwrite_ssize_t {
				return static_cast<pwrite_ssize_t>(size - 1U);
			});

	EXPECT_THROW(io->writePages({0U, 9U}, page.data(), page.size()), std::system_error);
}

TEST_F(StorageIOTest, TypedPageRangePreservesShortAndFailedReadSemantics) {
	auto io = createIO(true);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> page{};
	{
		StorageIO::ScopedNativeReadHookForTesting shortRead(
				[](file_handle_t, void *, size_t size, pwrite_off_t) -> pwrite_ssize_t {
					return static_cast<pwrite_ssize_t>(size - 1U);
				});
		EXPECT_EQ(io->readPages({0U, 9U}, page.data(), page.size()), page.size() - 1U);
	}
	{
		StorageIO::ScopedNativeReadHookForTesting failedRead(
				[](file_handle_t, void *, size_t, pwrite_off_t) -> pwrite_ssize_t { return -1; });
		EXPECT_THROW(static_cast<void>(io->readPages({0U, 9U}, page.data(), page.size())), std::runtime_error);
	}
}

TEST_F(StorageIOTest, TypedPageWriteThroughDropsCachedImageBeforeShortNativeWrite) {
	auto io = createIO(true);
	io->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> page{};
	page.fill(std::byte{0xD4U});
	io->writePages({0U, 7U}, page.data(), page.size());
	io->sync();
	ASSERT_EQ(io->bufferPoolStats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	StorageIO::ScopedNativeWriteHookForTesting shortWrite(
			[](file_handle_t, const void *, size_t size, pwrite_off_t) -> pwrite_ssize_t {
				return static_cast<pwrite_ssize_t>(size - 1U);
			});

	EXPECT_THROW(io->writePagesThrough({0U, 8U}, page.data(), page.size()), std::system_error);
	EXPECT_EQ(io->bufferPoolStats().resident_bytes, 0U);
	EXPECT_EQ(io->bufferPoolStats().dirty_bytes, 0U);
}
#endif

TEST_F(StorageIOTest, UncachedSafetyGuardsRejectEmptyBuffersBeforeNativeIo) {
	auto io = createIO(true);
	char byte = 0;

	StorageIOTestAccess::writeAtUncached(*io, 0U, nullptr, 1U);
	StorageIOTestAccess::writeAtUncached(*io, 0U, &byte, 0U);
	EXPECT_EQ(StorageIOTestAccess::readAtUncached(*io, 0U, nullptr, 1U), 0U);
	EXPECT_EQ(StorageIOTestAccess::readAtUncached(*io, 0U, &byte, 0U), 0U);
	EXPECT_EQ(io->stats(), StorageIOStats{});
}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST_F(StorageIOTest, NativeFaultHooksRejectEmptyAndNestedScopes) {
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeWriteHookForTesting(StorageIO::NativeWriteHookForTesting{})),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(
						 StorageIO::ScopedAllThreadNativeWriteHookForTesting(StorageIO::NativeWriteHookForTesting{})),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeSyncHookForTesting(StorageIO::NativeSyncHookForTesting{})),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(
						 StorageIO::ScopedAllThreadNativeSyncHookForTesting(StorageIO::NativeSyncHookForTesting{})),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeReadHookForTesting(StorageIO::NativeReadHookForTesting{})),
				 std::invalid_argument);
	EXPECT_THROW(
			static_cast<void>(StorageIO::ScopedNativeTruncateHookForTesting(StorageIO::NativeTruncateHookForTesting{})),
			std::invalid_argument);

	const StorageIO::NativeWriteHookForTesting writeHook = [](file_handle_t, const void *, size_t,
															  pwrite_off_t) -> pwrite_ssize_t { return 0; };
	StorageIO::ScopedNativeWriteHookForTesting activeWriteHook(writeHook);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeWriteHookForTesting(writeHook)), std::logic_error);
	StorageIO::ScopedAllThreadNativeWriteHookForTesting activeAllThreadWriteHook(writeHook);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedAllThreadNativeWriteHookForTesting(writeHook)), std::logic_error);

	const StorageIO::NativeSyncHookForTesting syncHook = [](file_handle_t) { return 0; };
	StorageIO::ScopedNativeSyncHookForTesting activeSyncHook(syncHook);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeSyncHookForTesting(syncHook)), std::logic_error);
	StorageIO::ScopedAllThreadNativeSyncHookForTesting activeAllThreadSyncHook(syncHook);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedAllThreadNativeSyncHookForTesting(syncHook)), std::logic_error);

	const StorageIO::NativeReadHookForTesting readHook = [](file_handle_t, void *, size_t,
															pwrite_off_t) -> pwrite_ssize_t { return 0; };
	StorageIO::ScopedNativeReadHookForTesting activeReadHook(readHook);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeReadHookForTesting(readHook)), std::logic_error);

	const StorageIO::NativeTruncateHookForTesting truncateHook = [](file_handle_t, uint64_t) { return 0; };
	StorageIO::ScopedNativeTruncateHookForTesting activeTruncateHook(truncateHook);
	EXPECT_THROW(static_cast<void>(StorageIO::ScopedNativeTruncateHookForTesting(truncateHook)), std::logic_error);
}

TEST_F(StorageIOTest, ThreadLocalWriteHookTakesPrecedenceOverAllThreadWriteHook) {
	auto io = createIO(true);
	const char first = 'a';
	const char second = 'b';
	std::atomic<uint32_t> allThreadCalls{0U};
	std::atomic<uint32_t> localCalls{0U};
	StorageIO::ScopedAllThreadNativeWriteHookForTesting allThreadHook(
			[&allThreadCalls](file_handle_t fd, const void *bytes, size_t size, pwrite_off_t offset) {
				allThreadCalls.fetch_add(1U, std::memory_order_relaxed);
				return portable_pwrite(fd, bytes, size, offset);
			});
	{
		StorageIO::ScopedNativeWriteHookForTesting localHook(
				[&localCalls](file_handle_t fd, const void *bytes, size_t size, pwrite_off_t offset) {
					localCalls.fetch_add(1U, std::memory_order_relaxed);
					return portable_pwrite(fd, bytes, size, offset);
				});
		io->writeAt(0U, &first, 1U);
	}
	io->writeAt(1U, &second, 1U);
	EXPECT_EQ(localCalls.load(std::memory_order_relaxed), 1U);
	EXPECT_EQ(allThreadCalls.load(std::memory_order_relaxed), 1U);
}

TEST_F(StorageIOTest, ThreadLocalSyncHookTakesPrecedenceOverAllThreadSyncHook) {
	auto io = createIO(true);
	std::atomic<uint32_t> allThreadCalls{0U};
	std::atomic<uint32_t> localCalls{0U};
	StorageIO::ScopedAllThreadNativeSyncHookForTesting allThreadHook([&allThreadCalls](file_handle_t fd) {
		allThreadCalls.fetch_add(1U, std::memory_order_relaxed);
		return portable_fsync(fd);
	});
	{
		StorageIO::ScopedNativeSyncHookForTesting localHook([&localCalls](file_handle_t fd) {
			localCalls.fetch_add(1U, std::memory_order_relaxed);
			return portable_fsync(fd);
		});
		io->sync();
	}
	io->sync();
	EXPECT_EQ(localCalls.load(std::memory_order_relaxed), 1U);
	EXPECT_EQ(allThreadCalls.load(std::memory_order_relaxed), 1U);
}

TEST_F(StorageIOTest, NativeWriteFailureWithoutPlatformErrorUsesGenericIoError) {
	auto io = createIO(true);
	const char byte = 0;
	StorageIO::ScopedNativeWriteHookForTesting writeFailure(
			[](file_handle_t, const void *, size_t, pwrite_off_t) -> pwrite_ssize_t {
#ifdef _WIN32
				SetLastError(ERROR_SUCCESS);
#else
				errno = 0;
#endif
				return -1;
			});

	EXPECT_THROW(io->writeAt(0U, &byte, 1U), std::system_error);
}
#endif

// ============================================================================
// writeAt / readAt roundtrip with fstream fallback
// ============================================================================

TEST_F(StorageIOTest, WriteAtReadAtRoundtrip_FstreamFallback) {
	auto io = createIO(/*withNativeFds=*/false);
	ASSERT_FALSE(io->hasPwriteSupport());
	ASSERT_FALSE(io->hasPreadSupport());

	const char data[] = "Fstream fallback!";
	io->writeAt(200, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(200, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

TEST_F(StorageIOTest, FstreamFallbackSerializesConcurrentPositionedReadsAndWrites) {
	auto io = createIO(/*withNativeFds=*/false);
	const std::array<char, 64> stableBytes = [] {
		std::array<char, 64> bytes{};
		bytes.fill(static_cast<char>(0x5A));
		return bytes;
	}();
	io->writeAt(0, stableBytes.data(), stableBytes.size());
	io->sync();

	std::barrier start{2};
	std::atomic<bool> stableRead{true};
	std::exception_ptr workerFailure;
	std::mutex failureMutex;
	const auto recordFailure = [&](std::exception_ptr error) {
		std::lock_guard lock(failureMutex);
		if (!workerFailure) {
			workerFailure = std::move(error);
		}
	};
	std::thread writer([&] {
		try {
			start.arrive_and_wait();
			std::array<char, 64> bytes{};
			for (uint32_t iteration = 0; iteration < 512U; ++iteration) {
				bytes.fill(static_cast<char>(iteration));
				io->writeAt(512, bytes.data(), bytes.size());
			}
		} catch (...) {
			recordFailure(std::current_exception());
		}
	});
	std::thread reader([&] {
		try {
			start.arrive_and_wait();
			std::array<char, 64> bytes{};
			for (uint32_t iteration = 0; iteration < 512U; ++iteration) {
				if (io->readAt(0, bytes.data(), bytes.size()) != bytes.size() || bytes != stableBytes) {
					stableRead.store(false, std::memory_order_relaxed);
					break;
				}
			}
		} catch (...) {
			recordFailure(std::current_exception());
		}
	});
	writer.join();
	reader.join();

	EXPECT_EQ(workerFailure, nullptr);
	EXPECT_TRUE(stableRead.load(std::memory_order_relaxed));
}

// ============================================================================
// writeAt at non-zero offset
// ============================================================================

TEST_F(StorageIOTest, WriteAtMiddleOfFile) {
	auto io = createIO(true);

	const uint64_t offset = 2048;
	const char data[] = "middle write";
	io->writeAt(offset, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(offset, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);

	// Verify beginning is still zeros
	char zeroBuf[16] = {};
	char expected[16] = {};
	std::memset(expected, 0, sizeof(expected));
	io->readAt(0, zeroBuf, sizeof(zeroBuf));
	EXPECT_EQ(std::memcmp(zeroBuf, expected, sizeof(expected)), 0);
}

// ============================================================================
// append returns correct offset
// ============================================================================

TEST_F(StorageIOTest, AppendReturnsCorrectOffset) {
	auto io = createIO(false); // append uses fstream

	const char data1[] = "first append";
	uint64_t offset1 = io->append(data1, sizeof(data1));
	EXPECT_EQ(offset1, testSize_); // Should be at end of existing file

	const char data2[] = "second append";
	uint64_t offset2 = io->append(data2, sizeof(data2));
	EXPECT_EQ(offset2, testSize_ + sizeof(data1));

	// Verify data can be read back
	io->flushStream(); // Ensure fstream writes are visible to reads
	char buffer[sizeof(data1)] = {};
	size_t bytesRead = io->readAt(offset1, buffer, sizeof(data1));
	EXPECT_EQ(bytesRead, sizeof(data1));
	EXPECT_EQ(std::memcmp(data1, buffer, sizeof(data1)), 0);
}

TEST_F(StorageIOTest, ReserveAppendSpaceExtendsWithZeroReadableGap) {
	auto io = createIO(false);

	uint64_t offset = io->reserveAppendSpace(128);
	EXPECT_EQ(offset, testSize_);
	EXPECT_EQ(std::filesystem::file_size(testFile_), testSize_ + 128);

	std::vector<char> buffer(128, 1);
	size_t bytesRead = io->readAt(offset, buffer.data(), buffer.size());
	EXPECT_EQ(bytesRead, buffer.size());
	EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(), [](char c) { return c == 0; }));

	const char data[] = "after reserve";
	uint64_t appendOffset = io->append(data, sizeof(data));
	EXPECT_EQ(appendOffset, testSize_ + 128);
	const StorageIOStats stats = io->stats();
	EXPECT_EQ(stats.reserved_bytes, 128U);
	EXPECT_EQ(stats.reserve_calls, 1U);
	EXPECT_EQ(stats.write_bytes, 1U + sizeof(data));
}

TEST_F(StorageIOTest, ReserveAppendSpaceNativeExtendsWithZeroReadableGap) {
	auto io = createIO(true);

	uint64_t offset = io->reserveAppendSpace(128);
	EXPECT_EQ(offset, testSize_);
	EXPECT_EQ(std::filesystem::file_size(testFile_), testSize_ + 128);

	std::vector<char> buffer(128, 1);
	size_t bytesRead = io->readAt(offset, buffer.data(), buffer.size());
	EXPECT_EQ(bytesRead, buffer.size());
	EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(), [](char c) { return c == 0; }));
}

TEST_F(StorageIOTest, FileSizeUsesNativeAndFstreamPaths) {
	auto nativeIo = createIO(true);
	EXPECT_EQ(nativeIo->fileSize(), testSize_);
	nativeIo->reserveAppendSpace(128);
	EXPECT_EQ(nativeIo->fileSize(), testSize_ + 128U);

	auto streamIo = createIO(false);
	EXPECT_EQ(streamIo->fileSize(), testSize_ + 128U);
}

TEST_F(StorageIOTest, SequentialReadTracksBytesWithoutUsingThePageCache) {
	auto io = createIO(true);
	const std::array<char, 4U> expected{'z', 'y', 'x', '\0'};
	io->writeAt(128U, expected.data(), expected.size());

	std::array<char, 4U> actual{};
	EXPECT_EQ(io->readAtSequential(128U, actual.data(), actual.size()), actual.size());
	EXPECT_EQ(actual, expected);
	EXPECT_EQ(io->stats().read_bytes, actual.size());
	EXPECT_EQ(io->stats().read_calls, 1U);
}

TEST_F(StorageIOTest, SequentialReadTreatsEmptyRequestsAsNoOps) {
	auto io = createIO(true);
	char byte = 0;

	EXPECT_EQ(io->readAtSequential(0U, nullptr, 1U), 0U);
	EXPECT_EQ(io->readAtSequential(0U, &byte, 0U), 0U);
	EXPECT_EQ(io->stats(), StorageIOStats{});
}

TEST_F(StorageIOTest, TypedPageOperationsSupportDirectNativeIoAndRejectNullReads) {
	auto io = createIO(true);
	const BufferPoolPageKey key{0U, 1U};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> expected{};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> actual{};
	expected.fill(std::byte{0x4BU});

	EXPECT_THROW(io->writeAt(key, nullptr, expected.size()), std::invalid_argument);
	io->writeAt(key, expected.data(), expected.size());
	EXPECT_EQ(io->readAt(key, actual.data(), actual.size()), actual.size());
	EXPECT_EQ(actual, expected);
	EXPECT_THROW(static_cast<void>(io->readAt(key, nullptr, expected.size())), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(io->readPages(key, nullptr, expected.size())), std::invalid_argument);
}

TEST_F(StorageIOTest, TypedPageWriteThroughSupportsDirectNativeIoAndRejectsNullBuffer) {
	auto io = createIO(true);
	const BufferPoolPageKey key{0U, 1U};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> expected{};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> actual{};
	expected.fill(std::byte{0xB4U});

	EXPECT_THROW(io->writePagesThrough(key, nullptr, expected.size()), std::invalid_argument);
	io->writePagesThrough(key, expected.data(), expected.size());
	EXPECT_EQ(io->readPages(key, actual.data(), actual.size()), actual.size());
	EXPECT_EQ(actual, expected);
}

TEST_F(StorageIOTest, TruncateShrinksNativeFilesAndRejectsExtension) {
	auto io = createIO(true);
	const char data[] = "tail";
	io->writeAt(3072U, data, sizeof(data));

	EXPECT_NO_THROW(io->truncate(2048U));
	EXPECT_EQ(io->fileSize(), 2048U);
	EXPECT_NO_THROW(io->truncate(2048U));
	EXPECT_THROW(io->truncate(2049U), std::invalid_argument);
}

TEST_F(StorageIOTest, TruncateRejectsFstreamOnlyIo) {
	auto io = createIO(false);
	EXPECT_THROW(io->truncate(testSize_ - 1U), std::runtime_error);
}

TEST_F(StorageIOTest, TruncateFlushesAndInvalidatesCachedTailPages) {
	auto io = createIO(true);
	const size_t pageSize = RSS_BUFFER_POOL_PAGE_BYTES;
	ASSERT_GE(testSize_, pageSize);
	ASSERT_EQ(io->reserveAppendSpace(pageSize), testSize_);
	io->enableBufferPool(2U * pageSize);
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> firstPage{};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> tailPage{};
	firstPage.fill(std::byte{0x11U});
	tailPage.fill(std::byte{0x22U});
	io->writeAt({0U, 1U}, firstPage.data(), firstPage.size());
	io->writeAt({static_cast<uint64_t>(pageSize), 1U}, tailPage.data(), tailPage.size());
	ASSERT_EQ(io->bufferPoolStats().resident_bytes, 2U * pageSize);

	io->truncate(pageSize);
	EXPECT_EQ(io->fileSize(), pageSize);
	EXPECT_EQ(io->bufferPoolStats().resident_bytes, pageSize);
	EXPECT_EQ(io->bufferPoolStats().dirty_bytes, 0U);
}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST_F(StorageIOTest, TruncateAndSyncSurfaceNativeDurabilityFailures) {
	auto io = createIO(true);
	{
		StorageIO::ScopedNativeTruncateHookForTesting failedTruncate([](file_handle_t, uint64_t) {
#ifdef _WIN32
			SetLastError(ERROR_SUCCESS);
#else
			errno = 0;
#endif
			return -1;
		});
		EXPECT_THROW(io->truncate(testSize_ - 1U), std::system_error);
	}
	{
		StorageIO::ScopedNativeSyncHookForTesting failedSync([](file_handle_t) {
#ifdef _WIN32
			SetLastError(ERROR_SUCCESS);
#else
			errno = 0;
#endif
			return -1;
		});
		EXPECT_THROW(io->sync(), std::system_error);
	}
	EXPECT_EQ(io->stats().sync_calls, 0U);
}
#endif

TEST_F(StorageIOTest, FileSizeRejectsAnIoWithoutFileAccess) {
	auto io = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_THROW(static_cast<void>(io->fileSize()), std::runtime_error);
}

TEST_F(StorageIOTest, FallbackOperationsRejectAnIoWithoutAStream) {
	auto io = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	const char data[] = "missing stream";
	char buffer[sizeof(data)] = {};
	EXPECT_THROW(io->writeAt(0, data, sizeof(data)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(io->readAt(0, buffer, sizeof(buffer))), std::runtime_error);
	EXPECT_THROW(static_cast<void>(io->append(data, sizeof(data))), std::runtime_error);
	EXPECT_THROW(static_cast<void>(io->reserveAppendSpace(sizeof(data))), std::runtime_error);
}

TEST_F(StorageIOTest, FileSizeReportsClosedNativeHandle) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	file_handle_t writeFd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(writeFd, INVALID_FILE_HANDLE);
	portable_close_rw(writeFd);
	auto io = std::make_shared<StorageIO>(stream, writeFd, INVALID_FILE_HANDLE);

	EXPECT_THROW(static_cast<void>(io->fileSize()), std::runtime_error);
}

TEST_F(StorageIOTest, FileSizeReportsClosedFstream) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	stream->close();

	EXPECT_THROW(static_cast<void>(io->fileSize()), std::runtime_error);
}

TEST_F(StorageIOTest, WriteAtReportsClosedFstream) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	stream->close();

	const char data[] = "closed fstream";
	EXPECT_THROW(io->writeAt(0, data, sizeof(data)), std::runtime_error);
}

TEST_F(StorageIOTest, AppendNativeUsesEndAfterReserve) {
	auto io = createIO(true);

	uint64_t reservedOffset = io->reserveAppendSpace(128);
	EXPECT_EQ(reservedOffset, testSize_);

	const char data[] = "native append after reserve";
	uint64_t appendOffset = io->append(data, sizeof(data));
	EXPECT_EQ(appendOffset, testSize_ + 128);

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(appendOffset, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// writeAt with null buffer or zero size is a no-op
// ============================================================================

TEST_F(StorageIOTest, WriteAtNullBufferNoOp) {
	auto io = createIO(true);
	// Should not throw
	io->writeAt(0, nullptr, 10);
	io->writeAt(0, "data", 0);
}

// ============================================================================
// readAt with null buffer or zero size returns 0
// ============================================================================

TEST_F(StorageIOTest, ReadAtNullBufferReturnsZero) {
	auto io = createIO(true);
	EXPECT_EQ(io->readAt(0, nullptr, 10), 0u);
	char buf[1];
	EXPECT_EQ(io->readAt(0, buf, 0), 0u);
}

// ============================================================================
// append with null buffer throws
// ============================================================================

TEST_F(StorageIOTest, AppendNullBufferThrows) {
	auto io = createIO(false);
	EXPECT_THROW(io->append(nullptr, 10), std::invalid_argument);
	EXPECT_THROW(io->append("data", 0), std::invalid_argument);
	EXPECT_THROW(io->reserveAppendSpace(0), std::invalid_argument);
}

TEST_F(StorageIOTest, AppendAndReserveReportClosedStreamWriteFailures) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	stream->close();

	const char data[] = "closed";
	EXPECT_THROW(io->append(data, sizeof(data)), std::runtime_error);
	stream->clear();
	EXPECT_THROW(io->reserveAppendSpace(8), std::runtime_error);
}

TEST_F(StorageIOTest, AppendAndReserveReportClosedNativeWriteFailures) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	portable_close_rw(wfd);
	auto io = std::make_shared<StorageIO>(stream, wfd, INVALID_FILE_HANDLE);

	const char data[] = "closed native";
	EXPECT_THROW(io->append(data, sizeof(data)), std::runtime_error);
	EXPECT_THROW(io->reserveAppendSpace(8), std::runtime_error);
}

TEST_F(StorageIOTest, NativeWritePathsReportReadOnlyHandleFailures) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());

	file_handle_t readOnlyAsWrite = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(readOnlyAsWrite, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(stream, readOnlyAsWrite, INVALID_FILE_HANDLE);

	const char data[] = "read only write";
	EXPECT_THROW(io->writeAt(0, data, sizeof(data)), std::runtime_error);
	EXPECT_THROW(io->append(data, sizeof(data)), std::runtime_error);
	EXPECT_THROW(io->reserveAppendSpace(8), std::runtime_error);

	portable_close(readOnlyAsWrite);
}

// ============================================================================
// sync does not throw
// ============================================================================

TEST_F(StorageIOTest, SyncDoesNotThrow) {
	auto io = createIO(true);
	EXPECT_NO_THROW(io->sync());
}

TEST_F(StorageIOTest, SyncRecordsDataAndSuperblockDurabilityFences) {
	auto io = createIO(true);
	EXPECT_NO_THROW(io->sync(StorageIOSyncKind::SIOSK_DATA_FENCE));
	EXPECT_NO_THROW(io->sync(StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE));
	const StorageIOStats stats = io->stats();
	EXPECT_EQ(stats.sync_calls, 2U);
	EXPECT_EQ(stats.data_sync_calls, 1U);
	EXPECT_EQ(stats.superblock_sync_calls, 1U);
	EXPECT_THROW(io->sync(static_cast<StorageIOSyncKind>(0U)), std::invalid_argument);
}

TEST_F(StorageIOTest, SyncReportsClosedNativeWriteHandle) {
	file_handle_t writeFd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(writeFd, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(nullptr, writeFd, INVALID_FILE_HANDLE);
	ASSERT_EQ(portable_close_rw(writeFd), 0);

	EXPECT_THROW(io->sync(), std::runtime_error);
	EXPECT_EQ(io->stats().sync_calls, 0U);
}

TEST_F(StorageIOTest, SyncReportsFailedFstreamFlush) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->is_open());
	auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	// A closed fstream can accept a no-op flush on some standard libraries; a
	// badbit models the portable observable contract of an underlying I/O fault.
	stream->setstate(std::ios::badbit);

	EXPECT_THROW(io->sync(), std::runtime_error);
	EXPECT_EQ(io->stats().sync_calls, 0U);
}

// ============================================================================
// flushStream does not throw
// ============================================================================

TEST_F(StorageIOTest, FlushStreamDoesNotThrow) {
	auto io = createIO(true);
	EXPECT_NO_THROW(io->flushStream());
}

TEST_F(StorageIOTest, BufferPoolHelpersAreNoOpsWithoutCacheAndFlushCachedWrites) {
	auto io = createIO(true);
	const std::array<BufferPoolPageKey, 1U> keys{{{0U, 0U}}};
	const std::array<BufferPoolPageKey, 0U> emptyKeys{};

	// These helpers deliberately avoid speculative I/O until native storage has
	// configured its cache; callers may safely issue best-effort prefetch hints.
	io->prefetchPages(keys);
	io->prefetchPages(emptyKeys);
	EXPECT_EQ(io->bufferPoolStats(), BufferPoolStats{});
	EXPECT_NO_THROW(io->resetBufferPoolStats());

	io->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES);
	EXPECT_THROW(io->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES), std::logic_error);
	io->prefetchPages(emptyKeys);
	EXPECT_EQ(io->bufferPoolStats().prefetch_requests, 0U);

	const char byte = static_cast<char>(0x71);
	io->writeAt(0U, &byte, 1U);
	EXPECT_EQ(io->bufferPoolStats().dirty_bytes, RSS_BUFFER_POOL_PAGE_BYTES);
	io->flushStream();
	EXPECT_EQ(io->bufferPoolStats().dirty_bytes, 0U);
	io->disableBufferPool();
	EXPECT_EQ(io->bufferPoolStats(), BufferPoolStats{});
}

TEST_F(StorageIOTest, BufferPoolRoutesByteReadsPrefetchAndExternalRebaseThroughTheCache) {
	auto io = createIO(true);
	io->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);
	const char written = static_cast<char>(0x6C);
	char observed = 0;
	const std::array<BufferPoolPageKey, 1U> keys{{{0U, 1U}}};

	io->writeAt(0U, &written, 1U);
	EXPECT_EQ(io->readAt(0U, &observed, 1U), 1U);
	EXPECT_EQ(observed, written);
	io->flushStream();
	io->prefetchPages(keys);
	EXPECT_EQ(io->bufferPoolStats().prefetch_requests, 1U);
	EXPECT_EQ(io->bufferPoolStats().resident_bytes, 2U * RSS_BUFFER_POOL_PAGE_BYTES);

	io->resetBufferPoolStats();
	EXPECT_EQ(io->bufferPoolStats().prefetch_requests, 0U);
	io->discardBufferPoolForExternalRebase();
	EXPECT_EQ(io->bufferPoolStats().resident_bytes, 0U);
}

TEST_F(StorageIOTest, CacheAwareAppendAndReserveInvalidateTheDurableTailRange) {
	auto io = createIO(true);
	io->enableBufferPool(RSS_BUFFER_POOL_PAGE_BYTES);
	const char cached = static_cast<char>(0x29);
	const char appended = static_cast<char>(0x7C);

	io->writeAt(0U, &cached, 1U);
	EXPECT_EQ(io->append(&appended, 1U), testSize_);
	EXPECT_EQ(io->reserveAppendSpace(1U), testSize_ + 1U);
	EXPECT_EQ(io->fileSize(), testSize_ + 2U);
}

TEST_F(StorageIOTest, ReadOnlyIoRejectsMutationsBeforeTouchingTheStream) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in);
	ASSERT_TRUE(stream->is_open());
	StorageIO io(std::move(stream), INVALID_FILE_HANDLE, INVALID_FILE_HANDLE, StorageIOAccessMode::SIOAM_READ_ONLY);
	const char byte = static_cast<char>(0x61);

	EXPECT_THROW(io.writeAt(0U, &byte, 1U), std::logic_error);
	EXPECT_THROW(static_cast<void>(io.append(&byte, 1U)), std::logic_error);
	EXPECT_THROW(static_cast<void>(io.reserveAppendSpace(1U)), std::logic_error);
}

TEST_F(StorageIOTest, ControlRecordWriteInvalidatesTheGenerationTaggedCacheEntry) {
	auto io = createIO(true);
	const BufferPoolPageKey key{0U, 7U};
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> initial{};
	std::array<std::byte, 16U> replacement{};
	for (size_t index = 0U; index < replacement.size(); ++index) {
		replacement[index] = static_cast<std::byte>(index + 1U);
	}

	io->enableBufferPool(2U * RSS_BUFFER_POOL_PAGE_BYTES);
	io->writeAt(key, initial.data(), initial.size());
	io->sync();
	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> cached{};
	ASSERT_EQ(io->readAt(key, cached.data(), cached.size()), cached.size());
	ASSERT_EQ(io->bufferPoolStats().resident_bytes, RSS_BUFFER_POOL_PAGE_BYTES);

	io->writeControlRecord(key, 64U, replacement);
	EXPECT_EQ(io->bufferPoolStats().resident_bytes, 0U);

	std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> reread{};
	ASSERT_EQ(io->readAt(key, reread.data(), reread.size()), reread.size());
	EXPECT_TRUE(std::equal(replacement.begin(), replacement.end(), reread.begin() + 64U));
	EXPECT_EQ(reread[63U], std::byte{0U});
	EXPECT_EQ(reread[80U], std::byte{0U});
}

TEST_F(StorageIOTest, ControlRecordWriteRejectsInvalidTypedPageRanges) {
	auto io = createIO(true);
	const std::array<std::byte, 1U> byte{std::byte{0x11}};
	const BufferPoolPageKey valid{0U, 1U};

	EXPECT_THROW(io->writeControlRecord(valid, RSS_BUFFER_POOL_PAGE_BYTES, byte), std::invalid_argument);
	EXPECT_THROW(io->writeControlRecord({0U, 0U}, 0U, byte), std::invalid_argument);
	EXPECT_THROW(io->writeControlRecord({1U, 1U}, 0U, byte), std::invalid_argument);
	EXPECT_THROW(io->writeControlRecord(valid, 0U, std::span<const std::byte>{}), std::invalid_argument);
}

// ============================================================================
// writeAt/readAt null buffer and zero size with fstream fallback
// ============================================================================

TEST_F(StorageIOTest, WriteAtNullBufferNoOp_FstreamFallback) {
	auto io = createIO(false);
	// Should not throw — early return before touching fstream
	io->writeAt(0, nullptr, 10);
	io->writeAt(0, "data", 0);
}

TEST_F(StorageIOTest, ReadAtNullBufferReturnsZero_FstreamFallback) {
	auto io = createIO(false);
	EXPECT_EQ(io->readAt(0, nullptr, 10), 0u);
	char buf[1];
	EXPECT_EQ(io->readAt(0, buf, 0), 0u);
}

// ============================================================================
// writeAt at offset 0 (both native fd and fstream)
// ============================================================================

TEST_F(StorageIOTest, WriteAtOffsetZero_NativeFd) {
	auto io = createIO(true);
	const char data[] = "AtZero";
	io->writeAt(0, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(0, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

TEST_F(StorageIOTest, WriteAtOffsetZero_FstreamFallback) {
	auto io = createIO(false);
	const char data[] = "AtZero";
	io->writeAt(0, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(0, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// readAt beyond file end — pread returns short read (not error)
// ============================================================================

TEST_F(StorageIOTest, ReadAtBeyondFileEnd_NativeFd) {
	auto io = createIO(true);
	char buffer[64] = {};
	// File is 4096 bytes; reading at offset 4096 should return 0 bytes
	size_t bytesRead = io->readAt(testSize_, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 0u);
}

TEST_F(StorageIOTest, ReadAtBeyondFileEnd_FstreamFallback) {
	auto io = createIO(false);
	char buffer[64] = {};
	// File is 4096 bytes; reading at offset 4096 should return 0 bytes
	size_t bytesRead = io->readAt(testSize_, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 0u);
}

TEST_F(StorageIOTest, ReadAtPartialRead_NativeFd) {
	auto io = createIO(true);
	char buffer[64] = {};
	// Read starting 32 bytes before EOF — should get only 32 bytes
	size_t bytesRead = io->readAt(testSize_ - 32, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 32u);
}

TEST_F(StorageIOTest, ReadAtPartialRead_FstreamFallback) {
	auto io = createIO(false);
	char buffer[64] = {};
	// Read starting 32 bytes before EOF — should get only 32 bytes
	size_t bytesRead = io->readAt(testSize_ - 32, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 32u);
}

// ============================================================================
// pwrite/pread error paths via invalid (closed) file descriptors
// ============================================================================

TEST_F(StorageIOTest, WriteAtPwriteError_InvalidFd) {
	// Create IO with a valid fstream but a bogus write fd
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->good());

	// Open a real fd then close it to make it invalid
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	portable_close_rw(wfd); // Now wfd is closed/invalid

	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	auto io = std::make_shared<StorageIO>(stream, wfd, rfd);

	// pwrite on a closed fd should fail and throw
	const char data[] = "fail";
	EXPECT_THROW(io->writeAt(0, data, sizeof(data)), std::runtime_error);

	if (rfd != INVALID_FILE_HANDLE)
		portable_close(rfd);
}

TEST_F(StorageIOTest, ReadAtPreadError_InvalidFd) {
	auto stream = std::make_shared<std::fstream>(testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->good());

	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(rfd, INVALID_FILE_HANDLE);
	portable_close(rfd); // Now rfd is closed/invalid

	auto io = std::make_shared<StorageIO>(stream, wfd, rfd);

	char buffer[16] = {};
	EXPECT_THROW(io->readAt(0, buffer, sizeof(buffer)), std::runtime_error);

	if (wfd != INVALID_FILE_HANDLE)
		portable_close_rw(wfd);
}

// ============================================================================
// sync with fstream fallback (no native write fd)
// ============================================================================

TEST_F(StorageIOTest, SyncFstreamFallback) {
	auto io = createIO(false);
	// With INVALID_FILE_HANDLE for writeFd, sync should flush the stream
	EXPECT_NO_THROW(io->sync());
}

// ============================================================================
// flushStream with fstream fallback
// ============================================================================

TEST_F(StorageIOTest, FlushStreamFstreamFallback) {
	auto io = createIO(false);
	EXPECT_NO_THROW(io->flushStream());
}

// ============================================================================
// append with native fd IO
// ============================================================================

TEST_F(StorageIOTest, AppendWithNativeFds) {
	auto io = createIO(true);
	const char data[] = "append with fds";
	uint64_t offset = io->append(data, sizeof(data));
	EXPECT_EQ(offset, testSize_);

	// Flush so pread can see the appended data
	io->flushStream();

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(offset, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// append null buffer throws (with native fds)
// ============================================================================

TEST_F(StorageIOTest, AppendNullBufferThrows_NativeFds) {
	auto io = createIO(true);
	EXPECT_THROW(io->append(nullptr, 10), std::invalid_argument);
	EXPECT_THROW(io->append("data", 0), std::invalid_argument);
}

// ============================================================================
// Multiple sequential writes and reads at various offsets
// ============================================================================

// ============================================================================
// sync and flushStream with null stream
// ============================================================================

TEST_F(StorageIOTest, SyncWithNullStream) {
	// Create IO with valid write fd but null stream
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(nullptr, wfd, INVALID_FILE_HANDLE);
	// sync should use portable_fsync (writeFd_ is valid), so null stream is fine
	EXPECT_NO_THROW(io->sync());
	portable_close_rw(wfd);
}

TEST_F(StorageIOTest, FlushStreamWithNullStream) {
	// Create IO with null stream
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(nullptr, wfd, INVALID_FILE_HANDLE);
	// flushStream should check stream_ and skip if null
	EXPECT_NO_THROW(io->flushStream());
	portable_close_rw(wfd);
}

TEST_F(StorageIOTest, SyncWithNullStreamAndInvalidFd) {
	// Both writeFd_ == INVALID and stream_ == null — tests the else-if false branch
	auto io = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_NO_THROW(io->sync());
}

// ============================================================================
// Multiple sequential writes and reads at various offsets
// ============================================================================

TEST_F(StorageIOTest, MultipleWriteReadRoundtrip_FstreamFallback) {
	auto io = createIO(false);

	const char data1[] = "first";
	const char data2[] = "second";
	io->writeAt(0, data1, sizeof(data1));
	io->writeAt(512, data2, sizeof(data2));

	char buf1[sizeof(data1)] = {};
	char buf2[sizeof(data2)] = {};
	EXPECT_EQ(io->readAt(0, buf1, sizeof(buf1)), sizeof(data1));
	EXPECT_EQ(io->readAt(512, buf2, sizeof(buf2)), sizeof(data2));
	EXPECT_EQ(std::memcmp(data1, buf1, sizeof(data1)), 0);
	EXPECT_EQ(std::memcmp(data2, buf2, sizeof(data2)), 0);
}

TEST_F(StorageIOTest, SyncTraceSeparatesSuccessfulDurabilityFencesAndRejectsUnknownKind) {
 auto io = createIO(true);
 using graph::debug::PerfTrace;
 struct Cleanup { ~Cleanup() { PerfTrace::setEnabled(false); PerfTrace::setValueCollectionEnabled(false); PerfTrace::reset(); } } cleanup;
 PerfTrace::setEnabled(false); PerfTrace::setValueCollectionEnabled(true); PerfTrace::reset();
 io->sync(StorageIOSyncKind::SIOSK_DATA_FENCE);
 io->sync(StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE);
 io->sync();
 EXPECT_THROW(io->sync(static_cast<StorageIOSyncKind>(99)), std::invalid_argument);
 const auto trace = PerfTrace::snapshotAndReset();
 for (const auto *phase : {"storage_io.sync.data_fence", "storage_io.sync.superblock_fence", "storage_io.sync.general"}) {
  EXPECT_EQ(trace.at(phase).totalValue, 1);
  EXPECT_EQ(trace.at(phase).calls, 0U); // counter-only diagnostics remain untimed
 }
 PerfTrace::setEnabled(true);
 io->sync(StorageIOSyncKind::SIOSK_DATA_FENCE);
 const auto timed = PerfTrace::snapshotAndReset();
 EXPECT_EQ(timed.at("storage_io.sync.data_fence").totalValue, 1);
 EXPECT_EQ(timed.at("storage_io.sync.data_fence").calls, 1U);
 EXPECT_GT(timed.at("storage_io.sync.data_fence").totalNs, 0U);
}
