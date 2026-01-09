/**
 * @file WALRecord.hpp
 * @author Nexepic
 * @date 2025/3/24
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#pragma once

#include <cstdint>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace graph::storage::wal {

	// Magic number for WAL records
	constexpr uint32_t WAL_RECORD_MAGIC = 0x57414C52; // "WALR" in ASCII

	// Type of WAL operations
	enum class WALRecordType : uint8_t {
		NODE_INSERT = 1,
		NODE_UPDATE = 2,
		NODE_DELETE = 3,
		EDGE_INSERT = 4,
		EDGE_UPDATE = 5,
		EDGE_DELETE = 6,
		TRANSACTION_BEGIN = 7,
		TRANSACTION_COMMIT = 8,
		TRANSACTION_ROLLBACK = 9,
		CHECKPOINT = 10
	};

	// WAL record header (fixed size)
	struct WALRecordHeader {
		uint32_t magic = WAL_RECORD_MAGIC; // Magic number for validation
		uint32_t recordSize; // Total size of the record including header
		uint64_t lsn; // Log Sequence Number
		WALRecordType recordType; // Type of operation
		uint64_t transactionId; // Transaction ID (0 if not part of transaction)
		uint32_t checksum; // CRC32 checksum of record
	};

	// Base class for all WAL records
	class WALRecord {
	public:
		WALRecord(WALRecordType type, uint64_t txnId = 0) : header_({WAL_RECORD_MAGIC, 0, 0, type, txnId, 0}) {}

		virtual ~WALRecord() = default;

		// Get record type
		WALRecordType getType() const { return header_.recordType; }

		// Get transaction ID
		uint64_t getTransactionId() const { return header_.transactionId; }

		// Get Log Sequence Number
		uint64_t getLSN() const { return header_.lsn; }

		// Set Log Sequence Number
		void setLSN(uint64_t lsn) { header_.lsn = lsn; }

		// Get total record size
		virtual size_t getSize() const = 0;

		// Serialize record to binary
		virtual void serialize(uint8_t *buffer) const = 0;

		// Deserialize record from binary
		virtual void deserialize(std::istream &in) = 0;

		// Deserialize record from binary
		static std::unique_ptr<WALRecord> deserialize(const uint8_t *buffer);

		// Create record from stream
		static std::unique_ptr<WALRecord> createFromStream(std::istream &in);

	protected:
		WALRecordHeader header_;

		// Calculate CRC32 checksum
		uint32_t calculateChecksum() const;
	};

} // namespace graph::storage::wal
