/**
 * @file test_CypherAlgo.cpp
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
#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

class CypherAlgoTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_algo_" + boost::uuids::to_string(uuid) + ".dat");
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

TEST_F(CypherAlgoTest, AlgoShortestPath) {
	(void) execute("CREATE (a:City {name: 'A'})-[r1:ROAD]->(b:City {name: 'B'})");
	(void) execute("MATCH (b:City {name: 'B'}) CREATE (b)-[r2:ROAD]->(c:City {name: 'C'})");

	int64_t idA = execute("MATCH (n:City {name: 'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idC = execute("MATCH (n:City {name: 'C'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idC) + ")");
	ASSERT_EQ(res.rowCount(), 3UL); // A, B, C
}

TEST_F(CypherAlgoTest, AlgoShortestPathNoPath) {
	(void) execute("CREATE (a:Isle {name: 'A'})");
	(void) execute("CREATE (b:Isle {name: 'B'})");

	int64_t idA = execute("MATCH (n:Isle {name: 'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idB = execute("MATCH (n:Isle {name: 'B'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idB) + ")");
	EXPECT_TRUE(res.isEmpty());
}

// --- Additional Shortest Path Tests ---

TEST_F(CypherAlgoTest, AlgoShortestPathDirectConnection) {
	// A -> B directly
	(void) execute("CREATE (a:Direct {id:'A'})-[r:LINK]->(b:Direct {id:'B'})");

	int64_t idA = execute("MATCH (n:Direct {id:'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idB = execute("MATCH (n:Direct {id:'B'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idB) + ")");
	ASSERT_EQ(res.rowCount(), 2UL); // A and B
}

TEST_F(CypherAlgoTest, AlgoShortestPathMultiplePaths) {
	// Create diamond: A -> B, A -> C, B -> D, C -> D
	(void) execute("CREATE (a:Dia {id:'A'})-[r1:LINK]->(b:Dia {id:'B'})");
	(void) execute("MATCH (a:Dia {id:'A'}) CREATE (a)-[r2:LINK]->(c:Dia {id:'C'})");
	(void) execute("MATCH (b:Dia {id:'B'}) CREATE (b)-[r3:LINK]->(d:Dia {id:'D'})");
	(void) execute("MATCH (c:Dia {id:'C'}) CREATE (c)-[r4:LINK]->(d:Dia {id:'D'})");

	int64_t idA = execute("MATCH (n:Dia {id:'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idD = execute("MATCH (n:Dia {id:'D'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idD) + ")");
	// Should return one of the shortest paths (A->B->D or A->C->D)
	ASSERT_GE(res.rowCount(), 3UL); // At least 3 nodes
	ASSERT_LE(res.rowCount(), 4UL); // At most 4 nodes
}

TEST_F(CypherAlgoTest, AlgoShortestPathComplex) {
	// Create more complex graph
	// A -> B -> C -> D
	// A -> X -> D (shorter path to D)
	(void) execute("CREATE (a:Complex {id:'A'})-[r1:LINK]->(b:Complex {id:'B'})");
	(void) execute("MATCH (b:Complex {id:'B'}) CREATE (b)-[r2:LINK]->(c:Complex {id:'C'})");
	(void) execute("MATCH (c:Complex {id:'C'}) CREATE (c)-[r3:LINK]->(d:Complex {id:'D'})");
	(void) execute("MATCH (a:Complex {id:'A'}) CREATE (a)-[r4:LINK]->(x:Complex {id:'X'})");
	(void) execute("MATCH (x:Complex {id:'X'}) CREATE (x)-[r5:LINK]->(d:Complex {id:'D'})");

	int64_t idA = execute("MATCH (n:Complex {id:'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idD = execute("MATCH (n:Complex {id:'D'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idD) + ")");
	// Should return a path (3-4 nodes depending on which path is chosen)
	ASSERT_GE(res.rowCount(), 3UL); // At least 3 nodes
	ASSERT_LE(res.rowCount(), 5UL); // At most 5 nodes (A, X, D, or A, B, C, D)
}
