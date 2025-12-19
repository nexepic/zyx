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
#include <tuple>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;

class IndexManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file path
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_indexManager_" + to_string(uuid) + ".dat");

		// Initialize Database
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
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

	// Helper to check if a specific index exists in the detailed list
	bool hasIndexMetadata(const std::string &targetName, const std::string &targetProp) const {
		auto list = indexManager->listIndexesDetailed();
		for (const auto &[name, type, label, prop]: list) {
			if (name == targetName && prop == targetProp)
				return true;
		}
		return false;
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
};

// ============================================================================
// Lifecycle Tests (Create, Drop, Metadata)
// ============================================================================

TEST_F(IndexManagerTest, CreateAndDropNamedIndex) {
	// 1. Create a Named Index on Node Property
	// Arguments: name, entityType, label, property
	bool created = indexManager->createIndex("idx_user_age", "node", "User", "age");
	EXPECT_TRUE(created);

	// 2. Verify Physical Existence
	EXPECT_TRUE(indexManager->hasPropertyIndex("node", "age"));

	// 3. Verify Metadata Existence
	auto list = indexManager->listIndexesDetailed();
	ASSERT_EQ(list.size(), 1UL);

	EXPECT_EQ(std::get<0>(list[0]), "idx_user_age"); // Name
	EXPECT_EQ(std::get<1>(list[0]), "node"); // Entity Type
	EXPECT_EQ(std::get<2>(list[0]), "User"); // Label
	EXPECT_EQ(std::get<3>(list[0]), "age"); // Property

	// 4. Drop by Name
	bool dropped = indexManager->dropIndexByName("idx_user_age");
	EXPECT_TRUE(dropped);

	// 5. Verify Removal
	EXPECT_FALSE(indexManager->hasPropertyIndex("node", "age"));
	EXPECT_TRUE(indexManager->listIndexesDetailed().empty());
}

TEST_F(IndexManagerTest, DropIndexByDefinition) {
	// 1. Create Anonymous Index (Name is empty, system auto-generates it)
	(void) indexManager->createIndex("", "node", "Product", "sku");

	// Verify it exists (auto-generated name)
	auto list = indexManager->listIndexesDetailed();
	ASSERT_EQ(list.size(), 1UL);
	std::string autoName = std::get<0>(list[0]);
	EXPECT_FALSE(autoName.empty());

	// 2. Drop by Definition (Simulating DROP INDEX ON :Product(sku))
	bool dropped = indexManager->dropIndexByDefinition("Product", "sku");
	EXPECT_TRUE(dropped);

	// 3. Verify Removal
	EXPECT_TRUE(indexManager->listIndexesDetailed().empty());
}

TEST_F(IndexManagerTest, DuplicateIndexCreation) {
	// 1. Create first time
	EXPECT_TRUE(indexManager->createIndex("idx_dup", "node", "A", "p"));

	// 2. Create duplicate name (Should fail)
	EXPECT_FALSE(indexManager->createIndex("idx_dup", "node", "B", "q"));

	// 3. Create same definition but different name
	// Assuming EntityTypeIndexManager prevents duplicate physical indexes on same key
	EXPECT_FALSE(indexManager->createIndex("idx_diff_name", "node", "A", "p"));
}

TEST_F(IndexManagerTest, LabelIndexLifecycle) {
	// Create Label Index (Empty property)
	// Pass non-empty name so we can verify it by name
	EXPECT_TRUE(indexManager->createIndex("idx_labels", "node", "", ""));

	EXPECT_TRUE(indexManager->hasLabelIndex("node"));

	// Check Metadata
	EXPECT_TRUE(hasIndexMetadata("idx_labels", ""));

	// Drop
	EXPECT_TRUE(indexManager->dropIndexByName("idx_labels"));
	EXPECT_FALSE(indexManager->hasLabelIndex("node"));
}

// ============================================================================
// Live Update Tests (Observer Pattern)
// ============================================================================

TEST_F(IndexManagerTest, LivePropertyUpdate) {
	const std::string propKey = "status";

	// 1. Setup Index
	(void) indexManager->createIndex("idx_status", "node", "Task", propKey);

	// 2. Insert Node
	graph::Node node(1, "Task");
	dataManager->addNode(node);
	dataManager->addNodeProperties(1, {{propKey, std::string("TODO")}});

	// Verify Index contains "TODO"
	auto res1 = indexManager->findNodeIdsByProperty(propKey, std::string("TODO"));
	ASSERT_EQ(res1.size(), 1UL);
	EXPECT_EQ(res1[0], 1);

	// 3. Update Node (TODO -> DONE)
	// This triggers onNodeUpdated in IndexManager
	dataManager->addNodeProperties(1, {{propKey, std::string("DONE")}});

	// Verify Index updated
	auto resOld = indexManager->findNodeIdsByProperty(propKey, std::string("TODO"));
	EXPECT_TRUE(resOld.empty());

	auto resNew = indexManager->findNodeIdsByProperty(propKey, std::string("DONE"));
	ASSERT_EQ(resNew.size(), 1UL);
	EXPECT_EQ(resNew[0], 1);
}

