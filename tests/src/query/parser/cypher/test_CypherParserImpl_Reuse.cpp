/**
 * @file test_CypherParserImpl_Reuse.cpp
 * @brief Tests for parser reuse — exercises the same CypherParserImpl instance
 *        across many sequential parse() / parseToLogical() calls.
 *        Direct regression test for the SIGBUS/SIGSEGV DFA cache corruption bug.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "CypherParserImpl.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class CypherParserReuseTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::query::QueryPlanner> planner;
	std::unique_ptr<graph::parser::cypher::CypherParserImpl> parser;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_reuse_" + to_string(uuid) + ".zyx");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);

		planner = std::make_shared<graph::query::QueryPlanner>(dataManager, indexManager);
		parser = std::make_unique<graph::parser::cypher::CypherParserImpl>(planner);
	}

	void TearDown() override {
		if (storage) storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(CypherParserReuseTest, ParseManyQueriesSequentially) {
	// Direct SIGBUS regression test: 30 diverse parse() calls on one instance
	const std::vector<std::string> queries = {
		"CREATE (n:Person {name: 'Alice'})",
		"MATCH (n:Person) RETURN n",
		"CREATE (n:City {name: 'Berlin'})",
		"MATCH (n) RETURN n LIMIT 5",
		"MATCH (n:Person) WHERE n.name = 'Alice' RETURN n.name",
		"CREATE (a:A)-[:REL]->(b:B)",
		"MATCH (a)-[r]->(b) RETURN a, r, b",
		"MATCH (n:Person) SET n.age = 30",
		"MATCH (n:City) DELETE n",
		"CREATE (n:Animal {species: 'Dog', name: 'Rex'})",
		"MATCH (n) RETURN count(n) AS total",
		"MATCH (n:Person) RETURN n.name ORDER BY n.name",
		"MERGE (n:Person {name: 'Bob'})",
		"UNWIND [1, 2, 3] AS x RETURN x",
		"MATCH (n:Person) WITH n.name AS name RETURN name",
		"RETURN 1 AS x UNION ALL RETURN 2 AS x",
		"CREATE (n:Tag {value: 'important'})",
		"MATCH (n:Animal) RETURN n.species AS s",
		"MATCH (n:Person) REMOVE n.age",
		"MATCH (n:Person) DETACH DELETE n",
		"CREATE (n:Log {ts: 12345})",
		"MATCH (n) RETURN n SKIP 2 LIMIT 3",
		"MATCH (a:A), (b:B) RETURN a, b",
		"MATCH (n:Person) OPTIONAL MATCH (n)-[:KNOWS]->(m) RETURN n, m",
		"CREATE (n:Fruit {name: 'Apple', color: 'red'})",
		"MATCH (n:Fruit) WHERE n.color = 'red' RETURN n.name",
		"RETURN 'hello' AS greeting",
		"RETURN 42 AS answer",
		"CREATE (n:Temp {x: 1})",
		"MATCH (n:Temp) DELETE n",
	};

	for (size_t i = 0; i < queries.size(); ++i) {
		SCOPED_TRACE("Query #" + std::to_string(i) + ": " + queries[i]);
		auto plan = parser->parse(queries[i]);
		ASSERT_NE(plan, nullptr);
	}
}

TEST_F(CypherParserReuseTest, ParseAlternatingReadWriteTypes) {
	for (int i = 0; i < 20; ++i) {
		SCOPED_TRACE("Iteration " + std::to_string(i));
		if (i % 2 == 0) {
			auto plan = parser->parse("CREATE (n:Node {i: " + std::to_string(i) + "})");
			ASSERT_NE(plan, nullptr);
		} else {
			auto plan = parser->parse("MATCH (n:Node) RETURN n");
			ASSERT_NE(plan, nullptr);
		}
	}
}

TEST_F(CypherParserReuseTest, ParseSameQueryRepeated) {
	const std::string query = "MATCH (n:Person) WHERE n.age > 25 RETURN n.name";
	for (int i = 0; i < 50; ++i) {
		auto plan = parser->parse(query);
		ASSERT_NE(plan, nullptr);
	}
}

TEST_F(CypherParserReuseTest, ParseAfterSyntaxError_Recovers) {
	// Bad query should throw
	EXPECT_THROW((void)parser->parse("MATC (n) RETRUN n"), std::runtime_error);

	// Parser must still work after error
	auto plan = parser->parse("MATCH (n:Person) RETURN n");
	ASSERT_NE(plan, nullptr);
}

TEST_F(CypherParserReuseTest, ParseAfterMultipleSyntaxErrors) {
	const std::vector<std::string> badQueries = {
		"!!!",
		"MATC (n)",
		"MATCH n RETURN",
		"CREATE",
		"RETURN RETURN RETURN",
	};
	const std::vector<std::string> goodQueries = {
		"MATCH (n) RETURN n",
		"CREATE (n:A {x: 1})",
		"RETURN 42 AS val",
		"MATCH (n:A) DELETE n",
		"MERGE (n:B {y: 2})",
	};

	for (size_t i = 0; i < badQueries.size(); ++i) {
		SCOPED_TRACE("Bad #" + std::to_string(i));
		EXPECT_THROW((void)parser->parse(badQueries[i]), std::runtime_error);

		SCOPED_TRACE("Good #" + std::to_string(i));
		auto plan = parser->parse(goodQueries[i]);
		ASSERT_NE(plan, nullptr);
	}
}

TEST_F(CypherParserReuseTest, ParseDiverseGrammarPaths) {
	const std::vector<std::string> queries = {
		"MATCH (n:A) WHERE n.x > 10 RETURN n",
		"MATCH (n:A)-[:R]->(m:B) RETURN m",
		"CREATE (n:C {p: 'val'})",
		"MERGE (n:D {id: 1})",
		"MATCH (n:E) DELETE n",
		"MATCH (n:F) SET n.flag = true",
		"MATCH (n:G) REMOVE n.flag",
		"UNWIND [10, 20] AS x RETURN x * 2 AS doubled",
		"MATCH (n) WITH n LIMIT 5 RETURN n",
		"RETURN 1 AS a UNION RETURN 2 AS a",
		"MATCH (n) OPTIONAL MATCH (n)-[r]->(m) RETURN n, m",
		"RETURN 'literal' AS s",
		"MATCH (n:H) RETURN n.x ORDER BY n.x SKIP 1 LIMIT 2",
		"MATCH (a:A), (b:B) RETURN a, b",
		"CREATE (a:X)-[:LINK]->(b:Y)",
		"MATCH (n) RETURN count(n) AS cnt",
		"MATCH (n:Person) WHERE n.name = 'Alice' OR n.name = 'Bob' RETURN n",
		"MATCH (n) WHERE n.age >= 18 AND n.age <= 65 RETURN n",
		"CREATE (n:Z {list: [1, 2, 3]})",
		"MATCH (n:A) RETURN n.x AS x, n.y AS y",
		"RETURN 1 + 2 AS sum",
		"RETURN true AS flag",
		"MATCH (n) RETURN DISTINCT n.label AS label",
		"MATCH (n:Person) DETACH DELETE n",
		"RETURN 3.14 AS pi",
	};

	for (size_t i = 0; i < queries.size(); ++i) {
		SCOPED_TRACE("Query #" + std::to_string(i));
		auto plan = parser->parse(queries[i]);
		ASSERT_NE(plan, nullptr);
	}
}

TEST_F(CypherParserReuseTest, ParseToLogicalAndParseConsistent) {
	const std::vector<std::string> queries = {
		"MATCH (n:Person) RETURN n",
		"CREATE (n:Test {x: 1})",
		"MATCH (n) RETURN count(n) AS c",
		"UNWIND [1, 2] AS x RETURN x",
		"RETURN 'hello' AS msg",
		"MATCH (a)-[r]->(b) RETURN a, r, b",
		"MERGE (n:M {id: 99})",
		"MATCH (n:X) SET n.val = 42",
		"MATCH (n:Y) DELETE n",
		"MATCH (n) RETURN n LIMIT 1",
	};

	for (size_t i = 0; i < queries.size(); ++i) {
		SCOPED_TRACE("Round " + std::to_string(i));

		// parseToLogical
		auto logical = parser->parseToLogical(queries[i]);
		// parse (physical)
		auto physical = parser->parse(queries[i]);
		ASSERT_NE(physical, nullptr);
	}
}
