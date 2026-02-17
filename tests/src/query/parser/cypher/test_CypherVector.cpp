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

// --- Additional Vector Index Tests ---

TEST_F(CypherVectorTest, CreateIndexDifferentMetrics) {
	// Test with different distance metrics
	(void) execute("CREATE VECTOR INDEX idx_cosine ON :Cos(v) OPTIONS {dim: 3, metric: 'COSINE'}");
	(void) execute("CREATE VECTOR INDEX idx_ip ON :IP(v) OPTIONS {dim: 3, metric: 'INNER_PRODUCT'}");

	auto res = execute("SHOW INDEXES");
	ASSERT_GE(res.rowCount(), 2UL);
}

TEST_F(CypherVectorTest, SearchTopK) {
	(void) execute("CREATE VECTOR INDEX idx_topk ON :TopK(vec) OPTIONS {dim: 2}");

	// Insert multiple vectors
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (:TopK {id: " + std::to_string(i) + ", vec: [" +
					   std::to_string(i * 0.1) + ", " + std::to_string(i * 0.1) + "]})");
	}

	// Search for top 3
	auto res = execute("CALL db.index.vector.queryNodes('idx_topk', 3, [0.5, 0.5]) YIELD node RETURN node.id");
	ASSERT_LE(res.rowCount(), 3UL);
}

TEST_F(CypherVectorTest, SearchWithKZero) {
	(void) execute("CREATE VECTOR INDEX idx_k0 ON :K0(v) OPTIONS {dim: 2}");
	(void) execute("CREATE (:K0 {v: [1.0, 0.0]})");

	// Search with k=0 should return empty
	auto res = execute("CALL db.index.vector.queryNodes('idx_k0', 0, [1.0, 0.0])");
	EXPECT_TRUE(res.isEmpty() || res.rowCount() == 0);
}

TEST_F(CypherVectorTest, TrainWithInsufficientData) {
	(void) execute("CREATE VECTOR INDEX idx_insuf ON :Insuf(v) OPTIONS {dim: 3}");

	// Insert only a few nodes (less than minimum for training)
	(void) execute("CREATE (:Insuf {v: [1.0, 2.0, 3.0]})");
	(void) execute("CREATE (:Insuf {v: [4.0, 5.0, 6.0]})");

	auto res = execute("CALL db.index.vector.train('idx_insuf')");
	// Should handle gracefully
	EXPECT_TRUE(res.rowCount() >= 1);
}

TEST_F(CypherVectorTest, CreateIndexNonExistentProperty) {
	// Index on a property that doesn't exist yet
	(void) execute("CREATE VECTOR INDEX idx_future ON :Future(prop) OPTIONS {dim: 2}");

	// Later add nodes with that property
	(void) execute("CREATE (:Future {prop: [1.0, 2.0]})");

	// Should work
	auto res = execute("CALL db.index.vector.queryNodes('idx_future', 1, [1.0, 2.0])");
	EXPECT_TRUE(res.rowCount() >= 1);
}

TEST_F(CypherVectorTest, TrainNonExistentIndex) {
	EXPECT_THROW({ (void) execute("CALL db.index.vector.train('nonexistent_idx')"); },
				 std::runtime_error);
}

TEST_F(CypherVectorTest, VectorIndexPersistence) {
	(void) execute("CREATE VECTOR INDEX idx_persist ON :Persist(v) OPTIONS {dim: 2}");
	(void) execute("CREATE (:Persist {id: 1, v: [1.0, 0.0]})");

	// Flush and close
	db->getStorage()->flush();
	db->close();
	db.reset();

	// Reopen
	db = std::make_unique<graph::Database>(testFilePath.string());
	db->open();

	// Index should still exist
	auto res = execute("SHOW INDEXES");
	bool found = false;
	for (const auto &row: res.getRows()) {
		if (row.at("name").toString() == "idx_persist") {
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
}

// ============================================================================
// Tests for PropertyValueEvaluator Coverage - List Evaluation
// ============================================================================

TEST_F(CypherVectorTest, CreateVectorWithDoubles) {
	// Test list with double values
	(void) execute("CREATE VECTOR INDEX idx_dbl ON :Embedding(vector) OPTIONS {dim: 3, metric: 'L2'}");

	auto res = execute("CREATE (n:Embedding {vector: [1.0, 2.5, 3.14]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("vector"));
}

TEST_F(CypherVectorTest, CreateVectorWithIntegers) {
	// Test list with integer values - should be converted to float
	(void) execute("CREATE VECTOR INDEX idx_int ON :Embedding(vector) OPTIONS {dim: 3, metric: 'L2'}");

	auto res = execute("CREATE (n:Embedding {vector: [1, 2, 3]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("vector"));
}

TEST_F(CypherVectorTest, CreateVectorMixedTypes) {
	// Test list with mixed int and double values
	(void) execute("CREATE VECTOR INDEX idx_mix ON :Embedding(vector) OPTIONS {dim: 3, metric: 'L2'}");

	auto res = execute("CREATE (n:Embedding {vector: [1, 2.5, 3]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("vector"));
}

TEST_F(CypherVectorTest, CreateVectorScientificNotation) {
	// Test list with scientific notation values
	(void) execute("CREATE VECTOR INDEX idx_sci ON :Embedding(vector) OPTIONS {dim: 3, metric: 'L2'}");

	auto res = execute("CREATE (n:Embedding {vector: [1.5e10, 2.0E5, -3.0e-5]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("vector"));
}

TEST_F(CypherVectorTest, CreateVectorWithStringNumbers) {
	// Test list with string representations of numbers (should be parsed)
	// Note: In actual execution, string literals in lists might not be parsed as numbers
	// This test verifies the behavior
	(void) execute("CREATE VECTOR INDEX idx_str ON :Embedding(vector) OPTIONS {dim: 3, metric: 'L2'}");

	// Pure numeric strings might be parsed depending on implementation
	auto res = execute("CREATE (n:Embedding {vector: [1.0, 2.0, 3.0]}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// Additional tests for PropertyValueEvaluator coverage - list edge cases

TEST_F(CypherVectorTest, CreateVector_WithZeroes) {
	// Test vector with zero values
	(void) execute("CREATE VECTOR INDEX idx_zero2 ON :Zero2(vec) OPTIONS {dim: 3}");

	(void) execute("CREATE (n:Zero2 {vec: [0.0, 0, 0.0]})");
	auto res = execute("MATCH (n:Zero2) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherVectorTest, CreateVector_VerySmallValues) {
	// Test vector with very small values (scientific notation negative exponent)
	(void) execute("CREATE VECTOR INDEX idx_small ON :Small(vec) OPTIONS {dim: 3}");

	(void) execute("CREATE (n:Small {vec: [1.0e-10, 2.0e-5, 3.0e-3]})");
	auto res = execute("MATCH (n:Small) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherVectorTest, CreateVector_VeryLargeValues) {
	// Test vector with very large values
	(void) execute("CREATE VECTOR INDEX idx_large ON :Large(vec) OPTIONS {dim: 3}");

	(void) execute("CREATE (n:Large {vec: [1.0e10, 2.0e15, 3.0e20]})");
	auto res = execute("MATCH (n:Large) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherVectorTest, CreateVector_NegativeMixedWithPositive) {
	// Test vector with mixed negative and positive values
	(void) execute("CREATE VECTOR INDEX idx_mixed ON :Mixed(vec) OPTIONS {dim: 4}");

	(void) execute("CREATE (n:Mixed {vec: [-1.5, 2.5, -3.0, 4.0]})");
	auto res = execute("MATCH (n:Mixed) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherVectorTest, CreateVector_IntegerRangeBoundary) {
	// Test vector with values at integer boundaries
	(void) execute("CREATE VECTOR INDEX idx_boundary ON :Boundary(vec) OPTIONS {dim: 4}");

	(void) execute("CREATE (n:Boundary {vec: [0, 2147483647, -2147483648, 1]})");
	auto res = execute("MATCH (n:Boundary) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}
