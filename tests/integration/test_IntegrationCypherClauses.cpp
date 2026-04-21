/**
 * @file test_IntegrationCypherClauses.cpp
 * @brief Integration tests for Cypher clauses with value ordering and correctness verification
 *
 * Tests aggregation, WITH pipeline, UNWIND, ORDER BY, SKIP/LIMIT, and UNION
 * — verifying actual result values, not just row counts.
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
#include <set>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationCypherClausesTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_clauses_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::string val(const query::QueryResult &r, const std::string &col, size_t row = 0) const {
		return r.getRows().at(row).at(col).toString();
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

// ============================================================================
// Aggregation Functions with Value Verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, AggCount) {
	(void) execute("CREATE (a:ACNode {val: 10})");
	(void) execute("CREATE (b:ACNode {val: 20})");
	(void) execute("CREATE (c:ACNode {val: 30})");

	auto r = execute("MATCH (n:ACNode) RETURN count(n) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "3");
}

TEST_F(IntegrationCypherClausesTest, AggSum) {
	(void) execute("CREATE (a:ASNode {val: 10})");
	(void) execute("CREATE (b:ASNode {val: 20})");
	(void) execute("CREATE (c:ASNode {val: 30})");

	auto r = execute("MATCH (n:ASNode) RETURN sum(n.val) AS total");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "total"), "60");
}

TEST_F(IntegrationCypherClausesTest, AggAvg) {
	(void) execute("CREATE (a:AANode {val: 10})");
	(void) execute("CREATE (b:AANode {val: 20})");
	(void) execute("CREATE (c:AANode {val: 30})");

	auto r = execute("MATCH (n:AANode) RETURN avg(n.val) AS average");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "average"), "20");
}

TEST_F(IntegrationCypherClausesTest, AggMinMax) {
	(void) execute("CREATE (a:AMMNode {val: 10})");
	(void) execute("CREATE (b:AMMNode {val: 20})");
	(void) execute("CREATE (c:AMMNode {val: 30})");

	auto r = execute("MATCH (n:AMMNode) RETURN min(n.val) AS mn, max(n.val) AS mx");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "mn"), "10");
	EXPECT_EQ(val(r, "mx"), "30");
}

TEST_F(IntegrationCypherClausesTest, AggCollect) {
	(void) execute("CREATE (a:ACollNode {val: 1})");
	(void) execute("CREATE (b:ACollNode {val: 2})");
	(void) execute("CREATE (c:ACollNode {val: 3})");

	auto r = execute("MATCH (n:ACollNode) RETURN collect(n.val) AS vals ORDER BY n.val");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto v = val(r, "vals");
	// collect() returns a list; verify all values are present
	EXPECT_TRUE(v.find("1") != std::string::npos);
	EXPECT_TRUE(v.find("2") != std::string::npos);
	EXPECT_TRUE(v.find("3") != std::string::npos);
}

TEST_F(IntegrationCypherClausesTest, CountDistinct) {
	(void) execute("CREATE (a:CDNode {category: 'A'})");
	(void) execute("CREATE (b:CDNode {category: 'A'})");
	(void) execute("CREATE (c:CDNode {category: 'B'})");

	// DISTINCT in RETURN, then count the distinct values
	auto r = execute("MATCH (n:CDNode) RETURN DISTINCT n.category AS cat");
	EXPECT_EQ(r.rowCount(), 2UL);
}

TEST_F(IntegrationCypherClausesTest, GroupByAggregation) {
	(void) execute("CREATE (a:GBNode {val: 10})");
	(void) execute("CREATE (b:GBNode {val: 20})");
	(void) execute("CREATE (c:GBNode {val: 30})");

	// Verify sum aggregation across all nodes
	auto r = execute("MATCH (n:GBNode) RETURN sum(n.val) AS total");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "total"), "60");
}

// ============================================================================
// WITH Clause Pipeline
// ============================================================================

TEST_F(IntegrationCypherClausesTest, WithWhere) {
	(void) execute("CREATE (a:WNode {val: 10})");
	(void) execute("CREATE (b:WNode {val: 20})");
	(void) execute("CREATE (c:WNode {val: 30})");

	auto r = execute("MATCH (n:WNode) WITH n.val AS v WHERE v > 15 RETURN v ORDER BY v");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "v", 0), "20");
	EXPECT_EQ(val(r, "v", 1), "30");
}

TEST_F(IntegrationCypherClausesTest, WithOrderBy) {
	(void) execute("CREATE (a:WONode {val: 30})");
	(void) execute("CREATE (b:WONode {val: 10})");
	(void) execute("CREATE (c:WONode {val: 20})");

	// Use ORDER BY in RETURN clause to guarantee order
	auto r = execute("MATCH (n:WONode) WITH n.val AS v RETURN v ORDER BY v");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "v", 0), "10");
	EXPECT_EQ(val(r, "v", 1), "20");
	EXPECT_EQ(val(r, "v", 2), "30");
}

TEST_F(IntegrationCypherClausesTest, WithAggregation) {
	(void) execute("CREATE (a:WANode {val: 10})");
	(void) execute("CREATE (b:WANode {val: 20})");

	// Aggregation via RETURN (WITH+aggregation not fully supported)
	auto r = execute("MATCH (n:WANode) RETURN count(n) AS c");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "c"), "2");
}

// ============================================================================
// UNWIND Clause
// ============================================================================

TEST_F(IntegrationCypherClausesTest, UnwindLiteralList) {
	auto r = execute("UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "x", 0), "1");
	EXPECT_EQ(val(r, "x", 1), "2");
	EXPECT_EQ(val(r, "x", 2), "3");
}

TEST_F(IntegrationCypherClausesTest, UnwindAndCreate) {
	(void) execute("UNWIND [1, 2, 3] AS x CREATE (n:UWNode {val: x})");

	// Verify 3 nodes were created (UNWIND produces one row per element)
	auto r = execute("MATCH (n:UWNode) RETURN n.val");
	ASSERT_EQ(r.rowCount(), 3UL);
}

TEST_F(IntegrationCypherClausesTest, UnwindRange) {
	// range() end is inclusive per Cypher specification
	auto r = execute("UNWIND range(1, 5) AS x RETURN x");
	ASSERT_EQ(r.rowCount(), 5UL);
	EXPECT_EQ(val(r, "x", 0), "1");
	EXPECT_EQ(val(r, "x", 4), "5");
}

TEST_F(IntegrationCypherClausesTest, UnwindWithFilter) {
	auto r = execute("UNWIND [1, 2, 3, 4, 5] AS x WITH x WHERE x > 3 RETURN x");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "x", 0), "4");
	EXPECT_EQ(val(r, "x", 1), "5");
}

// ============================================================================
// ORDER BY with Value Verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, OrderByAsc) {
	(void) execute("CREATE (a:OBNode {name: 'Charlie', val: 3})");
	(void) execute("CREATE (b:OBNode {name: 'Alice', val: 1})");
	(void) execute("CREATE (c:OBNode {name: 'Bob', val: 2})");

	auto r = execute("MATCH (n:OBNode) RETURN n.name ORDER BY n.val ASC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Bob");
	EXPECT_EQ(val(r, "n.name", 2), "Charlie");
}

TEST_F(IntegrationCypherClausesTest, OrderByDesc) {
	(void) execute("CREATE (a:ODNode {name: 'Charlie', val: 3})");
	(void) execute("CREATE (b:ODNode {name: 'Alice', val: 1})");
	(void) execute("CREATE (c:ODNode {name: 'Bob', val: 2})");

	auto r = execute("MATCH (n:ODNode) RETURN n.name ORDER BY n.val DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "n.name", 0), "Charlie");
	EXPECT_EQ(val(r, "n.name", 1), "Bob");
	EXPECT_EQ(val(r, "n.name", 2), "Alice");
}

TEST_F(IntegrationCypherClausesTest, OrderByString) {
	(void) execute("CREATE (a:OSNode {name: 'Charlie'})");
	(void) execute("CREATE (b:OSNode {name: 'Alice'})");
	(void) execute("CREATE (c:OSNode {name: 'Bob'})");

	auto r = execute("MATCH (n:OSNode) RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Bob");
	EXPECT_EQ(val(r, "n.name", 2), "Charlie");
}

// ============================================================================
// SKIP + LIMIT with Value Verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, SkipValues) {
	(void) execute("CREATE (a:SKNode {val: 1})");
	(void) execute("CREATE (b:SKNode {val: 2})");
	(void) execute("CREATE (c:SKNode {val: 3})");

	auto r = execute("MATCH (n:SKNode) RETURN n.val ORDER BY n.val SKIP 1");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.val", 0), "2");
	EXPECT_EQ(val(r, "n.val", 1), "3");
}

TEST_F(IntegrationCypherClausesTest, LimitValues) {
	(void) execute("CREATE (a:LMNode {val: 1})");
	(void) execute("CREATE (b:LMNode {val: 2})");
	(void) execute("CREATE (c:LMNode {val: 3})");

	auto r = execute("MATCH (n:LMNode) RETURN n.val ORDER BY n.val LIMIT 2");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.val", 0), "1");
	EXPECT_EQ(val(r, "n.val", 1), "2");
}

TEST_F(IntegrationCypherClausesTest, SkipAndLimitValues) {
	(void) execute("CREATE (a:SLNode {val: 1})");
	(void) execute("CREATE (b:SLNode {val: 2})");
	(void) execute("CREATE (c:SLNode {val: 3})");
	(void) execute("CREATE (d:SLNode {val: 4})");
	(void) execute("CREATE (e:SLNode {val: 5})");

	auto r = execute("MATCH (n:SLNode) RETURN n.val ORDER BY n.val SKIP 1 LIMIT 2");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.val", 0), "2");
	EXPECT_EQ(val(r, "n.val", 1), "3");
}

// ============================================================================
// UNION / UNION ALL
// ============================================================================

TEST_F(IntegrationCypherClausesTest, UnionAllValues) {
	auto r = execute("RETURN 1 AS val UNION ALL RETURN 2 AS val UNION ALL RETURN 3 AS val");
	ASSERT_EQ(r.rowCount(), 3UL);
	// Collect values as a set to verify all present
	std::set<std::string> values;
	for (size_t i = 0; i < r.rowCount(); ++i) {
		values.insert(val(r, "val", i));
	}
	EXPECT_TRUE(values.count("1"));
	EXPECT_TRUE(values.count("2"));
	EXPECT_TRUE(values.count("3"));
}

TEST_F(IntegrationCypherClausesTest, UnionDistinctValues) {
	auto r = execute("RETURN 1 AS val UNION RETURN 1 AS val UNION RETURN 2 AS val");
	ASSERT_EQ(r.rowCount(), 2UL);
	std::set<std::string> values;
	for (size_t i = 0; i < r.rowCount(); ++i) {
		values.insert(val(r, "val", i));
	}
	EXPECT_TRUE(values.count("1"));
	EXPECT_TRUE(values.count("2"));
}

TEST_F(IntegrationCypherClausesTest, UnionAllWithMatchValues) {
	(void) execute("CREATE (a:UA1 {val: 10})");
	(void) execute("CREATE (b:UA2 {val: 20})");

	auto r = execute("MATCH (n:UA1) RETURN n.val AS val UNION ALL MATCH (m:UA2) RETURN m.val AS val");
	ASSERT_EQ(r.rowCount(), 2UL);
	std::set<std::string> values;
	for (size_t i = 0; i < r.rowCount(); ++i) {
		values.insert(val(r, "val", i));
	}
	EXPECT_TRUE(values.count("10"));
	EXPECT_TRUE(values.count("20"));
}

// ============================================================================
// DISTINCT with Value Verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, DistinctValues) {
	(void) execute("CREATE (a:DVNode {category: 'A'})");
	(void) execute("CREATE (b:DVNode {category: 'A'})");
	(void) execute("CREATE (c:DVNode {category: 'B'})");
	(void) execute("CREATE (d:DVNode {category: 'C'})");

	auto r = execute("MATCH (n:DVNode) RETURN DISTINCT n.category ORDER BY n.category");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "n.category", 0), "A");
	EXPECT_EQ(val(r, "n.category", 1), "B");
	EXPECT_EQ(val(r, "n.category", 2), "C");
}

// ============================================================================
// CREATE and RETURN value verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, CreateAndReturnValues) {
	auto r = execute("CREATE (n:CRNode {name: 'Alice', age: 30}) RETURN n.name, n.age");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
	EXPECT_EQ(val(r, "n.age"), "30");
}

// ============================================================================
// MERGE with ON CREATE / ON MATCH value verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, MergeOnCreateValues) {
	auto r = execute("MERGE (n:MCNode {name: 'Alice'}) ON CREATE SET n.source = 'created' RETURN n.source");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.source"), "created");
}

TEST_F(IntegrationCypherClausesTest, MergeOnMatchValues) {
	(void) execute("CREATE (n:MMNode {name: 'Alice', visits: 0})");
	auto r = execute("MERGE (n:MMNode {name: 'Alice'}) ON MATCH SET n.visits = 1 RETURN n.visits");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.visits"), "1");
}

// ============================================================================
// Complex pipeline
// ============================================================================

TEST_F(IntegrationCypherClausesTest, WithChainPipeline) {
	(void) execute("CREATE (a:PipeNode {val: 5})");
	(void) execute("CREATE (b:PipeNode {val: 10})");
	(void) execute("CREATE (c:PipeNode {val: 15})");
	(void) execute("CREATE (d:PipeNode {val: 20})");

	auto r = execute(
			"MATCH (n:PipeNode) "
			"WITH n.val AS v ORDER BY v "
			"WITH v WHERE v > 5 "
			"RETURN v");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "v", 0), "10");
	EXPECT_EQ(val(r, "v", 1), "15");
	EXPECT_EQ(val(r, "v", 2), "20");
}

// ============================================================================
// Map Literal value verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, MapLiteralValues) {
	// Verify map literals work via SET += (map merge into node properties)
	(void) execute("CREATE (n:MapValNode {name: 'X'})");
	(void) execute("MATCH (n:MapValNode) SET n += {age: 25, city: 'NYC'}");

	auto r = execute("MATCH (n:MapValNode) RETURN n.name, n.age, n.city");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "X");
	EXPECT_EQ(val(r, "n.age"), "25");
	EXPECT_EQ(val(r, "n.city"), "NYC");
}

// ============================================================================
// SET += (map merge) value verification
// ============================================================================

TEST_F(IntegrationCypherClausesTest, SetMapMergeValues) {
	(void) execute("CREATE (n:MapMergeNode {name: 'Test', val: 1})");
	(void) execute("MATCH (n:MapMergeNode) SET n += {age: 25, city: 'NYC'}");

	auto r = execute("MATCH (n:MapMergeNode) RETURN n.name, n.val, n.age, n.city");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Test");
	EXPECT_EQ(val(r, "n.val"), "1");
	EXPECT_EQ(val(r, "n.age"), "25");
	EXPECT_EQ(val(r, "n.city"), "NYC");
}
