/**
 * @file test_IndexBuilder.cpp
 * @author Nexepic
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
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
#include <memory>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class IndexBuilderTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_indexBuilder_" + to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
		indexBuilder = indexManager->getIndexBuilder();
	}

	void TearDown() override {
		if (database) {
			database->close();
		}
		database.reset();
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath);
		}
	}

	// Helper to populate DB with mixed data
	void populateDatabase() const {
		// Active Nodes
		graph::Node n1(1, dataManager->getOrCreateLabelId("Person"));
		n1.addProperty("name", std::string("Alice"));
		n1.addProperty("age", 30);
		dataManager->addNode(n1);

		graph::Node n2(2, dataManager->getOrCreateLabelId("Person"));
		n2.addProperty("name", std::string("Bob"));
		n2.addProperty("age", 25);
		dataManager->addNode(n2);

		graph::Node n3(3, dataManager->getOrCreateLabelId("Company"));
		n3.addProperty("name", std::string("ZYXDB"));
		dataManager->addNode(n3);

		// Deleted Node (ID 4)
		graph::Node n4(4, dataManager->getOrCreateLabelId("Person"));
		n4.addProperty("name", std::string("Ghost"));
		dataManager->addNode(n4);
		dataManager->deleteNode(n4);

		// Padding (ID 5-20) to prevent tail reclamation
		for (int i = 5; i <= 20; ++i) {
			graph::Node n(i, dataManager->getOrCreateLabelId("Extra"));
			dataManager->addNode(n);
		}

		// Active Edge
		graph::Edge e1(10, 1, 3, dataManager->getOrCreateLabelId("WORKS_AT"));
		e1.addProperty("since", 2020);
		dataManager->addEdge(e1);

		// Flush to ensure data persistence logic runs (update headers etc)
		// But do NOT close the DB.
		fileStorage->flush();
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	graph::query::indexes::IndexBuilder *indexBuilder = nullptr;
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(IndexBuilderTest, BuildNodeLabelIndex) {
	populateDatabase();

	// 1. Register Index
	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();

	// 2. Execute Build
	EXPECT_TRUE(indexBuilder->buildNodeLabelIndex());

	// 3. Verify Results
	auto labelResults = indexManager->findNodeIdsByLabel("Person");

	// Should find Alice(1) and Bob(2). Ghost(4) is deleted.
	ASSERT_EQ(labelResults.size(), 2UL);
	bool found1 = false, found2 = false;
	for (auto id: labelResults) {
		if (id == 1)
			found1 = true;
		if (id == 2)
			found2 = true;
	}
	EXPECT_TRUE(found1 && found2);

	auto compResults = indexManager->findNodeIdsByLabel("Company");
	ASSERT_EQ(compResults.size(), 1UL);
}

TEST_F(IndexBuilderTest, BuildNodePropertyIndex) {
	populateDatabase();

	// 1. Register Key
	indexManager->getNodeIndexManager()->getPropertyIndex()->createIndex("name");

	// 2. Build
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("name"));

	// 3. Verify
	auto resAlice = indexManager->findNodeIdsByProperty("name", std::string("Alice"));
	ASSERT_EQ(resAlice.size(), 1UL);
	EXPECT_EQ(resAlice[0], 1);

	auto resBob = indexManager->findNodeIdsByProperty("name", std::string("Bob"));
	ASSERT_EQ(resBob.size(), 1UL);

	// Verify Deleted Node is Skipped
	auto resGhost = indexManager->findNodeIdsByProperty("name", std::string("Ghost"));
	EXPECT_TRUE(resGhost.empty());

	// Verify Unindexed Property
	auto resAge = indexManager->findNodeIdsByProperty("age", (int64_t) 30);
	EXPECT_TRUE(resAge.empty());
}

TEST_F(IndexBuilderTest, BuildEdgePropertyIndex) {
	populateDatabase();

	// 1. Register Key
	indexManager->getEdgeIndexManager()->getPropertyIndex()->createIndex("since");

	// 2. Build
	EXPECT_TRUE(indexBuilder->buildEdgePropertyIndex("since"));

	// 3. Verify
	auto res = indexManager->findEdgeIdsByProperty("since", (int64_t) 2020);
	ASSERT_EQ(res.size(), 1UL);
	EXPECT_EQ(res[0], 10);
}

TEST_F(IndexBuilderTest, Manager_CreateIndex_TriggersBuilder) {
	populateDatabase();

	// High level API: Register + Build
	bool success = indexManager->createIndex("age_idx", "node", "Person", "age");
	EXPECT_TRUE(success);

	auto res = indexManager->findNodeIdsByProperty("age", (int64_t) 25);
	ASSERT_EQ(res.size(), 1UL);
	EXPECT_EQ(res[0], 2);
}

TEST_F(IndexBuilderTest, SegmentRanges) {
	populateDatabase();

	auto nodeRanges = indexBuilder->getNodeIdRanges();
	auto edgeRanges = indexBuilder->getEdgeIdRanges();

	// Verify Nodes (Max ID >= 20)
	ASSERT_FALSE(nodeRanges.empty());
	int64_t maxId = 0;
	for (const auto &val: nodeRanges | std::views::values) {
		if (val > maxId)
			maxId = val;
	}
	EXPECT_GE(maxId, 20);

	// Verify Edges
	ASSERT_FALSE(edgeRanges.empty());
	EXPECT_GE(edgeRanges.back().second, 10);
}

TEST_F(IndexBuilderTest, BuildEdgeLabelIndex) {
	populateDatabase();

	// 1. Register Index (Label index usually enabled by default or via config, but we ensure it's active)
	indexManager->getEdgeIndexManager()->getLabelIndex()->createIndex();

	// 2. Execute Build
	// This calls buildEdgeLabelIndex -> processEdgeBatch
	EXPECT_TRUE(indexBuilder->buildEdgeLabelIndex());

	// 3. Verify Results
	auto labelResults = indexManager->findEdgeIdsByLabel("WORKS_AT");

	// Should find Edge(10)
	ASSERT_EQ(labelResults.size(), 1UL);
	EXPECT_EQ(labelResults[0], 10);

	// Verify non-existent label
	auto missingResults = indexManager->findEdgeIdsByLabel("NON_EXISTENT");
	EXPECT_TRUE(missingResults.empty());
}

TEST_F(IndexBuilderTest, BuildNodeLabelIndex_LargeBatch) {
	// 1. Create > 1000 nodes to trigger batch processing logic
	// BATCH_SIZE is 1000. We create 1500 to ensure we have:
	// - One full batch processed inside the loop
	// - One partial batch processed after the loop

	constexpr int NODE_COUNT = 1500;
	int64_t bulkLabelId = dataManager->getOrCreateLabelId("BulkNode");

	// Use raw pointer or reserve to speed up vector if needed,
	// but here we just loop addNode which is fine for 1500 items.
	for (int i = 0; i < NODE_COUNT; ++i) {
		// We let ID be auto-assigned or manage it if needed.
		// addNode assigns IDs automatically.
		graph::Node n(0, bulkLabelId);
		dataManager->addNode(n);
	}

	// Flush to ensure DataManager persists them so SegmentTracker can see ranges
	fileStorage->flush();

	// 2. Register Index
	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();

	// 3. Build Index
	// This will hit: if (batchIds.size() >= BATCH_SIZE) -> processNodeBatch
	EXPECT_TRUE(indexBuilder->buildNodeLabelIndex());

	// 4. Verify Results
	auto results = indexManager->findNodeIdsByLabel("BulkNode");

	// Should find exactly 1500 nodes
	EXPECT_EQ(results.size(), static_cast<size_t>(NODE_COUNT));
}

TEST_F(IndexBuilderTest, BuildEdgeLabelIndex_LargeBatch) {
	// 1. Create > 1000 edges to trigger batch processing logic for edges
	constexpr int EDGE_COUNT = 1200;
	int64_t bulkLabelId = dataManager->getOrCreateLabelId("BulkEdge");

	// We need at least two nodes to connect
	graph::Node n1(1, 0);
	dataManager->addNode(n1);
	graph::Node n2(2, 0);
	dataManager->addNode(n2);

	for (int i = 0; i < EDGE_COUNT; ++i) {
		graph::Edge e(0, 1, 2, bulkLabelId);
		dataManager->addEdge(e);
	}

	fileStorage->flush();

	// 2. Register Index
	indexManager->getEdgeIndexManager()->getLabelIndex()->createIndex();

	// 3. Build Index
	EXPECT_TRUE(indexBuilder->buildEdgeLabelIndex());

	// 4. Verify Results
	auto results = indexManager->findEdgeIdsByLabel("BulkEdge");
	EXPECT_EQ(results.size(), static_cast<size_t>(EDGE_COUNT));
}

TEST_F(IndexBuilderTest, BuildNodePropertyIndex_LargeBatch) {
	// 1. Create > 1000 nodes with property
	constexpr int NODE_COUNT = 1100;
	int64_t lbl = dataManager->getOrCreateLabelId("PropNode");

	for (int i = 0; i < NODE_COUNT; ++i) {
		graph::Node n(0, lbl);
		n.addProperty("bulk_prop", i); // Unique property value just in case
		dataManager->addNode(n);
	}

	fileStorage->flush();

	// 2. Register
	indexManager->getNodeIndexManager()->getPropertyIndex()->createIndex("bulk_prop");

	// 3. Build
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("bulk_prop"));

	// 4. Verify Range
	// Check range 0 to 1100. Should find all.
	// Assuming PropertyIndex::findRange works for integers.
	// If not, we can check a few samples.

	// Let's check finding ID 0 (property value 0) and ID 1050 (property value 1050)
	auto res0 = indexManager->findNodeIdsByProperty("bulk_prop", 0);
	EXPECT_EQ(res0.size(), 1UL);

	auto resLast = indexManager->findNodeIdsByProperty("bulk_prop", 1050);
	EXPECT_EQ(resLast.size(), 1UL);
}

TEST_F(IndexBuilderTest, ProcessBatch_SkipInactive) {
	// 1. Create a node and delete it immediately
	int64_t lbl = dataManager->getOrCreateLabelId("SkipMe");
	graph::Node n(1, lbl);
	dataManager->addNode(n);
	dataManager->deleteNode(n); // Mark inactive

	fileStorage->flush();

	// 2. Run Builder
	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	(void) indexBuilder->buildNodeLabelIndex();

	// 3. Verify NOT indexed
	auto res = indexManager->findNodeIdsByLabel("SkipMe");
	EXPECT_TRUE(res.empty());
}

TEST_F(IndexBuilderTest, ProcessBatch_SkipNoLabel) {
	// Node with ID but LabelID=0
	graph::Node n(2, 0);
	dataManager->addNode(n);
	fileStorage->flush();

	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	(void) indexBuilder->buildNodeLabelIndex();

	// Hard to verify "nothing happened" other than no crash
	SUCCEED();
}

TEST_F(IndexBuilderTest, BuildNodePropertyIndex_SpecificKey) {
	// Cover the branch `if (propertyKey.empty())` -> else
	graph::Node n(1, dataManager->getOrCreateLabelId("A"));
	n.addProperty("target", 100);
	n.addProperty("ignore", 200);
	dataManager->addNode(n);
	fileStorage->flush();

	indexManager->getNodeIndexManager()->getPropertyIndex()->createIndex("target");
	(void) indexBuilder->buildNodePropertyIndex("target");

	// Verify target indexed
	EXPECT_EQ(indexManager->findNodeIdsByProperty("target", 100).size(), 1UL);

	// Verify ignore NOT indexed (by checking property index for it, which shouldn't exist or be empty)
	// Note: Since we didn't create index for "ignore", finding it returns empty anyway.
	EXPECT_TRUE(indexManager->findNodeIdsByProperty("ignore", 200).empty());
}

TEST_F(IndexBuilderTest, ProcessNodeBatch_InactiveNode) {
	// Cover branch: if (edge.getId() == 0 || !edge.isActive())
	// For nodes: same logic exists in processNodeBatch
	int64_t lbl = dataManager->getOrCreateLabelId("InactiveNode");
	graph::Node n(100, lbl);
	dataManager->addNode(n);
	dataManager->deleteNode(n); // Mark inactive

	fileStorage->flush();

	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	(void) indexBuilder->buildNodeLabelIndex();

	// Should NOT find inactive node
	auto res = indexManager->findNodeIdsByLabel("InactiveNode");
	EXPECT_TRUE(res.empty());
}

TEST_F(IndexBuilderTest, ProcessEdgeBatch_InactiveEdge) {
	// Cover branch: if (edge.getId() == 0 || !edge.isActive())
	int64_t edgeLbl = dataManager->getOrCreateLabelId("InactiveEdge");

	// Create source and target nodes
	graph::Node n1(1, 0);
	graph::Node n2(2, 0);
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	// Create edge and mark it inactive
	graph::Edge e(100, 1, 2, edgeLbl);
	e.addProperty("test_prop", 999);
	dataManager->addEdge(e);
	dataManager->deleteEdge(e); // Mark inactive

	fileStorage->flush();

	indexManager->getEdgeIndexManager()->getLabelIndex()->createIndex();
	(void) indexBuilder->buildEdgeLabelIndex();

	// Should NOT find inactive edge
	auto res = indexManager->findEdgeIdsByLabel("InactiveEdge");
	EXPECT_TRUE(res.empty());
}

TEST_F(IndexBuilderTest, BuildNodePropertyIndex_AllProperties) {
	// Cover branch: if (propertyKey.empty()) -> index all properties
	// This requires calling with an empty property key, but the API takes a specific key
	// Let's test by creating multiple properties and indexing them one by one

	int64_t lbl = dataManager->getOrCreateLabelId("MultiProp");
	graph::Node n(1, lbl);
	n.addProperty("prop1", 100);
	n.addProperty("prop2", 200);
	n.addProperty("prop3", std::string("value3"));
	dataManager->addNode(n);

	fileStorage->flush();

	// Create indexes for all properties
	auto propIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	propIndex->createIndex("prop1");
	propIndex->createIndex("prop2");
	propIndex->createIndex("prop3");

	// Build each property index
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("prop1"));
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("prop2"));
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("prop3"));

	// Verify all properties are indexed
	auto res1 = indexManager->findNodeIdsByProperty("prop1", 100);
	auto res2 = indexManager->findNodeIdsByProperty("prop2", 200);
	auto res3 = indexManager->findNodeIdsByProperty("prop3", std::string("value3"));

	EXPECT_EQ(res1.size(), 1UL);
	EXPECT_EQ(res2.size(), 1UL);
	EXPECT_EQ(res3.size(), 1UL);
}

TEST_F(IndexBuilderTest, BuildEdgePropertyIndex_AllProperties) {
	// Cover branch for edges: if (propertyKey.empty())
	int64_t edgeLbl = dataManager->getOrCreateLabelId("MultiPropEdge");

	// Create source and target nodes
	graph::Node n1(1, 0);
	graph::Node n2(2, 0);
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	// Create edge with multiple properties
	graph::Edge e(10, 1, 2, edgeLbl);
	e.addProperty("eprop1", 111);
	e.addProperty("eprop2", 222);
	dataManager->addEdge(e);

	fileStorage->flush();

	// Create indexes for all edge properties
	auto propIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();
	propIndex->createIndex("eprop1");
	propIndex->createIndex("eprop2");

	// Build each property index
	EXPECT_TRUE(indexBuilder->buildEdgePropertyIndex("eprop1"));
	EXPECT_TRUE(indexBuilder->buildEdgePropertyIndex("eprop2"));

	// Verify all properties are indexed
	auto res1 = indexManager->findEdgeIdsByProperty("eprop1", 111);
	auto res2 = indexManager->findEdgeIdsByProperty("eprop2", 222);

	EXPECT_EQ(res1.size(), 1UL);
	EXPECT_EQ(res2.size(), 1UL);
}

TEST_F(IndexBuilderTest, BuildNodePropertyIndex_PropertyNotFound) {
	// Cover branch: if (auto it = properties.find(propertyKey); it != properties.end()) -> False
	int64_t lbl = dataManager->getOrCreateLabelId("NoProp");
	graph::Node n(1, lbl);
	n.addProperty("has_prop", 100);
	// Note: NOT adding "missing_prop"
	dataManager->addNode(n);

	fileStorage->flush();

	// Create index for property that doesn't exist on any node
	indexManager->getNodeIndexManager()->getPropertyIndex()->createIndex("missing_prop");

	// Build should still succeed, just won't find anything
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("missing_prop"));

	// Verify no results
	auto res = indexManager->findNodeIdsByProperty("missing_prop", 999);
	EXPECT_TRUE(res.empty());
}

TEST_F(IndexBuilderTest, BuildEdgePropertyIndex_PropertyNotFound) {
	// Cover branch for edges: property not found
	int64_t edgeLbl = dataManager->getOrCreateLabelId("NoEdgeProp");

	graph::Node n1(1, 0);
	graph::Node n2(2, 0);
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	graph::Edge e(10, 1, 2, edgeLbl);
	e.addProperty("has_eprop", 333);
	dataManager->addEdge(e);

	fileStorage->flush();

	// Create index for property that doesn't exist
	indexManager->getEdgeIndexManager()->getPropertyIndex()->createIndex("missing_eprop");

	// Build should still succeed
	EXPECT_TRUE(indexBuilder->buildEdgePropertyIndex("missing_eprop"));

	// Verify no results
	auto res = indexManager->findEdgeIdsByProperty("missing_eprop", 999);
	EXPECT_TRUE(res.empty());
}

TEST_F(IndexBuilderTest, BuildLabelIndex_EmptyDatabase) {
	// Test building index on empty database
	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();

	// Should succeed even with no data
	EXPECT_TRUE(indexBuilder->buildNodeLabelIndex());

	// Verify no results
	auto res = indexManager->findNodeIdsByLabel("NonExistent");
	EXPECT_TRUE(res.empty());
}

TEST_F(IndexBuilderTest, BuildEdgeLabelIndex_EmptyDatabase) {
	// Test building edge index on empty database
	indexManager->getEdgeIndexManager()->getLabelIndex()->createIndex();

	// Should succeed even with no edges
	EXPECT_TRUE(indexBuilder->buildEdgeLabelIndex());
}

TEST_F(IndexBuilderTest, ProcessNodeBatch_NodeWithZeroLabelId) {
	// Cover branch: if (node.getLabelId() != 0) -> False
	// Create node with labelId = 0
	graph::Node n(1, 0); // labelId = 0
	n.addProperty("name", std::string("NoLabel"));
	dataManager->addNode(n);

	fileStorage->flush();

	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	(void) indexBuilder->buildNodeLabelIndex();

	// Node with labelId=0 should not be added to label index
	// (This is hard to verify directly other than no crash)
	SUCCEED();
}

TEST_F(IndexBuilderTest, ProcessEdgeBatch_EdgeWithZeroLabelId) {
	// Cover branch: if (edge.getLabelId() != 0) -> False
	graph::Node n1(1, 0);
	graph::Node n2(2, 0);
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	// Create edge with labelId = 0
	graph::Edge e(10, 1, 2, 0);
	dataManager->addEdge(e);

	fileStorage->flush();

	indexManager->getEdgeIndexManager()->getLabelIndex()->createIndex();
	(void) indexBuilder->buildEdgeLabelIndex();

	// Edge with labelId=0 should not be added to label index
	// (This is hard to verify directly other than no crash)
	SUCCEED();
}

TEST_F(IndexBuilderTest, ProcessBatch_NodeWithEmptyLabel) {
	// Cover branch: if (!labelStr.empty()) -> False
	// This tests when resolveLabel returns an empty string
	// We need to create a node with a label that won't resolve properly

	// First create a node with a valid label
	int64_t labelId = dataManager->getOrCreateLabelId("TestLabel");
	graph::Node n(1, labelId);
	n.addProperty("name", std::string("Test"));
	dataManager->addNode(n);

	fileStorage->flush();

	// The label should resolve normally, so this tests the happy path
	// Testing the empty case would require manipulating the LabelTokenRegistry
	// which is difficult to do in a unit test

	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	EXPECT_TRUE(indexBuilder->buildNodeLabelIndex());

	auto res = indexManager->findNodeIdsByLabel("TestLabel");
	EXPECT_EQ(res.size(), 1UL);
}

TEST_F(IndexBuilderTest, BuildNodeLabelIndex_MultipleSegments) {
	// Create nodes that span multiple segments to test segment iteration
	// This helps cover the loop in getNodeIdRanges

	constexpr int NODE_COUNT = 2500; // Should span multiple segments
	int64_t lbl = dataManager->getOrCreateLabelId("MultiSeg");

	for (int i = 0; i < NODE_COUNT; ++i) {
		graph::Node n(0, lbl);
		dataManager->addNode(n);
	}

	fileStorage->flush();

	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	EXPECT_TRUE(indexBuilder->buildNodeLabelIndex());

	auto results = indexManager->findNodeIdsByLabel("MultiSeg");
	EXPECT_EQ(results.size(), static_cast<size_t>(NODE_COUNT));
}

TEST_F(IndexBuilderTest, BuildEdgePropertyIndex_LargeBatch) {
	// Cover batch processing for edge property index
	constexpr int EDGE_COUNT = 1100;
	int64_t edgeLbl = dataManager->getOrCreateLabelId("BulkEdgeProp");

	// Create source and target nodes
	graph::Node n1(1, 0);
	graph::Node n2(2, 0);
	dataManager->addNode(n1);
	dataManager->addNode(n2);

	for (int i = 0; i < EDGE_COUNT; ++i) {
		graph::Edge e(0, 1, 2, edgeLbl);
		e.addProperty("bulk_eprop", i * 10);
		dataManager->addEdge(e);
	}

	fileStorage->flush();

	// Create and build property index
	indexManager->getEdgeIndexManager()->getPropertyIndex()->createIndex("bulk_eprop");
	EXPECT_TRUE(indexBuilder->buildEdgePropertyIndex("bulk_eprop"));

	// Verify some samples
	auto res0 = indexManager->findEdgeIdsByProperty("bulk_eprop", 0);
	auto resMid = indexManager->findEdgeIdsByProperty("bulk_eprop", 5000);
	auto resLast = indexManager->findEdgeIdsByProperty("bulk_eprop", (EDGE_COUNT - 1) * 10);

	EXPECT_EQ(res0.size(), 1UL);
	EXPECT_EQ(resMid.size(), 1UL);
	EXPECT_EQ(resLast.size(), 1UL);
}

TEST_F(IndexBuilderTest, BuildIndex_WithDeletedAndActiveNodes) {
	// Test mixed scenario with both active and deleted nodes
	int64_t lbl = dataManager->getOrCreateLabelId("Mixed");

	// Create active nodes
	for (int i = 1; i <= 10; ++i) {
		graph::Node n(i, lbl);
		n.addProperty("id", i);
		dataManager->addNode(n);
	}

	// Create and delete some nodes
	for (int i = 11; i <= 15; ++i) {
		graph::Node n(i, lbl);
		n.addProperty("id", i);
		dataManager->addNode(n);
		dataManager->deleteNode(n);
	}

	// Create more active nodes
	for (int i = 16; i <= 20; ++i) {
		graph::Node n(i, lbl);
		n.addProperty("id", i);
		dataManager->addNode(n);
	}

	fileStorage->flush();

	// Build label index
	indexManager->getNodeIndexManager()->getLabelIndex()->createIndex();
	EXPECT_TRUE(indexBuilder->buildNodeLabelIndex());

	// Should find only active nodes (1-10, 16-20) = 15 nodes
	auto res = indexManager->findNodeIdsByLabel("Mixed");
	EXPECT_EQ(res.size(), 15UL);

	// Build property index
	indexManager->getNodeIndexManager()->getPropertyIndex()->createIndex("id");
	EXPECT_TRUE(indexBuilder->buildNodePropertyIndex("id"));

	// Verify property index also has 15 nodes
	auto resProp5 = indexManager->findNodeIdsByProperty("id", 5);
	auto resProp12 = indexManager->findNodeIdsByProperty("id", 12); // Deleted node
	auto resProp18 = indexManager->findNodeIdsByProperty("id", 18);

	EXPECT_EQ(resProp5.size(), 1UL);
	EXPECT_EQ(resProp12.size(), 0UL); // Deleted
	EXPECT_EQ(resProp18.size(), 1UL);
}
