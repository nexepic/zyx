/**
 * @file WALManager.cpp
 * @date 2026/3/19
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/wal/WALManager.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace graph::storage::wal {

	WALManager::~WALManager() {
		try {
			close();
		} catch (...) {
			// Destructors must not throw. Keeping the WAL is safer than losing
			// recovery records when a best-effort close fails.
		}
	}

	void WALManager::open(const std::string &dbPath) {
		if (isOpen_)
			return;

		walPath_ = dbPath + "-wal";

		bool exists = std::filesystem::exists(walPath_);

		if (!exists) {
			// Create new WAL file — use fstream to create, then close and reopen with native fd
			{
				std::ofstream tmp(walPath_, std::ios::binary | std::ios::trunc);
				if (!tmp.is_open()) {
					throw std::runtime_error("Cannot create WAL file: " + walPath_);
				}
			}
		}

		// Open with native file descriptor for pwrite/pread/fsync
		walFd_ = portable_open_rw(walPath_.c_str());
		if (walFd_ == INVALID_FILE_HANDLE) {
			throw std::runtime_error("Cannot open WAL file with native fd: " + walPath_);
		}

		if (!exists) {
			// Keep the file header durable as its own boundary. This avoids
			// making the first transaction commit pay for both WAL file creation
			// metadata and the transaction payload, which is especially visible
			// for large bulk imports.
			writeHeader(true);
			currentWriteOffset_ = sizeof(WALFileHeader);
			lastSyncedOffset_ = currentWriteOffset_;
		} else {
			// Determine current file size for write offset
			// Read to find end of file
			// Use lseek or stat to get file size
			auto fileSize = std::filesystem::file_size(walPath_);
			currentWriteOffset_ = fileSize;
			lastSyncedOffset_ = currentWriteOffset_;
		}

		writeBuffer_.reserve(walBufferSize_);
		isOpen_ = true;
	}

	void WALManager::close(CloseMode mode, CloseSyncMode syncMode) {
		if (isOpen_) {
			{
				std::lock_guard lock(commitMutex_);
				if (syncMode == CloseSyncMode::WSM_SYNC) {
					flushAndSync();
				} else if (!writeBuffer_.empty()) {
					flushBuffer();
				}
			}
			if (walFd_ != INVALID_FILE_HANDLE) {
				portable_close_rw(walFd_);
				walFd_ = INVALID_FILE_HANDLE;
			}
			isOpen_ = false;
		}

		if (mode == CloseMode::WCM_REMOVE_FILE && !walPath_.empty()) {
			try {
				std::filesystem::remove(walPath_);
			} catch (...) {
				// Non-fatal: callers only request removal after a successful
				// checkpoint, so a leftover empty WAL can be handled on next open.
			}
		}
	}

	void WALManager::writeHeader(bool syncHeader) {
		WALFileHeader header;
		auto buf = serializeFileHeader(header);
		ssize_t n = portable_pwrite(walFd_, buf.data(), buf.size(), 0);
		if (n < static_cast<ssize_t>(buf.size())) {
			throw std::runtime_error("Failed to write WAL header");
		}
		if (syncHeader) {
			portable_fsync(walFd_);
			lastSyncedOffset_ = (std::max)(lastSyncedOffset_, static_cast<uint64_t>(sizeof(WALFileHeader)));
		}
	}

	bool WALManager::validateHeader() {
		if (walFd_ == INVALID_FILE_HANDLE)
			return false;

		auto fileSize = std::filesystem::file_size(walPath_);
		if (fileSize < sizeof(WALFileHeader))
			return false;

		uint8_t buf[sizeof(WALFileHeader)];
		ssize_t n = portable_pread(walFd_, buf, sizeof(WALFileHeader), 0);
		if (n < static_cast<ssize_t>(sizeof(WALFileHeader)))
			return false;

		WALFileHeader header = deserializeFileHeader(buf);
		return header.magic == WAL_MAGIC && header.version == WAL_VERSION;
	}

	void WALManager::writeRecord(WALRecordType type, uint64_t txnId, const uint8_t *data, uint32_t dataSize) {
		if (!isOpen_)
			throw std::runtime_error("WAL is not open");

		std::lock_guard lock(commitMutex_);
		appendRecordLocked(type, txnId, data, dataSize);
	}

	void WALManager::appendRecordLocked(WALRecordType type, uint64_t txnId, const uint8_t *data, uint32_t dataSize) {
		WALRecordHeader recordHeader{};
		recordHeader.recordSize = static_cast<uint32_t>(sizeof(WALRecordHeader) + dataSize);
		recordHeader.txnId = txnId;
		recordHeader.type = type;
		recordHeader.checksum = (data && dataSize > 0) ? computeCRC32(data, dataSize) : 0;

		const size_t recordSize = sizeof(recordHeader) + dataSize;
		ensureAppendCapacityLocked(recordSize);
		const size_t offset = writeBuffer_.size();
		writeBuffer_.resize(offset + recordSize);
		auto *dest = writeBuffer_.data() + offset;
		std::memcpy(dest, &recordHeader, sizeof(recordHeader));
		if (data && dataSize > 0) {
			std::memcpy(dest + sizeof(recordHeader), data, dataSize);
		}

		// If buffer is full, flush to file (no fsync)
		if (writeBuffer_.size() >= walBufferSize_) {
			flushBuffer();
		}
	}

	void WALManager::appendEntityChangeRecordLocked(uint64_t txnId, uint8_t entityType, uint8_t changeType,
													int64_t entityId, const uint8_t *data, uint32_t dataSize) {
		WALEntityPayload payload{};
		payload.entityType = entityType;
		payload.changeType = changeType;
		payload.entityId = entityId;
		payload.dataSize = dataSize;

		std::array<uint8_t, sizeof(WALEntityPayload)> payloadBytes{};
		std::memcpy(payloadBytes.data(), &payload, sizeof(payload));

		uint32_t checksum = extendCRC32(0L, payloadBytes.data(), payloadBytes.size());
		if (data && dataSize > 0) {
			checksum = extendCRC32(checksum, data, dataSize);
		}

		WALRecordHeader recordHeader{};
		recordHeader.recordSize = static_cast<uint32_t>(sizeof(WALRecordHeader) + payloadBytes.size() + dataSize);
		recordHeader.txnId = txnId;
		recordHeader.type = WALRecordType::WAL_ENTITY_WRITE;
		recordHeader.checksum = checksum;

		const size_t recordSize = sizeof(recordHeader) + payloadBytes.size() + dataSize;
		ensureAppendCapacityLocked(recordSize);
		const size_t offset = writeBuffer_.size();
		writeBuffer_.resize(offset + recordSize);
		auto *dest = writeBuffer_.data() + offset;
		std::memcpy(dest, &recordHeader, sizeof(recordHeader));
		dest += sizeof(recordHeader);
		std::memcpy(dest, payloadBytes.data(), payloadBytes.size());
		if (data && dataSize > 0) {
			std::memcpy(dest + payloadBytes.size(), data, dataSize);
		}

		if (writeBuffer_.size() >= walBufferSize_) {
			flushBuffer();
		}
	}

	void WALManager::ensureAppendCapacityLocked(size_t appendSize) {
		const size_t required = writeBuffer_.size() + appendSize;
		if (required <= writeBuffer_.capacity()) {
			return;
		}

		size_t target = writeBuffer_.capacity();
		if (target == 0) {
			target = walBufferSize_ == 0 ? appendSize : walBufferSize_;
		}
		while (target < required) {
			if (target > std::numeric_limits<size_t>::max() / 2) {
				target = required;
				break;
			}
			target *= 2;
		}
		writeBuffer_.reserve(target);
	}

	void WALManager::flushBuffer() {
		// Must be called with commitMutex_ held
		if (writeBuffer_.empty())
			return;

		ssize_t n = portable_pwrite(walFd_, writeBuffer_.data(), writeBuffer_.size(),
									static_cast<int64_t>(currentWriteOffset_));
		if (n < static_cast<ssize_t>(writeBuffer_.size())) {
			throw std::runtime_error("Failed to write WAL buffer");
		}
		currentWriteOffset_ += writeBuffer_.size();
		writeBuffer_.clear();
	}

	void WALManager::flushAndSync() {
		// Must be called with commitMutex_ held
		flushBuffer();
		if (lastSyncedOffset_ >= currentWriteOffset_) {
			return;
		}
		portable_fsync(walFd_);
		lastSyncedOffset_ = currentWriteOffset_;
	}

	void WALManager::writeBegin(uint64_t txnId) { writeRecord(WALRecordType::WAL_TXN_BEGIN, txnId); }

	void WALManager::writeEntityChange(uint64_t txnId, uint8_t entityType, uint8_t changeType, int64_t entityId,
									   const std::vector<uint8_t> &serializedData) {
		if (!isOpen_)
			throw std::runtime_error("WAL is not open");

		std::lock_guard lock(commitMutex_);
		appendEntityChangeRecordLocked(txnId, entityType, changeType, entityId, serializedData.data(),
									   static_cast<uint32_t>(serializedData.size()));
	}

	void WALManager::writeEntityChanges(uint64_t txnId, const std::vector<WALEntityChange> &changes) {
		if (!isOpen_)
			throw std::runtime_error("WAL is not open");
		if (changes.empty()) {
			return;
		}

		std::lock_guard lock(commitMutex_);
		for (const auto &change : changes) {
			appendEntityChangeRecordLocked(txnId, change.entityType, change.changeType, change.entityId,
										   change.serializedData.data(),
										   static_cast<uint32_t>(change.serializedData.size()));
		}
	}

	void WALManager::writeEntityChangeViews(uint64_t txnId, std::span<const WALEntityChangeView> changes) {
		if (!isOpen_)
			throw std::runtime_error("WAL is not open");
		if (changes.empty()) {
			return;
		}

		std::lock_guard lock(commitMutex_);
		for (const auto &change : changes) {
			appendEntityChangeRecordLocked(txnId, change.entityType, change.changeType, change.entityId,
										   change.serializedData, change.serializedSize);
		}
	}

	void WALManager::writeCommit(uint64_t txnId) {
		// Append COMMIT record to buffer
		writeRecord(WALRecordType::WAL_TXN_COMMIT, txnId);

		std::unique_lock lock(commitMutex_);

		// Record the target offset that needs to be synced
		uint64_t myOffset = currentWriteOffset_ + writeBuffer_.size();

		if (commitInProgress_) {
			// Another thread is performing group commit — wait for it
			commitCV_.wait(lock, [this, myOffset] { return lastSyncedOffset_ >= myOffset; });
			return;
		}

		// Become the group commit leader
		commitInProgress_ = true;

		// Wait briefly to accumulate more commits from other threads
		if (groupCommitDelayUs_ > 0) {
#ifndef __EMSCRIPTEN__
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::microseconds(groupCommitDelayUs_));
			lock.lock();
#endif
		}

		// Flush buffer + fsync
		flushAndSync();
		commitInProgress_ = false;

		// Wake all waiting committers
		commitCV_.notify_all();
	}

	void WALManager::writeRollback(uint64_t txnId) {
		writeRecord(WALRecordType::WAL_TXN_ROLLBACK, txnId);

		// Flush buffer (no fsync needed for rollback)
		std::lock_guard lock(commitMutex_);
		flushBuffer();
	}

	void WALManager::sync() {
		if (isOpen_ && walFd_ != INVALID_FILE_HANDLE) {
			std::lock_guard lock(commitMutex_);
			flushAndSync();
		}
	}

	void WALManager::checkpoint() {
		if (!isOpen_)
			return;

		// Flush any remaining buffer
		{
			std::lock_guard lock(commitMutex_);
			flushAndSync();
		}

		// Close fd
		if (walFd_ != INVALID_FILE_HANDLE) {
			portable_close_rw(walFd_);
			walFd_ = INVALID_FILE_HANDLE;
		}

		// Truncate by removing and recreating
		std::filesystem::remove(walPath_);

		// Reopen
		{
			std::ofstream tmp(walPath_, std::ios::binary | std::ios::trunc);
			if (!tmp.is_open()) {
				throw std::runtime_error("Cannot recreate WAL file: " + walPath_);
			}
		}

		walFd_ = portable_open_rw(walPath_.c_str());
		if (walFd_ == INVALID_FILE_HANDLE) {
			throw std::runtime_error("Cannot reopen WAL file: " + walPath_);
		}

		writeHeader(true);
		currentWriteOffset_ = sizeof(WALFileHeader);
		lastSyncedOffset_ = currentWriteOffset_;
		writeBuffer_.clear();
	}

	bool WALManager::needsRecovery() {
		if (!isOpen_ || walFd_ == INVALID_FILE_HANDLE)
			return false;

		if (!validateHeader())
			return false;

		auto fileSize = std::filesystem::file_size(walPath_);
		return fileSize > sizeof(WALFileHeader);
	}

	WALReadResult WALManager::readRecords() {
		WALReadResult result;

		if (!isOpen_ || walFd_ == INVALID_FILE_HANDLE)
			return result;

		if (!validateHeader())
			return result;

		auto fileSize = std::filesystem::file_size(walPath_);
		size_t pos = sizeof(WALFileHeader);

		while (pos + sizeof(WALRecordHeader) <= fileSize) {
			// Read record header via pread
			uint8_t headerBuf[sizeof(WALRecordHeader)];
			ssize_t n = portable_pread(walFd_, headerBuf, sizeof(WALRecordHeader), static_cast<int64_t>(pos));
			if (n < static_cast<ssize_t>(sizeof(WALRecordHeader))) {
				result.corrupted = true;
				break;
			}

			WALRecordHeader recHeader = deserializeRecordHeader(headerBuf);

			// Validate record size
			if (recHeader.recordSize < sizeof(WALRecordHeader) || pos + recHeader.recordSize > fileSize) {
				result.corrupted = true;
				break;
			}

			uint32_t dataSize = recHeader.recordSize - static_cast<uint32_t>(sizeof(WALRecordHeader));

			std::vector<uint8_t> data;
			if (dataSize > 0) {
				data.resize(dataSize);
				n = portable_pread(walFd_, data.data(), dataSize,
								   static_cast<int64_t>(pos + sizeof(WALRecordHeader)));
				if (n < static_cast<ssize_t>(dataSize)) {
					result.corrupted = true;
					break;
				}

				// Verify CRC32 checksum
				uint32_t computed = computeCRC32(data.data(), dataSize);
				if (computed != recHeader.checksum) {
					result.corrupted = true;
					break;
				}
			} else {
				if (recHeader.checksum != 0) {
					result.corrupted = true;
					break;
				}
			}

			result.records.push_back({recHeader, std::move(data)});
			pos += recHeader.recordSize;
		}

		return result;
	}

} // namespace graph::storage::wal
