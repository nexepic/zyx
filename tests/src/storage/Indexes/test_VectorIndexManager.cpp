/**
 * @file test_VectorIndexManager.cpp
 * @author Nexepic
 * @date 2026/1/26
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

#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/VectorIndexManager.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

using namespace graph::query::indexes;

class ManagerPersistenceTest : public ::testing::Test {
protected:
	std::filesystem::path dbPath;

	void SetUp() override {
		// Deterministic path to allow reopening
		dbPath = std::filesystem::temp_directory_path() / "vim_persistence_test.db";
		std::filesystem::remove_all(dbPath); // Clean start
	}

	void TearDown() override {
		std::filesystem::remove_all(dbPath);
	}
};

TEST_F(ManagerPersistenceTest, ReloadFromDisk) {
	// 1. Session A: Create Indexes
	{
		graph::Database db(dbPath.string());
		db.open();
		auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

		vim->createIndex("idx_l2", 4, "L2");
		vim->createIndex("idx_ip", 8, "IP"); // Hits metricType == 1 logic

		// Verify in-memory state
		EXPECT_TRUE(vim->hasIndex("idx_l2"));
	} // DB Closed

	// 2. Session B: Re-open DB
	{
		graph::Database db(dbPath.string());
		db.open();
		auto vim = db.getQueryEngine()->getIndexManager()->getVectorIndexManager();

		// 3. Test hasIndex (Disk Path)
		// Since it's a fresh VIM instance, indexes_ map is empty.
		// hasIndex("idx_l2") triggers the try-catch block reading from Registry
		EXPECT_TRUE(vim->hasIndex("idx_l2")); // Covers lines 87-91
		EXPECT_FALSE(vim->hasIndex("non_existent")); // Covers line 92

		// 4. Test getIndex (Disk Path)
		// Triggers loading from disk logic (lines 42-53)
		auto idxL2 = vim->getIndex("idx_l2");
		EXPECT_NE(idxL2, nullptr);
		// We can't easily check config metric string directly unless exposed,
		// but we can verify behavior if needed.

		auto idxIP = vim->getIndex("idx_ip");
		EXPECT_NE(idxIP, nullptr);
		// This covers the `if (cfg.metricType == 1)` branch
	}
}