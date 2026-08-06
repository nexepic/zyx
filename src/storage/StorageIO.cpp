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
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/relocatable/cache/BufferPool.hpp"

namespace graph::storage {

	namespace {

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
		thread_local StorageIO::NativeWriteHookForTesting nativeWriteHookForTesting;
		thread_local StorageIO::NativeReadHookForTesting nativeReadHookForTesting;
		thread_local StorageIO::NativeSyncHookForTesting nativeSyncHookForTesting;
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

		[[noreturn]] void throwNativeWriteError(uint64_t offset) {
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
			throw std::system_error(error, "StorageIO::writeAt pwrite failed at offset " + std::to_string(offset));
		}

	} // namespace

	StorageIO::StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd) :
		StorageIO(std::move(stream), writeFd, readFd, StorageIOAccessMode::SIOAM_READ_WRITE) {}

	StorageIO::StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd,
						 StorageIOAccessMode accessMode) :
		stream_(std::move(stream)), writeFd_(writeFd), readFd_(readFd), access_mode_(accessMode) {
		if (access_mode_ != StorageIOAccessMode::SIOAM_READ_WRITE &&
			access_mode_ != StorageIOAccessMode::SIOAM_READ_ONLY) {
			throw std::invalid_argument("StorageIO received an unknown access mode");
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
		saturatingAdd(write_bytes_, bytes);
		saturatingAdd(write_calls_, 1U);
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

	void StorageIO::writeAt(uint64_t offset, const void *buf, size_t size) {
		if (!buf || size == 0)
			return;
		requireWritable("writeAt");
		requireAddressableRange(offset, size, "writeAt");
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->write(offset, {static_cast<const std::byte *>(buf), size});
			recordWrite(size);
			return;
		}
		writeAtUncached(offset, buf, size);
		recordWrite(size);
	}

	void StorageIO::writeAt(BufferPoolPageKey key, const void *buf, size_t size) {
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::writeAt typed page buffer is null");
		}
		requireTypedPageKey(key, size, "writeAt");
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

	void StorageIO::writeAtUncached(uint64_t offset, const void *buf, size_t size) {
		if (!buf || size == 0U) {
			return;
		}
		requireWritable("writeAt");
		requireAddressableRange(offset, size, "writeAt");

		if (writeFd_ != INVALID_FILE_HANDLE) {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
			const pwrite_ssize_t written =
					nativeWriteHookForTesting
							? nativeWriteHookForTesting(writeFd_, buf, size, static_cast<pwrite_off_t>(offset))
							: portable_pwrite(writeFd_, buf, size, static_cast<pwrite_off_t>(offset));
#else
			const pwrite_ssize_t written = portable_pwrite(writeFd_, buf, size, static_cast<pwrite_off_t>(offset));
#endif
			if (written < 0 || static_cast<size_t>(written) != size) {
				throwNativeWriteError(offset);
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
			const size_t result = pool->read(offset, {static_cast<std::byte *>(buf), size});
			recordRead(result);
			return result;
		}
		const size_t result = readAtUncached(offset, buf, size);
		recordRead(result);
		return result;
	}

	size_t StorageIO::readAt(BufferPoolPageKey key, void *buf, size_t size, BufferPoolReadHint hint) const {
		if (buf == nullptr) {
			throw std::invalid_argument("StorageIO::readAt typed page buffer is null");
		}
		requireTypedPageKey(key, size, "readAt");
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
		requireAddressableRange(offset, size, "append");
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
		requireAddressableRange(offset, size, "reserveAppendSpace");
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
			throwNativeWriteError(size);
		}
	}

	uint64_t StorageIO::fileSizeUncached() const {
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

	void StorageIO::sync() {
		if (const std::shared_ptr<BufferPool> pool = bufferPool()) {
			pool->flush();
		}
		syncUncached();
		saturatingAdd(sync_calls_, 1U);
	}

	void StorageIO::syncUncached() {
		if (writeFd_ != INVALID_FILE_HANDLE) {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
			const int result = nativeSyncHookForTesting ? nativeSyncHookForTesting(writeFd_) : portable_fsync(writeFd_);
#else
			const int result = portable_fsync(writeFd_);
#endif
			if (result != 0) {
				throw std::runtime_error("StorageIO::sync native durable sync failed");
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
		if (minimumSize == 0U || fileSizeUncached() >= minimumSize) {
			return;
		}
		requireWritable("ensureFileSize");
		requireAddressableRange(minimumSize - 1U, 1U, "ensureFileSize");
		const char zero = 0;
		writeAtUncached(minimumSize - 1U, &zero, 1U);
		if (writeFd_ == INVALID_FILE_HANDLE) {
			flushStreamUncached();
		}
	}

	void StorageIO::enableBufferPool(size_t capacityBytes) {
		auto pool = std::make_shared<BufferPool>(*this, capacityBytes);
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
		return {read_bytes_.load(std::memory_order_relaxed),	 write_bytes_.load(std::memory_order_relaxed),
				reserved_bytes_.load(std::memory_order_relaxed), read_calls_.load(std::memory_order_relaxed),
				write_calls_.load(std::memory_order_relaxed),	 reserve_calls_.load(std::memory_order_relaxed),
				sync_calls_.load(std::memory_order_relaxed)};
	}

	void StorageIO::resetStats() {
		read_bytes_.store(0U, std::memory_order_relaxed);
		write_bytes_.store(0U, std::memory_order_relaxed);
		reserved_bytes_.store(0U, std::memory_order_relaxed);
		read_calls_.store(0U, std::memory_order_relaxed);
		write_calls_.store(0U, std::memory_order_relaxed);
		reserve_calls_.store(0U, std::memory_order_relaxed);
		sync_calls_.store(0U, std::memory_order_relaxed);
	}

} // namespace graph::storage
