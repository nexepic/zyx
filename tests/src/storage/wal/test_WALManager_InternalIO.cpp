/**
 * @file test_WALManager_InternalIO.cpp
 * @brief Private-branch tests for WALManager defensive paths.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>

// The `#define private public` hack does not work on Windows because MSVC
// includes access specifiers in mangled names.
#define private public
#include "graph/storage/wal/WALManager.hpp"
#undef private
#endif

#include "graph/storage/wal/WALManager.hpp"

#include "graph/storage/wal/WALRecord.hpp"

namespace fs = std::filesystem;
using namespace graph::storage::wal;

class WALManagerInternalIOTest : public ::testing::Test {
protected:
	fs::path testDbPath;
	std::string walPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_wal_private_" + boost::uuids::to_string(uuid) + ".zyx");
		walPath = testDbPath.string() + "-wal";
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove(testDbPath, ec);
		fs::remove(walPath, ec);
	}

	static void writeRaw(const std::string &path, const std::vector<uint8_t> &bytes) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(out.is_open());
		out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		ASSERT_TRUE(out.good());
	}
};

#ifndef _WIN32
// The `#define private public` hack does not work on Windows because MSVC
// includes access specifiers in mangled names — the linker cannot match
// symbols compiled as private against references compiled as public.

TEST_F(WALManagerInternalIOTest, WriteRecordWithNonNullZeroLengthCoversShortCircuitFalsePath) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	uint8_t byte = 0xAB;
	EXPECT_NO_THROW(mgr.writeRecord(WALRecordType::WAL_TXN_BEGIN, 1, &byte, 0));

	mgr.close();
}

TEST_F(WALManagerInternalIOTest, WriteRecordWithNullPositiveLengthCoversChecksumFalsePath) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	EXPECT_NO_THROW(mgr.writeRecord(WALRecordType::WAL_TXN_BEGIN, 1, nullptr, 4));

	mgr.close();
}

TEST_F(WALManagerInternalIOTest, WriteRecordWithNonNullPayloadPersistsChecksumProtectedData) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
	ASSERT_NO_THROW(mgr.writeRecord(
			WALRecordType::WAL_TXN_BEGIN,
			11,
			payload.data(),
			static_cast<uint32_t>(payload.size())));
	mgr.sync();

	auto result = mgr.readRecords();
	ASSERT_FALSE(result.corrupted);
	ASSERT_EQ(result.records.size(), 1U);
	EXPECT_EQ(result.records.front().header.type, WALRecordType::WAL_TXN_BEGIN);
	EXPECT_EQ(result.records.front().header.checksum, computeCRC32(payload.data(), payload.size()));
	EXPECT_EQ(result.records.front().data, payload);

	mgr.close();
}

TEST_F(WALManagerInternalIOTest, WriteHeaderCanSkipSyncWhenCallerOwnsDurabilityBoundary) {
	WALManager mgr;
	mgr.open(testDbPath.string());
	mgr.lastSyncedOffset_ = 0;

	ASSERT_NO_THROW(mgr.writeHeader(false));
	EXPECT_EQ(mgr.lastSyncedOffset_, 0U);

	mgr.close();
}

TEST_F(WALManagerInternalIOTest, EnsureAppendCapacityUsesDefaultBufferWhenNoBufferExists) {
	WALManager mgr;
	mgr.setBufferSize(4096);

	ASSERT_NO_THROW(mgr.ensureAppendCapacityLocked(64));
	EXPECT_GE(mgr.writeBuffer_.capacity(), 4096U);
}

TEST_F(WALManagerInternalIOTest, ValidateHeaderReturnsFalseWhenFdIsInvalidEvenIfOpenFlagIsTrue) {
	WALFileHeader hdr;
	writeRaw(walPath, serializeFileHeader(hdr));

	WALManager mgr;
	mgr.isOpen_ = true;
	mgr.walPath_ = walPath;
	mgr.walFd_ = graph::storage::INVALID_FILE_HANDLE;

	EXPECT_FALSE(mgr.validateHeader());
}

TEST_F(WALManagerInternalIOTest, OpenStateMethodsSkipInvalidFdWithoutThrowing) {
	WALFileHeader hdr;
	writeRaw(walPath, serializeFileHeader(hdr));

	WALManager mgr;
	mgr.isOpen_ = true;
	mgr.walPath_ = walPath;
	mgr.walFd_ = graph::storage::INVALID_FILE_HANDLE;

	EXPECT_NO_THROW(mgr.sync());
	EXPECT_FALSE(mgr.needsRecovery());
	EXPECT_TRUE(mgr.readRecords().records.empty());
}

TEST_F(WALManagerInternalIOTest, CheckpointCanRecreateWhenOpenFlagHasInvalidFd) {
	WALFileHeader hdr;
	writeRaw(walPath, serializeFileHeader(hdr));

	WALManager mgr;
	mgr.isOpen_ = true;
	mgr.walPath_ = walPath;
	mgr.walFd_ = graph::storage::INVALID_FILE_HANDLE;

	ASSERT_NO_THROW(mgr.checkpoint());
	EXPECT_TRUE(mgr.isOpen_);
	EXPECT_TRUE(mgr.validateHeader());

	mgr.close();
}

TEST_F(WALManagerInternalIOTest, ValidateHeaderReturnsFalseOnShortReadFromNonSeekableFd) {
	WALFileHeader hdr;
	writeRaw(walPath, serializeFileHeader(hdr));

	int pipeFds[2] = {-1, -1};
	ASSERT_EQ(pipe(pipeFds), 0);

	WALManager mgr;
	mgr.isOpen_ = true;
	mgr.walPath_ = walPath;
	mgr.walFd_ = pipeFds[0];

	EXPECT_FALSE(mgr.validateHeader());

	close(pipeFds[0]);
	close(pipeFds[1]);
}

TEST_F(WALManagerInternalIOTest, FlushBufferThrowsWhenFdCannotWriteAllBytes) {
	WALFileHeader hdr;
	writeRaw(walPath, serializeFileHeader(hdr));

	const int roFd = ::open(walPath.c_str(), O_RDONLY);
	ASSERT_GE(roFd, 0);

	WALManager mgr;
	mgr.walFd_ = roFd;
	mgr.currentWriteOffset_ = 0;
	mgr.writeBuffer_ = {0x01, 0x02, 0x03, 0x04};

	EXPECT_THROW(mgr.flushBuffer(), std::runtime_error);
	close(roFd);
}

TEST_F(WALManagerInternalIOTest, WriteHeaderThrowsWhenFdCannotWriteAllBytes) {
	WALFileHeader hdr;
	writeRaw(walPath, serializeFileHeader(hdr));

	const int roFd = ::open(walPath.c_str(), O_RDONLY);
	ASSERT_GE(roFd, 0);

	WALManager mgr;
	mgr.walFd_ = roFd;
	EXPECT_THROW(mgr.writeHeader(), std::runtime_error);

	close(roFd);
}
#endif

TEST_F(WALManagerInternalIOTest, WriteRecordTriggersFlushWhenBufferReachesThreshold) {
	WALManager mgr;
	mgr.setBufferSize(1);
	mgr.open(testDbPath.string());

	EXPECT_NO_THROW(mgr.writeBegin(1));
	EXPECT_NO_THROW(mgr.writeRollback(1));

	mgr.close();
}
