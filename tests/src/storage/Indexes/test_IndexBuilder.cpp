/**
 * @file test_IndexBuilder.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
		graph::Node n1(1, "Person");
		n1.addProperty("name", std::string("Alice"));
		n1.addProperty("age", (int64_t) 30);
		dataManager->addNode(n1);

		graph::Node n2(2, "Person");
		n2.addProperty("name", std::string("Bob"));
		n2.addProperty("age", (int64_t) 25);
		dataManager->addNode(n2);

		graph::Node n3(3, "Company");
		n3.addProperty("name", std::string("MetrixDB"));
		dataManager->addNode(n3);

		// Deleted Node (ID 4)
		graph::Node n4(4, "Person");
		n4.addProperty("name", std::string("Ghost"));
		dataManager->addNode(n4);
		dataManager->deleteNode(n4);

		// Padding (ID 5-20) to prevent tail reclamation
		for (int i = 5; i <= 20; ++i) {
			graph::Node n(i, "Extra");
			dataManager->addNode(n);
		}

		// Active Edge
		graph::Edge e1(10, 1, 3, "WORKS_AT");
		e1.addProperty("since", (int64_t) 2020);
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
