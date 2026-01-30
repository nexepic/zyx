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
