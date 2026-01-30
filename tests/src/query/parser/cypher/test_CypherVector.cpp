/**
 * @file test_CypherVector.cpp
 * @author Nexepic
 * @date 2026/1/29
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

class CypherVectorTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_vec_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);

		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);
	}

	[[nodiscard]] graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}
};

// ============================================================================
// 1. DDL Tests (Create Index)
// ============================================================================

TEST_F(CypherVectorTest, CreateVectorIndex) {
	// Syntax: CREATE VECTOR INDEX name ON :Label(prop) OPTIONS {dim: 4, metric: 'L2'}
	auto res = execute("CREATE VECTOR INDEX vec_idx ON :Item(emb) OPTIONS {dimension: 4, metric: 'L2'}");
	// Usually returns empty result or status

	// Verify via SHOW INDEXES
	auto resShow = execute("SHOW INDEXES");
	bool found = false;
	for (const auto &row: resShow.getRows()) {
		if (row.at("name").toString() == "vec_idx" && row.at("type").toString() == "vector") {
			found = true;
		}
	}
	EXPECT_TRUE(found);
}

TEST_F(CypherVectorTest, CreateDuplicateVectorIndex) {
	(void) execute("CREATE VECTOR INDEX v1 ON :A(v) OPTIONS {dim: 4}");

	// Attempt to create duplicate name - Expect Error
	EXPECT_THROW({ (void) execute("CREATE VECTOR INDEX v1 ON :A(v) OPTIONS {dim: 4}"); }, std::runtime_error);
}

// ============================================================================
// 2. DML Tests (Insert & Auto-Index)
// ============================================================================

TEST_F(CypherVectorTest, InsertAndSearch) {
	(void) execute("CREATE VECTOR INDEX idx1 ON :Node(vec) OPTIONS {dim: 2, metric: 'L2'}");

	// Insert Nodes with Vectors
	(void) execute("CREATE (:Node {id: 1, vec: [1.0, 0.0]})");
	(void) execute("CREATE (:Node {id: 2, vec: [0.0, 1.0]})");
	(void) execute("CREATE (:Node {id: 3, vec: [0.0, 0.0]})");

	// Search: Closest to [0.9, 0.1] should be Node 1
	// Syntax: CALL db.index.vector.queryNodes('idx1', 2, [0.9, 0.1]) YIELD node, score
	auto res = execute("CALL db.index.vector.queryNodes('idx1', 2, [0.9, 0.1]) "
					   "YIELD node, score RETURN node.id, score");

	ASSERT_EQ(res.rowCount(), 2UL);

	// Verify Top 1
	auto row0 = res.getRows()[0];
	EXPECT_EQ(row0.at("node.id").toString(), "1");
	// Score should be L2 distance (squared). Diff is [0.1, -0.1]. SqSum = 0.01+0.01=0.02.
	// Allow epsilon
	EXPECT_NEAR(std::stod(row0.at("score").toString()), 0.02, 0.001);
}

// ============================================================================
// 3. Training Tests
// ============================================================================

TEST_F(CypherVectorTest, ManualTrain) {
	(void) execute("CREATE VECTOR INDEX idx_train ON :Data(v) OPTIONS {dim: 4}");

	// Insert enough data to sample
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (:Data {v: [0.5, 0.5, 0.5, 0.5]})");
	}

	auto res = execute("CALL db.index.vector.train('idx_train')");
	// Should return status
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("status").toString(), "Success");
}

TEST_F(CypherVectorTest, TrainEmptyIndex) {
	(void) execute("CREATE VECTOR INDEX idx_empty ON :Empty(v) OPTIONS {dim: 4}");

	auto res = execute("CALL db.index.vector.train('idx_empty')");
	// Should handle gracefully
	EXPECT_TRUE(res.getRows()[0].at("status").toString().find("Skipped") != std::string::npos);
}

// ============================================================================
// 4. Edge Cases
// ============================================================================

TEST_F(CypherVectorTest, SearchNonExistentIndex) {
	// In the internal API, execute() throws std::runtime_error for logic errors
	EXPECT_THROW({ (void) execute("CALL db.index.vector.queryNodes('fake', 1, [1.0])"); }, std::runtime_error);
}

TEST_F(CypherVectorTest, InsertBadDimension) {
	(void) execute("CREATE VECTOR INDEX idx_dim ON :N(v) OPTIONS {dim: 4}");

	// Insert dim 2
	(void) execute("CREATE (:N {v: [1.0, 1.0]})");

	// Search should not find it (or return it).
	// The insert logs an error but doesn't crash. The node exists in DB but not in Index.
	auto res = execute("CALL db.index.vector.queryNodes('idx_dim', 5, [1.0, 1.0, 1.0, 1.0])");
	EXPECT_TRUE(res.isEmpty() || res.rowCount() == 0);
}