TEST_F(IndexManagerTest, LiveDeletion) {
	// 1. Setup
	(void) indexManager->createIndex("idx_del", "node", "User", "id");

	graph::Node node(1, "User");
	dataManager->addNode(node);
	dataManager->addNodeProperties(1, {{"id", static_cast<int64_t>(100)}});

	EXPECT_EQ(indexManager->findNodeIdsByProperty("id", 100).size(), 1UL);

	// 2. Delete Node
	graph::Node n = dataManager->getNode(1);
	// Hydrate properties manually if getNode doesn't do it automatically,
	// so that EntityTypeIndexManager can see the old properties to remove them from index.
	auto props = dataManager->getNodeProperties(1);
	n.setProperties(props);

	dataManager->deleteNode(n);

	// 3. Verify Index is empty
	EXPECT_TRUE(indexManager->findNodeIdsByProperty("id", (int64_t) 100).empty());
}

TEST_F(IndexManagerTest, LiveLabelChange) {
	const std::string labelA = "Draft";
	const std::string labelB = "Published";
	constexpr int64_t nodeId = 50;

	// 1. Setup Label Indexes
	(void) indexManager->createIndex("idx_label_gen", "node", "", "");

	// 2. Insert Node with Label A
	graph::Node nodeA(nodeId, labelA);
	indexManager->onNodeAdded(nodeA);

	ASSERT_EQ(indexManager->findNodeIdsByLabel(labelA).size(), 1UL);

	// 3. Change Label (A -> B)
	graph::Node nodeB(nodeId, labelB);

	indexManager->onNodeUpdated(nodeA, nodeB);

	// 4. Verify
	auto resA = indexManager->findNodeIdsByLabel(labelA);
	EXPECT_TRUE(resA.empty()) << "Old label index entry should be removed";

	auto resB = indexManager->findNodeIdsByLabel(labelB);
	ASSERT_EQ(resB.size(), 1UL) << "New label index entry should be added";
	EXPECT_EQ(resB[0], nodeId);
}

TEST_F(IndexManagerTest, LivePropertyTypeChange) {
	const std::string propKey = "mix_val";
	(void) indexManager->createIndex("idx_mix", "node", "Mix", propKey);

	// 1. Set as String
	graph::Node n1(200, "Mix");
	n1.addProperty(propKey, std::string("100"));
	indexManager->onNodeAdded(n1);

	ASSERT_EQ(indexManager->findNodeIdsByProperty(propKey, std::string("100")).size(), 1UL);

	// 2. Change to Int
	graph::Node n2(200, "Mix");
	n2.addProperty(propKey, 100);

	indexManager->onNodeUpdated(n1, n2);

	// 3. Verify String entry gone
	EXPECT_TRUE(indexManager->findNodeIdsByProperty(propKey, std::string("100")).empty());

	// 4. Verify Int entry
	auto resInt = indexManager->findNodeIdsByProperty(propKey, 100);
	EXPECT_EQ(resInt.size(), 0UL);
}

// ============================================================================
// Isolation Tests (Node vs Edge)
// ============================================================================

TEST_F(IndexManagerTest, NodeEdgeIsolation) {
	// Both Node and Edge have property "weight"
	(void) indexManager->createIndex("idx_node_w", "node", "N", "weight");
	(void) indexManager->createIndex("idx_edge_w", "edge", "E", "weight");

	// Add Node
	graph::Node n(1, "N");
	dataManager->addNode(n);
	dataManager->addNodeProperties(1, {{"weight", static_cast<int64_t>(10)}});

	// Add Edge
	graph::Edge e(100, 1, 1, "E");
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(100, {{"weight", static_cast<int64_t>(20)}});

	// Verify Node Index only finds Node
	auto nodeRes = indexManager->findNodeIdsByProperty("weight", (int64_t) 10);
	EXPECT_EQ(nodeRes.size(), 1UL);
	EXPECT_EQ(nodeRes[0], 1);

	auto nodeResEmpty = indexManager->findNodeIdsByProperty("weight", (int64_t) 20);
	EXPECT_TRUE(nodeResEmpty.empty());

	// Verify Edge Index only finds Edge
	auto edgeRes = indexManager->findEdgeIdsByProperty("weight", (int64_t) 20);
	EXPECT_EQ(edgeRes.size(), 1UL);
	EXPECT_EQ(edgeRes[0], 100);
}

// ============================================================================
// Persistence Tests
// ============================================================================

TEST_F(IndexManagerTest, PersistenceAfterRestart) {
	// 1. Create Index and Data
	(void) indexManager->createIndex("idx_persist", "node", "SaveMe", "val");

	graph::Node n(1, "SaveMe");
	dataManager->addNode(n);
	dataManager->addNodeProperties(1, {{"val", std::string("test")}});

	// 2. Flush to disk (Triggers IStorageEventListener::onStorageFlush)
	fileStorage->flush();

	// 3. Restart DB
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndexMgr = database->getQueryEngine()->getIndexManager();

	// 4. Verify Metadata Loaded
	auto list = newIndexMgr->listIndexesDetailed();
	ASSERT_EQ(list.size(), 1UL);
	EXPECT_EQ(std::get<0>(list[0]), "idx_persist");

	// 5. Verify Data Searchable
	auto res = newIndexMgr->findNodeIdsByProperty("val", std::string("test"));
	ASSERT_EQ(res.size(), 1UL);
	EXPECT_EQ(res[0], 1);
}
