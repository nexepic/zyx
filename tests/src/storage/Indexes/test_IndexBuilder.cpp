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
		n3.addProperty("name", std::string("MetrixDB"));
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
