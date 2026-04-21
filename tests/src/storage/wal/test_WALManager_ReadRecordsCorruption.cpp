/**
 * @file test_WALManager_ReadRecordsCorruption.cpp
 * @brief Targeted corruption-path tests for WALManager::readRecords().
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecord.hpp"

namespace fs = std::filesystem;
using namespace graph::storage::wal;

class WALManagerReadRecordsCorruptionTest : public ::testing::Test {
protected:
	fs::path testDbPath;
	std::string walPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_wal_read_corrupt_" + boost::uuids::to_string(uuid) + ".zyx");
		walPath = testDbPath.string() + "-wal";
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove(testDbPath, ec);
		fs::remove(walPath, ec);
	}

	static void writeBytes(const std::string &path, const std::vector<uint8_t> &bytes) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(out.is_open());
		out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		ASSERT_TRUE(out.good());
	}
};

TEST_F(WALManagerReadRecordsCorruptionTest, ReadRecordsFlagsCorruptionOnInvalidRecordSize) {
	WALFileHeader fileHeader;
	auto fileHeaderBytes = serializeFileHeader(fileHeader);

	WALRecordHeader invalidRecordHeader{};
	invalidRecordHeader.recordSize = static_cast<uint32_t>(sizeof(WALRecordHeader) - 1);
	invalidRecordHeader.txnId = 1;
	invalidRecordHeader.type = WALRecordType::WAL_TXN_BEGIN;
	invalidRecordHeader.checksum = 0;
	auto recordHeaderBytes = serializeRecordHeader(invalidRecordHeader);

	std::vector<uint8_t> fileBytes;
	fileBytes.reserve(fileHeaderBytes.size() + recordHeaderBytes.size());
	fileBytes.insert(fileBytes.end(), fileHeaderBytes.begin(), fileHeaderBytes.end());
	fileBytes.insert(fileBytes.end(), recordHeaderBytes.begin(), recordHeaderBytes.end());
	writeBytes(walPath, fileBytes);

	WALManager mgr;
	mgr.open(testDbPath.string());
	auto result = mgr.readRecords();
	EXPECT_TRUE(result.corrupted);
	EXPECT_TRUE(result.records.empty());
	mgr.close();
}

TEST_F(WALManagerReadRecordsCorruptionTest, ReadRecordsFlagsCorruptionOnChecksumMismatch) {
	WALFileHeader fileHeader;
	auto fileHeaderBytes = serializeFileHeader(fileHeader);

	std::vector<uint8_t> payload = {0x11, 0x22, 0x33, 0x44};

	WALRecordHeader recordHeader{};
	recordHeader.recordSize = static_cast<uint32_t>(sizeof(WALRecordHeader) + payload.size());
	recordHeader.txnId = 99;
	recordHeader.type = WALRecordType::WAL_ENTITY_WRITE;
	recordHeader.checksum = 0xDEADBEEF; // Intentionally wrong
	auto recordHeaderBytes = serializeRecordHeader(recordHeader);

	std::vector<uint8_t> fileBytes;
	fileBytes.reserve(fileHeaderBytes.size() + recordHeaderBytes.size() + payload.size());
	fileBytes.insert(fileBytes.end(), fileHeaderBytes.begin(), fileHeaderBytes.end());
	fileBytes.insert(fileBytes.end(), recordHeaderBytes.begin(), recordHeaderBytes.end());
	fileBytes.insert(fileBytes.end(), payload.begin(), payload.end());
	writeBytes(walPath, fileBytes);

	WALManager mgr;
	mgr.open(testDbPath.string());
	auto result = mgr.readRecords();
	EXPECT_TRUE(result.corrupted);
	EXPECT_TRUE(result.records.empty());
	mgr.close();
}

TEST_F(WALManagerReadRecordsCorruptionTest, ReadRecordsFlagsCorruptionWhenZeroDataHasNonZeroChecksum) {
	WALFileHeader fileHeader;
	auto fileHeaderBytes = serializeFileHeader(fileHeader);

	WALRecordHeader recordHeader{};
	recordHeader.recordSize = static_cast<uint32_t>(sizeof(WALRecordHeader));
	recordHeader.txnId = 7;
	recordHeader.type = WALRecordType::WAL_TXN_COMMIT;
	recordHeader.checksum = 1; // Invalid when dataSize == 0
	auto recordHeaderBytes = serializeRecordHeader(recordHeader);

	std::vector<uint8_t> fileBytes;
	fileBytes.reserve(fileHeaderBytes.size() + recordHeaderBytes.size());
	fileBytes.insert(fileBytes.end(), fileHeaderBytes.begin(), fileHeaderBytes.end());
	fileBytes.insert(fileBytes.end(), recordHeaderBytes.begin(), recordHeaderBytes.end());
	writeBytes(walPath, fileBytes);

	WALManager mgr;
	mgr.open(testDbPath.string());
	auto result = mgr.readRecords();
	EXPECT_TRUE(result.corrupted);
	EXPECT_TRUE(result.records.empty());
	mgr.close();
}
