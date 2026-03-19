/**
 * @file WALRecord.hpp
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

#pragma once

#include <cstdint>
#include <vector>

namespace graph::storage::wal {

	// WAL file: <db_path>-wal
	// Header: 32 bytes (magic, version, db_size, salt1, salt2, reserved)
	// Records: variable size, each with CRC32 checksum

	constexpr uint32_t WAL_MAGIC = 0x5A594C57; // "ZYLW"
	constexpr uint32_t WAL_VERSION = 1;

	enum class WALRecordType : uint8_t {
		WAL_TXN_BEGIN = 1,
		WAL_TXN_COMMIT = 2,
		WAL_TXN_ROLLBACK = 3,
		WAL_ENTITY_WRITE = 4
	};

	struct WALFileHeader {
		uint32_t magic = WAL_MAGIC;
		uint32_t version = WAL_VERSION;
		uint64_t dbFileSize = 0;
		uint32_t salt1 = 0;
		uint32_t salt2 = 0;
		uint8_t reserved[8] = {};
	};
	static_assert(sizeof(WALFileHeader) == 32);

	struct WALRecordHeader {
		uint32_t recordSize; // Total size of this record including header
		uint64_t txnId;
		WALRecordType type;
		uint32_t checksum; // CRC32 of data after this header
		uint8_t padding[3] = {}; // Align to predictable boundary
	};

	// For WAL_ENTITY_WRITE records:
	struct WALEntityPayload {
		uint8_t entityType; // graph::EntityType value
		uint8_t changeType; // EntityChangeType value
		int64_t entityId;
		uint32_t dataSize; // 0 for DELETE
		// Followed by dataSize bytes of serialized entity
	};

	uint32_t computeCRC32(const uint8_t *data, size_t size);

	std::vector<uint8_t> serializeRecordHeader(const WALRecordHeader &header);
	WALRecordHeader deserializeRecordHeader(const uint8_t *data);

	std::vector<uint8_t> serializeFileHeader(const WALFileHeader &header);
	WALFileHeader deserializeFileHeader(const uint8_t *data);

	std::vector<uint8_t> serializeEntityPayload(const WALEntityPayload &payload);
	WALEntityPayload deserializeEntityPayload(const uint8_t *data);

} // namespace graph::storage::wal
