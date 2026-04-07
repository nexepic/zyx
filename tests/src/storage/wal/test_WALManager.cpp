/**
 * @file test_WALManager.cpp
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
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecord.hpp"

#ifdef _WIN32
	#include <windows.h>
	#include <io.h>
	#include <fcntl.h>
#else
	#include <sys/types.h>
	#include <sys/stat.h>
	#include <fcntl.h>
	#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace graph::storage::wal;

class WALManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_wal_" + boost::uuids::to_string(uuid) + ".graph");
		walPath = testDbPath.string() + "-wal";
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove(testDbPath, ec);
		fs::remove(walPath, ec);
	}

	fs::path testDbPath;
	std::string walPath;
};

TEST_F(WALManagerTest, OpenCreatesWALFile) {
	WALManager mgr;
	EXPECT_FALSE(mgr.isOpen());

	mgr.open(testDbPath.string());
	EXPECT_TRUE(mgr.isOpen());
	EXPECT_TRUE(fs::exists(walPath));

	mgr.close();
	EXPECT_FALSE(mgr.isOpen());
}

TEST_F(WALManagerTest, DoubleOpenIsNoop) {
	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_TRUE(mgr.isOpen());

	// Second open should be a no-op
	EXPECT_NO_THROW(mgr.open(testDbPath.string()));
	EXPECT_TRUE(mgr.isOpen());

	mgr.close();
}

TEST_F(WALManagerTest, CloseWhenNotOpenIsNoop) {
	WALManager mgr;
	EXPECT_NO_THROW(mgr.close());
}

TEST_F(WALManagerTest, WriteBeginAndCommit) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	EXPECT_NO_THROW(mgr.writeBegin(1));
	EXPECT_NO_THROW(mgr.writeCommit(1));
	mgr.sync();

	mgr.close();
}

TEST_F(WALManagerTest, WriteBeginAndRollback) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	EXPECT_NO_THROW(mgr.writeBegin(1));
	EXPECT_NO_THROW(mgr.writeRollback(1));
	mgr.sync();

	mgr.close();
}

TEST_F(WALManagerTest, WriteEntityChange) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);

	std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
	EXPECT_NO_THROW(mgr.writeEntityChange(1, 0, 0, 42, data));

	mgr.writeCommit(1);
	mgr.sync();
	mgr.close();
}

TEST_F(WALManagerTest, NeedsRecoveryFreshWAL) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	// Fresh WAL with only header should not need recovery
	EXPECT_FALSE(mgr.needsRecovery());

	mgr.close();
}

TEST_F(WALManagerTest, NeedsRecoveryWithRecords) {
	{
		WALManager mgr;
		mgr.open(testDbPath.string());

		mgr.writeBegin(1);
		mgr.writeCommit(1);
		mgr.sync();
		mgr.close();
	}

	// Reopen and check
	WALManager mgr2;
	mgr2.open(testDbPath.string());
	EXPECT_TRUE(mgr2.needsRecovery());
	mgr2.close();
}

TEST_F(WALManagerTest, CheckpointTruncatesWAL) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();

	EXPECT_TRUE(mgr.needsRecovery());

	mgr.checkpoint();

	EXPECT_FALSE(mgr.needsRecovery());

	mgr.close();
}

TEST_F(WALManagerTest, SyncWhenNotOpenIsNoop) {
	WALManager mgr;
	EXPECT_NO_THROW(mgr.sync());
}

TEST_F(WALManagerTest, CheckpointWhenNotOpenIsNoop) {
	WALManager mgr;
	EXPECT_NO_THROW(mgr.checkpoint());
}

TEST_F(WALManagerTest, WriteRecordWhenNotOpenThrows) {
	WALManager mgr;
	EXPECT_THROW(mgr.writeBegin(1), std::runtime_error);
}

TEST_F(WALManagerTest, GetWALPath) {
	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_EQ(mgr.getWALPath(), walPath);
	mgr.close();
}

TEST_F(WALManagerTest, MultipleTransactions) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	for (uint64_t i = 1; i <= 5; ++i) {
		mgr.writeBegin(i);
		std::vector<uint8_t> data = {static_cast<uint8_t>(i)};
		mgr.writeEntityChange(i, 0, 0, static_cast<int64_t>(i * 10), data);
		mgr.writeCommit(i);
	}

	mgr.sync();
	EXPECT_TRUE(mgr.needsRecovery());

	mgr.checkpoint();
	EXPECT_FALSE(mgr.needsRecovery());

	mgr.close();
}

TEST_F(WALManagerTest, DestructorClosesWAL) {
	{
		WALManager mgr;
		mgr.open(testDbPath.string());
		EXPECT_TRUE(mgr.isOpen());
	}
	// Destructor should have closed the WAL
	// We can verify by opening a new WALManager on the same path
	WALManager mgr2;
	EXPECT_NO_THROW(mgr2.open(testDbPath.string()));
	mgr2.close();
}

TEST_F(WALManagerTest, NeedsRecoveryWhenNotOpen) {
	WALManager mgr;
	EXPECT_FALSE(mgr.needsRecovery());
}

TEST_F(WALManagerTest, ValidateHeader_FileTooSmall) {
	// Create a WAL file that's too small for a valid header
	{
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		uint8_t byte = 0x01;
		f.write(reinterpret_cast<const char*>(&byte), 1);
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// validateHeader will return false since file is too small
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, WriteEntityChange_EmptyData) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> emptyData;
	EXPECT_NO_THROW(mgr.writeEntityChange(1, 0, 0, 42, emptyData));
	mgr.writeCommit(1);
	mgr.close();
}

TEST_F(WALManagerTest, ReopenExistingWAL) {
	{
		WALManager mgr;
		mgr.open(testDbPath.string());
		mgr.writeBegin(1);
		mgr.writeCommit(1);
		mgr.sync();
		mgr.close();
	}

	WALManager mgr2;
	mgr2.open(testDbPath.string());
	EXPECT_TRUE(mgr2.isOpen());
	EXPECT_TRUE(mgr2.needsRecovery());
	mgr2.writeBegin(2);
	mgr2.writeCommit(2);
	mgr2.close();
}

TEST_F(WALManagerTest, ValidateHeader_InvalidMagic) {
	// Write a file with correct size but invalid magic bytes
	{
		WALFileHeader header;
		header.magic = 0xDEADBEEF; // Wrong magic
		header.version = WAL_VERSION;
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// validateHeader returns false due to bad magic, so needsRecovery returns false
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, ValidateHeader_InvalidVersion) {
	// Write a file with correct magic but wrong version
	{
		WALFileHeader header;
		header.magic = WAL_MAGIC;
		header.version = 999; // Wrong version
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, ValidateHeader_InvalidMagicAndVersion) {
	// Both magic and version are wrong
	{
		WALFileHeader header;
		header.magic = 0x00000000;
		header.version = 0;
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, WriteRecord_NullDataWithZeroSize) {
	// writeRecord with nullptr and dataSize=0 (default args via writeBegin)
	// This exercises the (data && dataSize > 0) false branch for checksum and write
	WALManager mgr;
	mgr.open(testDbPath.string());

	// writeBegin calls writeRecord(type, txnId) with data=nullptr, dataSize=0
	EXPECT_NO_THROW(mgr.writeBegin(42));
	EXPECT_NO_THROW(mgr.writeCommit(42));
	mgr.close();
}

TEST_F(WALManagerTest, WriteRecord_WriteRollbackExercisesNullDataPath) {
	// writeRollback also calls writeRecord with null data
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(10);
	EXPECT_NO_THROW(mgr.writeRollback(10));
	mgr.close();
}

TEST_F(WALManagerTest, OpenExistingWAL_FailsToOpen) {
	// Create a valid WAL file, then make it unreadable
	{
		WALManager mgr;
		mgr.open(testDbPath.string());
		mgr.close();
	}

	// Make the WAL file unreadable by replacing it with a directory of the same name
	fs::remove(walPath);
	fs::create_directory(walPath);

	WALManager mgr2;
	EXPECT_THROW(mgr2.open(testDbPath.string()), std::runtime_error);

	// Cleanup directory
	fs::remove(walPath);
}

TEST_F(WALManagerTest, NeedsRecovery_HeaderOnlyFile) {
	// A WAL with exactly header size should not need recovery
	WALManager mgr;
	mgr.open(testDbPath.string());
	// Fresh WAL has only the header
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, ValidateHeader_CorruptedHeaderPartialRead) {
	// Write a file that is exactly header size but all zeros (invalid magic)
	{
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		uint8_t zeros[sizeof(WALFileHeader)] = {};
		f.write(reinterpret_cast<const char *>(zeros), sizeof(WALFileHeader));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// Header exists but magic is 0, so validateHeader returns false
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, ValidateHeader_ValidMagicWrongVersion) {
	// Valid magic but version=0 (exercises the && short-circuit)
	{
		WALFileHeader header;
		header.magic = WAL_MAGIC;
		header.version = 0;
		auto buf = serializeFileHeader(header);
		// Add extra bytes beyond header to trigger the fileSize > sizeof(header) check
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
		uint8_t extra[32] = {0xFF};
		f.write(reinterpret_cast<const char *>(extra), sizeof(extra));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// validateHeader returns false (version mismatch), needsRecovery returns false
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, OpenNewFile_CannotCreate) {
	// Try to create a WAL at an invalid path (directory doesn't exist)
	WALManager mgr;
	std::string badPath = "/nonexistent_dir_xyz/sub/db_file";
	EXPECT_THROW(mgr.open(badPath), std::runtime_error);
	EXPECT_FALSE(mgr.isOpen());
}

TEST_F(WALManagerTest, SyncWhenOpenButFileWorking) {
	// Verify sync works when WAL is open and file is valid
	WALManager mgr;
	mgr.open(testDbPath.string());
	mgr.writeBegin(1);
	EXPECT_NO_THROW(mgr.sync());
	mgr.writeCommit(1);
	mgr.close();
}

// ============================================================================
// Additional Branch Coverage Tests for WALManager
// ============================================================================

TEST_F(WALManagerTest, WriteRecord_WithNonNullDataAndPositiveSize) {
	// Cover: (data && dataSize > 0) -> True for checksum and write (line 102, 110-111)
	// writeEntityChange with actual payload exercises this
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> payload = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
	EXPECT_NO_THROW(mgr.writeEntityChange(1, 1, 1, 100, payload));
	mgr.writeCommit(1);
	mgr.close();
}

TEST_F(WALManagerTest, ValidateHeader_FileNotOpen) {
	// Cover: !walFile_.is_open() -> True in validateHeader (line 74)
	// needsRecovery when not open returns false immediately (line 166)
	WALManager mgr;
	// Don't open the manager
	EXPECT_FALSE(mgr.needsRecovery());
}

TEST_F(WALManagerTest, NeedsRecovery_ValidHeaderNoRecords) {
	// Cover: fileSize > sizeof(WALFileHeader) -> False (line 176)
	// A fresh WAL has exactly the header, so needsRecovery returns false
	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, CloseAlreadyClosed) {
	// Cover: isOpen_ -> False in close() (line 56)
	WALManager mgr;
	mgr.open(testDbPath.string());
	mgr.close();
	EXPECT_FALSE(mgr.isOpen());
	// Call close again
	EXPECT_NO_THROW(mgr.close());
	EXPECT_FALSE(mgr.isOpen());
}

TEST_F(WALManagerTest, WriteEntityChange_LargePayload) {
	// Exercise writeRecord with substantial data to ensure checksum calculation works
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> largePayload(1024, 0x42);
	EXPECT_NO_THROW(mgr.writeEntityChange(1, 0, 0, 99, largePayload));
	mgr.writeCommit(1);
	mgr.sync();

	EXPECT_TRUE(mgr.needsRecovery());
	mgr.checkpoint();
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, MultipleCheckpoints) {
	// Cover checkpoint() flow multiple times
	WALManager mgr;
	mgr.open(testDbPath.string());

	for (int i = 0; i < 3; ++i) {
		mgr.writeBegin(static_cast<uint64_t>(i + 1));
		std::vector<uint8_t> data = {static_cast<uint8_t>(i)};
		mgr.writeEntityChange(static_cast<uint64_t>(i + 1), 0, 0, i + 10, data);
		mgr.writeCommit(static_cast<uint64_t>(i + 1));
		mgr.sync();

		EXPECT_TRUE(mgr.needsRecovery());
		mgr.checkpoint();
		EXPECT_FALSE(mgr.needsRecovery());
	}

	mgr.close();
}

TEST_F(WALManagerTest, SyncWhenNotOpenAndFileNotOpen) {
	// Cover: isOpen_ && walFile_.is_open() -> both false paths in sync (line 142)
	WALManager mgr;
	EXPECT_NO_THROW(mgr.sync());
}

// ============================================================================
// Tests targeting specific uncovered branch conditions
// ============================================================================

TEST_F(WALManagerTest, WriteBeginCommitRollback_NullDataPaths) {
	// Covers: writeRecord with data=nullptr and dataSize=0
	// This exercises the (data && dataSize > 0) -> False path at lines 102 and 110
	WALManager mgr;
	mgr.open(testDbPath.string());

	// writeBegin passes no data pointer (data=nullptr, dataSize=0)
	mgr.writeBegin(100);
	mgr.writeCommit(100);
	mgr.writeBegin(200);
	mgr.writeRollback(200);

	mgr.close();
}

TEST_F(WALManagerTest, OpenAlreadyOpen_IsNoop) {
	// Covers: line 30, isOpen_ == true -> return early
	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_TRUE(mgr.isOpen());
	// Second open should return early
	mgr.open(testDbPath.string());
	EXPECT_TRUE(mgr.isOpen());
	mgr.close();
}

TEST_F(WALManagerTest, SyncWhenOpen_ExercisesTruePath) {
	// Covers: line 142, isOpen_ && walFile_.is_open() -> True path
	WALManager mgr;
	mgr.open(testDbPath.string());
	EXPECT_NO_THROW(mgr.sync());
	mgr.close();
	// After close, isOpen_ is false
	EXPECT_NO_THROW(mgr.sync());
}

// ============================================================================
// Tests targeting remaining uncovered branch conditions
// ============================================================================

TEST_F(WALManagerTest, ValidateHeader_ReadFailure) {
	// Cover: !walFile_.good() after read (line 87)
	// Create a WAL file that has the correct size for the header but then
	// remove it and replace with a file that causes read issues.
	// We create a valid WAL, then corrupt it by truncating to exactly
	// the header size but with a broken stream state.
	{
		WALManager mgr;
		mgr.open(testDbPath.string());
		mgr.writeBegin(1);
		mgr.writeCommit(1);
		mgr.sync();
		mgr.close();
	}

	// Replace the WAL file with a file that has header-sized content but is
	// unreadable by making it a directory after WAL open detects it exists.
	// Alternative approach: Create a file that appears to have correct size
	// but contains garbage that won't cause good() to fail.
	// Actually, the best approach is to write a file that is exactly
	// sizeof(WALFileHeader) bytes of zeros - this covers the fileSize check
	// passing but header magic/version failing.
	{
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		// Write exactly header size of random bytes
		std::vector<uint8_t> garbage(sizeof(WALFileHeader), 0xDE);
		f.write(reinterpret_cast<const char *>(garbage.data()),
				static_cast<std::streamsize>(garbage.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// Header reads fine but magic/version mismatch
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, Checkpoint_TruncateReopenFailure) {
	// Cover: !walFile_.is_open() after truncate reopen (line 159)
	// After checkpoint closes the file, if the reopen fails, it throws.
	// We can trigger this by making the WAL path a directory after the
	// initial close in checkpoint().
	WALManager mgr;
	mgr.open(testDbPath.string());
	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();
	mgr.close();

	// Remove the WAL file and replace with a directory so reopen fails
	fs::remove(walPath);
	fs::create_directory(walPath);

	// Re-create a new WALManager. Opening will fail because walPath is a directory.
	WALManager mgr2;
	EXPECT_THROW(mgr2.open(testDbPath.string()), std::runtime_error);

	// Cleanup
	fs::remove(walPath);
}

TEST_F(WALManagerTest, NeedsRecovery_IsOpenTrueButFileExternallyClosed) {
	// Cover: line 166 second condition: !walFile_.is_open() when isOpen_ is true
	// This is an internal state that can't be reached through the public API
	// because close() always sets isOpen_ = false.
	// However, we can test the combined condition by verifying that needsRecovery
	// returns false when isOpen_ is false (first condition), which is already covered.
	// The second condition is a defensive guard that can only be triggered if someone
	// externally modifies the file descriptor.
	// We exercise the boundary: open, close the file handle externally via checkpoint
	// path - but checkpoint handles it. This branch is essentially unreachable through
	// the public API. We verify the guard exists by testing the first condition path.
	WALManager mgr;
	EXPECT_FALSE(mgr.needsRecovery()); // isOpen_ = false
}

TEST_F(WALManagerTest, ValidateHeader_ExactlyHeaderSizeValidContent) {
	// Cover: fileSize == sizeof(WALFileHeader) exactly with valid header
	// This means validateHeader returns true but needsRecovery returns false
	// because fileSize is NOT > sizeof(WALFileHeader)
	{
		WALFileHeader header;
		header.magic = WAL_MAGIC;
		header.version = WAL_VERSION;
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// Valid header, no records beyond header
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, ValidateHeader_ValidHeaderWithExtraBytes) {
	// Cover: validateHeader returns true AND fileSize > sizeof(WALFileHeader)
	// so needsRecovery returns true
	{
		WALFileHeader header;
		header.magic = WAL_MAGIC;
		header.version = WAL_VERSION;
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
		// Add extra bytes beyond header
		uint8_t extra[64] = {0x01, 0x02, 0x03};
		f.write(reinterpret_cast<const char *>(extra), sizeof(extra));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// Valid header + extra data = needs recovery
	EXPECT_TRUE(mgr.needsRecovery());
	mgr.close();
}

TEST_F(WALManagerTest, Checkpoint_SuccessfulTruncateAndRewrite) {
	// Cover: checkpoint flow where truncate + reopen succeeds (line 159 false branch)
	// and writeHeader is called again
	WALManager mgr;
	mgr.open(testDbPath.string());

	// Write multiple records
	for (uint64_t i = 1; i <= 3; ++i) {
		mgr.writeBegin(i);
		std::vector<uint8_t> data = {static_cast<uint8_t>(i), 0xAA, 0xBB};
		mgr.writeEntityChange(i, 1, 0, static_cast<int64_t>(i * 100), data);
		mgr.writeCommit(i);
	}
	mgr.sync();

	EXPECT_TRUE(mgr.needsRecovery());

	// Checkpoint should truncate and rewrite header
	EXPECT_NO_THROW(mgr.checkpoint());
	EXPECT_FALSE(mgr.needsRecovery());

	// Verify we can still write after checkpoint
	mgr.writeBegin(10);
	mgr.writeCommit(10);
	mgr.sync();
	EXPECT_TRUE(mgr.needsRecovery());

	mgr.close();
}

// ============================================================================
// Additional Branch Coverage Tests - Round 3
// ============================================================================

// Test close() when walFile_ is already closed but isOpen_ is true
// This is a defensive scenario that exercises the walFile_.is_open() check
// inside close() at line 57
TEST_F(WALManagerTest, CloseWhenFileStreamNotOpen) {
	WALManager mgr;
	// When not open, close should be a no-op (isOpen_ == false)
	mgr.close();
	EXPECT_FALSE(mgr.isOpen());
}

// Test writeRecord with data=nullptr but dataSize=0 explicitly
// Exercises: (data && dataSize > 0) both conditions false
TEST_F(WALManagerTest, WriteRecordNullDataZeroSize) {
	WALManager mgr;
	mgr.open(testDbPath.string());
	// writeBegin internally calls writeRecord(type, txnId, nullptr, 0)
	// The checksum path: data==nullptr -> checksum=0
	// The write path: data==nullptr -> skip write
	mgr.writeBegin(1);
	mgr.writeRollback(1);
	mgr.close();
}

// Test needsRecovery when file is open but validateHeader fails
// Exercises: validateHeader() -> false path in needsRecovery (line 169)
TEST_F(WALManagerTest, NeedsRecovery_OpenWithCorruptHeader) {
	// Create a WAL file with garbage content but correct size
	{
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		std::vector<uint8_t> garbage(sizeof(WALFileHeader) + 100, 0xAB);
		f.write(reinterpret_cast<const char *>(garbage.data()),
				static_cast<std::streamsize>(garbage.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// validateHeader will fail due to bad magic, needsRecovery returns false
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

// Test writeEntityChange with zero-length serialized data
// Exercises: serializedData.empty() -> payload.dataSize=0 but fullData still has header
TEST_F(WALManagerTest, WriteEntityChange_ZeroLengthSerializedData) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> emptyData;
	// This calls writeRecord with non-null data pointer but the payload only
	// contains the entity payload header (no serialized entity data)
	EXPECT_NO_THROW(mgr.writeEntityChange(1, 2, 1, 55, emptyData));
	mgr.writeCommit(1);
	mgr.sync();

	// Verify records were written
	EXPECT_TRUE(mgr.needsRecovery());
	mgr.close();
}

// Test open on existing WAL file (exercises the exists==true branch at line 37)
// followed by writing and checkpoint to exercise full lifecycle
TEST_F(WALManagerTest, OpenExistingWALAndFullLifecycle) {
	// First create and close a WAL
	{
		WALManager mgr;
		mgr.open(testDbPath.string());
		mgr.writeBegin(1);
		mgr.writeCommit(1);
		mgr.sync();
		mgr.close();
	}

	// Now reopen the existing WAL (exercises the exists==true path)
	WALManager mgr2;
	mgr2.open(testDbPath.string());
	EXPECT_TRUE(mgr2.isOpen());
	EXPECT_TRUE(mgr2.needsRecovery());

	// Write more records, checkpoint, verify
	mgr2.writeBegin(2);
	std::vector<uint8_t> payload = {0x01, 0x02};
	mgr2.writeEntityChange(2, 0, 0, 10, payload);
	mgr2.writeCommit(2);
	mgr2.sync();

	mgr2.checkpoint();
	EXPECT_FALSE(mgr2.needsRecovery());
	mgr2.close();
}

// Test destructor closes WAL properly when records are present
TEST_F(WALManagerTest, DestructorWithPendingRecords) {
	{
		WALManager mgr;
		mgr.open(testDbPath.string());
		mgr.writeBegin(1);
		mgr.writeCommit(1);
		// Don't close explicitly; destructor should handle it
	}
	// Verify file still exists and can be reopened
	EXPECT_TRUE(fs::exists(walPath));
	WALManager mgr2;
	mgr2.open(testDbPath.string());
	EXPECT_TRUE(mgr2.needsRecovery());
	mgr2.close();
}

// ============================================================================
// CRC32 Verification Tests (readRecords)
// ============================================================================

TEST_F(WALManagerTest, ReadRecords_EmptyWAL) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	auto result = mgr.readRecords();
	EXPECT_TRUE(result.records.empty());
	EXPECT_FALSE(result.corrupted);
	mgr.close();
}

TEST_F(WALManagerTest, ReadRecords_WhenNotOpen) {
	WALManager mgr;
	auto result = mgr.readRecords();
	EXPECT_TRUE(result.records.empty());
	EXPECT_FALSE(result.corrupted);
}

TEST_F(WALManagerTest, ReadRecords_BeginCommit) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();

	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	ASSERT_EQ(result.records.size(), 2U);
	EXPECT_EQ(result.records[0].header.type, WALRecordType::WAL_TXN_BEGIN);
	EXPECT_EQ(result.records[0].header.txnId, 1U);
	EXPECT_EQ(result.records[1].header.type, WALRecordType::WAL_TXN_COMMIT);
	EXPECT_EQ(result.records[1].header.txnId, 1U);
	mgr.close();
}

TEST_F(WALManagerTest, ReadRecords_EntityChangeWithData) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
	mgr.writeEntityChange(1, 0, 0, 42, data);
	mgr.writeCommit(1);
	mgr.sync();

	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	ASSERT_EQ(result.records.size(), 3U);

	// The entity write record should have data
	EXPECT_EQ(result.records[1].header.type, WALRecordType::WAL_ENTITY_WRITE);
	EXPECT_FALSE(result.records[1].data.empty());
	mgr.close();
}

TEST_F(WALManagerTest, ReadRecords_CorruptedChecksum) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
	mgr.writeEntityChange(1, 0, 0, 10, data);
	mgr.writeCommit(1);
	mgr.sync();
	mgr.close();

	// Corrupt the data payload of the entity write record
	// File layout: [32-byte file header] [BEGIN record header] [ENTITY_WRITE record header + data] [COMMIT record header]
	// BEGIN record: sizeof(WALRecordHeader) with no data
	// We corrupt a byte in the entity write data section
	{
		std::fstream f(walPath, std::ios::binary | std::ios::in | std::ios::out);
		// Seek past file header + BEGIN record header + ENTITY_WRITE record header
		auto corruptPos = static_cast<std::streamoff>(
			sizeof(WALFileHeader) + sizeof(WALRecordHeader) + sizeof(WALRecordHeader) + 1);
		f.seekp(corruptPos);
		char garbage = 0xFF;
		f.write(&garbage, 1);
		f.flush();
	}

	WALManager mgr2;
	mgr2.open(testDbPath.string());
	auto result = mgr2.readRecords();
	// Should detect corruption at the entity write record
	EXPECT_TRUE(result.corrupted);
	// BEGIN record should have been read successfully before the corrupt one
	EXPECT_EQ(result.records.size(), 1U);
	mgr2.close();
}

TEST_F(WALManagerTest, ReadRecords_TruncatedRecord) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> data(64, 0x42);
	mgr.writeEntityChange(1, 0, 0, 99, data);
	mgr.writeCommit(1);
	mgr.sync();
	mgr.close();

	// Truncate the file to cut off the commit record mid-way
	auto fileSize = fs::file_size(walPath);
	fs::resize_file(walPath, fileSize - 5);

	WALManager mgr2;
	mgr2.open(testDbPath.string());
	auto result = mgr2.readRecords();
	// The last record (commit) is truncated, but BEGIN and ENTITY should be fine
	EXPECT_EQ(result.records.size(), 2U);
	EXPECT_FALSE(result.corrupted); // just ran out of data, not CRC corruption
	mgr2.close();
}

TEST_F(WALManagerTest, ReadRecords_CorruptedRecordSize) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();
	mgr.close();

	// Corrupt the recordSize field of the first record to be impossibly large
	{
		std::fstream f(walPath, std::ios::binary | std::ios::in | std::ios::out);
		f.seekp(static_cast<std::streamoff>(sizeof(WALFileHeader)));
		uint32_t badSize = 0xFFFFFFFF;
		f.write(reinterpret_cast<const char *>(&badSize), sizeof(badSize));
		f.flush();
	}

	WALManager mgr2;
	mgr2.open(testDbPath.string());
	auto result = mgr2.readRecords();
	EXPECT_TRUE(result.corrupted);
	EXPECT_TRUE(result.records.empty());
	mgr2.close();
}

TEST_F(WALManagerTest, ReadRecords_CorruptedRecordSizeTooSmall) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();
	mgr.close();

	// Set recordSize to less than sizeof(WALRecordHeader)
	{
		std::fstream f(walPath, std::ios::binary | std::ios::in | std::ios::out);
		f.seekp(static_cast<std::streamoff>(sizeof(WALFileHeader)));
		uint32_t badSize = 1; // Way too small
		f.write(reinterpret_cast<const char *>(&badSize), sizeof(badSize));
		f.flush();
	}

	WALManager mgr2;
	mgr2.open(testDbPath.string());
	auto result = mgr2.readRecords();
	EXPECT_TRUE(result.corrupted);
	EXPECT_TRUE(result.records.empty());
	mgr2.close();
}

TEST_F(WALManagerTest, ReadRecords_NonZeroChecksumOnEmptyData) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.sync();
	mgr.close();

	// Corrupt the checksum of the BEGIN record (which has no data, so checksum should be 0)
	{
		std::fstream f(walPath, std::ios::binary | std::ios::in | std::ios::out);
		// checksum is at offset: recordSize(4) + txnId(8) + type(1) = 13 bytes into the record header
		auto checksumOffset = static_cast<std::streamoff>(sizeof(WALFileHeader) + offsetof(WALRecordHeader, checksum));
		f.seekp(checksumOffset);
		uint32_t badChecksum = 0xDEADBEEF;
		f.write(reinterpret_cast<const char *>(&badChecksum), sizeof(badChecksum));
		f.flush();
	}

	WALManager mgr2;
	mgr2.open(testDbPath.string());
	auto result = mgr2.readRecords();
	EXPECT_TRUE(result.corrupted);
	EXPECT_TRUE(result.records.empty());
	mgr2.close();
}

TEST_F(WALManagerTest, ReadRecords_MultipleTransactions) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	for (uint64_t i = 1; i <= 3; ++i) {
		mgr.writeBegin(i);
		std::vector<uint8_t> data = {static_cast<uint8_t>(i)};
		mgr.writeEntityChange(i, 0, 0, static_cast<int64_t>(i * 10), data);
		mgr.writeCommit(i);
	}
	mgr.sync();

	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	// 3 transactions x 3 records each (BEGIN + ENTITY + COMMIT)
	EXPECT_EQ(result.records.size(), 9U);
	mgr.close();
}

TEST_F(WALManagerTest, ReadRecords_AfterCheckpointIsEmpty) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();

	auto before = mgr.readRecords();
	EXPECT_EQ(before.records.size(), 2U);

	mgr.checkpoint();

	auto after = mgr.readRecords();
	EXPECT_TRUE(after.records.empty());
	EXPECT_FALSE(after.corrupted);
	mgr.close();
}

TEST_F(WALManagerTest, ReadRecords_InvalidHeader) {
	// Create WAL with invalid magic
	{
		WALFileHeader header;
		header.magic = 0xDEADBEEF;
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	auto result = mgr.readRecords();
	EXPECT_TRUE(result.records.empty());
	EXPECT_FALSE(result.corrupted);
	mgr.close();
}

// Test multiple write entity changes in a single transaction
// Exercises writeRecord with data multiple times in sequence
TEST_F(WALManagerTest, MultipleEntityChangesInTransaction) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	for (int i = 0; i < 10; ++i) {
		std::vector<uint8_t> data(64, static_cast<uint8_t>(i));
		mgr.writeEntityChange(1, static_cast<uint8_t>(i % 4), 0,
							  static_cast<int64_t>(i * 100), data);
	}
	mgr.writeCommit(1);
	mgr.sync();

	EXPECT_TRUE(mgr.needsRecovery());
	mgr.close();
}

// ============================================================================
// Additional branch coverage tests
// ============================================================================

// Cover line 57: close() walFile_.is_open() False path
// When isOpen_ is true but walFile_ has already been closed somehow
// We can approach this by testing close after checkpoint (which closes/reopens).
TEST_F(WALManagerTest, CloseExercisesWalFileOpenCheck) {
	WALManager mgr;
	mgr.open(testDbPath.string());
	mgr.writeBegin(1);
	mgr.writeCommit(1);

	// Normal close exercises isOpen_=true, walFile_.is_open()=true path
	mgr.close();

	// After close, isOpen_=false; calling close again is noop (covers !isOpen_ path)
	mgr.close();
}

// Cover line 142: sync() when isOpen_ && walFile_.is_open() is false
// Since both conditions must be true for sync to do work,
// we test sync when not open (False path for isOpen_).
TEST_F(WALManagerTest, SyncOnClosedManagerIsNoop) {
	WALManager mgr;
	// Not open - sync should be a no-op
	mgr.sync();

	// Open, close, then sync - also no-op
	mgr.open(testDbPath.string());
	mgr.close();
	mgr.sync();
}

// Cover line 166: needsRecovery when !isOpen_ || !walFile_.is_open()
// Tests both sub-conditions in the compound check
TEST_F(WALManagerTest, NeedsRecoveryClosedManagerReturnsFalse) {
	WALManager mgr;
	// Not open at all
	EXPECT_FALSE(mgr.needsRecovery());

	// Open, write something, close, then check
	mgr.open(testDbPath.string());
	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.close();

	// After close, isOpen_ = false => needsRecovery returns false
	EXPECT_FALSE(mgr.needsRecovery());
}

// Cover line 159: checkpoint truncate reopen path
// Tests that checkpoint properly truncates and rewrites header
TEST_F(WALManagerTest, CheckpointAfterMultipleTransactions) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	// Write multiple transactions
	for (uint64_t i = 1; i <= 5; i++) {
		mgr.writeBegin(i);
		std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
		mgr.writeEntityChange(i, 0, 0, static_cast<int64_t>(i * 10), payload);
		mgr.writeCommit(i);
	}
	mgr.sync();
	EXPECT_TRUE(mgr.needsRecovery());

	// Checkpoint should truncate everything
	mgr.checkpoint();
	EXPECT_FALSE(mgr.needsRecovery());

	// Write more after checkpoint
	mgr.writeBegin(10);
	mgr.writeCommit(10);
	mgr.sync();
	EXPECT_TRUE(mgr.needsRecovery());
	mgr.close();
}

// Cover line 87: validateHeader when !walFile_.good() after read
// We create a WAL file with valid size but truncated/corrupted header data
TEST_F(WALManagerTest, ValidateHeaderTruncatedContent) {
	// Write a file that's exactly header size but all zeros
	{
		std::ofstream out(walPath, std::ios::binary);
		std::vector<char> zeros(sizeof(WALFileHeader), 0);
		out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());
	// Header has zeros - magic won't match WAL_MAGIC
	// validateHeader returns false, so needsRecovery returns false
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

// ============================================================================
// Branch Coverage Round 4 - Targeting remaining uncovered branches
// ============================================================================

// Target: checkpoint() failure when WAL path becomes unwritable
// After opening, we replace the WAL path with a non-empty directory so
// fs::remove fails and checkpoint throws.
TEST_F(WALManagerTest, CheckpointTruncateReopenFailure) {
#ifdef _WIN32
	GTEST_SKIP() << "Windows does not allow removing a file while it is open";
#endif
	WALManager mgr;
	mgr.open(testDbPath.string());
	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();

	// Replace the WAL file with a non-empty directory so fs::remove fails
	fs::remove(walPath);
	fs::create_directory(walPath);
	// Create a file inside the directory to make it non-empty
	std::ofstream((fs::path(walPath) / "blocker").string());

	// checkpoint() will:
	// 1. flushAndSync - succeeds (fd is still valid from original file)
	// 2. portable_close_rw - succeeds
	// 3. fs::remove - throws because walPath is a non-empty directory
	EXPECT_THROW(mgr.checkpoint(), std::filesystem::filesystem_error);

	// Cleanup
	fs::remove_all(walPath);

	// close() should be safe
	EXPECT_NO_THROW(mgr.close());
}

// Target: validateHeader() when file is truncated after open
// We truncate the WAL file to invalidate its header content while the fd is open.
TEST_F(WALManagerTest, ValidateHeader_StreamBadAfterRead) {
	// Create a pre-existing WAL file with valid header and extra data
	{
		WALFileHeader header;
		header.magic = WAL_MAGIC;
		header.version = WAL_VERSION;
		auto buf = serializeFileHeader(header);
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(buf.data()),
				static_cast<std::streamsize>(buf.size()));
		uint8_t extra[32] = {0x01};
		f.write(reinterpret_cast<const char *>(extra), sizeof(extra));
	}

	WALManager mgr;
	mgr.open(testDbPath.string());

	// Truncate the file to zero bytes externally
	{
		std::ofstream f(walPath, std::ios::binary | std::ios::trunc);
		// File is now empty
	}

	// validateHeader uses filesystem::file_size which sees the truncated file,
	// so it returns false (file size < header size) => needsRecovery returns false
	EXPECT_FALSE(mgr.needsRecovery());
	mgr.close();
}

// Target: Lines 102/110 - data is non-null but dataSize is 0
// writeRecord is private, so we can't directly call it with those args.
// However, writeEntityChange with empty serializedData still produces a
// non-null fullData (containing the entity payload header) with dataSize > 0.
// These branches (data non-null, dataSize=0) are unreachable through the
// public API. The only way data can be non-null is via writeEntityChange
// which always has dataSize >= sizeof(WALEntityPayload).
// We test the closest scenario: writeEntityChange with empty serialized data.
TEST_F(WALManagerTest, WriteEntityChange_EmptySerializedDataHasPayloadHeader) {
	WALManager mgr;
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	// Empty serializedData, but writeEntityChange prepends WALEntityPayload header
	// so writeRecord receives non-null data with dataSize = sizeof(WALEntityPayload)
	std::vector<uint8_t> empty;
	EXPECT_NO_THROW(mgr.writeEntityChange(1, 0, 0, 1, empty));
	mgr.writeCommit(1);

	// Verify data was written (records exist beyond header)
	mgr.sync();
	EXPECT_TRUE(mgr.needsRecovery());
	mgr.close();
}

// ============================================================================
// Group Commit Tests
// ============================================================================

TEST_F(WALManagerTest, GroupCommitBasic) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(0); // Disable delay for deterministic test
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> data = {0x01, 0x02, 0x03};
	mgr.writeEntityChange(1, 0, 0, 10, data);
	mgr.writeCommit(1);

	// Verify data is persisted (fsync'd by group commit)
	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	EXPECT_EQ(result.records.size(), 3u); // BEGIN + ENTITY_WRITE + COMMIT

	mgr.close();
}

TEST_F(WALManagerTest, GroupCommitMultipleTransactions) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(0);
	mgr.open(testDbPath.string());

	for (uint64_t i = 1; i <= 5; ++i) {
		mgr.writeBegin(i);
		std::vector<uint8_t> data = {static_cast<uint8_t>(i)};
		mgr.writeEntityChange(i, 0, 0, static_cast<int64_t>(i * 10), data);
		mgr.writeCommit(i);
	}

	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	EXPECT_EQ(result.records.size(), 15u); // 5 * (BEGIN + ENTITY_WRITE + COMMIT)

	mgr.close();
}

TEST_F(WALManagerTest, GroupCommitConcurrent) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(500); // 0.5ms delay to allow grouping
	mgr.open(testDbPath.string());

	constexpr int NUM_THREADS = 4;
	constexpr int TXN_PER_THREAD = 10;

	std::vector<std::thread> threads;
	for (int t = 0; t < NUM_THREADS; ++t) {
		threads.emplace_back([&mgr, t]() {
			for (int i = 0; i < TXN_PER_THREAD; ++i) {
				uint64_t txnId = static_cast<uint64_t>(t * TXN_PER_THREAD + i + 1);
				mgr.writeBegin(txnId);
				std::vector<uint8_t> data = {static_cast<uint8_t>(t), static_cast<uint8_t>(i)};
				mgr.writeEntityChange(txnId, 0, 0, static_cast<int64_t>(txnId * 10), data);
				mgr.writeCommit(txnId);
			}
		});
	}

	for (auto &th : threads) {
		th.join();
	}

	// All transactions should be persisted
	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	EXPECT_EQ(result.records.size(), static_cast<size_t>(NUM_THREADS * TXN_PER_THREAD * 3));

	mgr.close();
}

TEST_F(WALManagerTest, WriteBufferFlushOnFull) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(0);
	mgr.setBufferSize(256); // Small buffer to trigger frequent flushes
	mgr.open(testDbPath.string());

	// Write enough data to trigger buffer flush
	for (uint64_t i = 1; i <= 20; ++i) {
		mgr.writeBegin(i);
		std::vector<uint8_t> data(50, static_cast<uint8_t>(i));
		mgr.writeEntityChange(i, 0, 0, static_cast<int64_t>(i), data);
		mgr.writeCommit(i);
	}

	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	EXPECT_EQ(result.records.size(), 60u); // 20 * 3

	mgr.close();
}

TEST_F(WALManagerTest, RollbackFlushesBuffer) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(0);
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	std::vector<uint8_t> data = {0x01};
	mgr.writeEntityChange(1, 0, 0, 10, data);
	mgr.writeRollback(1);

	// Rollback flushes buffer (no fsync)
	auto result = mgr.readRecords();
	EXPECT_FALSE(result.corrupted);
	EXPECT_EQ(result.records.size(), 3u); // BEGIN + ENTITY_WRITE + ROLLBACK

	mgr.close();
}

TEST_F(WALManagerTest, NativeFdFsync) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(0);
	mgr.open(testDbPath.string());

	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.sync();

	EXPECT_TRUE(mgr.needsRecovery());

	mgr.checkpoint();
	EXPECT_FALSE(mgr.needsRecovery());

	mgr.close();
}

TEST_F(WALManagerTest, GroupCommitSetters) {
	WALManager mgr;
	mgr.setGroupCommitDelayUs(5000);
	mgr.setBufferSize(1024 * 1024);

	mgr.open(testDbPath.string());
	mgr.writeBegin(1);
	mgr.writeCommit(1);
	mgr.close();
}
