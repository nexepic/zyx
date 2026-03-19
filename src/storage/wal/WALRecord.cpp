/**
 * @file WALRecord.cpp
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

#include "graph/storage/wal/WALRecord.hpp"
#include <cstring>
#include <zlib.h>

namespace graph::storage::wal {

	uint32_t computeCRC32(const uint8_t *data, size_t size) {
		return static_cast<uint32_t>(crc32(0L, data, static_cast<uInt>(size)));
	}

	std::vector<uint8_t> serializeRecordHeader(const WALRecordHeader &header) {
		std::vector<uint8_t> buf(sizeof(WALRecordHeader));
		std::memcpy(buf.data(), &header, sizeof(WALRecordHeader));
		return buf;
	}

	WALRecordHeader deserializeRecordHeader(const uint8_t *data) {
		WALRecordHeader header{};
		std::memcpy(&header, data, sizeof(WALRecordHeader));
		return header;
	}

	std::vector<uint8_t> serializeFileHeader(const WALFileHeader &header) {
		std::vector<uint8_t> buf(sizeof(WALFileHeader));
		std::memcpy(buf.data(), &header, sizeof(WALFileHeader));
		return buf;
	}

	WALFileHeader deserializeFileHeader(const uint8_t *data) {
		WALFileHeader header{};
		std::memcpy(&header, data, sizeof(WALFileHeader));
		return header;
	}

	std::vector<uint8_t> serializeEntityPayload(const WALEntityPayload &payload) {
		std::vector<uint8_t> buf(sizeof(WALEntityPayload));
		std::memcpy(buf.data(), &payload, sizeof(WALEntityPayload));
		return buf;
	}

	WALEntityPayload deserializeEntityPayload(const uint8_t *data) {
		WALEntityPayload payload{};
		std::memcpy(&payload, data, sizeof(WALEntityPayload));
		return payload;
	}

} // namespace graph::storage::wal
