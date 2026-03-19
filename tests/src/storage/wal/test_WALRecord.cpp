/**
 * @file test_WALRecord.cpp
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

#include <gtest/gtest.h>
#include "graph/storage/wal/WALRecord.hpp"

using namespace graph::storage::wal;

TEST(WALRecordTest, FileHeaderSizeIs32Bytes) { EXPECT_EQ(sizeof(WALFileHeader), 32UL); }

TEST(WALRecordTest, FileHeaderDefaultValues) {
	WALFileHeader header;
	EXPECT_EQ(header.magic, WAL_MAGIC);
	EXPECT_EQ(header.version, WAL_VERSION);
	EXPECT_EQ(header.dbFileSize, 0UL);
	EXPECT_EQ(header.salt1, 0U);
	EXPECT_EQ(header.salt2, 0U);
}

TEST(WALRecordTest, FileHeaderRoundTrip) {
	WALFileHeader original;
	original.dbFileSize = 123456;
	original.salt1 = 0xDEAD;
	original.salt2 = 0xBEEF;

	auto buf = serializeFileHeader(original);
	ASSERT_EQ(buf.size(), sizeof(WALFileHeader));

	WALFileHeader deserialized = deserializeFileHeader(buf.data());
	EXPECT_EQ(deserialized.magic, WAL_MAGIC);
	EXPECT_EQ(deserialized.version, WAL_VERSION);
	EXPECT_EQ(deserialized.dbFileSize, 123456UL);
	EXPECT_EQ(deserialized.salt1, 0xDEADU);
	EXPECT_EQ(deserialized.salt2, 0xBEEFU);
}

TEST(WALRecordTest, RecordHeaderRoundTrip) {
	WALRecordHeader original{};
	original.recordSize = 100;
	original.txnId = 42;
	original.type = WALRecordType::WAL_TXN_COMMIT;
	original.checksum = 0x12345678;

	auto buf = serializeRecordHeader(original);
	ASSERT_EQ(buf.size(), sizeof(WALRecordHeader));

	WALRecordHeader deserialized = deserializeRecordHeader(buf.data());
	EXPECT_EQ(deserialized.recordSize, 100U);
	EXPECT_EQ(deserialized.txnId, 42UL);
	EXPECT_EQ(deserialized.type, WALRecordType::WAL_TXN_COMMIT);
	EXPECT_EQ(deserialized.checksum, 0x12345678U);
}

TEST(WALRecordTest, EntityPayloadRoundTrip) {
	WALEntityPayload original{};
	original.entityType = 1;
	original.changeType = 2;
	original.entityId = 999;
	original.dataSize = 256;

	auto buf = serializeEntityPayload(original);
	ASSERT_EQ(buf.size(), sizeof(WALEntityPayload));

	WALEntityPayload deserialized = deserializeEntityPayload(buf.data());
	EXPECT_EQ(deserialized.entityType, 1);
	EXPECT_EQ(deserialized.changeType, 2);
	EXPECT_EQ(deserialized.entityId, 999);
	EXPECT_EQ(deserialized.dataSize, 256U);
}

TEST(WALRecordTest, CRC32Deterministic) {
	std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};

	uint32_t crc1 = computeCRC32(data.data(), data.size());
	uint32_t crc2 = computeCRC32(data.data(), data.size());

	EXPECT_EQ(crc1, crc2);
	EXPECT_NE(crc1, 0U);
}

TEST(WALRecordTest, CRC32DifferentDataDifferentChecksum) {
	std::vector<uint8_t> data1 = {0x01, 0x02, 0x03};
	std::vector<uint8_t> data2 = {0x04, 0x05, 0x06};

	uint32_t crc1 = computeCRC32(data1.data(), data1.size());
	uint32_t crc2 = computeCRC32(data2.data(), data2.size());

	EXPECT_NE(crc1, crc2);
}

TEST(WALRecordTest, WALMagicConstant) { EXPECT_EQ(WAL_MAGIC, 0x5A594C57U); }

TEST(WALRecordTest, WALVersionConstant) { EXPECT_EQ(WAL_VERSION, 1U); }

TEST(WALRecordTest, RecordTypeValues) {
	EXPECT_EQ(static_cast<uint8_t>(WALRecordType::WAL_TXN_BEGIN), 1);
	EXPECT_EQ(static_cast<uint8_t>(WALRecordType::WAL_TXN_COMMIT), 2);
	EXPECT_EQ(static_cast<uint8_t>(WALRecordType::WAL_TXN_ROLLBACK), 3);
	EXPECT_EQ(static_cast<uint8_t>(WALRecordType::WAL_ENTITY_WRITE), 4);
}
