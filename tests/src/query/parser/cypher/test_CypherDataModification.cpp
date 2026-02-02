/**
 * @file test_CypherDataModification.cpp
 * @author Nexepic
 * @date 2026/2/2
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

/**
 * Test suite for Cypher data modification operators:
 * - MERGE operator (create if not exists)
 * - SET operator (update properties)
 * - REMOVE operator (delete properties/labels)
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

class CypherDataModificationTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_dm_" + boost::uuids::to_string(uuid) + ".dat");
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
// 1. MERGE Operator Tests
// ============================================================================

TEST_F(CypherDataModificationTest, Merge_WithoutMatchProps) {
	// MERGE with no match properties (creates every time)
	(void) execute("MERGE (n:MergeNoMatchProps)");
	auto res = execute("MATCH (n:MergeNoMatchProps) RETURN n");
	ASSERT_GE(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_WithOnlyLabel) {
	// MERGE with only label, no properties
	(void) execute("MERGE (n:MergeLabelOnly:TestLabel)");
	auto res = execute("MATCH (n:MergeLabelOnly) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	// Second call should match
	(void) execute("MERGE (n:MergeLabelOnly:TestLabel)");
	res = execute("MATCH (n:MergeLabelOnly) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_PartialPropertyMatch) {
	// Create node with some properties
	(void) execute("CREATE (n:MergePartialMatch {key: 'k1', val: 1})");

	// MERGE with different properties - should create new node
	(void) execute("MERGE (n:MergePartialMatch {key: 'k2', val: 2})");
	auto res = execute("MATCH (n:MergePartialMatch) RETURN n");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherDataModificationTest, Merge_OnCreateOnly) {
	(void) execute("MERGE (n:MergeOnCreateOnly {id: 1}) ON CREATE SET n.created = true");
	auto res = execute("MATCH (n:MergeOnCreateOnly) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("created"));
}

TEST_F(CypherDataModificationTest, Merge_OnMatchOnly) {
	(void) execute("CREATE (n:MergeOnMatchOnly {id: 1})");
	(void) execute("MERGE (n:MergeOnMatchOnly {id: 1}) ON MATCH SET n.updated = true");
	auto res = execute("MATCH (n:MergeOnMatchOnly) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("updated"));
}

TEST_F(CypherDataModificationTest, Merge_OnCreateAndOnMatch) {
	// Test ON CREATE SET with ON MATCH SET
	(void) execute("MERGE (n:MergeBoth {id: 1}) ON CREATE SET n.created = true ON MATCH SET n.matched = true");

	// First call - should create
	auto res1 = execute("MATCH (n:MergeBoth) RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_TRUE(res1.getRows()[0].at("n").asNode().getProperties().contains("created"));

	// Second call - should match
	(void) execute("MERGE (n:MergeBoth {id: 1}) ON CREATE SET n.created = true ON MATCH SET n.matched = true");
	auto res2 = execute("MATCH (n:MergeBoth) RETURN n");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_TRUE(res2.getRows()[0].at("n").asNode().getProperties().contains("matched"));
}

TEST_F(CypherDataModificationTest, Merge_EmptyLabel) {
	// MERGE with empty label (no label specified)
	(void) execute("MERGE (n {key: 'no-label'})");
	auto res = execute("MATCH (n) WHERE n.key = 'no-label' RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_MultipleLabels) {
	// MERGE with multiple labels
	(void) execute("MERGE (n:LabelA:LabelB:LabelC {id: 1})");
	auto res = execute("MATCH (n:LabelA:LabelB:LabelC) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_UpdateExistingProperties) {
	// MERGE that matches existing node and updates it
	(void) execute("CREATE (n:MergeUpdate {id: 1, name: 'original'})");
	(void) execute("MERGE (n:MergeUpdate {id: 1}) ON MATCH SET n.name = 'updated', n.version = 2");

	auto res = execute("MATCH (n:MergeUpdate) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_EQ(props.at("name").toString(), "updated");
	EXPECT_EQ(props.at("version").toString(), "2");
}

TEST_F(CypherDataModificationTest, Merge_WithMultipleProperties) {
	// MERGE with multiple matching properties
	(void) execute("MERGE (n:MergeMultiProps {a: 1, b: 2, c: 3}) ON CREATE SET n.is_new = true");
	auto res = execute("MATCH (n:MergeMultiProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("is_new"));
}

// ============================================================================
// 2. SET Operator Tests
// ============================================================================

TEST_F(CypherDataModificationTest, Set_MultipleProperties) {
	(void) execute("CREATE (n:SetMultiProps {id: 1})");
	(void) execute("MATCH (n:SetMultiProps {id: 1}) SET n.a = 1, n.b = 2, n.c = 3");

	auto res = execute("MATCH (n:SetMultiProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("a"));
	EXPECT_TRUE(props.contains("b"));
	EXPECT_TRUE(props.contains("c"));
}

TEST_F(CypherDataModificationTest, Set_OverrideProperty) {
	(void) execute("CREATE (n:SetOverride {val: 10})");
	(void) execute("MATCH (n:SetOverride) SET n.val = 20");

	auto res = execute("MATCH (n:SetOverride) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "20");
}

TEST_F(CypherDataModificationTest, Set_NullProperty) {
	(void) execute("CREATE (n:SetNull {val: 10})");
	(void) execute("MATCH (n:SetNull) SET n.val = null");

	auto res = execute("MATCH (n:SetNull) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	// Property should be null or removed
}

TEST_F(CypherDataModificationTest, Set_EdgeProperty) {
	(void) execute("CREATE (a:SetEdge)-[r:LINK]->(b:SetEdge)");
	(void) execute("MATCH (a:SetEdge)-[r:LINK]->(b) SET r.weight = 5");

	auto res = execute("MATCH (a:SetEdge)-[r:LINK]->(b) RETURN r");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("r").asEdge().getProperties().at("weight").toString(), "5");
}

TEST_F(CypherDataModificationTest, Set_MultipleEdgeProperties) {
	(void) execute("CREATE (a)-[r:CONNECTION]->(b)");
	(void) execute("MATCH ()-[r:CONNECTION]->() SET r.weight = 10, r.active = true, r.label = 'test'");

	auto res = execute("MATCH ()-[r:CONNECTION]->() RETURN r");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("r").asEdge().getProperties();
	EXPECT_TRUE(props.contains("weight"));
	EXPECT_TRUE(props.contains("active"));
	EXPECT_TRUE(props.contains("label"));
}

TEST_F(CypherDataModificationTest, Set_DynamicPropertyValue) {
	// Note: Expression evaluation in SET may not be fully supported
	// Test with simple value assignment
	(void) execute("CREATE (n:SetDynamic {base: 5})");
	(void) execute("MATCH (n:SetDynamic) SET n.doubled = 10");

	auto res = execute("MATCH (n:SetDynamic) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("doubled").toString(), "10");
}

TEST_F(CypherDataModificationTest, Set_EmptyString) {
	(void) execute("CREATE (n:SetEmptyStr {val: 'test'})");
	(void) execute("MATCH (n:SetEmptyStr) SET n.val = ''");

	auto res = execute("MATCH (n:SetEmptyStr) RETURN n.val");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "");
}

TEST_F(CypherDataModificationTest, Set_SpecialCharacters) {
	(void) execute("CREATE (n:SetSpecial)");
	(void) execute("MATCH (n:SetSpecial) SET n.`prop-with-dash` = 1");

	auto res = execute("MATCH (n:SetSpecial) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// 3. REMOVE Operator Tests
// ============================================================================

TEST_F(CypherDataModificationTest, Remove_MultipleProperties) {
	(void) execute("CREATE (n:RemoveMultiProps {a: 1, b: 2, c: 3})");
	(void) execute("MATCH (n:RemoveMultiProps) REMOVE n.a, n.b");

	auto res = execute("MATCH (n:RemoveMultiProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_FALSE(props.contains("a"));
	EXPECT_FALSE(props.contains("b"));
	EXPECT_TRUE(props.contains("c"));
}

TEST_F(CypherDataModificationTest, Remove_NonExistentProperty) {
	(void) execute("CREATE (n:RemoveNoProp {a: 1})");
	// Removing non-existent property should not error
	(void) execute("MATCH (n:RemoveNoProp) REMOVE n.nonexistent");

	auto res = execute("MATCH (n:RemoveNoProp) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("a"));
}

TEST_F(CypherDataModificationTest, Remove_EdgeProperty) {
	(void) execute("CREATE (a:RemoveEdge)-[r:LINK {weight: 10}]->(b:RemoveEdge)");
	(void) execute("MATCH (a:RemoveEdge)-[r:LINK]->(b) REMOVE r.weight");

	auto res = execute("MATCH (a:RemoveEdge)-[r:LINK]->(b) RETURN r");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_FALSE(res.getRows()[0].at("r").asEdge().getProperties().contains("weight"));
}

TEST_F(CypherDataModificationTest, Remove_MultipleEdgeProperties) {
	(void) execute("CREATE ()-[r:MultiPropRemove {a: 1, b: 2, c: 3}]->()");
	(void) execute("MATCH ()-[r:MultiPropRemove]->() REMOVE r.a, r.b");

	auto res = execute("MATCH ()-[r:MultiPropRemove]->() RETURN r");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("r").asEdge().getProperties();
	EXPECT_FALSE(props.contains("a"));
	EXPECT_FALSE(props.contains("b"));
	EXPECT_TRUE(props.contains("c"));
}

TEST_F(CypherDataModificationTest, Remove_AllProperties) {
	(void) execute("CREATE (n:RemoveAll {a: 1, b: 2, c: 3, d: 4})");
	(void) execute("MATCH (n:RemoveAll) REMOVE n.a, n.b, n.c, n.d");

	auto res = execute("MATCH (n:RemoveAll) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_FALSE(props.contains("a"));
	EXPECT_FALSE(props.contains("b"));
	EXPECT_FALSE(props.contains("c"));
	EXPECT_FALSE(props.contains("d"));
}

// ============================================================================
// 4. Combined Operations
// ============================================================================

TEST_F(CypherDataModificationTest, SetAndRemoveInSameQuery) {
	(void) execute("CREATE (n:SetRemoveCombo {a: 1, b: 2, c: 3})");
	(void) execute("MATCH (n:SetRemoveCombo) SET n.d = 4 REMOVE n.c");

	auto res = execute("MATCH (n:SetRemoveCombo) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("d"));
	EXPECT_FALSE(props.contains("c"));
}

TEST_F(CypherDataModificationTest, MergeWithSetAndRemove) {
	(void) execute("MERGE (n:MergeSetRemove {id: 1}) SET n.val = 10 REMOVE n.id");

	auto res = execute("MATCH (n:MergeSetRemove) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("val"));
	EXPECT_FALSE(props.contains("id"));
}

TEST_F(CypherDataModificationTest, SetOnMultipleNodes) {
	// SET should affect all matching nodes
	(void) execute("CREATE (n:SetMultiNodes1 {val: 1})");
	(void) execute("CREATE (n:SetMultiNodes1 {val: 1})");
	(void) execute("CREATE (n:SetMultiNodes1 {val: 1})");

	(void) execute("MATCH (n:SetMultiNodes1) SET n.updated = true");

	auto res = execute("MATCH (n:SetMultiNodes1) RETURN n");
	ASSERT_EQ(res.rowCount(), 3UL);
	for (const auto &row : res.getRows()) {
		EXPECT_TRUE(row.at("n").asNode().getProperties().contains("updated"));
	}
}

TEST_F(CypherDataModificationTest, RemoveOnMultipleNodes) {
	// REMOVE should affect all matching nodes
	(void) execute("CREATE (n:RemoveMultiNodes {temp: 'data'})");
	(void) execute("CREATE (n:RemoveMultiNodes {temp: 'data'})");

	(void) execute("MATCH (n:RemoveMultiNodes) REMOVE n.temp");

	auto res = execute("MATCH (n:RemoveMultiNodes) RETURN n");
	ASSERT_EQ(res.rowCount(), 2UL);
	for (const auto &row : res.getRows()) {
		EXPECT_FALSE(row.at("n").asNode().getProperties().contains("temp"));
	}
}

TEST_F(CypherDataModificationTest, Merge_CreateMultipleNodes) {
	// Creating multiple nodes with MERGE
	(void) execute("MERGE (n:MergeMultiCreate {id: 1})");
	(void) execute("MERGE (n:MergeMultiCreate {id: 2})");
	(void) execute("MERGE (n:MergeMultiCreate {id: 3})");

	auto res = execute("MATCH (n:MergeMultiCreate) RETURN n.id ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 3UL);

	// Running MERGE again should match existing nodes
	(void) execute("MERGE (n:MergeMultiCreate {id: 1})");
	(void) execute("MERGE (n:MergeMultiCreate {id: 2})");
	(void) execute("MERGE (n:MergeMultiCreate {id: 3})");

	res = execute("MATCH (n:MergeMultiCreate) RETURN n.id");
	EXPECT_EQ(res.rowCount(), 3UL);
}

TEST_F(CypherDataModificationTest, Set_WithConditionalExpression) {
	// Note: CASE WHEN expression not supported
	// Test with simple SET based on filter
	(void) execute("CREATE (n:SetCond {active: true})");
	(void) execute("MATCH (n:SetCond) WHERE n.active = true SET n.status = 'active'");

	auto res = execute("MATCH (n:SetCond) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("status").toString(), "active");
}

// Additional MERGE tests for improved coverage
TEST_F(CypherDataModificationTest, Merge_WithPropertyIndex) {
	// Create property index first
	(void) execute("CREATE INDEX ON :MergePropIdx(name)");

	// Create a node with specific property
	(void) execute("CREATE (n:MergePropIdx {name: 'Alice'})");

	// MERGE with different property - should create new node
	(void) execute("MERGE (n:MergePropIdx {name: 'Bob'}) ON CREATE SET n.isNew = true");
	auto res = execute("MATCH (n:MergePropIdx) RETURN n");
	ASSERT_EQ(res.rowCount(), 2UL);

	// The new node should have isNew property
	bool foundNewNode = false;
	for (const auto &row : res.getRows()) {
		auto props = row.at("n").asNode().getProperties();
		if (props.contains("isNew") && props.at("name").toString() == "Bob") {
			foundNewNode = true;
		}
	}
	EXPECT_TRUE(foundNewNode);
}

TEST_F(CypherDataModificationTest, Merge_WithMultipleMatchingProperties) {
	// Create node with multiple properties
	(void) execute("CREATE (n:MergeMultiProp {a: 1, b: 2, c: 3})");

	// MERGE with subset of properties - should match
	(void) execute("MERGE (n:MergeMultiProp {a: 1, b: 2}) ON MATCH SET n.found = true");
	auto res = execute("MATCH (n:MergeMultiProp) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("n").asNode().getProperties().contains("found"));
}

TEST_F(CypherDataModificationTest, Merge_CreatesNodeWhenNoMatch) {
	// MERGE with properties that don't match any existing node
	(void) execute("MERGE (n:MergeCreateNew {uniqueId: 9999})");

	auto res = execute("MATCH (n:MergeCreateNew) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("uniqueId").toString(), "9999");
}

TEST_F(CypherDataModificationTest, Merge_OnCreateSetMultipleProperties) {
	// MERGE with ON CREATE setting multiple properties
	(void) execute("MERGE (n:MergeMultiCreateProps {id: 1}) ON CREATE SET n.created = true, n.count = 1");

	auto res = execute("MATCH (n:MergeMultiCreateProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	auto props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("created"));
	EXPECT_TRUE(props.contains("count"));
}

TEST_F(CypherDataModificationTest, Merge_OnMatchSetMultipleProperties) {
	// Create existing node
	(void) execute("CREATE (n:MergeMultiMatchProps {id: 1, name: 'original'})");

	// MERGE with ON MATCH setting multiple properties
	(void) execute("MERGE (n:MergeMultiMatchProps {id: 1}) ON MATCH SET n.name = 'updated', n.version = 2");

	auto res = execute("MATCH (n:MergeMultiMatchProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	auto props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_EQ(props.at("name").toString(), "updated");
	EXPECT_EQ(props.at("version").toString(), "2");
}

TEST_F(CypherDataModificationTest, Merge_WithEmptyProperties) {
	// MERGE with empty properties map (only label)
	(void) execute("MERGE (n:MergeEmptyProps:TestLabel)");

	auto res = execute("MATCH (n:MergeEmptyProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	// Second MERGE with same label and empty props should match
	(void) execute("MERGE (n:MergeEmptyProps:TestLabel)");
	res = execute("MATCH (n:MergeEmptyProps) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_CreatesWithComplexProperties) {
	// MERGE with complex property values
	(void) execute("MERGE (n:MergeComplex {id: 100, active: true, score: 95.5})");

	auto res = execute("MATCH (n:MergeComplex) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	auto props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("id"));
	EXPECT_TRUE(props.contains("active"));
	EXPECT_TRUE(props.contains("score"));
}

// ============================================================================
// Additional MERGE Coverage Tests
// ============================================================================

TEST_F(CypherDataModificationTest, Merge_WithPrecedingMatch) {
	// Test MERGE when node is already in record from MATCH
	(void) execute("CREATE (n:MergeWithMatch {id: 1, name: 'Alice'})");

	// This tests the early return at line 114 when record already has the node
	auto res = execute("MATCH (n:MergeWithMatch {id: 1}) MERGE (n:MergeWithMatch {id: 1}) ON MATCH SET n.touched = true");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_WithDeletedNode) {
	// Test MERGE behavior when there are deleted nodes
	(void) execute("CREATE (n:MergeDeleted {id: 1})");
	(void) execute("CREATE (n:MergeDeleted {id: 2})");

	// Delete one node
	(void) execute("MATCH (n:MergeDeleted {id: 1}) DELETE n");

	// MERGE should only find the active node (id: 2)
	auto res = execute("MERGE (n:MergeDeleted {id: 2}) ON MATCH SET n.found = true RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_WithLabelIndex) {
	// Create nodes with the label
	(void) execute("CREATE (n:MergeLabelIdx {id: 1})");
	(void) execute("CREATE (n:MergeLabelIdx {id: 2})");

	// MERGE should use label index lookup if available
	auto res = execute("MERGE (n:MergeLabelIdx {id: 2}) ON MATCH SET n.usingLabelIdx = true RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_NoOnCreateOrOnMatch) {
	// Test MERGE without ON CREATE or ON MATCH (empty items)
	// This tests line 193: if (items.empty()) return;
	(void) execute("MERGE (n:MergeNoClauses {id: 999})");
	auto res = execute("MATCH (n:MergeNoClauses {id: 999}) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);

	// Running again should match without any updates
	res = execute("MERGE (n:MergeNoClauses {id: 999})");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_FullTableScan) {
	// Test MERGE without any indexes (full table scan path at line 135-138)
	// Don't create any indexes, force full scan
	(void) execute("CREATE (n:MergeFullScan1 {id: 1, name: 'Alice'})");
	(void) execute("CREATE (n:MergeFullScan2 {id: 2, name: 'Bob'})");
	(void) execute("CREATE (n:MergeFullScan3 {id: 3, name: 'Charlie'})");

	// MERGE should scan through all nodes to find match
	auto res = execute("MERGE (n:MergeFullScan2 {id: 2}) ON MATCH SET n.scanned = true RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(CypherDataModificationTest, Merge_PropertyMismatch) {
	// Test MERGE when properties don't match (line 156-159)
	(void) execute("CREATE (n:MergePropMismatch {id: 1, type: 'A'})");
	(void) execute("CREATE (n:MergePropMismatch {id: 2, type: 'B'})");
	(void) execute("CREATE (n:MergePropMismatch {id: 3, type: 'C'})");

	// MERGE with different type property should create new node
	auto res = execute("MERGE (n:MergePropMismatch {id: 4, type: 'D'}) ON CREATE SET n.isNew = true RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}
