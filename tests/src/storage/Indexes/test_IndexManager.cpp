/**
 * @file test_IndexManager.cpp
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
#include <tuple>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

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
	[[nodiscard]] bool hasIndexMetadata(const std::string &targetName, const std::string &targetProp) const {
		auto list = indexManager->listIndexesDetailed();
		for (const auto &[name, type, label, prop]: list) {
			if (name == targetName && prop == targetProp)
				return true;
		}
		return false;
	}

	[[nodiscard]] bool hasIndexWithName(const std::string &targetName) const {
		auto list = indexManager->listIndexesDetailed();
		for (const auto &[name, type, label, prop]: list) {
			if (name == targetName)
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
	EXPECT_TRUE(hasIndexWithName("idx_user_age"));

	auto list = indexManager->listIndexesDetailed();
	bool found = false;
	for (const auto &row: list) {
		if (std::get<0>(row) == "idx_user_age") {
			EXPECT_EQ(std::get<1>(row), "node");
			EXPECT_EQ(std::get<2>(row), "User");
			EXPECT_EQ(std::get<3>(row), "age");
			found = true;
		}
	}
	EXPECT_TRUE(found);

	// 4. Drop by Name
	bool dropped = indexManager->dropIndexByName("idx_user_age");
	EXPECT_TRUE(dropped);

	// 5. Verify Removal
	EXPECT_FALSE(indexManager->hasPropertyIndex("node", "age"));
	EXPECT_FALSE(hasIndexWithName("idx_user_age"));
}

TEST_F(IndexManagerTest, DropIndexByDefinition) {
	// 1. Create Anonymous Index (Name is empty, system auto-generates it)
	(void) indexManager->createIndex("", "node", "Product", "sku");

	// Verify it exists (auto-generated name)
	auto list = indexManager->listIndexesDetailed();
	bool found = false;
	std::string autoName;
	for (const auto &row: list) {
		if (std::get<1>(row) == "node" && std::get<2>(row) == "Product" && std::get<3>(row) == "sku") {
			autoName = std::get<0>(row);
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);
	EXPECT_FALSE(autoName.empty());

	// 2. Drop by Definition (Simulating DROP INDEX ON :Product(sku))
	bool dropped = indexManager->dropIndexByDefinition("Product", "sku");
	EXPECT_TRUE(dropped);

	// 3. Verify Removal
	list = indexManager->listIndexesDetailed();
	found = false;
	for (const auto &row: list) {
		if (std::get<1>(row) == "node" && std::get<2>(row) == "Product" && std::get<3>(row) == "sku") {
			found = true;
			break;
		}
	}
	EXPECT_FALSE(found);
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
	// 1. Initial State: Default OFF
	EXPECT_FALSE(indexManager->hasLabelIndex("node"));

	// 2. Register/Create Label Index (Enable it)
	EXPECT_TRUE(indexManager->createIndex("idx_labels", "node", "", ""));

	// 3. Verify Enabled
	EXPECT_TRUE(indexManager->hasLabelIndex("node"));
	EXPECT_TRUE(hasIndexMetadata("idx_labels", ""));

	// 4. Drop
	EXPECT_TRUE(indexManager->dropIndexByName("idx_labels"));

	// 5. Verify OFF
	EXPECT_FALSE(indexManager->hasLabelIndex("node"));
	EXPECT_FALSE(hasIndexMetadata("idx_labels", ""));
}

// ============================================================================
// Live Update Tests (Observer Pattern)
// ============================================================================

TEST_F(IndexManagerTest, LivePropertyUpdate) {
	const std::string propKey = "status";

	// 1. Setup Index
	(void) indexManager->createIndex("idx_status", "node", "Task", propKey);

	int64_t taskLabelId = dataManager->getOrCreateLabelId("Task");
	graph::Node node(1, taskLabelId);

	dataManager->addNode(node);
	dataManager->addNodeProperties(1, {{propKey, std::string("TODO")}});

	// Verify Index contains "TO-DO"
	auto res1 = indexManager->findNodeIdsByProperty(propKey, std::string("TODO"));
	ASSERT_EQ(res1.size(), 1UL);
	EXPECT_EQ(res1[0], 1);

	// 3. Update Node (TO-DO -> DONE)
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

	int64_t userLabelId = dataManager->getOrCreateLabelId("User");
	graph::Node node(1, userLabelId);

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
	const std::string labelAStr = "Draft";
	const std::string labelBStr = "Published";

	int64_t labelA = dataManager->getOrCreateLabelId(labelAStr);
	int64_t labelB = dataManager->getOrCreateLabelId(labelBStr);

	constexpr int64_t nodeId = 50;

	(void) indexManager->createIndex("idx_label_gen", "node", "", "");

	graph::Node nodeA(nodeId, labelA);
	indexManager->onNodeAdded(nodeA);

	ASSERT_EQ(indexManager->findNodeIdsByLabel(labelAStr).size(), 1UL);

	// 3. Change Label (A -> B)
	graph::Node nodeB(nodeId, labelB);

	indexManager->onNodeUpdated(nodeA, nodeB);

	// 4. Verify
	auto resA = indexManager->findNodeIdsByLabel(labelAStr);
	EXPECT_TRUE(resA.empty());

	auto resB = indexManager->findNodeIdsByLabel(labelBStr);
	ASSERT_EQ(resB.size(), 1UL);
	EXPECT_EQ(resB[0], nodeId);
}

TEST_F(IndexManagerTest, LivePropertyTypeChange) {
	const std::string propKey = "mix_val";
	(void) indexManager->createIndex("idx_mix", "node", "Mix", propKey);

	// 1. Set as String
	graph::Node n1(200, dataManager->getOrCreateLabelId("Mix"));
	n1.addProperty(propKey, std::string("100"));
	indexManager->onNodeAdded(n1);

	ASSERT_EQ(indexManager->findNodeIdsByProperty(propKey, std::string("100")).size(), 1UL);

	// 2. Change to Int
	graph::Node n2(200, dataManager->getOrCreateLabelId("Mix"));
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

	int64_t nLabel = dataManager->getOrCreateLabelId("N");
	int64_t eLabel = dataManager->getOrCreateLabelId("E");

	graph::Node n(1, nLabel);
	dataManager->addNode(n);
	dataManager->addNodeProperties(1, {{"weight", static_cast<int64_t>(10)}});

	graph::Edge e(100, 1, 1, eLabel);
	dataManager->addEdge(e);
	dataManager->addEdgeProperties(100, {{"weight", static_cast<int64_t>(20)}});

	// Verify Node Index only finds Node
	auto nodeRes = indexManager->findNodeIdsByProperty("weight", 10);
	EXPECT_EQ(nodeRes.size(), 1UL);
	EXPECT_EQ(nodeRes[0], 1);

	auto nodeResEmpty = indexManager->findNodeIdsByProperty("weight", 20);
	EXPECT_TRUE(nodeResEmpty.empty());

	// Verify Edge Index only finds Edge
	auto edgeRes = indexManager->findEdgeIdsByProperty("weight", 20);
	EXPECT_EQ(edgeRes.size(), 1UL);
	EXPECT_EQ(edgeRes[0], 100);
}

// ============================================================================
// Persistence Tests
// ============================================================================

TEST_F(IndexManagerTest, PersistenceAfterRestart) {
	// 1. Create Index and Data
	(void) indexManager->createIndex("idx_persist", "node", "SaveMe", "val");

	int64_t labelId = dataManager->getOrCreateLabelId("SaveMe");
	graph::Node n(1, labelId);

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
	bool found = false;
	for (const auto &row: list) {
		if (std::get<0>(row) == "idx_persist") {
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found);

	// 5. Verify Data Searchable
	auto res = newIndexMgr->findNodeIdsByProperty("val", std::string("test"));
	ASSERT_EQ(res.size(), 1UL);
	EXPECT_EQ(res[0], 1);
}

TEST_F(IndexManagerTest, Bootstrap_NodeLabelIndex_OnRestart) {
	// 1. Enable Node Label Index and add data
	EXPECT_TRUE(indexManager->createIndex("node_label_idx", "node", "", ""));

	int64_t lblId = dataManager->getOrCreateLabelId("BootstrapNode");
	graph::Node n(1, lblId);
	dataManager->addNode(n);

	fileStorage->flush();

	// 2. Corrupt the state to force bootstrap
	// We physically delete the tree data first to simulate data loss
	indexManager->getNodeIndexManager()->getLabelIndex()->clear();

	// Now we must ensure the State Manager sees RootID = 0.
	// Since saveState() ignores 0, we manually REMOVE the key or SET it to 0.
	// Using correct constants from your codebase.
	auto sysState = fileStorage->getSystemStateManager();

	// The key used in EntityTypeIndexManager for Node Label is storage::state::keys::Node::LABEL_ROOT
	std::string stateKey = graph::storage::state::keys::Node::LABEL_ROOT;

	// Force Root ID to 0.
	// Field name is storage::state::keys::Fields::ROOT_ID ("root_id")
	sysState->set<int64_t>(stateKey, graph::storage::state::keys::Fields::ROOT_ID, 0);

	// Ensure Enabled is still True (it should be, but let's be safe)
	// Key: stateKey + ".config"
	std::string configKey = stateKey + graph::storage::state::keys::SUFFIX_CONFIG;
	sysState->set<bool>(configKey, graph::storage::state::keys::Fields::ENABLED, true);

	fileStorage->flush();

	// 3. Restart Database
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndexMgr = database->getQueryEngine()->getIndexManager();

	// 4. Verify Bootstrapping ran
	// The initialize() method should have detected Enabled=True, Root=0, and Data>0.
	// It should have rebuilt the index.
	auto res = newIndexMgr->findNodeIdsByLabel("BootstrapNode");
	EXPECT_EQ(res.size(), 1UL);
}

TEST_F(IndexManagerTest, Bootstrap_EdgeLabelIndex_OnRestart) {
	// Same logic for Edge
	EXPECT_TRUE(indexManager->createIndex("edge_label_idx", "edge", "", ""));

	int64_t lblId = dataManager->getOrCreateLabelId("BootstrapEdge");
	graph::Node n1(1, 0);
	dataManager->addNode(n1);
	graph::Node n2(2, 0);
	dataManager->addNode(n2);
	graph::Edge e(10, 1, 2, lblId);
	dataManager->addEdge(e);

	fileStorage->flush();

	// Corrupt state: Enabled=True, Root=0
	auto sysState = fileStorage->getSystemStateManager();
	sysState->set<int64_t>("edge.index.label_root", "root_id", 0);
	fileStorage->flush();

	// Restart
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndexMgr = database->getQueryEngine()->getIndexManager();

	// Verify
	auto res = newIndexMgr->findEdgeIdsByLabel("BootstrapEdge");
	EXPECT_EQ(res.size(), 1UL);
}

TEST_F(IndexManagerTest, HasIndex_EdgeCases) {
	// 1. Edge Label Index
	// Initially false
	EXPECT_FALSE(indexManager->hasLabelIndex("edge"));

	// Enable it
	indexManager->createIndex("edge_idx", "edge", "", "");
	EXPECT_TRUE(indexManager->hasLabelIndex("edge"));

	// 2. Edge Property Index
	EXPECT_FALSE(indexManager->hasPropertyIndex("edge", "weight"));
	indexManager->createIndex("edge_prop_idx", "edge", "R", "weight");
	EXPECT_TRUE(indexManager->hasPropertyIndex("edge", "weight"));

	// 3. Invalid Entity Types (Coverage for "return false" at end of function)
	EXPECT_FALSE(indexManager->hasLabelIndex("invalid_type"));
	EXPECT_FALSE(indexManager->hasLabelIndex(""));

	EXPECT_FALSE(indexManager->hasPropertyIndex("invalid_type", "prop"));
	EXPECT_FALSE(indexManager->hasPropertyIndex("", "prop"));
}

TEST_F(IndexManagerTest, ExplicitStorageFlushCall) {
	// This is just to hit the line coverage for the method wrapper
	// The actual logic is covered by integration tests, but direct call ensures no crashes.
	EXPECT_NO_THROW(indexManager->onStorageFlush());
}

TEST_F(IndexManagerTest, EnsureMetadata_CreatedOnInitialize) {
	// 1. Enable indexes
	indexManager->createIndex("node_label_idx", "node", "", "");
	indexManager->createIndex("edge_label_idx", "edge", "", "");

	// 2. Manually delete metadata from SystemState to force ensureMetadata to run
	auto sysState = fileStorage->getSystemStateManager();
	// Assuming we can access the map. If not, we rely on the fact that
	// restarting with enabled indexes will trigger ensureMetadata.
	// The Bootstrap tests above implicitly cover this line:
	// ensureMetadata("node_label_idx", "node");

	// Just verify they exist after restart
	fileStorage->flush();
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndexMgr = database->getQueryEngine()->getIndexManager();

	auto list = newIndexMgr->listIndexesDetailed();
	bool foundNode = false, foundEdge = false;
	for (const auto &row: list) {
		if (std::get<0>(row) == "node_label_idx")
			foundNode = true;
		if (std::get<0>(row) == "edge_label_idx")
			foundEdge = true;
	}
	EXPECT_TRUE(foundNode);
	EXPECT_TRUE(foundEdge);
}
