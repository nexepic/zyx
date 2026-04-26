/**
 * @file test_Database_WALRecovery.cpp
 * @date 2026/04/24
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include "graph/core/Database.hpp"
#include "graph/storage/wal/WALRecord.hpp"

namespace fs = std::filesystem;
using namespace graph;

class DatabaseWALRecoveryTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_db_wal_recovery_" + boost::uuids::to_string(uuid) + ".zyx");
		fs::remove_all(testDbPath);
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
		fs::remove(testDbPath.string() + "-wal", ec);
	}

	fs::path testDbPath;
};

// ============================================================================
// WAL file exists with only header (needsRecovery() returns false)
// Covers: Database.cpp Branch (170:9) False — WAL exists but no recovery needed
// ============================================================================

TEST_F(DatabaseWALRecoveryTest, WALExistsButNoRecoveryNeeded) {
	// Phase 1: Create a valid database
	{
		auto db = std::make_unique<Database>(testDbPath.string());
		db->open();
		db->close();
	}

	// Phase 2: Create a WAL file with only a valid header (no records)
	std::string walPath = testDbPath.string() + "-wal";
	{
		storage::wal::WALFileHeader header;
		header.magic = storage::wal::WAL_MAGIC;
		header.version = storage::wal::WAL_VERSION;
		header.dbFileSize = 0;
		header.salt1 = 0;
		header.salt2 = 0;
		std::memset(header.reserved, 0, sizeof(header.reserved));

		std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(walOut.is_open());
		walOut.write(reinterpret_cast<const char *>(&header), sizeof(header));
		walOut.close();
	}
	ASSERT_TRUE(fs::exists(walPath));
	ASSERT_EQ(fs::file_size(walPath), sizeof(storage::wal::WALFileHeader));

	// Phase 3: Reopen the database — WAL exists but needsRecovery() should return false
	// (file size == sizeof(WALFileHeader), no records to replay)
	{
		auto db = std::make_unique<Database>(testDbPath.string());
		db->open();

		// Begin a transaction to trigger ensureWALAndTransactionManager()
		// This exercises the needsRecovery() == false branch
		auto txn = db->beginTransaction();
		EXPECT_TRUE(txn.isActive());
		txn.commit();

		db->close();
	}
}

// ============================================================================
// WAL file exists with valid header and records — needsRecovery() returns true
// Then ensureWALForWrites early returns because walManager_ already set
// Covers: Database.cpp Branch (193:8) True — walManager_ already exists
// ============================================================================

TEST_F(DatabaseWALRecoveryTest, WALRecoveryThenWriteReusesWALManager) {
	std::string walPath = testDbPath.string() + "-wal";

	// Phase 1: Create a DB, begin a write txn, then simulate crash by
	// preserving the WAL file
	{
		auto db = std::make_unique<Database>(testDbPath.string());
		db->open();
		{
			auto txn = db->beginTransaction();
			// auto-rollback on destruction — WAL should have begin + rollback records
		}

		// Read WAL contents before close destroys them
		std::vector<uint8_t> walContent;
		{
			std::ifstream walIn(walPath, std::ios::binary);
			if (walIn.is_open()) {
				walContent.assign(std::istreambuf_iterator<char>(walIn),
								  std::istreambuf_iterator<char>());
			}
		}

		db.reset(); // destructor closes DB, may remove WAL

		// Restore WAL file to simulate crash
		if (!walContent.empty()) {
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			walOut.write(reinterpret_cast<const char *>(walContent.data()),
						 static_cast<std::streamsize>(walContent.size()));
		}
	}

	// Phase 2: Reopen — WAL exists with records, so needsRecovery() == true
	// After recovery, walManager_ is already set. Then beginTransaction() calls
	// ensureWALForWrites() which should see walManager_ is set and return early.
	if (fs::exists(walPath) && fs::file_size(walPath) > sizeof(storage::wal::WALFileHeader)) {
		auto db = std::make_unique<Database>(testDbPath.string());
		db->open();

		auto txn = db->beginTransaction();
		EXPECT_TRUE(txn.isActive());
		txn.commit();

		db->close();
	}
}
