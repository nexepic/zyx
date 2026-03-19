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
#include "graph/storage/wal/WALManager.hpp"
#include "graph/storage/wal/WALRecord.hpp"

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
		fs::remove(testDbPath);
		fs::remove(walPath);
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
