/**
 * @file test_CypherParserImpl_Reuse.cpp
 * @brief Native logical-parser reuse and error-recovery regressions.
 *
 * The ANTLR runtime keeps DFA state across calls. These tests exercise one
 * native parser instance repeatedly so that a regression cannot be hidden by
 * constructing a physical planner or a legacy storage backend per parse.
 **/

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"

namespace graph::parser::cypher {

class CypherParserReuseTest : public ::testing::Test {
protected:
	std::unique_ptr<CypherParserImpl> parser = std::make_unique<CypherParserImpl>();

	void expectLogicalPlan(const std::string &query) {
		SCOPED_TRACE(query);
		query::QueryPlan plan = parser->parseToLogical(query);
		ASSERT_NE(plan.root, nullptr);
	}
};

TEST_F(CypherParserReuseTest, ParsesManyQueriesSequentially) {
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

	for (const std::string &query : queries) {
		expectLogicalPlan(query);
	}
}

TEST_F(CypherParserReuseTest, ParsesAlternatingReadAndWriteQueries) {
	for (int i = 0; i < 20; ++i) {
		if (i % 2 == 0) {
			expectLogicalPlan("CREATE (n:Node {i: " + std::to_string(i) + "})");
		} else {
			expectLogicalPlan("MATCH (n:Node) RETURN n");
		}
	}
}

TEST_F(CypherParserReuseTest, ParsesTheSameQueryRepeatedly) {
	const std::string query = "MATCH (n:Person) WHERE n.age > 25 RETURN n.name";
	for (int i = 0; i < 50; ++i) {
		expectLogicalPlan(query);
	}
}

TEST_F(CypherParserReuseTest, RecoversAfterSyntaxErrors) {
	EXPECT_THROW((void) parser->parseToLogical("MATC (n) RETRUN n"), std::runtime_error);
	expectLogicalPlan("MATCH (n:Person) RETURN n");
}

TEST_F(CypherParserReuseTest, RecoversAfterMultipleSyntaxErrors) {
	const std::vector<std::string> malformed = {
		"!!!", "MATC (n)", "MATCH n RETURN", "CREATE", "RETURN RETURN RETURN",
	};
	const std::vector<std::string> valid = {
		"MATCH (n) RETURN n", "CREATE (n:A {x: 1})", "RETURN 42 AS val", "MATCH (n:A) DELETE n",
		"MERGE (n:B {y: 2})",
	};

	ASSERT_EQ(malformed.size(), valid.size());
	for (size_t i = 0; i < malformed.size(); ++i) {
		EXPECT_THROW((void) parser->parseToLogical(malformed[i]), std::runtime_error);
		expectLogicalPlan(valid[i]);
	}
}

TEST_F(CypherParserReuseTest, ParsesDiverseGrammarPaths) {
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

	for (const std::string &query : queries) {
		expectLogicalPlan(query);
	}
}

} // namespace graph::parser::cypher
