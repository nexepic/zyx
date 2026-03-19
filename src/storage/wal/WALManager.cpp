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
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace graph::storage::wal {

	WALManager::~WALManager() { close(); }

	void WALManager::open(const std::string &dbPath) {
		if (isOpen_)
			return;

		walPath_ = dbPath + "-wal";

		bool exists = std::filesystem::exists(walPath_);

		if (exists) {
			// Open existing WAL for reading (recovery check) and appending
			walFile_.open(walPath_, std::ios::binary | std::ios::in | std::ios::out);
			if (!walFile_.is_open()) {
				throw std::runtime_error("Cannot open WAL file: " + walPath_);
			}
		} else {
			// Create new WAL file
			walFile_.open(walPath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
			if (!walFile_.is_open()) {
				throw std::runtime_error("Cannot create WAL file: " + walPath_);
			}
			writeHeader();
		}

		isOpen_ = true;
	}

	void WALManager::close() {
		if (isOpen_) {
			if (walFile_.is_open()) {
				walFile_.flush();
				walFile_.close();
			}
			isOpen_ = false;
		}
	}

	void WALManager::writeHeader() {
		WALFileHeader header;
		auto buf = serializeFileHeader(header);
		walFile_.seekp(0);
		walFile_.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
		walFile_.flush();
	}

	bool WALManager::validateHeader() {
		if (!walFile_.is_open())
			return false;

		walFile_.seekg(0, std::ios::end);
		auto fileSize = walFile_.tellg();

		if (fileSize < static_cast<std::streamoff>(sizeof(WALFileHeader)))
			return false;

		walFile_.seekg(0);
		uint8_t buf[sizeof(WALFileHeader)];
		walFile_.read(reinterpret_cast<char *>(buf), sizeof(WALFileHeader));

		if (!walFile_.good())
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

		// Seek to end for append
		walFile_.seekp(0, std::ios::end);
		walFile_.write(reinterpret_cast<const char *>(headerBuf.data()), static_cast<std::streamsize>(headerBuf.size()));

		if (data && dataSize > 0) {
			walFile_.write(reinterpret_cast<const char *>(data), dataSize);
		}
	}

	void WALManager::writeBegin(uint64_t txnId) { writeRecord(WALRecordType::WAL_TXN_BEGIN, txnId); }

	void WALManager::writeEntityChange(uint64_t txnId, uint8_t entityType, uint8_t changeType, int64_t entityId,
									   const std::vector<uint8_t> &serializedData) {
		// Build payload: WALEntityPayload header + serialized data
		WALEntityPayload payload{};
		payload.entityType = entityType;
		payload.changeType = changeType;
		payload.entityId = entityId;
		payload.dataSize = static_cast<uint32_t>(serializedData.size());

		auto payloadHeader = serializeEntityPayload(payload);

		// Combine payload header + entity data
		std::vector<uint8_t> fullData;
		fullData.reserve(payloadHeader.size() + serializedData.size());
		fullData.insert(fullData.end(), payloadHeader.begin(), payloadHeader.end());
		fullData.insert(fullData.end(), serializedData.begin(), serializedData.end());

		writeRecord(WALRecordType::WAL_ENTITY_WRITE, txnId, fullData.data(), static_cast<uint32_t>(fullData.size()));
	}

	void WALManager::writeCommit(uint64_t txnId) { writeRecord(WALRecordType::WAL_TXN_COMMIT, txnId); }

	void WALManager::writeRollback(uint64_t txnId) { writeRecord(WALRecordType::WAL_TXN_ROLLBACK, txnId); }

	void WALManager::sync() {
		if (isOpen_ && walFile_.is_open()) {
			walFile_.flush();
			// Note: std::fstream::flush() doesn't guarantee fsync.
			// For production use, we'd need platform-specific fsync.
			// For now, flush() provides reasonable durability.
		}
	}

	void WALManager::checkpoint() {
		if (!isOpen_)
			return;

		// Close existing WAL
		walFile_.close();

		// Truncate by reopening in trunc mode
		walFile_.open(walPath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		if (!walFile_.is_open()) {
			throw std::runtime_error("Cannot truncate WAL file: " + walPath_);
		}
		writeHeader();
	}

	bool WALManager::needsRecovery() {
		if (!isOpen_ || !walFile_.is_open())
			return false;

		if (!validateHeader())
			return false;

		// Check if there are any records beyond the file header
		walFile_.seekg(0, std::ios::end);
		auto fileSize = walFile_.tellg();

		return fileSize > static_cast<std::streamoff>(sizeof(WALFileHeader));
	}

} // namespace graph::storage::wal
