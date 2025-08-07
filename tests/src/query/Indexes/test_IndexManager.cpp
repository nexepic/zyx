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
#include "graph/core/Database.hpp"
#include "graph/query/indexes/IndexManager.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class IndexManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_indexManager_" + to_string(uuid) + ".dat");
		// Use the full Database setup as it correctly initializes the full chain:
		// Database -> QueryEngine -> IndexManager -> EntityTypeIndexManagers
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		// Get the IndexManager instance from the database's query engine.
		indexManager = database->getQueryEngine()->getIndexManager();

		// Add a node with properties to be used in tests.
		graph::Node node1(1, "Person");
		dataManager->addNode(node1);
		dataManager->addNodeProperties(1, {{"name", std::string("Alice")}, {"age", static_cast<int64_t>(42)}});
		fileStorage->flush();
	}

	void TearDown() override {
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	static bool hasIndex(const std::vector<std::pair<std::string, std::string>> &indexes, const std::string &type,
						 const std::string &key = "") {
		return std::any_of(indexes.begin(), indexes.end(),
						   [&](const auto &pair) { return pair.first == type && pair.second == key; });
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::storage::DataManager> dataManager;
};

// Test building all node indexes (label and property) and then dropping them individually.
TEST_F(IndexManagerTest, BuildAndDropNodeIndexes) {
	// Act: Build all indexes for nodes. This creates both label and property indexes.
	EXPECT_TRUE(indexManager->buildIndexes("node"));

	// Assert: Check that both label and property indexes for "name" and "age" were created.
	auto nodeIndexes = indexManager->listIndexes("node");
	ASSERT_EQ(nodeIndexes.size(), 3); // label + name + age
	EXPECT_TRUE(hasIndex(nodeIndexes, "label"));
	EXPECT_TRUE(hasIndex(nodeIndexes, "property", "name"));
	EXPECT_TRUE(hasIndex(nodeIndexes, "property", "age"));

	// Act & Assert: Drop the label index and verify it's gone.
	EXPECT_TRUE(indexManager->dropIndex("node", "label"));
	nodeIndexes = indexManager->listIndexes("node");
	ASSERT_EQ(nodeIndexes.size(), 2);
	EXPECT_FALSE(hasIndex(nodeIndexes, "label"));

	// Act & Assert: Drop one property index and verify.
	EXPECT_TRUE(indexManager->dropIndex("node", "property", "name"));
	nodeIndexes = indexManager->listIndexes("node");
	ASSERT_EQ(nodeIndexes.size(), 1);
	EXPECT_FALSE(hasIndex(nodeIndexes, "property", "name"));
	EXPECT_TRUE(hasIndex(nodeIndexes, "property", "age"));

	// Act & Assert: Drop all remaining property indexes and verify everything is empty.
	EXPECT_TRUE(indexManager->dropIndex("node", "property")); // Drop all properties
	EXPECT_TRUE(indexManager->listIndexes("node").empty());
}

// Test building only a specific property index for nodes.
TEST_F(IndexManagerTest, BuildAndDropSpecificNodePropertyIndex) {
	const std::string key = "name";

	// Act: Build only the property index for the "name" key.
	EXPECT_TRUE(indexManager->buildPropertyIndex("node", key));

	// Assert: Check that ONLY the specified property index exists.
	auto nodeIndexes = indexManager->listIndexes("node");
	ASSERT_EQ(nodeIndexes.size(), 1);
	EXPECT_TRUE(hasIndex(nodeIndexes, "property", key));
	EXPECT_FALSE(hasIndex(nodeIndexes, "label"));
	EXPECT_FALSE(hasIndex(nodeIndexes, "property", "age"));

	// Act & Assert: Drop the specific index and verify the list is empty.
	EXPECT_TRUE(indexManager->dropIndex("node", "property", key));
	EXPECT_TRUE(indexManager->listIndexes("node").empty());
}

// New Test: Verify that edge index management works correctly and is isolated from nodes.
TEST_F(IndexManagerTest, BuildAndDropEdgeIndexes) {
	// Arrange: Add an edge with a label and properties.
	graph::Edge edge1(10, 1, 1, "LINKS_TO");
	dataManager->addEdge(edge1);
	dataManager->addEdgeProperties(10, {{"weight", 2.5}});
	fileStorage->flush();

	// Act: Build all indexes for edges.
	EXPECT_TRUE(indexManager->buildIndexes("edge"));

	// Assert: Check that edge indexes are created.
	auto edgeIndexes = indexManager->listIndexes("edge");
	ASSERT_EQ(edgeIndexes.size(), 2); // label + weight
	EXPECT_TRUE(hasIndex(edgeIndexes, "label"));
	EXPECT_TRUE(hasIndex(edgeIndexes, "property", "weight"));

	// Assert: Check that NO node indexes were created by this operation.
	EXPECT_TRUE(indexManager->listIndexes("node").empty());

	// Act & Assert: Drop the edge indexes and verify.
	EXPECT_TRUE(indexManager->dropIndex("edge", "label"));
	EXPECT_TRUE(indexManager->dropIndex("edge", "property", "weight"));
	EXPECT_TRUE(indexManager->listIndexes("edge").empty());
}

// Test that persisting state does not throw an exception. This is a basic sanity check.
TEST_F(IndexManagerTest, PersistStateNoThrow) {
	// Act & Assert
	EXPECT_NO_THROW(indexManager->persistState());
}
