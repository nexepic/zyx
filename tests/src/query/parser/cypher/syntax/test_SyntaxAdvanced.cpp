/**
 * @file test_SyntaxAdvanced.cpp
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

#include "SyntaxTestHelper.hpp"

TEST_F(CypherSyntaxTest, ValidPagination) {
	EXPECT_TRUE(validate("MATCH (n) RETURN n LIMIT 10"));
	EXPECT_TRUE(validate("MATCH (n) RETURN n SKIP 5 LIMIT 10"));
	EXPECT_FALSE(validate("MATCH (n) RETURN n LIMIT 10 SKIP 5")); // Wrong Order
}

TEST_F(CypherSyntaxTest, ValidOrderBy) { EXPECT_TRUE(validate("MATCH (n) RETURN n ORDER BY n.age DESC, n.name ASC")); }

TEST_F(CypherSyntaxTest, ValidVarLengthPath) {
	EXPECT_TRUE(validate("MATCH (a)-[*]->(b) RETURN b"));
	EXPECT_TRUE(validate("MATCH (a)-[*1..5]->(b) RETURN b"));
}

TEST_F(CypherSyntaxTest, ValidCartesianProduct) {
	EXPECT_TRUE(validate("MATCH (a), (b) RETURN a, b"));
	EXPECT_TRUE(validate("MATCH (a) MATCH (b) RETURN a, b"));
}

TEST_F(CypherSyntaxTest, ValidUnwind) {
	EXPECT_TRUE(validate("UNWIND [1, 2, 3] AS x RETURN x"));
	EXPECT_TRUE(validate("UNWIND [1, 'two', true] AS x RETURN x"));
	EXPECT_FALSE(validate("UNWIND [1, 2 AS x")); // Malformed
}

TEST_F(CypherSyntaxTest, ValidMultiPartQueryClauseShapes) {
	// The parser has distinct alternatives for WITH/read/update/subquery shapes.
	// Keep these as syntax contracts because their semantic execution depends on
	// a database, while the grammar itself must accept each legal arrangement.
	EXPECT_TRUE(validate("WITH 1 AS x WITH x AS y MATCH (n) RETURN n"));
	EXPECT_TRUE(validate("WITH 1 AS x CREATE (n:Probe {value: x})"));
	EXPECT_TRUE(validate("MATCH (n) WITH n MATCH (m) RETURN n, m"));
	EXPECT_TRUE(validate("MATCH (n) WITH n CALL { WITH n CREATE (m:Probe) RETURN m } RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) RETURN n"));
	EXPECT_TRUE(validate("MATCH (n) CREATE (m:Probe) RETURN m"));
}

TEST_F(CypherSyntaxTest, ValidGrammarSurfaceContracts) {
	// Each statement represents a separately documented grammar alternative.  Keep
	// this syntax-only: execution semantics belong to the integration suites.
	const std::vector<std::string> queries = {
			"EXPLAIN MATCH (n) RETURN n",
			"PROFILE SHOW INDEX",
			"CREATE INDEX idx FOR (n:User) ON (n.name, n.email)",
			"DROP INDEX idx",
			"CREATE CONSTRAINT user_email FOR (n:User) REQUIRE n.email IS UNIQUE",
			"CREATE CONSTRAINT edge_weight FOR ()-[r:REL]-() REQUIRE r.weight IS NOT NULL",
			"CREATE CONSTRAINT user_age FOR (n:User) REQUIRE n.age IS :: INTEGER",
			"CREATE CONSTRAINT user_key FOR (n:User) REQUIRE (n.id, n.name) IS NODE KEY",
			"DROP CONSTRAINT user_email IF EXISTS",
			"SHOW CONSTRAINT",
			"MATCH (n) RETURN n UNION MATCH (m) RETURN m",
			"MATCH (n) RETURN n UNION ALL MATCH (m) RETURN m",
			"CALL dbms.listConfig YIELD *",
			"OPTIONAL MATCH (n:User $properties) WHERE n.name STARTS WITH 'A' RETURN DISTINCT n ORDER BY n.name "
			"ASCENDING SKIP 1 LIMIT 2",
			"MATCH p=(a:User:Active {name: 'A'})<-[r:KNOWS|FOLLOWS*1..3 {since: 1}]-(b) RETURN p",
			"MATCH ((a)-[:REL]->(b)) RETURN a",
			"MATCH (n) SET n.age = 1, n = n, n += {active: true}, n:Active:Verified RETURN n",
			"MATCH (n) REMOVE n.age, n:Inactive RETURN n",
			"MATCH (n), (m) DELETE n, m",
			"FOREACH (x IN [1, 2] | CREATE (:Item {id: x}) SET x = x)",
			"LOAD CSV WITH HEADERS FROM 'file:///people.csv' AS row FIELDTERMINATOR ';' RETURN row",
			"CALL { WITH 1 AS seed RETURN seed } IN TRANSACTIONS OF 2 ROWS RETURN 1",
			"WITH 1 AS x WHERE x = 1 RETURN x",
			"RETURN true, false, null, 42, 0x2A, 0o52, {name: 'Ada', age: 42}, $name, $1",
			"RETURN count(*), any(x IN [1] WHERE x = 1), all(x IN [1] WHERE x = 1), none(x IN [1] WHERE x = 2), "
			"single(x IN [1] WHERE x = 1)",
			"RETURN reduce(total = 0, x IN [1, 2] | total + x)",
			"MATCH (n) RETURN exists((n)-[:REL]->()), shortestPath((n)-[:REL]->(m)), allShortestPaths((n)-[:REL]->(m))",
			"MATCH (n) RETURN n{.*, .name, score: n.score}",
			"RETURN coalesce(1, 2), CASE 1 WHEN 1 THEN 'one' ELSE 'other' END, CASE WHEN true THEN 1 END",
			"RETURN NOT (1 BETWEEN 0 AND 2) OR 1 XOR 2 = 2 AND 3 <> 4",
			"RETURN 1 + 2 - 3 * 4 / 5 % 6 ^ 2, -n.age, n.items[0], n.items[..2], n.items[1..]",
			"RETURN [x IN [1, 2] WHERE x > 1 | x * 2], [(a)-[:REL]->(b) WHERE a.id = 1 | b.name]",
	};
	for (const std::string &query: queries) {
		SCOPED_TRACE(query);
		EXPECT_TRUE(validate(query));
	}
}

TEST_F(CypherSyntaxTest, ValidProcedureAndMutationClauseCompositions) {
	// Keep these as parser contracts: each is a supported clause composition
	// whose runtime semantics are covered by the execution-layer suites.
	const std::vector<std::string> queries = {
			"CALL dbms.listConfig() YIELD name AS setting WHERE setting STARTS WITH 'db' RETURN setting",
			"MERGE (n:User {id: 1}) ON CREATE SET n.created = true ON MATCH SET n.seen = true RETURN n",
			"MATCH (n:User) DETACH DELETE n",
			"LOAD CSV FROM 'file:///people.csv' AS row RETURN row",
			"CALL { MATCH (n:User) RETURN n } IN TRANSACTIONS RETURN 1",
	};
	for (const std::string &query: queries) {
		SCOPED_TRACE(query);
		EXPECT_TRUE(validate(query));
	}
}

TEST_F(CypherSyntaxTest, ValidStandaloneCallAndPatternSyntaxVariants) {
	// These alternatives are parser-only contracts: they validate accepted
	// Cypher forms without introducing persistent-state setup into unit tests.
	const std::vector<std::string> queries = {
			"CALL dbms.listConfig YIELD name AS config_name, value WHERE config_name IS NOT NULL",
			"CALL dbms.listConfig() YIELD name, value",
			"EXPLAIN CREATE INDEX ON :User(email)",
			"PROFILE DROP CONSTRAINT temporary_constraint",
			"MATCH (source)-->(target) RETURN source, target",
			"MATCH (source)<--(target) RETURN source, target",
			"MATCH (source)--(target) RETURN source, target",
			"RETURN utility.combine(DISTINCT 1, 2);",
			"RETURN CASE WHEN false THEN 1 WHEN true THEN 2 ELSE 3 END",
			"RETURN {}, [], [1]",
	};
	for (const std::string &query: queries) {
		SCOPED_TRACE(query);
		EXPECT_TRUE(validate(query));
	}
}
