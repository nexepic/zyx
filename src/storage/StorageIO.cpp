/**
 * @file StorageIO.cpp
 * @brief Unified cross-platform I/O abstraction implementation
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/StorageIO.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <system_error>
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/relocatable/cache/BufferPool.hpp"

namespace graph::storage {

	namespace {

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
		thread_local StorageIO::NativeWriteHookForTesting nativeWriteHookForTesting;
		std::mutex allThreadNativeWriteHookMutex;
		StorageIO::NativeWriteHookForTesting allThreadNativeWriteHookForTesting;
		thread_local StorageIO::NativeReadHookForTesting nativeReadHookForTesting;
		thread_local StorageIO::NativeSyncHookForTesting nativeSyncHookForTesting;
		std::mutex allThreadNativeSyncHookMutex;
		StorageIO::NativeSyncHookForTesting allThreadNativeSyncHookForTesting;
		thread_local StorageIO::NativeTruncateHookForTesting nativeTruncateHookForTesting;
#endif

		void requireAddressableRange(uint64_t offset, size_t size, const char *operation) {
			if (size > std::numeric_limits<uint64_t>::max() - offset) {
				throw std::invalid_argument(std::string("StorageIO::") + operation + " range overflows uint64_t");
			}
			if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::invalid_argument(std::string("StorageIO::") + operation +
											" offset exceeds platform I/O range");
			}
			if (size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
				throw std::invalid_argument(std::string("StorageIO::") + operation + " size exceeds stream I/O range");
			}
		}

		[[noreturn]] void throwNativeIoError(std::string operation) {
#ifdef _WIN32
			const DWORD nativeError = GetLastError();
			const std::error_code error = nativeError == ERROR_SUCCESS ? std::make_error_code(std::errc::io_error)
																	   : std::error_code(static_cast<int>(nativeError),
																						 std::system_category());
#else
			const int nativeError = errno;
			const std::error_code error = nativeError == 0 ? std::make_error_code(std::errc::io_error)
														   : std::error_code(nativeError, std::generic_category());
#endif
			throw std::system_error(error, std::move(operation));
		}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
		[[nodiscard]] bool nativeWriteHookActive() {
			if (nativeWriteHookForTesting) {
				return true;
			}
			std::lock_guard lock(allThreadNativeWriteHookMutex);
			return static_cast<bool>(allThreadNativeWriteHookForTesting);
		}
#endif

	} // namespace

	StorageIO::StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd) :
		StorageIO(std::move(stream), writeFd, readFd, StorageIOAccessMode::SIOAM_READ_WRITE) {}

	StorageIO::StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd,
                         StorageIOAccessMode accessMode) :
        StorageIO(std::move(stream), writeFd, readFd, accessMode, (std::numeric_limits<uint64_t>::max)()) {}

    StorageIO::StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd,
                         StorageIOAccessMode accessMode, uint64_t maximumFileBytes) :
        stream_(std::move(stream)), writeFd_(writeFd), readFd_(readFd), access_mode_(accessMode),
        maximum_file_bytes_(maximumFileBytes) {
        if (maximumFileBytes == 0U) throw std::invalid_argument("StorageIO requires a positive file byte limit");
		if (access_mode_ != StorageIOAccessMode::SIOAM_READ_WRITE &&
			access_mode_ != StorageIOAccessMode::SIOAM_READ_ONLY) {
			throw std::invalid_argument("StorageIO received an unknown access mode");
		}
	}

    void StorageIO::requireWritableRange(uint64_t offset, size_t size, const char *operation) const {
        requireWritable(operation);
        requireAddressableRange(offset, size, operation);
        if (offset > maximum_file_bytes_ || size > maximum_file_bytes_ - offset) {
            throw StorageFileLimitExceeded();
        }
    }

	StorageIO::~StorageIO() noexcept {
		try {
			disableBufferPool();
		} catch (...) {
			// Dropping a dirty COW page on destruction would silently violate the
			// storage contract. Noexcept teardown therefore fails closed instead.
			std::terminate();
		}
	}

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
	StorageIO::ScopedNativeWriteHookForTesting::ScopedNativeWriteHookForTesting(NativeWriteHookForTesting hook) {
		if (!hook) {
			throw std::invalid_argument("StorageIO native write test hook is empty");
		}
		if (nativeWriteHookForTesting) {
			throw std::logic_error("StorageIO native write test hook is already active on this thread");
		}
		nativeWriteHookForTesting = std::move(hook);
	}

	StorageIO::ScopedNativeWriteHookForTesting::~ScopedNativeWriteHookForTesting() { nativeWriteHookForTesting = {}; }

	StorageIO::ScopedAllThreadNativeWriteHookForTesting::ScopedAllThreadNativeWriteHookForTesting(
			NativeWriteHookForTesting hook) {
		if (!hook) {
			throw std::invalid_argument("StorageIO all-thread native write test hook is empty");
		}
		std::lock_guard lock(allThreadNativeWriteHookMutex);
		if (allThreadNativeWriteHookForTesting) {
			throw std::logic_error("StorageIO all-thread native write test hook is already active");
		}
		allThreadNativeWriteHookForTesting = std::move(hook);
	}

	StorageIO::ScopedAllThreadNativeWriteHookForTesting::~ScopedAllThreadNativeWriteHookForTesting() {
		std::lock_guard lock(allThreadNativeWriteHookMutex);
		allThreadNativeWriteHookForTesting = {};
	}

	StorageIO::ScopedNativeReadHookForTesting::ScopedNativeReadHookForTesting(NativeReadHookForTesting hook) {
		if (!hook) {
			throw std::invalid_argument("StorageIO native read test hook is empty");
		}
		if (nativeReadHookForTesting) {
			throw std::logic_error("StorageIO native read test hook is already active on this thread");
		}
		nativeReadHookForTesting = std::move(hook);
	}

	StorageIO::ScopedNativeReadHookForTesting::~ScopedNativeReadHookForTesting() { nativeReadHookForTesting = {}; }

	StorageIO::ScopedNativeSyncHookForTesting::ScopedNativeSyncHookForTesting(NativeSyncHookForTesting hook) {
		if (!hook) {
			throw std::invalid_argument("StorageIO native sync test hook is empty");
		}
		if (nativeSyncHookForTesting) {
			throw std::logic_error("StorageIO native sync test hook is already active on this thread");
		}
		nativeSyncHookForTesting = std::move(hook);
	}

	StorageIO::ScopedNativeSyncHookForTesting::~ScopedNativeSyncHookForTesting() { nativeSyncHookForTesting = {}; }

	StorageIO::ScopedAllThreadNativeSyncHookForTesting::ScopedAllThreadNativeSyncHookForTesting(
			NativeSyncHookForTesting hook) {
		if (!hook) {
			throw std::invalid_argument("StorageIO all-thread native sync test hook is empty");
		}
		std::lock_guard lock(allThreadNativeSyncHookMutex);
		if (allThreadNativeSyncHookForTesting) {
			throw std::logic_error("StorageIO all-thread native sync test hook is already active");
		}
		allThreadNativeSyncHookForTesting = std::move(hook);
	}

	StorageIO::ScopedAllThreadNativeSyncHookForTesting::~ScopedAllThreadNativeSyncHookForTesting() {
		std::lock_guard lock(allThreadNativeSyncHookMutex);
		allThreadNativeSyncHookForTesting = {};
	}

	StorageIO::ScopedNativeTruncateHookForTesting::ScopedNativeTruncateHookForTesting(
			NativeTruncateHookForTesting hook) {
		if (!hook) {
			throw std::invalid_argument("StorageIO native truncate test hook is empty");
		}
		if (nativeTruncateHookForTesting) {
			throw std::logic_error("StorageIO native truncate test hook is already active on this thread");
		}
		nativeTruncateHookForTesting = std::move(hook);
	}

	StorageIO::ScopedNativeTruncateHookForTesting::~ScopedNativeTruncateHookForTesting() {
		nativeTruncateHookForTesting = {};
	}
#endif

	void StorageIO::saturatingAdd(std::atomic<uint64_t> &counter, uint64_t value) noexcept {
		uint64_t current = counter.load(std::memory_order_relaxed);
		while (current != std::numeric_limits<uint64_t>::max()) {
			const uint64_t next = value > std::numeric_limits<uint64_t>::max() - current
										  ? std::numeric_limits<uint64_t>::max()
										  : current + value;
			if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
				return;
			}
		}
	}

	void StorageIO::recordRead(size_t bytes) const noexcept {
		saturatingAdd(read_bytes_, bytes);
		saturatingAdd(read_calls_, 1U);
	}

	void StorageIO::recordWrite(size_t bytes) noexcept {
		saturatingAdd(data_mutation_epoch_, 1U);
		saturatingAdd(write_bytes_, bytes);
		saturatingAdd(write_calls_, 1U);
		if (debug::PerfTrace::areValuesEnabled()) {
			// Diagnostics must not turn an already completed write into failure.
			try {
				debug::PerfTrace::addValue("storage_io.completed_write_bytes",
					static_cast<int64_t>(std::min<uint64_t>(bytes, std::numeric_limits<int64_t>::max())));
				debug::PerfTrace::addValue("storage_io.completed_write_calls", 1);
			} catch (...) {
				// The authoritative saturating I/O counters above remain available.
			}
		}
	}

	void StorageIO::requireTypedPageKey(BufferPoolPageKey key, size_t size, const char *operation) {
		if (key.generation == 0U) {
			throw std::invalid_argument(std::string("StorageIO::") + operation +
										" requires a nonzero durable page generation");
		}
		if (key.page_offset % RSS_BUFFER_POOL_PAGE_BYTES != 0U || size != RSS_BUFFER_POOL_PAGE_BYTES) {
			throw std::invalid_argument(std::string("StorageIO::") + operation +
										" requires one aligned complete native page");
		}
		requireAddressableRange(key.page_offset, size, operation);
	}

	void StorageIO::requireTypedPageRange(BufferPoolPageKey firstKey, size_t size, const char *operation) {
		if (firstKey.generation == 0U || size == 0U || size % RSS_BUFFER_POOL_PAGE_BYTES != 0U ||
			firstKey.page_offset % RSS_BUFFER_POOL_PAGE_BYTES != 0U) {
			throw std::invalid_argument(std::string("StorageIO::") + operation +
										" requires a nonempty aligned complete typed-page range");
		}
		requireAddressableRange(firstKey.page_offset, size, operation);
	}

	void StorageIO::writeAt(uint64_t offset, const void *buf, size_t size) {
		saturatingAdd(data_mutation_epoch_, 1U);
		if (!buf || size == 0)
			return;
		requireWritable("writeAt");
		requireWritableRange(offset, size, "writeAt");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->write(offset, {static_cast<const std::byte *>(buf), size});
			recordWrite(size);
			return;
		}
		writeAtUncached(offset, buf, size);
		recordWrite(size);
	}

	void StorageIO::writeAt(BufferPoolPageKey key, const void *buf, size_t size) {
		saturatingAdd(data_mutation_epoch_, 1U);
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::writeAt typed page buffer is null");
		}
		requireTypedPageKey(key, size, "writeAt");
        requireWritableRange(key.page_offset, size, "writeAt");
		requireWritable("writeAt");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->writePage(key, std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES>(
										 static_cast<const std::byte *>(buf), RSS_BUFFER_POOL_PAGE_BYTES));
			recordWrite(size);
			return;
		}
		writeAtUncached(key.page_offset, buf, size);
		recordWrite(size);
	}

	void StorageIO::writePages(BufferPoolPageKey firstKey, const void *buf, size_t size) {
		saturatingAdd(data_mutation_epoch_, 1U);
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::writePages typed page buffer is null");
		}
		requireTypedPageRange(firstKey, size, "writePages");
        requireWritableRange(firstKey.page_offset, size, "writePages");
		requireWritable("writePages");
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
		// Preserve page-granular fault-injection semantics when a test hook is
		// active. Production writes retain the batched native I/O path.
		if (nativeWriteHookActive() && !bufferPool()) {
			const auto *bytes = static_cast<const std::byte *>(buf);
			for (size_t offset = 0U; offset < size; offset += RSS_PAGE_SIZE) {
				writeAtUncached(firstKey.page_offset + offset, bytes + offset, RSS_PAGE_SIZE);
				recordWrite(RSS_PAGE_SIZE);
			}
			return;
		}
#endif
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->writePages(firstKey, {static_cast<const std::byte *>(buf), size});
			recordWrite(size);
			return;
		}
		writeAtUncached(firstKey.page_offset, buf, size);
		recordWrite(size);
	}

	void StorageIO::writePagesThrough(BufferPoolPageKey firstKey, const void *buf, size_t size) {
		saturatingAdd(data_mutation_epoch_, 1U);
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::writePagesThrough typed page buffer is null");
		}
		requireTypedPageRange(firstKey, size, "writePagesThrough");
        requireWritableRange(firstKey.page_offset, size, "writePagesThrough");
		requireWritable("writePagesThrough");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->writePagesThrough(firstKey, {static_cast<const std::byte *>(buf), size});
			recordWrite(size);
			return;
		}
		writeAtUncached(firstKey.page_offset, buf, size);
		recordWrite(size);
	}

	void StorageIO::writeControlRecord(BufferPoolPageKey key, size_t byteOffset, std::span<const std::byte> bytes) {
		saturatingAdd(data_mutation_epoch_, 1U);
		if (bytes.empty() || byteOffset > RSS_BUFFER_POOL_PAGE_BYTES ||
			bytes.size() > RSS_BUFFER_POOL_PAGE_BYTES - byteOffset) {
			throw std::invalid_argument("StorageIO::writeControlRecord range is outside one native page");
		}
		// This operation preserves a previously complete control record.  It must
		// never route through generic byte I/O, which has generation zero and could
		// silently replace the generation-tagged cache entry for this page.
		requireTypedPageKey(key, RSS_BUFFER_POOL_PAGE_BYTES, "writeControlRecord");
		requireWritable("writeControlRecord");
		const uint64_t offset = key.page_offset + static_cast<uint64_t>(byteOffset);
		requireWritableRange(offset, bytes.size(), "writeControlRecord");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			// Invalidation also flushes an earlier complete page image before the
			// exact-record write.  On failure no stale cache entry remains that could
			// overwrite the surviving control record during a later flush.
			pool->invalidateRange(key.page_offset, RSS_BUFFER_POOL_PAGE_BYTES);
		}
		writeAtUncached(offset, bytes.data(), bytes.size());
		recordWrite(bytes.size());
	}

	void StorageIO::writeAtUncached(uint64_t offset, const void *buf, size_t size) {
		// Also invalidate on a failed partial native write.
		saturatingAdd(data_mutation_epoch_, 1U);
		if (!buf || size == 0U) {
			return;
		}
		requireWritable("writeAt");
		requireWritableRange(offset, size, "writeAt");

		if (writeFd_ != INVALID_FILE_HANDLE) {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
			NativeWriteHookForTesting allThreadHook;
			if (!nativeWriteHookForTesting) {
				std::lock_guard lock(allThreadNativeWriteHookMutex);
				allThreadHook = allThreadNativeWriteHookForTesting;
			}
			const pwrite_ssize_t written =
					nativeWriteHookForTesting
							? nativeWriteHookForTesting(writeFd_, buf, size, static_cast<pwrite_off_t>(offset))
					: allThreadHook ? allThreadHook(writeFd_, buf, size, static_cast<pwrite_off_t>(offset))
									: portable_pwrite(writeFd_, buf, size, static_cast<pwrite_off_t>(offset));
#else
			const pwrite_ssize_t written = portable_pwrite(writeFd_, buf, size, static_cast<pwrite_off_t>(offset));
#endif
			if (written < 0) {
				throwNativeIoError("StorageIO::writeAt pwrite failed at offset " + std::to_string(offset));
			}
			if (static_cast<size_t>(written) != size) {
				throw std::system_error(std::make_error_code(std::errc::io_error),
										"StorageIO::writeAt native write was incomplete at offset " +
												std::to_string(offset));
			}
		} else {
			std::lock_guard lock(stream_mutex_);
			if (!stream_) {
				throw std::runtime_error("StorageIO::writeAt requires a stream or native write handle");
			}
			stream_->seekp(static_cast<std::streamoff>(offset));
			stream_->write(static_cast<const char *>(buf), static_cast<std::streamsize>(size));
			if (!*stream_) {
				throw std::runtime_error("StorageIO::writeAt fstream write failed at offset " + std::to_string(offset));
			}
		}
	}

	size_t StorageIO::readAt(uint64_t offset, void *buf, size_t size) const {
		if (!buf || size == 0)
			return 0;
		requireAddressableRange(offset, size, "readAt");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			const size_t result = pool->read(offset, {static_cast<std::byte *>(buf), size},
                SequentialReadScope::activeFor(*this) ? BufferPoolReadHint::BPRH_SEQUENTIAL : BufferPoolReadHint::BPRH_NORMAL);
			recordRead(result);
			return result;
		}
		const size_t result = readAtUncached(offset, buf, size);
		recordRead(result);
		return result;
	}

	size_t StorageIO::readAtSequential(uint64_t offset, void *buf, size_t size) const {
		if (!buf || size == 0U) {
			return 0U;
		}
		requireAddressableRange(offset, size, "readAtSequential");
		const size_t result = readAtUncached(offset, buf, size);
		recordRead(result);
		return result;
	}

	size_t StorageIO::readAt(BufferPoolPageKey key, void *buf, size_t size, BufferPoolReadHint hint) const {
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::readAt typed page buffer is null");
		}
		requireTypedPageKey(key, size, "readAt");
        if (SequentialReadScope::activeFor(*this)) hint = BufferPoolReadHint::BPRH_SEQUENTIAL;
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			const size_t result = pool->readPage(key,
												 std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES>(
														 static_cast<std::byte *>(buf), RSS_BUFFER_POOL_PAGE_BYTES),
												 hint);
			recordRead(result);
			return result;
		}
		const size_t result = readAtUncached(key.page_offset, buf, size);
		recordRead(result);
		return result;
	}

	size_t StorageIO::readPages(BufferPoolPageKey firstKey, void *buf, size_t size, BufferPoolReadHint hint) const {
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::readPages typed page buffer is null");
		}
		requireTypedPageRange(firstKey, size, "readPages");
        if (SequentialReadScope::activeFor(*this)) hint = BufferPoolReadHint::BPRH_SEQUENTIAL;
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			const size_t result = pool->readPages(firstKey, {static_cast<std::byte *>(buf), size}, hint);
			recordRead(result);
			return result;
		}
		const size_t result = readAtUncached(firstKey.page_offset, buf, size);
		recordRead(result);
		return result;
	}

	size_t StorageIO::readAtUncached(uint64_t offset, void *buf, size_t size) const {
		if (!buf || size == 0U) {
			return 0U;
		}
		requireAddressableRange(offset, size, "readAt");

		if (readFd_ != INVALID_FILE_HANDLE) {
			const pwrite_ssize_t bytesRead =
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
					nativeReadHookForTesting
							? nativeReadHookForTesting(readFd_, buf, size, static_cast<pwrite_off_t>(offset))
							: portable_pread(readFd_, buf, size, static_cast<int64_t>(offset));
#else
					portable_pread(readFd_, buf, size, static_cast<int64_t>(offset));
#endif
			if (bytesRead < 0) {
				throw std::runtime_error("StorageIO::readAt pread failed at offset " + std::to_string(offset));
			}
			return static_cast<size_t>(bytesRead);
		}

		std::lock_guard lock(stream_mutex_);
		if (!stream_) {
			throw std::runtime_error("StorageIO::readAt requires a stream or native read handle");
		}
		stream_->seekg(static_cast<std::streamoff>(offset));
		stream_->read(static_cast<char *>(buf), static_cast<std::streamsize>(size));
		auto bytesRead = stream_->gcount();
		stream_->clear(stream_->rdstate() & std::ios::badbit); // Clear eof/fail but preserve badbit
		return static_cast<size_t>(bytesRead);
	}

	uint64_t StorageIO::append(const void *buf, size_t size) {
		if (!buf || size == 0) {
			throw std::invalid_argument("StorageIO::append called with null buffer or zero size");
		}
		requireWritable("append");
		const uint64_t offset = fileSizeUncached();
		requireWritableRange(offset, size, "append");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			// Flush and remove a cached tail before the direct append changes its page.
			pool->invalidateRange(offset, size);
		}
		writeAtUncached(offset, buf, size);
		if (writeFd_ == INVALID_FILE_HANDLE) {
			// A cache miss can use a native read handle even when writes fall back
			// to fstream, so make this direct append visible before it is cached.
			flushStreamUncached();
		}
		recordWrite(size);
		return offset;
	}

	uint64_t StorageIO::reserveAppendSpace(size_t size) {
		if (size == 0) {
			throw std::invalid_argument("StorageIO::reserveAppendSpace called with zero size");
		}
		requireWritable("reserveAppendSpace");
		const uint64_t offset = fileSizeUncached();
		requireWritableRange(offset, size, "reserveAppendSpace");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->invalidateRange(offset, size);
		}
		ensureFileSizeUncached(offset + static_cast<uint64_t>(size));
		recordWrite(1U);
		saturatingAdd(reserved_bytes_, size);
		saturatingAdd(reserve_calls_, 1U);
		return offset;
	}

	uint64_t StorageIO::fileSize() const { return fileSizeUncached(); }

	void StorageIO::truncate(uint64_t size) {
		saturatingAdd(data_mutation_epoch_, 1U);
		requireWritable("truncate");
		const uint64_t currentSize = fileSizeUncached();
		if (size > currentSize) {
			throw std::invalid_argument("StorageIO::truncate cannot extend the file");
		}
		if (writeFd_ == INVALID_FILE_HANDLE) {
			throw std::runtime_error("StorageIO::truncate requires a native write handle");
		}
		if (size == currentSize) {
			return;
		}
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			// Flush before discarding cache entries so no dirty tail page can be
			// written back after the operating system has shortened the file.
			pool->flush();
			uint64_t rangeOffset = size;
			uint64_t remaining = currentSize - size;
			while (remaining != 0U) {
				const size_t chunk =
						static_cast<size_t>(std::min<uint64_t>(remaining, std::numeric_limits<size_t>::max()));
				pool->invalidateRange(rangeOffset, chunk);
				rangeOffset += chunk;
				remaining -= chunk;
			}
		}
		flushStreamUncached();
		const int truncateResult =
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
				nativeTruncateHookForTesting ? nativeTruncateHookForTesting(writeFd_, size)
											 : portable_ftruncate(writeFd_, size);
#else
				portable_ftruncate(writeFd_, size);
#endif
		if (truncateResult != 0) {
			throwNativeIoError("StorageIO::truncate native truncate failed at size " + std::to_string(size));
		}
	}

	uint64_t StorageIO::fileSizeUncached() const {
		debug::ScopedPerfTimer sizeTimer("storage_io.file_size");
		debug::PerfTrace::addValue("storage_io.file_size_calls", 1);
		file_handle_t nativeHandle = writeFd_ != INVALID_FILE_HANDLE ? writeFd_ : readFd_;
		if (nativeHandle != INVALID_FILE_HANDLE) {
			uint64_t size = 0;
			if (portable_file_size(nativeHandle, &size) != 0) {
				throw std::runtime_error("StorageIO::fileSize native file-size query failed");
			}
			return size;
		}
		if (!stream_) {
			throw std::runtime_error("StorageIO::fileSize requires a stream or native file handle");
		}
		std::lock_guard lock(stream_mutex_);
		stream_->clear();
		stream_->seekg(0, std::ios::end);
		const std::streampos endPosition = stream_->tellg();
		if (endPosition < 0) {
			throw std::runtime_error("StorageIO::fileSize failed to determine stream length");
		}
		stream_->clear();
		return static_cast<uint64_t>(endPosition);
	}

	void StorageIO::sync() { sync(StorageIOSyncKind::SIOSK_GENERAL); }

	void StorageIO::sync(StorageIOSyncKind kind) {
		if (kind != StorageIOSyncKind::SIOSK_GENERAL && kind != StorageIOSyncKind::SIOSK_DATA_FENCE &&
			kind != StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE) {
			throw std::invalid_argument("StorageIO::sync requires a known sync kind");
		}
		const char *phase = kind == StorageIOSyncKind::SIOSK_DATA_FENCE ? "storage_io.sync.data_fence" :
			kind == StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE ? "storage_io.sync.superblock_fence" : "storage_io.sync.general";
		const bool timed = debug::PerfTrace::isEnabled();
		const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->flush();
		}
		syncUncached();
		saturatingAdd(sync_calls_, 1U);
		if (kind == StorageIOSyncKind::SIOSK_DATA_FENCE) {
			saturatingAdd(data_sync_calls_, 1U);
		} else if (kind == StorageIOSyncKind::SIOSK_SUPERBLOCK_FENCE) {
			saturatingAdd(superblock_sync_calls_, 1U);
		}
		// Observability must not fail an already completed durability fence.
		try {
			debug::PerfTrace::addValue(phase, 1);
			if (timed) debug::PerfTrace::addDuration(phase, static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()));
		} catch (...) {
			// Authoritative saturating sync counters remain available.
		}
	}

	void StorageIO::syncUncached() {
		if (writeFd_ != INVALID_FILE_HANDLE) {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
			if (nativeSyncHookForTesting) {
				if (nativeSyncHookForTesting(writeFd_) != 0) {
					throwNativeIoError("StorageIO::sync native durable sync failed");
				}
				return;
			}
			NativeSyncHookForTesting allThreadHook;
			{
				std::lock_guard lock(allThreadNativeSyncHookMutex);
				allThreadHook = allThreadNativeSyncHookForTesting;
			}
			const int result = allThreadHook ? allThreadHook(writeFd_) : portable_fsync(writeFd_);
#else
			const int result = portable_fsync(writeFd_);
#endif
			if (result != 0) {
				throwNativeIoError("StorageIO::sync native durable sync failed");
			}
		} else if (stream_) {
			std::lock_guard lock(stream_mutex_);
			stream_->flush();
			if (!*stream_) {
				throw std::runtime_error("StorageIO::sync fstream flush failed");
			}
		}
	}

	void StorageIO::flushStream() {
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->flush();
		}
		flushStreamUncached();
	}

	void StorageIO::flushStreamUncached() {
		if (stream_) {
			std::lock_guard lock(stream_mutex_);
			stream_->flush();
		}
	}

	void StorageIO::ensureFileSizeUncached(uint64_t minimumSize) {
        if (minimumSize == 0U) return;
        requireWritableRange(minimumSize - 1U, 1U, "ensureFileSize");
        if (fileSizeUncached() >= minimumSize) return;
		requireWritable("ensureFileSize");
		requireAddressableRange(minimumSize - 1U, 1U, "ensureFileSize");
		const char zero = 0;
		writeAtUncached(minimumSize - 1U, &zero, 1U);
		if (writeFd_ == INVALID_FILE_HANDLE) {
			flushStreamUncached();
		}
	}

	void StorageIO::enableBufferPool(size_t capacityBytes,
                                     std::shared_ptr<std::pmr::memory_resource> memoryOwner) {
		auto pool = std::make_shared<BufferPool>(*this, capacityBytes, std::move(memoryOwner));
		std::lock_guard lock(buffer_pool_mutex_);
		if (buffer_pool_ != nullptr) {
			throw std::logic_error("StorageIO buffer pool is already enabled");
		}
		buffer_pool_ = std::move(pool);
	}

	void StorageIO::disableBufferPool() {
		const std::shared_ptr<BufferPool> pool = bufferPool();
		if (pool == nullptr) {
			return;
		}
		pool->flush();
		std::lock_guard lock(buffer_pool_mutex_);
		if (buffer_pool_ == pool) {
			buffer_pool_.reset();
		}
	}

	std::optional<BufferPool::Pin> StorageIO::pinTypedPage(const BufferPoolPageKey key,
														  const BufferPoolReadHint hint) const {
		if (key.generation == 0U || key.page_offset % RSS_BUFFER_POOL_PAGE_BYTES != 0U) {
			throw std::invalid_argument("StorageIO::pinTypedPage requires an aligned durable page key");
		}
		const std::shared_ptr<BufferPool> pool = bufferPool();
		if (pool == nullptr) {
			return std::nullopt;
		}
		BufferPool::Pin pin = pool->pin(key, hint);
		recordRead(pin.validBytes());
		return pin;
	}

	void StorageIO::discardBufferPoolForExternalRebase() {
		saturatingAdd(data_mutation_epoch_, 1U);
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->discardForExternalRebase();
		}
	}

	void StorageIO::prefetchPages(std::span<const BufferPoolPageKey> keys, BufferPoolReadHint admissionHint) const {
		if (keys.empty()) {
			return;
		}
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->prefetch(keys, admissionHint);
		}
	}

	BufferPoolStats StorageIO::bufferPoolStats() const {
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			return pool->stats();
		}
		return {};
	}

	void StorageIO::resetBufferPoolStats() {
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->resetStats();
		}
	}

	void StorageIO::requireWritable(const char *operation) const {
		if (!isWritable()) {
			throw std::logic_error(std::string("StorageIO::") + operation + " requires writable access");
		}
	}

	std::shared_ptr<BufferPool> StorageIO::bufferPool() const {
		std::lock_guard lock(buffer_pool_mutex_);
		return buffer_pool_;
	}

	StorageIOStats StorageIO::stats() const {
		return {read_bytes_.load(std::memory_order_relaxed),		   write_bytes_.load(std::memory_order_relaxed),
				reserved_bytes_.load(std::memory_order_relaxed),	   read_calls_.load(std::memory_order_relaxed),
				write_calls_.load(std::memory_order_relaxed),		   reserve_calls_.load(std::memory_order_relaxed),
				sync_calls_.load(std::memory_order_relaxed),		   data_sync_calls_.load(std::memory_order_relaxed),
				superblock_sync_calls_.load(std::memory_order_relaxed)};
	}

	void StorageIO::resetStats() {
		read_bytes_.store(0U, std::memory_order_relaxed);
		write_bytes_.store(0U, std::memory_order_relaxed);
		reserved_bytes_.store(0U, std::memory_order_relaxed);
		read_calls_.store(0U, std::memory_order_relaxed);
		write_calls_.store(0U, std::memory_order_relaxed);
		reserve_calls_.store(0U, std::memory_order_relaxed);
		sync_calls_.store(0U, std::memory_order_relaxed);
		data_sync_calls_.store(0U, std::memory_order_relaxed);
		superblock_sync_calls_.store(0U, std::memory_order_relaxed);
	}

} // namespace graph::storage
