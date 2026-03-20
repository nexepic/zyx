/**
 * @file test_WritingClauseHandler.cpp
 * @brief Consolidated tests for WritingClauseHandler
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

#include "QueryTestFixture.hpp"

class WritingClauseHandlerTest : public QueryTestFixture {};

// === Create Tests ===

TEST_F(WritingClauseHandlerTest, Create_CompletePattern) {
	// Tests CREATE clause with nodes and edges
	// Note: Only the first variable is available in RETURN clause (implementation limitation)
	auto res = execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'}) RETURN a.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("a.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Create_ComplexPattern) {
	// Tests CREATE with complex pattern
	auto res = execute("CREATE (a:Start)-[:LINK1]->(b)-[:LINK2]->(c:End) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Create_EdgeWithProperties) {
	// Tests CREATE clause with edge properties
	auto res = execute("CREATE (a)-[r:CONNECTED {weight: 1.5}]->(b) RETURN r.weight");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("r.weight").toString(), "1.5");
}

TEST_F(WritingClauseHandlerTest, Create_MultipleNodes) {
	// Tests CREATE clause with multiple nodes
	// Note: Only the first variable is available in RETURN clause (implementation limitation)
	auto res = execute("CREATE (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) RETURN a.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("a.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Create_MultipleNodesInSinglePattern) {
	// Tests CREATE with multiple nodes connected by edges
	auto res = execute("CREATE (a:Test)-[:LINK]->(b:Test)-[:LINK]->(c:Test) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Create_MultipleRelationships) {
	// Tests CREATE with multiple relationships from same node
	auto res = execute("CREATE (a)-[:L1]->(b), (a)-[:L2]->(c) RETURN a");
	ASSERT_EQ(res.rowCount(), 1UL);
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

TEST_F(WritingClauseHandlerTest, Create_NodeWithEdge) {
	// Tests CREATE clause with edge
	auto res = execute("CREATE (a)-[r:LINK]->(b) RETURN a, r, b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("a").isNode());
	EXPECT_TRUE(res.getRows()[0].at("r").isEdge());
	EXPECT_TRUE(res.getRows()[0].at("b").isNode());
}

TEST_F(WritingClauseHandlerTest, Create_NodeWithProperties) {
	// Tests CREATE clause with node properties
	auto res = execute("CREATE (n:Test {id: 1, value: 100, active: true}) RETURN n.id, n.value, n.active");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
	EXPECT_EQ(res.getRows()[0].at("n.active").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Create_SingleNode) {
	// Tests CREATE clause with single node
	auto res = execute("CREATE (n:Person {name: 'Alice'}) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Alice");
}

TEST_F(WritingClauseHandlerTest, Create_WithDuplicateLabels) {
	// Tests CREATE with duplicate labels (same label specified multiple times)
	// This might be handled as single label or error
	auto res = execute("CREATE (n:Test:Test) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Create_WithNullProperties) {
	// Tests CREATE with null property values
	auto res = execute("CREATE (n:Test {value: null}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(WritingClauseHandlerTest, Create_WithRelationship) {
	// Tests CREATE with relationship
	auto res = execute("CREATE (a)-[:LINK]->(b) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// === Delete Tests ===

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

TEST_F(WritingClauseHandlerTest, Delete_DetachMultiple) {
	// Tests DETACH DELETE with multiple relationships
	(void) execute("CREATE (a)-[:L1]->(b)-[:L2]->(c)");
	(void) execute("CREATE (a)-[:L3]->(d)");

	auto res = execute("MATCH (a) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_DetachMultipleNodes) {
	// Tests DETACH DELETE with multiple targets
	(void) execute("CREATE (a)-[:L1]->(b)");
	(void) execute("CREATE (a)-[:L2]->(c)");

	auto res = execute("MATCH (a) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_DetachSingleNode) {
	// Tests DETACH DELETE
	(void) execute("CREATE (a)-[:LINK]->(b)");

	auto res = execute("MATCH (a) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_DetachVsNonDetach) {
	// Tests difference between DELETE and DETACH DELETE
	// First test non-detach (should fail if relationships exist)
	(void) execute("CREATE (a:Test)-[:LINK]->(b:Test)");

	EXPECT_THROW({
		execute("MATCH (a:Test) DELETE a");
	}, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, Delete_DetachWithRelationships) {
	// Tests DETACH DELETE with relationships
	(void) execute("CREATE (a:Test)-[:LINK]->(b:Test)");

	auto res = execute("MATCH (a:Test) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);

	// Verify nodes are deleted - count(*) may return 0 rows if no nodes exist
	auto res2 = execute("MATCH (n:Test) RETURN n");
	EXPECT_EQ(res2.rowCount(), 0UL);
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

TEST_F(WritingClauseHandlerTest, Delete_MultipleVariables) {
	// Covers line 70 with multiple expressions (loop)
	(void) execute("CREATE (a:Test {id: 1})");
	(void) execute("CREATE (b:Test {id: 2})");

	auto res = execute("MATCH (a:Test {id: 1}), (b:Test {id: 2}) DELETE a, b");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_NoDetachMultiple) {
	// Tests DELETE without DETACH, multiple nodes
	(void) execute("CREATE (a:Test {id: 1})");
	(void) execute("CREATE (b:Test {id: 2})");

	auto res = execute("MATCH (a:Test {id: 1}), (b:Test {id: 2}) DELETE a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

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

TEST_F(WritingClauseHandlerTest, Delete_SingleVariable) {
	// Covers line 70 with single expression
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) DELETE n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_WithDetach) {
	// Covers True branch at line 67 (K_DETACH != nullptr)
	(void) execute("CREATE (a:Test)-[:LINK]->(b:Test)");

	auto res = execute("MATCH (a:Test) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_WithoutDetach) {
	// Covers False branch at line 67
	(void) execute("CREATE (a:Test)");

	auto res = execute("MATCH (a:Test) DELETE a");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Delete_WithoutPrecedingMatch) {
	// Tests DELETE without preceding MATCH clause
	// This should throw an error
	EXPECT_THROW({
		execute("DELETE n");
	}, std::runtime_error);
}

// === DetachDelete Tests ===

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

// === EdgeCase Tests ===

TEST_F(WritingClauseHandlerTest, EdgeCase_CreateAndDelete) {
	// Tests CREATE followed by DELETE in same query
	// Note: COUNT(*) returns null in DELETE clauses
	(void) execute("CREATE (n:Test)");
	auto res = execute("CREATE (n:Test) DELETE n RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_CreateAndSet) {
	// Tests CREATE followed by SET in same query
	auto res = execute("CREATE (n:Test) SET n.name = 'Test' RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Test");
}

TEST_F(WritingClauseHandlerTest, EdgeCase_DeleteBeforeCreate) {
	// Tests DELETE -> CREATE sequence
	// Note: DELETE passes through matched rows from the child operator
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test {id: 1}) DELETE n CREATE (n:Test {id: 2}) RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_DeleteWithoutMatch) {
	// Tests DELETE clause without preceding MATCH
	// Should throw error
	EXPECT_THROW({ execute("DELETE n"); }, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_MatchAndSetAndDelete) {
	// Tests MATCH -> SET -> DELETE sequence
	// Note: COUNT(*) returns null in DELETE clauses
	(void) execute("CREATE (n:Test {id: 1})");
	auto res = execute("MATCH (n:Test {id: 1}) SET n.marked = true DELETE n RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_RemoveWithoutMatch) {
	// Tests REMOVE clause without preceding MATCH
	// Should throw error
	EXPECT_THROW({ execute("REMOVE n.prop"); }, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_SetOnNonExistentNode) {
	// Tests SET clause on node from MATCH that returns no results
	// Should return 0 rows (no error thrown)
	auto res = execute("MATCH (n:NonExistent) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(WritingClauseHandlerTest, EdgeCase_SetWithComplexValue) {
	// Tests SET clause with complex expression value
	// Note: Current implementation may only support literals
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.value = 42 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "42");
}

TEST_F(WritingClauseHandlerTest, EdgeCase_SetWithoutMatch) {
	// Tests SET clause without preceding MATCH or CREATE
	// Should throw error
	EXPECT_THROW({ execute("SET n.value = 100"); }, std::runtime_error);
}

// === Merge Tests ===

TEST_F(WritingClauseHandlerTest, Merge_BothOnCreateAndOnMatch) {
	// Tests MERGE with both ON CREATE and ON MATCH
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// Existing node - ON MATCH triggers
	auto res1 = execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.new = true ON MATCH SET n.existing = true RETURN n.new, n.existing");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("n.existing").toString(), "true");

	// New node - ON CREATE triggers
	auto res2 = execute("MERGE (n:Person {name: 'Bob'}) ON CREATE SET n.new = true ON MATCH SET n.existing = true RETURN n.new, n.existing");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("n.new").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_BothOnMatchAndOnCreate) {
	// Tests both ON MATCH and ON CREATE
	(void) execute("CREATE (n:Person {name: 'Dave'})");

	// Existing node - ON MATCH triggers
	auto res1 = execute("MERGE (n:Person {name: 'Dave'}) ON MATCH SET n.matched = true ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// New node - ON CREATE triggers
	auto res2 = execute("MERGE (n:Person {name: 'Eve'}) ON MATCH SET n.matched = true ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Merge_CreateNewNode) {
	// Tests MERGE clause creating new node
	auto res = execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.created = true RETURN n.created");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.created").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_CreateWhenNotExists) {
	// Tests MERGE creates new node when not exists
	auto res = execute("MERGE (n:NewNode {name: 'test'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Merge_MatchExisting) {
	// Tests MERGE clause matching existing node
	(void) execute("CREATE (n:Person {name: 'Bob', count: 0})");

	auto res = execute("MERGE (n:Person {name: 'Bob'}) ON MATCH SET n.count = 1 RETURN n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Merge_MatchExistingNode) {
	// Tests MERGE matching existing node
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MERGE (n:Person {name: 'Bob'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Merge_MatchWhenExists) {
	// Tests MERGE matches existing node
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MERGE (n:Person {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
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

TEST_F(WritingClauseHandlerTest, Merge_MultipleOnClauses) {
	// Tests MERGE with multiple ON clauses
	(void) execute("CREATE (n:Person {name: 'George'})");

	auto res = execute("MERGE (n:Person {name: 'George'}) ON MATCH SET n.v1 = 1 ON MATCH SET n.v2 = 2 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
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

TEST_F(WritingClauseHandlerTest, Merge_MultipleSetItemsInOnCreate) {
	// Tests MERGE with multiple SET items in ON CREATE
	auto res = execute("MERGE (n:Person {name: 'Charlie'}) ON CREATE SET n.created = true, n.timestamp = 123 RETURN n.created, n.timestamp");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.created").toString(), "true");
	EXPECT_EQ(res.getRows()[0].at("n.timestamp").toString(), "123");
}

TEST_F(WritingClauseHandlerTest, Merge_MultipleSetItemsInOnMatch) {
	// Tests MERGE with multiple SET items in ON MATCH
	(void) execute("CREATE (n:Person {name: 'David'})");

	auto res = execute("MERGE (n:Person {name: 'David'}) ON MATCH SET n.updated = true, n.count = 1 RETURN n.updated, n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Merge_NoLabel) {
	// Tests MERGE clause without label
	auto res = execute("MERGE (n {name: 'Henry'}) ON CREATE SET n.new = true RETURN n.new");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.new").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_OnCreateOnly) {
	// Tests MERGE with ON CREATE only (Line 133 True branch)
	(void) execute("CREATE (n:Person {name: 'Charlie'})");

	auto res = execute("MERGE (n:Person {name: 'Charlie'}) ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Merge_OnMatchOnly) {
	// Tests MERGE with ON MATCH only (Line 135 True branch)
	(void) execute("CREATE (n:Person {name: 'David'})");

	auto res = execute("MERGE (n:Person {name: 'David'}) ON MATCH SET n.found = true RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Merge_OnlyOnCreate) {
	// Tests MERGE with only ON CREATE clause
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// First run - node exists, ON CREATE not triggered
	auto res1 = execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.created = true RETURN n.created");
	EXPECT_EQ(res1.rowCount(), 1UL);

	// Second run - new node, ON CREATE triggered
	auto res2 = execute("MERGE (n:Person {name: 'Bob'}) ON CREATE SET n.created = true RETURN n.created");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("n.created").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Merge_OnlyOnMatch) {
	// Tests MERGE with only ON MATCH clause
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// Node exists, ON MATCH should trigger
	auto res = execute("MERGE (n:Person {name: 'Alice'}) ON MATCH SET n.updated = true RETURN n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");

	// New node, ON MATCH not triggered
	auto res2 = execute("MERGE (n:Person {name: 'Bob'}) ON MATCH SET n.updated = true RETURN n.updated");
	EXPECT_EQ(res2.rowCount(), 1UL);
	// n.updated should not exist or be null
}

TEST_F(WritingClauseHandlerTest, Merge_WithBothActions) {
	// Tests MERGE clause with both ON CREATE and ON MATCH
	auto res = execute("MERGE (n:Person {name: 'Eve'}) ON CREATE SET n.created = true ON MATCH SET n.matched = true RETURN n.created, n.matched");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.created").toString(), "true");
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

TEST_F(WritingClauseHandlerTest, Merge_WithProperties) {
	// Tests MERGE clause with properties in pattern
	(void) execute("CREATE (n:Person {name: 'George', id: 100})");

	auto res = execute("MERGE (n:Person {name: 'George', id: 100}) ON MATCH SET n.count = 1 RETURN n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Merge_WithRelationship) {
	// Tests MERGE with relationship pattern
	(void) execute("CREATE (a:Person {name: 'Alice'})");

	auto res = execute("MERGE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'}) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Merge_WithoutOnClauses) {
	// Covers False branches in loop (no ON MATCH/CREATE)
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// MERGE with existing node (no ON clauses)
	auto res = execute("MERGE (n:Person {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// === Other Tests ===

TEST_F(WritingClauseHandlerTest, CreateThenRemove) {
	// Tests CREATE followed by REMOVE
	(void) execute("CREATE (n:Test:Temp {value: 100})");
	auto res = execute("MATCH (n:Test) REMOVE n:Temp RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, CreateThenSet) {
	// Tests CREATE followed by SET
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerTest, CreateThenSetThenReturn) {
	// Tests CREATE followed by SET
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerTest, MatchThenDelete) {
	// Tests MATCH followed by DELETE
	(void) execute("CREATE (n:Test {id: 1})");
	auto res = execute("MATCH (n:Test) DELETE n");
	EXPECT_EQ(res.rowCount(), 1UL);

	// Verify deletion - check directly without count(*)
	auto res2 = execute("MATCH (n:Test) RETURN n");
	EXPECT_EQ(res2.rowCount(), 0UL);
}

TEST_F(WritingClauseHandlerTest, MatchThenRemoveThenReturn) {
	// Tests MATCH followed by REMOVE
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, MatchThenSetThenReturn) {
	// Tests MATCH followed by SET
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) SET n.updated = true RETURN n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, MergeThenSet) {
	// Tests MERGE followed by SET
	(void) execute("CREATE (n:Person {name: 'Eve'})");
	auto res = execute("MERGE (n:Person {name: 'Eve'}) SET n.age = 30 RETURN n.age");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.age").toString(), "30");
}

// === Remove Tests ===

TEST_F(WritingClauseHandlerTest, Remove_AllProperties) {
	// Tests REMOVE clause removing all properties
	(void) execute("CREATE (n:Test {p1: 1, p2: 2, p3: 3})");
	(void) execute("MATCH (n:Test) REMOVE n.p1, n.p2, n.p3");

	auto res = execute("MATCH (n:Test) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.empty());
}

TEST_F(WritingClauseHandlerTest, Remove_AllPropertiesFromNode) {
	// Tests removing all properties from node
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) REMOVE n.a, n.b, n.c RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
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

TEST_F(WritingClauseHandlerTest, Remove_LabelFromNonExistentLabel) {
	// Tests REMOVE label that node doesn't have
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) REMOVE n:NonExistent RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Remove_MultipleLabels) {
	// Tests REMOVE multiple labels
	(void) execute("CREATE (n:Test:Label1:Label2:Label3)");

	auto res = execute("MATCH (n:Test) REMOVE n:Label1, n:Label2, n:Label3 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Remove_MultipleProperties) {
	// Tests REMOVE clause removing multiple properties
	(void) execute("CREATE (n:Test {keep: 1, remove1: 2, remove2: 3, keep2: 4})");
	auto res = execute("MATCH (n:Test) REMOVE n.remove1, n.remove2 RETURN n.keep, n.keep2");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.keep").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.keep2").toString(), "4");
}

TEST_F(WritingClauseHandlerTest, Remove_NonExistentProperty) {
	// Tests REMOVE clause on non-existent property (should handle gracefully)
	(void) execute("CREATE (n:Test {keep: 1})");
	auto res = execute("MATCH (n:Test) REMOVE n.nonexistent RETURN n.keep");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.keep").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Remove_Property) {
	// Tests REMOVE clause removing property
	(void) execute("CREATE (n:TestRemove {keep: 1, remove: 2})");
	auto res = execute("MATCH (n:TestRemove) REMOVE n.remove RETURN n.keep");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.keep").toString(), "1");
}

TEST_F(WritingClauseHandlerTest, Remove_PropertyAndLabel) {
	// Tests both property and label removal in same query
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n.value, n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Remove_PropertyFromNonExistentProperty) {
	// Tests REMOVE property that doesn't exist
	(void) execute("CREATE (n:Test {id: 1})");

	// Removing non-existent property should not error
	auto res = execute("MATCH (n:Test) REMOVE n.nonexistent RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerTest, Remove_WithoutPrecedingClause) {
	// Tests REMOVE without preceding MATCH or CREATE
	// This should throw an error
	EXPECT_THROW({
		execute("REMOVE n:Label");
	}, std::runtime_error);
}

// === Set Tests ===

TEST_F(WritingClauseHandlerTest, Set_AddNewProperty) {
	// Tests SET adding new property to node
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.newProp = 'value' RETURN n.newProp");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.newProp").toString(), "value");
}

TEST_F(WritingClauseHandlerTest, Set_AfterCreate) {
	// Tests SET after CREATE
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerTest, Set_AfterMatch) {
	// Covers False branch at line 52 (rootOp exists)
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) SET n.value = 200 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

TEST_F(WritingClauseHandlerTest, Set_DifferentTypes) {
	// Tests SET with different value types
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.str = 'test', n.num = 42, n.bool = true RETURN n.str, n.num, n.bool");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.str").toString(), "test");
	EXPECT_EQ(res.getRows()[0].at("n.num").toString(), "42");
	EXPECT_EQ(res.getRows()[0].at("n.bool").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Set_MultipleChainedSets) {
	// Tests multiple SET clauses in sequence
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) SET n.value = 10 SET n.value = 20 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
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

TEST_F(WritingClauseHandlerTest, Set_MultipleProperties) {
	// Tests SET with multiple properties
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.a = 1, n.b = 2, n.c = 3 RETURN n.a, n.b, n.c");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("n.c").toString(), "3");
}

TEST_F(WritingClauseHandlerTest, Set_MultiplePropertiesAtOnce) {
	// Tests SET with multiple properties
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.a = 1, n.b = 2, n.c = 3 RETURN n.a, n.b, n.c");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("n.c").toString(), "3");
}

TEST_F(WritingClauseHandlerTest, Set_OverwriteExistingProperty) {
	// Tests SET overwriting existing property
	(void) execute("CREATE (n:Test {value: 10})");

	auto res = execute("MATCH (n:Test) SET n.value = 20 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
}

TEST_F(WritingClauseHandlerTest, Set_OverwriteProperty) {
	// Tests overwriting existing property
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) SET n.value = 200 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

TEST_F(WritingClauseHandlerTest, Set_SingleProperty) {
	// Tests SET with single property
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerTest, Set_UpdateExistingProperty) {
	// Tests SET clause updating existing property
	(void) execute("CREATE (n:Test {value: 100})");
	auto res = execute("MATCH (n:Test) SET n.value = 200 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

TEST_F(WritingClauseHandlerTest, Set_WithBooleanValue) {
	// Tests SET clause with boolean value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.active = true RETURN n.active");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.active").toString(), "true");
}

TEST_F(WritingClauseHandlerTest, Set_WithDoubleLiteral) {
	// Tests SET with double literal
	(void) execute("CREATE (n:Test {a: 10})");

	auto res = execute("MATCH (n:Test) SET n.result = 30.5 RETURN n.result");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.result").toString(), "30.5");
}

TEST_F(WritingClauseHandlerTest, Set_WithDoubleValue) {
	// Tests SET clause with double value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.score = 95.5 RETURN n.score");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.score").toString(), "95.5");
}

TEST_F(WritingClauseHandlerTest, Set_WithIntegerLiteral) {
	// Tests SET with integer literal
	(void) execute("CREATE (n:Test {a: 10})");

	auto res = execute("MATCH (n:Test) SET n.sum = 15 RETURN n.sum");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.sum").toString(), "15");
}

TEST_F(WritingClauseHandlerTest, Set_WithIntegerValue) {
	// Tests SET clause with integer value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.count = 123 RETURN n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "123");
}

TEST_F(WritingClauseHandlerTest, Set_WithLiteralValues) {
	// Tests SET with literal values (not expressions)
	(void) execute("CREATE (n:Test {a: true})");

	auto res = execute("MATCH (n:Test) SET n.result = false RETURN n.result");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.result").toString(), "false");
}

TEST_F(WritingClauseHandlerTest, Set_WithNegativeValue) {
	// Tests SET clause with negative value
	(void) execute("CREATE (n:TestNeg)");
	auto res = execute("MATCH (n:TestNeg) SET n.value = -42 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "-42");
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

TEST_F(WritingClauseHandlerTest, Set_WithStringValue) {
	// Tests SET clause with string value
	(void) execute("CREATE (n:Test)");
	auto res = execute("MATCH (n:Test) SET n.name = 'Charlie' RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Charlie");
}

TEST_F(WritingClauseHandlerTest, Set_WithoutMatchOrCreate) {
	// Covers True branch at line 52 (!rootOp check)
	// This should throw an error
	EXPECT_THROW({
		execute("SET n.value = 100");
	}, std::runtime_error);
}

TEST_F(WritingClauseHandlerTest, Set_WithoutPrecedingClause) {
	// Tests SET clause without preceding MATCH or CREATE
	// This should throw an error
	EXPECT_THROW({
		execute("SET n.value = 100");
	}, std::runtime_error);
}

