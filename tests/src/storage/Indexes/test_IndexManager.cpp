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
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

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
		return std::ranges::any_of(indexes,
						   [&](const auto &pair) { return pair.first == type && pair.second == key; });
	}

	bool hasIndexListEntry(const std::string &entityType, const std::string &indexType, const std::string &key = "") {
		auto indexes = indexManager->listIndexes(entityType);
		return std::ranges::any_of(indexes,
						   [&](const auto &pair) { return pair.first == indexType && pair.second == key; });
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
	ASSERT_EQ(nodeIndexes.size(), 3UL); // label + name + age
	EXPECT_TRUE(hasIndex(nodeIndexes, "label"));
	EXPECT_TRUE(hasIndex(nodeIndexes, "property", "name"));
	EXPECT_TRUE(hasIndex(nodeIndexes, "property", "age"));

	// Act & Assert: Drop the label index and verify it's gone.
	EXPECT_TRUE(indexManager->dropIndex("node", "label"));
	nodeIndexes = indexManager->listIndexes("node");
	ASSERT_EQ(nodeIndexes.size(), 2UL);
	EXPECT_FALSE(hasIndex(nodeIndexes, "label"));

	// Act & Assert: Drop one property index and verify.
	EXPECT_TRUE(indexManager->dropIndex("node", "property", "name"));
	nodeIndexes = indexManager->listIndexes("node");
	ASSERT_EQ(nodeIndexes.size(), 1UL);
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
	ASSERT_EQ(nodeIndexes.size(), 1UL);
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
	ASSERT_EQ(edgeIndexes.size(), 2UL); // label + weight
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

TEST_F(IndexManagerTest, LiveIndexUpdate_CreateBeforeInsert) {
	const std::string propertyKey = "salary";
	constexpr int64_t targetValue = 5000;
	// Inserting ID 100 into a fresh DB causes the SegmentTracker/FileStorage
	// to crash due to sparse segment allocation bugs (Entity range out of bounds).
	constexpr int64_t nodeId = 1;

	// 1. Create the index explicitly BEFORE adding any data with this property.
	EXPECT_TRUE(indexManager->buildPropertyIndex("node", propertyKey));
	EXPECT_TRUE(indexManager->hasPropertyIndex("node", propertyKey));
	EXPECT_TRUE(hasIndexListEntry("node", "property", propertyKey));
	std::cout << "111" << std::endl;

	// 2. Insert Data
	// Step 2a: Create Node (Triggers onNodeAdded)
	graph::Node node(nodeId, "Employee");
	std::cout << "222" << std::endl;
	dataManager->addNode(node);
	std::cout << "333" << std::endl;

	// Step 2b: Add Property (Triggers onNodeUpdated via DataManager snapshot logic)
	dataManager->addNodeProperties(nodeId, {{propertyKey, targetValue}});

	// 3. Verify Memory State (Immediate Query)
	auto results = indexManager->findNodeIdsByProperty(propertyKey, targetValue);
	ASSERT_EQ(results.size(), 1UL) << "Index failed to update in memory immediately after insertion.";
	EXPECT_EQ(results[0], nodeId);

	fileStorage->flush();

	// 4. Simulate Restart
	database->close();

	// Reopen
	auto newDatabase = std::make_unique<graph::Database>(testFilePath.string());
	newDatabase->open();
	auto newIndexManager = newDatabase->getQueryEngine()->getIndexManager();

	// 5. Verify Disk State (Persistence)
	// Check if the index definition loaded
	EXPECT_TRUE(newIndexManager->hasPropertyIndex("node", propertyKey));

	// Check if the data loaded
	auto reloadedResults = newIndexManager->findNodeIdsByProperty(propertyKey, targetValue);
	ASSERT_EQ(reloadedResults.size(), 1UL) << "Index data was not persisted to disk correctly.";
	EXPECT_EQ(reloadedResults[0], nodeId);
}

TEST_F(IndexManagerTest, LiveLabelIndexUpdate) {
	// 1. Enable Label Index
	EXPECT_TRUE(indexManager->buildIndexes("node")); // Builds label + any existing props

	// 2. Insert Node
	graph::Node node(1, "LiveLabelTest");
	dataManager->addNode(node);

	// 3. Check Memory
	auto results = indexManager->findNodeIdsByLabel("LiveLabelTest");
	ASSERT_EQ(results.size(), 1UL);
	EXPECT_EQ(results[0], 1);

	// 4. Persistence
	fileStorage->flush();
	database->close();
	database.reset(); // Destroy old instance

	// 5. Reload
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndexMgr = database->getQueryEngine()->getIndexManager();

	// 6. Check Disk
	auto loadedResults = newIndexMgr->findNodeIdsByLabel("LiveLabelTest");
	ASSERT_EQ(loadedResults.size(), 1UL);
	EXPECT_EQ(loadedResults[0], 1);
}

// Test Property Removal Updates
TEST_F(IndexManagerTest, LiveIndexUpdate_PropertyRemoval) {
	const std::string key = "status";

	// 1. Setup Index
	indexManager->buildPropertyIndex("node", key);

	// 2. Add Data
	graph::Node node(300, "Task");
	dataManager->addNode(node);
	dataManager->addNodeProperties(300, {{key, std::string("Pending")}});

	// Verify addition
	EXPECT_EQ(indexManager->findNodeIdsByProperty(key, std::string("Pending")).size(), 1UL);

	// 3. Update Property (Change Value)
	// DataManager should snapshot old="Pending", new="Done", and notify IndexManager.
	dataManager->addNodeProperties(300, {{key, std::string("Done")}});

	// Verify update
	EXPECT_TRUE(indexManager->findNodeIdsByProperty(key, std::string("Pending")).empty());
	EXPECT_EQ(indexManager->findNodeIdsByProperty(key, std::string("Done")).size(), 1UL);

	// 4. Remove Property
	dataManager->removeNodeProperty(300, key);

	// Verify removal
	EXPECT_TRUE(indexManager->findNodeIdsByProperty(key, std::string("Done")).empty());
}

TEST_F(IndexManagerTest, LiveIndexUpdate_ExistingPropertyModification_WithColdCache) {
	const std::string key = "rank";
	constexpr int64_t initialValue = 10;
	constexpr int64_t updatedValue = 20;
	constexpr int64_t nodeId = 1; // Use ID 1 to be safe

	// 1. Setup Index and Initial Data
	EXPECT_TRUE(indexManager->buildPropertyIndex("node", key));

	graph::Node node(nodeId, "Player");
	dataManager->addNode(node);
	dataManager->addNodeProperties(nodeId, {{key, initialValue}});

	// Ensure written to disk so we can reload it
	fileStorage->flush();

	// ================================================================
	// [CRITICAL STEP] CLEAR CACHE TO SIMULATE "COLD" READ
	// This forces the next get(nodeId) to load from disk.
	// If Node uses Lazy Loading for properties, oldNode will be a shell,
	// and the "Simple Code" will fail because it lazy-loads AFTER the update.
	// ================================================================
	fileStorage->clearCache();

	// 2. UPDATE the existing property
	// If using the "Simple Code":
	//   a. oldNode = get(id) -> Returns Node from disk (Properties might be lazy/empty)
	//   b. update property in DB -> DB is now 20
	//   c. notify(oldNode, newNode)
	//   d. IndexManager calls oldNode.getProperties() -> Lazy loads from DB -> Gets 20!
	//   e. 20 == 20 -> No Index Update -> FAIL.
	//
	// If using the "Fixed Code" (Explicit Snapshot):
	//   a. oldNode = get(id)
	//   b. existingProps = getProperties(id) -> Explicitly loads 10
	//   c. oldNode.setProperties(existingProps) -> oldNode is now frozen as 10
	//   d. update DB -> 20
	//   e. notify -> 10 vs 20 -> Index Updates -> PASS.
	dataManager->addNodeProperties(nodeId, {{key, updatedValue}});

	// 3. Verify Update Logic
	auto oldResults = indexManager->findNodeIdsByProperty(key, initialValue);
	EXPECT_TRUE(oldResults.empty()) << "Bug Detected: Old index entry (10) remains. Snapshot failed.";

	auto newResults = indexManager->findNodeIdsByProperty(key, updatedValue);
	ASSERT_EQ(newResults.size(), 1UL) << "Bug Detected: New index entry (20) missing. Snapshot failed.";
	EXPECT_EQ(newResults[0], nodeId);
}

TEST_F(IndexManagerTest, Isolation_NodeVsEdgeState) {
	// Enable both
	indexManager->buildPropertyIndex("node", "name");
	indexManager->buildPropertyIndex("edge", "weight");

	fileStorage->flush();

	// Check if listing is correct
	auto nodeIdx = indexManager->listIndexes("node");
	auto edgeIdx = indexManager->listIndexes("edge");

	EXPECT_TRUE(hasIndexListEntry("node", "property", "name"));
	EXPECT_FALSE(hasIndexListEntry("node", "property", "weight")); // Weight shouldn't be in node

	EXPECT_TRUE(hasIndexListEntry("edge", "property", "weight"));
}
