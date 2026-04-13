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
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace graph::storage::wal {

	WALManager::~WALManager() { close(); }

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
			writeHeader();
			currentWriteOffset_ = sizeof(WALFileHeader);
		} else {
			// Determine current file size for write offset
			// Read to find end of file
			// Use lseek or stat to get file size
			auto fileSize = std::filesystem::file_size(walPath_);
			currentWriteOffset_ = fileSize;
		}

		lastSyncedOffset_ = currentWriteOffset_;
		isOpen_ = true;
	}

	void WALManager::close() {
		if (isOpen_) {
			{
				std::lock_guard lock(commitMutex_);
				if (!writeBuffer_.empty()) {
					flushAndSync();
				}
			}
			if (walFd_ != INVALID_FILE_HANDLE) {
				portable_close_rw(walFd_);
				walFd_ = INVALID_FILE_HANDLE;
			}
			isOpen_ = false;
		}
	}

	void WALManager::writeHeader() {
		WALFileHeader header;
		auto buf = serializeFileHeader(header);
		ssize_t n = portable_pwrite(walFd_, buf.data(), buf.size(), 0);
		if (n < static_cast<ssize_t>(buf.size())) {
			throw std::runtime_error("Failed to write WAL header");
		}
		portable_fsync(walFd_);
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

		WALRecordHeader recordHeader{};
		recordHeader.recordSize = static_cast<uint32_t>(sizeof(WALRecordHeader) + dataSize);
		recordHeader.txnId = txnId;
		recordHeader.type = type;
		recordHeader.checksum = (data && dataSize > 0) ? computeCRC32(data, dataSize) : 0;

		auto headerBuf = serializeRecordHeader(recordHeader);

		// Append to write buffer (under commitMutex_)
		std::lock_guard lock(commitMutex_);
		writeBuffer_.insert(writeBuffer_.end(), headerBuf.begin(), headerBuf.end());
		if (data && dataSize > 0) {
			writeBuffer_.insert(writeBuffer_.end(), data, data + dataSize);
		}

		// If buffer is full, flush to file (no fsync)
		if (writeBuffer_.size() >= walBufferSize_) {
			flushBuffer();
		}
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
		portable_fsync(walFd_);
		lastSyncedOffset_ = currentWriteOffset_;
	}

	void WALManager::writeBegin(uint64_t txnId) { writeRecord(WALRecordType::WAL_TXN_BEGIN, txnId); }

	void WALManager::writeEntityChange(uint64_t txnId, uint8_t entityType, uint8_t changeType, int64_t entityId,
									   const std::vector<uint8_t> &serializedData) {
		WALEntityPayload payload{};
		payload.entityType = entityType;
		payload.changeType = changeType;
		payload.entityId = entityId;
		payload.dataSize = static_cast<uint32_t>(serializedData.size());

		auto payloadHeader = serializeEntityPayload(payload);

		std::vector<uint8_t> fullData;
		fullData.reserve(payloadHeader.size() + serializedData.size());
		fullData.insert(fullData.end(), payloadHeader.begin(), payloadHeader.end());
		fullData.insert(fullData.end(), serializedData.begin(), serializedData.end());

		writeRecord(WALRecordType::WAL_ENTITY_WRITE, txnId, fullData.data(), static_cast<uint32_t>(fullData.size()));
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

		writeHeader();
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
