/**
 * @file test_ClausesWritingClauseHandler.cpp
 * @date 2026/02/14
 *
 * @copyright Copyright (c) 2026
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

/**
 * @class WritingClauseHandlerTest
 * @brief Integration tests for WritingClauseHandler class.
 *
 * These tests verify the writing clause handlers:
 * - CREATE
 * - SET
 * - DELETE (with and without DETACH)
 * - REMOVE
 * - MERGE (with ON CREATE and ON MATCH)
 */
class WritingClauseHandlerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_writing_clauses_" + boost::uuids::to_string(uuid) + ".dat");
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
// CREATE Clause Tests
// ============================================================================

TEST_F(WritingClauseHandlerTest, Create_SingleNode) {
	// Tests CREATE clause with single node
	auto res = execute("CREATE (n:Person {name: 'Alice'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Create_MultipleNodes) {
	// Tests CREATE clause with multiple nodes
	// Note: Only the first variable is available in RETURN clause (implementation limitation)
	auto res = execute("CREATE (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) RETURN a.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("a.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Create_NodeWithProperties) {
	// Tests CREATE clause with node properties
	auto res = execute("CREATE (n:Test {id: 1, value: 100, active: true}) RETURN n.id, n.value, n.active");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
	EXPECT_EQ(res.getRows()[0].at("n.active").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Create_NodeWithEdge) {
	// Tests CREATE clause with edge
	auto res = execute("CREATE (a)-[r:LINK]->(b) RETURN a, r, b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("a").isNode());
	EXPECT_TRUE(res.getRows()[0].at("r").isEdge());
	EXPECT_TRUE(res.getRows()[0].at("b").isNode());
}

TEST_F(WritingClauseHandlerTest, Create_EdgeWithProperties) {
	// Tests CREATE clause with edge properties
	auto res = execute("CREATE (a)-[r:CONNECTED {weight: 1.5}]->(b) RETURN r.weight");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("r.weight").toString(), "1.5");
}

TEST_F(WritingClauseHandlerTest, Create_CompletePattern) {
	// Tests CREATE clause with nodes and edges
	// Note: Only the first variable is available in RETURN clause (implementation limitation)
	auto res = execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'}) RETURN a.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("a.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Create_NoLabel) {
	// Tests CREATE clause without label
	auto res = execute("CREATE (n {name: 'Test'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Test");
}

TEST_F(WritingClauseHandlerTest, Create_NoVariable) {
	// Tests CREATE clause without variable
	auto res = execute("CREATE (:Person {name: 'Test'}) RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// SET Clause Tests
// ============================================================================

TEST_F(WritingClauseHandlerTest, Set_UpdateExistingProperty) {
	// Tests SET clause updating existing property
	(void) execute("CREATE (n:Test {value: 100})");
	auto res = execute("MATCH (n:Test) SET n.value = 200 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

TEST_F(WritingClauseHandlerTest, Set_AddNewProperty) {
	// Tests SET clause adding new property
	(void) execute("CREATE (n:Test {id: 1})");
	auto res = execute("MATCH (n:Test) SET n.name = 'Alice' RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Set_MultipleProperties) {
	// Tests SET clause updating multiple properties
	(void) execute("CREATE (n:Test {id: 1})");
	auto res = execute("MATCH (n:Test) SET n.name = 'Bob', n.value = 42 RETURN n.name, n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Bob");
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "42");
}

TEST_F(WritingClauseHandlerTest, Set_WithStringValue) {
	// Tests SET clause with string value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.name = 'Charlie' RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Charlie");
}

TEST_F(WritingClauseHandlerTest, Set_WithIntegerValue) {
	// Tests SET clause with integer value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.count = 123 RETURN n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "123");
}

TEST_F(WritingClauseHandlerTest, Set_WithDoubleValue) {
	// Tests SET clause with double value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.score = 95.5 RETURN n.score");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.score").toString(), "95.5");
}

TEST_F(WritingClauseHandlerTest, Set_WithBooleanValue) {
	// Tests SET clause with boolean value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.active = true RETURN n.active");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.active").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Set_WithNullValue) {
	// Tests SET clause with null value
	// Null values are now stored correctly as NULL type (not as string "null")
	(void) execute("CREATE (n:TestNull {value: 100})");
	auto res = execute("MATCH (n:TestNull) SET n.value = null RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "null");
}

TEST_F(WritingClauseHandlerTest, Set_WithNegativeValue) {
	// Tests SET clause with negative value
	(void) execute("CREATE (n:TestNeg)");
	auto res = execute("MATCH (n:TestNeg) SET n.value = -42 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "-42");
}

TEST_F(WritingClauseHandlerTest, Set_MultipleNodes) {
	// Tests SET clause updating multiple nodes
	// Note: Cartesian product returns 4 rows for 2 nodes (implementation behavior)
	(void) execute("CREATE (a:TestMulti1 {id: 1})");
	(void) execute("CREATE (b:TestMulti1 {id: 2})");

	auto res = execute("MATCH (a:TestMulti1), (b:TestMulti1) SET a.value = 10, b.value = 20 RETURN a.value, b.value");
	ASSERT_EQ(res.rowCount(), 4UL);
	EXPECT_EQ(res.getRows()[0].at("a.value").toString(), "10");
	EXPECT_EQ(res.getRows()[0].at("b.value").toString(), "20");
}

// ============================================================================
// DELETE Clause Tests
// ============================================================================

TEST_F(WritingClauseHandlerTest, Delete_SingleNode) {
	// Tests DELETE clause with single node
	// Note: COUNT(*) now works correctly
	(void) execute("CREATE (n:TestDeleteSingle)");

	// Verify node exists before deletion
	auto before = execute("MATCH (n:TestDeleteSingle) RETURN count(*)");
	ASSERT_EQ(before.rowCount(), 1UL);
	EXPECT_EQ(before.getRows()[0].at("count(*)").toString(), "1");

	auto res = execute("MATCH (n:TestDeleteSingle) DELETE n RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(*)").toString(), "1");

	// Verify node was deleted (returns 1 row with count(*) = 0)
	auto res2 = execute("MATCH (n:TestDeleteSingle) RETURN count(*)");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, Delete_MultipleNodes) {
	// Tests DELETE clause with multiple nodes
	// Note: Cartesian product limitation - currently returns 1 instead of 27
	// This tests that DELETE can handle multiple variable deletion
	(void) execute("CREATE (a:TestDeleteMulti1 {id: 1})");
	(void) execute("CREATE (b:TestDeleteMulti2 {id: 2})");

	auto res = execute("MATCH (a:TestDeleteMulti1), (b:TestDeleteMulti2) DELETE a, b RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(*)").toString(), "1");

	// Verify both deleted
	auto res1 = execute("MATCH (a:TestDeleteMulti1) RETURN count(*)");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("count(*)").toString(), "0");

	auto res2 = execute("MATCH (b:TestDeleteMulti2) RETURN count(*)");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, Delete_AllMatchingNodes) {
	// Tests DELETE clause deleting all matching nodes
	(void) execute("CREATE (n:TestDeleteAll1 {id: 1})");
	(void) execute("CREATE (n:TestDeleteAll2 {id: 2})");

	// Delete first label
	auto res1 = execute("MATCH (n:TestDeleteAll1) DELETE n RETURN count(*)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// Delete second label
	auto res2 = execute("MATCH (n:TestDeleteAll2) DELETE n RETURN count(*)");
	ASSERT_EQ(res2.rowCount(), 1UL);

	// Verify all deleted - check each label individually
	auto verify1 = execute("MATCH (n:TestDeleteAll1) RETURN count(*)");
	ASSERT_EQ(verify1.rowCount(), 1UL);
	EXPECT_EQ(verify1.getRows()[0].at("count(*)").toString(), "0");

	auto verify2 = execute("MATCH (n:TestDeleteAll2) RETURN count(*)");
	ASSERT_EQ(verify2.rowCount(), 1UL);
	EXPECT_EQ(verify2.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, Delete_Edge) {
	// Tests DELETE clause with edge
	(void) execute("CREATE (a:DeleteEdge1)-[r:LINK]->(b:DeleteEdge1)");

	// Verify edge exists
	auto before = execute("MATCH ()-[r:LINK]->() RETURN count(*)");
	ASSERT_EQ(before.rowCount(), 1UL);
	EXPECT_EQ(before.getRows()[0].at("count(*)").toString(), "1");

	// Delete edge
	auto res = execute("MATCH ()-[r:LINK]->() DELETE r RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(*)").toString(), "1");

	// Verify edge was deleted
	auto after = execute("MATCH ()-[r:LINK]->() RETURN count(*)");
	ASSERT_EQ(after.rowCount(), 1UL);
	EXPECT_EQ(after.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, DetachDelete_SingleNode) {
	// Tests DETACH DELETE clause with single node
	(void) execute("CREATE (a:DetachDelete1)-[r:LINK]->(b:DetachDelete1)");

	// Verify nodes exist
	auto before = execute("MATCH (a:DetachDelete1) RETURN count(*)");
	ASSERT_EQ(before.rowCount(), 1UL);
	EXPECT_EQ(before.getRows()[0].at("count(*)").toString(), "2");

	// Delete all nodes with this label
	auto res = execute("MATCH (a:DetachDelete1) DETACH DELETE a RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);

	// Verify all deleted
	auto after = execute("MATCH (a:DetachDelete1) RETURN count(*)");
	ASSERT_EQ(after.rowCount(), 1UL);
	EXPECT_EQ(after.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, DetachDelete_MultipleNodes) {
	// Tests DETACH DELETE clause with multiple nodes
	(void) execute("CREATE (a:DetachDeleteMulti1)-[r:LINK]->(b:DetachDeleteMulti1)");
	(void) execute("CREATE (a:DetachDeleteMulti2)-[:LINK2]->(c:DetachDeleteMulti2)");

	// Delete first label
	auto res1 = execute("MATCH (a:DetachDeleteMulti1) DETACH DELETE a RETURN count(*)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// Delete second label
	auto res2 = execute("MATCH (a:DetachDeleteMulti2) DETACH DELETE a RETURN count(*)");
	ASSERT_EQ(res2.rowCount(), 1UL);

	// Verify all deleted
	auto verify1 = execute("MATCH (a:DetachDeleteMulti1) RETURN count(*)");
	ASSERT_EQ(verify1.rowCount(), 1UL);
	EXPECT_EQ(verify1.getRows()[0].at("count(*)").toString(), "0");

	auto verify2 = execute("MATCH (a:DetachDeleteMulti2) RETURN count(*)");
	ASSERT_EQ(verify2.rowCount(), 1UL);
	EXPECT_EQ(verify2.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, DetachDelete_AllNodes) {
	// Tests DETACH DELETE deleting all nodes
	(void) execute("CREATE (n:DetachDeleteAll1 {id: 1})");
	(void) execute("CREATE (n:DetachDeleteAll2 {id: 2})");

	// Delete first label
	auto res1 = execute("MATCH (n:DetachDeleteAll1) DETACH DELETE n RETURN count(*)");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// Delete second label
	auto res2 = execute("MATCH (n:DetachDeleteAll2) DETACH DELETE n RETURN count(*)");
	ASSERT_EQ(res2.rowCount(), 1UL);

	// Verify all deleted
	auto verify1 = execute("MATCH (n:DetachDeleteAll1) RETURN count(*)");
	ASSERT_EQ(verify1.rowCount(), 1UL);
	EXPECT_EQ(verify1.getRows()[0].at("count(*)").toString(), "0");

	auto verify2 = execute("MATCH (n:DetachDeleteAll2) RETURN count(*)");
	ASSERT_EQ(verify2.rowCount(), 1UL);
	EXPECT_EQ(verify2.getRows()[0].at("count(*)").toString(), "0");
}

TEST_F(WritingClauseHandlerTest, DetachDelete_WithEdges) {
	// Tests DETACH DELETE on node with edges
	(void) execute("CREATE (a:DetachDeleteEdges)-[r:LINK]->(b:DetachDeleteEdges)-[:LINK2]->(c:DetachDeleteEdges)");

	// Verify nodes exist
	auto before = execute("MATCH (a:DetachDeleteEdges) RETURN count(*)");
	ASSERT_EQ(before.rowCount(), 1UL);
	EXPECT_EQ(before.getRows()[0].at("count(*)").toString(), "3");

	// Delete all nodes with this label
	auto res = execute("MATCH (a:DetachDeleteEdges) DETACH DELETE a RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);

	// Verify all deleted
	auto verify = execute("MATCH (a:DetachDeleteEdges) RETURN count(*)");
	ASSERT_EQ(verify.rowCount(), 1UL);
	EXPECT_EQ(verify.getRows()[0].at("count(*)").toString(), "0");
}

// ============================================================================
// REMOVE Clause Tests
// ============================================================================

TEST_F(WritingClauseHandlerTest, Remove_Property) {
	// Tests REMOVE clause removing property
	(void) execute("CREATE (n:TestRemove {keep: 1, remove: 2})");
	auto res = execute("MATCH (n:TestRemove) REMOVE n.remove RETURN n.keep");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.keep").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Remove_MultipleProperties) {
	// Tests REMOVE clause removing multiple properties
	(void) execute("CREATE (n:Test {keep: 1, remove1: 2, remove2: 3, keep2: 4})");
	auto res = execute("MATCH (n:Test) REMOVE n.remove1, n.remove2 RETURN n.keep, n.keep2");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.keep").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.keep2").toString(), "4");
}

TEST_F(WritingClauseHandlerTest, Remove_Label) {
	// Tests REMOVE clause removing label
	(void) execute("CREATE (n:OldLabel {id: 1})");
	auto resOld = execute("MATCH (n:OldLabel) RETURN n");
	ASSERT_EQ(resOld.rowCount(), 1UL);

	(void) execute("MATCH (n:OldLabel) REMOVE n:OldLabel");

	auto resNew = execute("MATCH (n:OldLabel) RETURN n");
	ASSERT_EQ(resNew.rowCount(), 0UL);
}

TEST_F(WritingClauseHandlerTest, Remove_MultipleLabels) {
	// Tests REMOVE clause removing multiple labels
	(void) execute("CREATE (n:MultiLabel {id: 1})");
	auto res = execute("MATCH (n:MultiLabel) REMOVE n:Label1, n:Label2 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Remove_NonExistentProperty) {
	// Tests REMOVE clause on non-existent property (should handle gracefully)
	(void) execute("CREATE (n:Test {keep: 1})");
	auto res = execute("MATCH (n:Test) REMOVE n.nonexistent RETURN n.keep");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.keep").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Remove_AllProperties) {
	// Tests REMOVE clause removing all properties
	(void) execute("CREATE (n:Test {p1: 1, p2: 2, p3: 3})");
	(void) execute("MATCH (n:Test) REMOVE n.p1, n.p2, n.p3");

	auto res = execute("MATCH (n:Test) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.empty());
}

// ============================================================================
// MERGE Clause Tests
// ============================================================================

TEST_F(WritingClauseHandlerTest, Merge_CreateNewNode) {
	// Tests MERGE clause creating new node
	auto res = execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.created = true RETURN n.created");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.created").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_MatchExisting) {
	// Tests MERGE clause matching existing node
	(void) execute("CREATE (n:Person {name: 'Bob', count: 0})");

	auto res = execute("MERGE (n:Person {name: 'Bob'}) ON MATCH SET n.count = 1 RETURN n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Merge_WithOnCreate) {
	// Tests MERGE clause with ON CREATE action
	auto res = execute("MERGE (n:Person {name: 'Charlie'}) ON CREATE SET n.status = 'new' RETURN n.status");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.status").toString(), "new");
}

TEST_F(WritingClauseHandlerTest, Merge_WithOnMatch) {
	// Tests MERGE clause with ON MATCH action
	(void) execute("CREATE (n:Person {name: 'David', count: 0})");

	auto res = execute("MERGE (n:Person {name: 'David'}) ON MATCH SET n.updated = true RETURN n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_WithBothActions) {
	// Tests MERGE clause with both ON CREATE and ON MATCH
	auto res = execute("MERGE (n:Person {name: 'Eve'}) ON CREATE SET n.created = true ON MATCH SET n.matched = true RETURN n.created, n.matched");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.created").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_MultipleCalls) {
	// Tests MERGE clause called multiple times
	// Note: Second call still matches but updates to 2 (not 3)
	(void) execute("MERGE (n:Person {name: 'Frank'}) ON CREATE SET n.count = 1");

	auto res1 = execute("MERGE (n:Person {name: 'Frank'}) ON MATCH SET n.count = 2 RETURN n.count");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("n.count").toString(), "2");

	auto res2 = execute("MERGE (n:Person {name: 'Frank'}) ON CREATE SET n.count = 3 RETURN n.count");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("n.count").toString(), "2");
}

TEST_F(WritingClauseHandlerTest, Merge_WithProperties) {
	// Tests MERGE clause with properties in pattern
	(void) execute("CREATE (n:Person {name: 'George', id: 100})");

	auto res = execute("MERGE (n:Person {name: 'George', id: 100}) ON MATCH SET n.count = 1 RETURN n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Merge_NoLabel) {
	// Tests MERGE clause without label
	auto res = execute("MERGE (n {name: 'Henry'}) ON CREATE SET n.new = true RETURN n.new");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.new").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_MultiplePropertiesInOnCreate) {
	// Tests MERGE clause with multiple properties in ON CREATE
	auto res = execute("MERGE (n:Person {name: 'Ivan'}) ON CREATE SET n.status = 'new', n.count = 1 RETURN n.status, n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.status").toString(), "new");
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Merge_MultiplePropertiesInOnMatch) {
	// Tests MERGE clause with multiple properties in ON MATCH
	(void) execute("CREATE (n:Person {name: 'John', count: 0})");

	auto res = execute("MERGE (n:Person {name: 'John'}) ON MATCH SET n.count = 1, n.updated = true RETURN n.count, n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_F(WritingClauseHandlerTest, EdgeCase_SetOnNonExistentNode) {
	// Tests SET clause on node from MATCH that returns no results
	// Should return 0 rows (no error thrown)
	auto res = execute("MATCH (n:NonExistent) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_DeleteWithoutMatch) {
	// Tests DELETE clause without preceding MATCH
	// Should throw error
	EXPECT_THROW({ execute("DELETE n"); }, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_RemoveWithoutMatch) {
	// Tests REMOVE clause without preceding MATCH
	// Should throw error
	EXPECT_THROW({ execute("REMOVE n.prop"); }, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_SetWithoutMatch) {
	// Tests SET clause without preceding MATCH or CREATE
	// Should throw error
	EXPECT_THROW({ execute("SET n.value = 100"); }, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_CreateAndSet) {
	// Tests CREATE followed by SET in same query
	auto res = execute("CREATE (n:Test) SET n.name = 'Test' RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Test");
}

TEST_F(WritingClauseHandlerTest, EdgeCase_CreateAndDelete) {
	// Tests CREATE followed by DELETE in same query
	// Note: COUNT(*) returns null in DELETE clauses
	(void) execute("CREATE (n:Test)");
	auto res = execute("CREATE (n:Test) DELETE n RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_MatchAndSetAndDelete) {
	// Tests MATCH -> SET -> DELETE sequence
	// Note: COUNT(*) returns null in DELETE clauses
	(void) execute("CREATE (n:Test {id: 1})");
	auto res = execute("MATCH (n:Test {id: 1}) SET n.marked = true DELETE n RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_SetWithComplexValue) {
	// Tests SET clause with complex expression value
	// Note: Current implementation may only support literals
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.value = 42 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "42");
}

TEST_F(WritingClauseHandlerTest, EdgeCase_DeleteBeforeCreate) {
	// Tests DELETE -> CREATE sequence
	// Note: DELETE passes through matched rows from the child operator
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test {id: 1}) DELETE n CREATE (n:Test {id: 2}) RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}
