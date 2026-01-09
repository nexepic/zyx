/**
 * @file test_DataManager.cpp
 * @author Nexepic
 * @date 2025/8/15
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
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Blob.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/storage/indexes/IEntityObserver.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::storage;

// Test observer to track notifications from DataManager
class TestEntityObserver final : public IEntityObserver {
public:
	void onNodeAdded(const Node &node) override { addedNodes.push_back(node); }

	void onNodeUpdated(const Node &oldNode, const Node &newNode) override {
		updatedNodes.emplace_back(oldNode, newNode);
	}

	void onNodeDeleted(const Node &node) override { deletedNodes.push_back(node); }

	void onEdgeAdded(const Edge &edge) override { addedEdges.push_back(edge); }

	void onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) override {
		updatedEdges.emplace_back(oldEdge, newEdge);
	}

	void onEdgeDeleted(const Edge &edge) override { deletedEdges.push_back(edge); }

	std::vector<Node> addedNodes;
	std::vector<std::pair<Node, Node>> updatedNodes;
	std::vector<Node> deletedNodes;
	std::vector<Edge> addedEdges;
	std::vector<std::pair<Edge, Edge>> updatedEdges;
	std::vector<Edge> deletedEdges;

	void reset() {
		addedNodes.clear();
		updatedNodes.clear();
		deletedNodes.clear();
		addedEdges.clear();
		updatedEdges.clear();
		deletedEdges.clear();
	}
};

class DataManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file for each test to ensure isolation
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_dataManager_" + to_string(uuid) + ".dat");

		// Initialize Database and get the DataManager instance
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Register a test observer to monitor notifications
		observer = std::make_shared<TestEntityObserver>();
		dataManager->registerObserver(observer);
	}

	void TearDown() override {
		// Clean up resources
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// Helper to simulate "Save" (Flush to disk and clear dirty state)
	void simulateSave() const {
		// New Architecture: Prepare snapshot then commit it
		(void) dataManager->prepareFlushSnapshot();
		dataManager->commitFlushSnapshot();
	}

	// Helper methods to create test entities.
	static Node createTestNode(const std::string &label = "TestNode") {
		Node node;
		node.setLabel(label);
		return node;
	}

	static Edge createTestEdge(int64_t sourceId, int64_t targetId, const std::string &label = "TestEdge") {
		Edge edge;
		edge.setSourceNodeId(sourceId);
		edge.setTargetNodeId(targetId);
		edge.setLabel(label);
		return edge;
	}

	static Property createTestProperty(int64_t entityId, uint32_t entityType,
									   const std::unordered_map<std::string, PropertyValue> &properties) {
		Property property;
		property.getMutableMetadata().entityId = entityId;
		property.getMutableMetadata().entityType = entityType;
		property.setProperties(properties);
		property.getMutableMetadata().isActive = true;
		return property;
	}

	static Blob createTestBlob(const std::string &data) {
		Blob blob;
		blob.setData(data);
		blob.getMutableMetadata().isActive = true;
		return blob;
	}

	static Index createTestIndex(Index::NodeType nodeType, uint32_t indexType) {
		Index index(0, nodeType, indexType);
		index.getMutableMetadata().isActive = true;
		return index;
	}

	static State createTestState(const std::string &key) {
		State state;
		state.setKey(key);
		return state;
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<TestEntityObserver> observer;
};

TEST_F(DataManagerTest, Initialization) {
	ASSERT_NE(nullptr, dataManager);
	EXPECT_NE(nullptr, dataManager->getNodeManager());
	EXPECT_NE(nullptr, dataManager->getEdgeManager());
	EXPECT_NE(nullptr, dataManager->getPropertyManager());
	EXPECT_NE(nullptr, dataManager->getBlobManager());
	EXPECT_NE(nullptr, dataManager->getSegmentIndexManager());

	auto header = dataManager->getFileHeader();
	EXPECT_STREQ(graph::storage::FILE_HEADER_MAGIC_STRING, header.magic);
	EXPECT_NE(0U, header.version);
}

TEST_F(DataManagerTest, NodeCRUD) {
	// 1. Create
	auto node = createTestNode("Person");
	dataManager->addNode(node);
	EXPECT_NE(0, node.getId());
	ASSERT_EQ(1UL, observer->addedNodes.size());
	EXPECT_EQ(node.getId(), observer->addedNodes[0].getId());

	// 2. Retrieve
	auto retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrievedNode.getId());
	EXPECT_EQ("Person", retrievedNode.getLabel());
	EXPECT_TRUE(retrievedNode.isActive());

	// 3. Update (while still in 'ADDED' state)
	// [IMPORTANT] Reset observer to verify the update specifically
	observer->reset();

	node.setLabel("StagingPerson");
	dataManager->updateNode(node);

	retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ("StagingPerson", retrievedNode.getLabel());

	// Since DataManager now notifies even for ADDED entities, we expect 1 update event.
	// Old Label: Person, New Label: StagingPerson
	ASSERT_EQ(1UL, observer->updatedNodes.size());
	EXPECT_EQ("Person", observer->updatedNodes[0].first.getLabel());
	EXPECT_EQ("StagingPerson", observer->updatedNodes[0].second.getLabel());

	// 4. Simulate Save & Update
	simulateSave(); // Replaces markAllSaved()
	observer->reset();

	Node oldNode = retrievedNode;
	node.setLabel("UpdatedPerson");
	dataManager->updateNode(node);
	retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ("UpdatedPerson", retrievedNode.getLabel());

	ASSERT_EQ(1UL, observer->updatedNodes.size());
	EXPECT_EQ(oldNode.getLabel(), observer->updatedNodes[0].first.getLabel());
	EXPECT_EQ("UpdatedPerson", observer->updatedNodes[0].second.getLabel());

	// 5. Delete
	dataManager->deleteNode(node);
	dataManager->clearCache();
	retrievedNode = dataManager->getNode(node.getId());

	if (retrievedNode.getId() != 0) {
		EXPECT_FALSE(retrievedNode.isActive());
	} else {
		EXPECT_EQ(0, retrievedNode.getId());
	}

	ASSERT_EQ(1UL, observer->deletedNodes.size());
	EXPECT_EQ(node.getId(), observer->deletedNodes[0].getId());
}

TEST_F(DataManagerTest, NodeProperties) {
	auto node = createTestNode("Person");
	dataManager->addNode(node);

	std::unordered_map<std::string, PropertyValue> properties = {{"name", PropertyValue("John Doe")},
																 {"age", PropertyValue(30)},
																 {"active", PropertyValue(true)},
																 {"score", PropertyValue(98.5)}};
	dataManager->addNodeProperties(node.getId(), properties);

	auto retrievedProps = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(4UL, retrievedProps.size());
	EXPECT_EQ("John Doe", std::get<std::string>(retrievedProps.at("name").getVariant()));
	EXPECT_EQ(30, std::get<int64_t>(retrievedProps.at("age").getVariant()));
	EXPECT_TRUE(std::get<bool>(retrievedProps.at("active").getVariant()));
	EXPECT_DOUBLE_EQ(98.5, std::get<double>(retrievedProps.at("score").getVariant()));

	dataManager->removeNodeProperty(node.getId(), "age");
	retrievedProps = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(3UL, retrievedProps.size());
	EXPECT_EQ(retrievedProps.find("age"), retrievedProps.end());
}

TEST_F(DataManagerTest, EdgeCRUD) {
	auto sourceNode = createTestNode("Source");
	auto targetNode = createTestNode("Target");
	dataManager->addNode(sourceNode);
	dataManager->addNode(targetNode);
	observer->reset();

	// 1. Create
	auto edge = createTestEdge(sourceNode.getId(), targetNode.getId(), "KNOWS");
	dataManager->addEdge(edge);
	EXPECT_NE(0, edge.getId());
	ASSERT_EQ(1UL, observer->addedEdges.size());

	// 2. Retrieve
	auto retrievedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ(edge.getId(), retrievedEdge.getId());
	EXPECT_EQ("KNOWS", retrievedEdge.getLabel());
	EXPECT_TRUE(retrievedEdge.isActive());

	// 3. Simulate Save
	simulateSave();
	observer->reset();

	// 4. Update
	Edge oldEdge = retrievedEdge;
	edge.setLabel("UPDATED_KNOWS");
	dataManager->updateEdge(edge);
	retrievedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ("UPDATED_KNOWS", retrievedEdge.getLabel());

	ASSERT_EQ(1UL, observer->updatedEdges.size());
	EXPECT_EQ(oldEdge.getLabel(), observer->updatedEdges[0].first.getLabel());
	EXPECT_EQ("UPDATED_KNOWS", observer->updatedEdges[0].second.getLabel());

	// 5. Delete
	dataManager->deleteEdge(edge);
	dataManager->clearCache();
	retrievedEdge = dataManager->getEdge(edge.getId());
	if (retrievedEdge.getId() != 0)
		EXPECT_FALSE(retrievedEdge.isActive());
	else
		EXPECT_EQ(0, retrievedEdge.getId());

	ASSERT_EQ(1UL, observer->deletedEdges.size());
	EXPECT_EQ(edge.getId(), observer->deletedEdges[0].getId());
}

TEST_F(DataManagerTest, EdgeProperties) {
	auto sourceNode = createTestNode("Source");
	auto targetNode = createTestNode("Target");
	dataManager->addNode(sourceNode);
	dataManager->addNode(targetNode);
	auto edge = createTestEdge(sourceNode.getId(), targetNode.getId(), "CONNECTS");
	dataManager->addEdge(edge);

	std::unordered_map<std::string, PropertyValue> properties = {
			{"weight", PropertyValue(2.5)}, {"since", PropertyValue(2023)}, {"active", PropertyValue(true)}};
	dataManager->addEdgeProperties(edge.getId(), properties);

	auto retrievedProps = dataManager->getEdgeProperties(edge.getId());
	EXPECT_EQ(3UL, retrievedProps.size());
	EXPECT_DOUBLE_EQ(2.5, std::get<double>(retrievedProps.at("weight").getVariant()));
	EXPECT_EQ(2023, std::get<int64_t>(retrievedProps.at("since").getVariant()));
	EXPECT_TRUE(std::get<bool>(retrievedProps.at("active").getVariant()));

	dataManager->removeEdgeProperty(edge.getId(), "since");
	retrievedProps = dataManager->getEdgeProperties(edge.getId());
	EXPECT_EQ(2UL, retrievedProps.size());
	EXPECT_EQ(retrievedProps.find("since"), retrievedProps.end());
}

TEST_F(DataManagerTest, FindEdgesByNode) {
	auto node1 = createTestNode("Node1");
	auto node2 = createTestNode("Node2");
	auto node3 = createTestNode("Node3");
	dataManager->addNode(node1);
	dataManager->addNode(node2);
	dataManager->addNode(node3);
	auto edge1 = createTestEdge(node1.getId(), node2.getId(), "CONNECTS_TO");
	auto edge2 = createTestEdge(node2.getId(), node3.getId(), "CONNECTS_TO");
	auto edge3 = createTestEdge(node3.getId(), node1.getId(), "CONNECTS_TO");
	dataManager->addEdge(edge1);
	dataManager->addEdge(edge2);
	dataManager->addEdge(edge3);

	auto outEdges = dataManager->findEdgesByNode(node1.getId(), "out");
	ASSERT_EQ(1UL, outEdges.size());
	EXPECT_EQ(edge1.getId(), outEdges[0].getId());
	EXPECT_EQ(node2.getId(), outEdges[0].getTargetNodeId());

	auto inEdges = dataManager->findEdgesByNode(node1.getId(), "in");
	ASSERT_EQ(1UL, inEdges.size());
	EXPECT_EQ(edge3.getId(), inEdges[0].getId());
	EXPECT_EQ(node3.getId(), inEdges[0].getSourceNodeId());

	auto allEdges = dataManager->findEdgesByNode(node1.getId(), "both");
	EXPECT_EQ(2UL, allEdges.size());
}

TEST_F(DataManagerTest, PropertyEntityCRUD) {
	auto node = createTestNode("PropertyHolder");
	dataManager->addNode(node);

	std::unordered_map<std::string, PropertyValue> props = {{"test_key", PropertyValue("test_value")}};
	auto property = createTestProperty(node.getId(), Node::typeId, props);
	dataManager->addPropertyEntity(property);
	EXPECT_NE(0, property.getId());

	auto retrievedProperty = dataManager->getProperty(property.getId());
	EXPECT_EQ(property.getId(), retrievedProperty.getId());
	EXPECT_EQ(node.getId(), retrievedProperty.getEntityId());
	ASSERT_TRUE(retrievedProperty.hasPropertyValue("test_key"));
	EXPECT_EQ("test_value", std::get<std::string>(retrievedProperty.getPropertyValues().at("test_key").getVariant()));

	props["test_key"] = PropertyValue(42);
	property.setProperties(props);
	dataManager->updatePropertyEntity(property);
	retrievedProperty = dataManager->getProperty(property.getId());
	EXPECT_EQ(42, std::get<int64_t>(retrievedProperty.getPropertyValues().at("test_key").getVariant()));

	dataManager->deleteProperty(property);
	dataManager->clearCache();
	retrievedProperty = dataManager->getProperty(property.getId());
	if (retrievedProperty.getId() != 0)
		EXPECT_FALSE(retrievedProperty.isActive());
	else
		EXPECT_EQ(0, retrievedProperty.getId());
}

TEST_F(DataManagerTest, BlobEntityCRUD) {
	std::string blobDataStr = "12345";
	auto blob = createTestBlob(blobDataStr);
	dataManager->addBlobEntity(blob);
	EXPECT_NE(0, blob.getId());

	auto retrievedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(blob.getId(), retrievedBlob.getId());
	EXPECT_EQ(blobDataStr, retrievedBlob.getDataAsString());

	std::string newBlobDataStr = "67890";
	blob.setData(newBlobDataStr);
	dataManager->updateBlobEntity(blob);
	retrievedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(newBlobDataStr, retrievedBlob.getDataAsString());

	dataManager->deleteBlob(blob);
	dataManager->clearCache();
	retrievedBlob = dataManager->getBlob(blob.getId());
	if (retrievedBlob.getId() != 0)
		EXPECT_FALSE(retrievedBlob.isActive());
	else
		EXPECT_EQ(0, retrievedBlob.getId());
}

TEST_F(DataManagerTest, IndexEntityCRUD) {
	auto index = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(index);
	EXPECT_NE(0, index.getId());

	auto retrievedIndex = dataManager->getIndex(index.getId());
	EXPECT_EQ(index.getId(), retrievedIndex.getId());
	EXPECT_EQ(1U, retrievedIndex.getIndexType());
	EXPECT_EQ(Index::NodeType::LEAF, retrievedIndex.getNodeType());

	index.setLevel(5);
	dataManager->updateIndexEntity(index);
	retrievedIndex = dataManager->getIndex(index.getId());
	EXPECT_EQ(5, retrievedIndex.getLevel());

	dataManager->deleteIndex(index);
	dataManager->clearCache();
	retrievedIndex = dataManager->getIndex(index.getId());
	if (retrievedIndex.getId() != 0)
		EXPECT_FALSE(retrievedIndex.isActive());
	else
		EXPECT_EQ(0, retrievedIndex.getId());
}

TEST_F(DataManagerTest, StateEntityCRUD) {
	auto state = createTestState("test.state.key");
	dataManager->addStateEntity(state);
	EXPECT_NE(0, state.getId());

	auto retrievedState = dataManager->getState(state.getId());
	EXPECT_EQ(state.getId(), retrievedState.getId());
	EXPECT_EQ("test.state.key", retrievedState.getKey());

	auto foundState = dataManager->findStateByKey("test.state.key");
	EXPECT_EQ(state.getId(), foundState.getId());

	state.setKey("updated.state.key");
	dataManager->updateStateEntity(state);
	auto notFoundState = dataManager->findStateByKey("test.state.key");
	EXPECT_EQ(0, notFoundState.getId());
	foundState = dataManager->findStateByKey("updated.state.key");
	EXPECT_EQ(state.getId(), foundState.getId());
	EXPECT_EQ("updated.state.key", foundState.getKey());

	dataManager->deleteState(state);
	dataManager->clearCache();
	retrievedState = dataManager->getState(state.getId());
	if (retrievedState.getId() != 0)
		EXPECT_FALSE(retrievedState.isActive());
	else
		EXPECT_EQ(0, retrievedState.getId());
}

TEST_F(DataManagerTest, StateProperties) {
	auto state = createTestState("config.state");
	dataManager->addStateEntity(state);

	std::unordered_map<std::string, PropertyValue> properties = {
			{"setting1", PropertyValue("value1")}, {"setting2", PropertyValue(42)}, {"setting3", PropertyValue(true)}};
	dataManager->addStateProperties("config.state", properties);

	auto retrievedProps = dataManager->getStateProperties("config.state");
	EXPECT_EQ(3UL, retrievedProps.size());
	EXPECT_EQ("value1", std::get<std::string>(retrievedProps.at("setting1").getVariant()));
	EXPECT_EQ(42, std::get<int64_t>(retrievedProps.at("setting2").getVariant()));
	EXPECT_TRUE(std::get<bool>(retrievedProps.at("setting3").getVariant()));

	dataManager->removeState("config.state");
	auto foundState = dataManager->findStateByKey("config.state");
	EXPECT_EQ(0, foundState.getId());

	auto emptyProps = dataManager->getStateProperties("config.state");
	EXPECT_TRUE(emptyProps.empty());
}

TEST_F(DataManagerTest, BatchOperations) {
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 10; i++) {
		auto node = createTestNode("BatchNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	auto nodes = dataManager->getNodeBatch(nodeIds);
	ASSERT_EQ(10UL, nodes.size());
	for (size_t i = 0; i < nodes.size(); i++) {
		EXPECT_EQ(nodeIds[i], nodes[i].getId());
		EXPECT_EQ("BatchNode" + std::to_string(i), nodes[i].getLabel());
	}

	auto rangeNodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 5);
	EXPECT_EQ(5UL, rangeNodes.size());

	rangeNodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 20);
	EXPECT_EQ(10UL, rangeNodes.size());
}

TEST_F(DataManagerTest, CacheManagement) {
	auto node = createTestNode("CacheNode");
	dataManager->addNode(node);

	auto retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrievedNode.getId());

	dataManager->clearCache();

	// Should still retrieve from Dirty Layer (PersistenceManager) even if cache is cleared
	retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrievedNode.getId());
}

TEST_F(DataManagerTest, DirtyTracking) {
	auto node = createTestNode("DirtyNode");
	dataManager->addNode(node);
	EXPECT_TRUE(dataManager->hasUnsavedChanges());

	auto dirtyNodes = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::ADDED});
	ASSERT_EQ(1UL, dirtyNodes.size());
	EXPECT_EQ(EntityChangeType::ADDED, dirtyNodes[0].changeType);
	EXPECT_EQ(node.getId(), dirtyNodes[0].backup->getId());

	simulateSave();
	EXPECT_FALSE(dataManager->hasUnsavedChanges());

	node.setLabel("UpdatedDirtyNode");
	dataManager->updateNode(node);
	EXPECT_TRUE(dataManager->hasUnsavedChanges());

	dirtyNodes = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::MODIFIED});
	ASSERT_EQ(1UL, dirtyNodes.size());
	EXPECT_EQ(EntityChangeType::MODIFIED, dirtyNodes[0].changeType);
}

TEST_F(DataManagerTest, AutoFlush) {
	bool autoFlushCalled = false;

	if (database) {
		auto idxMgr = database->getQueryEngine()->getIndexManager();
		// Drop it to disable auto-indexing
		idxMgr->getNodeIndexManager()->dropIndex("label", "");

		database->getStorage()->flush();
	}

	// Note: The new auto-flush logic checks SUM of all entities.
	dataManager->setMaxDirtyEntities(5);
	dataManager->setAutoFlushCallback([&autoFlushCalled]() { autoFlushCalled = true; });

	for (int i = 0; i < 4; i++) {
		auto node = createTestNode("AutoFlushNode" + std::to_string(i));
		dataManager->addNode(node);
	}
	// With index disabled and dirty count reset, 4 nodes < 5 limit
	EXPECT_FALSE(autoFlushCalled);

	auto node = createTestNode("ThresholdNode");
	dataManager->addNode(node);
	// 5 nodes >= 5 limit
	EXPECT_TRUE(autoFlushCalled);
}

TEST_F(DataManagerTest, ObserverNotifications) {
	observer->reset();

	// --- PHASE 1: Add ---
	auto source = createTestNode("Source");
	auto target = createTestNode("Target");
	dataManager->addNode(source);
	dataManager->addNode(target);

	// Adding an edge internally triggers `linkEdge`, which updates the connected nodes
	// (setting firstOutEdgeId/firstInEdgeId) and the edge itself.
	auto edge = createTestEdge(source.getId(), target.getId(), "RELATES_TO");
	dataManager->addEdge(edge);

	EXPECT_EQ(2UL, observer->addedNodes.size());
	EXPECT_EQ(1UL, observer->addedEdges.size());

	// Linking the edge updates both Source and Target nodes + the Edge itself.
	// Since we enabled notifications for ADDED entities, these internal updates are now reported.
	EXPECT_EQ(2UL, observer->updatedNodes.size()); // Source updated, Target updated
	EXPECT_EQ(1UL, observer->updatedEdges.size()); // Edge updated (pointers)

	// --- PHASE 2: Explicit Update ---
	simulateSave();
	observer->reset();

	Node oldSource = dataManager->getNode(source.getId());
	source.setLabel("UpdatedSource");
	dataManager->updateNode(source);

	Edge oldEdge = dataManager->getEdge(edge.getId());
	edge.setLabel("UPDATED_RELATION");
	dataManager->updateEdge(edge);

	EXPECT_EQ(1UL, observer->updatedNodes.size());
	EXPECT_EQ(1UL, observer->updatedEdges.size());

	// --- PHASE 3: Delete ---
	observer->reset();

	// Deleting the edge unlinks it from the nodes.
	// `unlinkEdge` calls updateNode on Source and Target.
	dataManager->deleteEdge(edge);
	dataManager->deleteNode(target);

	EXPECT_EQ(1UL, observer->deletedNodes.size());
	EXPECT_EQ(target.getId(), observer->deletedNodes[0].getId());

	EXPECT_EQ(1UL, observer->deletedEdges.size());
	EXPECT_EQ(edge.getId(), observer->deletedEdges[0].getId());

	// Unlinking the edge updates both Source and Target nodes.
	EXPECT_EQ(2UL, observer->updatedNodes.size());

	std::vector<int64_t> updatedNodeIds;
	for (const auto &pair: observer->updatedNodes) {
		updatedNodeIds.push_back(pair.second.getId());
	}

	// Verify that both connected nodes were updated during the edge deletion
	ASSERT_TRUE(std::find(updatedNodeIds.begin(), updatedNodeIds.end(), source.getId()) != updatedNodeIds.end());
	ASSERT_TRUE(std::find(updatedNodeIds.begin(), updatedNodeIds.end(), target.getId()) != updatedNodeIds.end());
}

TEST_F(DataManagerTest, MultipleEntityTypes) {
	auto node = createTestNode("MultiTypeNode");
	dataManager->addNode(node);

	auto state = createTestState("multi.type.state");
	dataManager->addStateEntity(state);

	auto index = createTestIndex(Index::NodeType::INTERNAL, 2);
	dataManager->addIndexEntity(index);

	std::string blobData = "some binary data";
	auto blob = createTestBlob(blobData);
	dataManager->addBlobEntity(blob);

	auto retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ("MultiTypeNode", retrievedNode.getLabel());

	auto retrievedState = dataManager->findStateByKey("multi.type.state");
	EXPECT_EQ(state.getId(), retrievedState.getId());

	auto retrievedIndex = dataManager->getIndex(index.getId());
	EXPECT_EQ(2U, retrievedIndex.getIndexType());

	auto retrievedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(blobData, retrievedBlob.getDataAsString());

	// --- Dirty Check ---
	EXPECT_TRUE(dataManager->hasUnsavedChanges());

	// 1. Check Node
	EXPECT_EQ(1UL, dataManager->getDirtyEntityInfos<Node>({EntityChangeType::ADDED}).size());

	// 2. Check State (Expect >= 1 due to system config/index metadata)
	auto dirtyStates = dataManager->getDirtyEntityInfos<State>({EntityChangeType::ADDED});
	EXPECT_GE(dirtyStates.size(), 1UL);

	// Verify our specific state is in the list
	bool stateFound = false;
	for (const auto &info: dirtyStates) {
		if (info.backup.has_value() && info.backup->getId() == state.getId()) {
			stateFound = true;
			break;
		}
	}
	EXPECT_TRUE(stateFound) << "The manually created state was not found in dirty list.";

	// 3. Check Index (Expect >= 1 due to Label Index B-Tree updates)
	auto dirtyIndexes = dataManager->getDirtyEntityInfos<Index>({EntityChangeType::ADDED});
	EXPECT_GE(dirtyIndexes.size(), 1UL);

	// Verify our specific index is in the list
	bool indexFound = false;
	for (const auto &info: dirtyIndexes) {
		if (info.backup.has_value() && info.backup->getId() == index.getId()) {
			indexFound = true;
			break;
		}
	}
	EXPECT_TRUE(indexFound) << "The manually created index was not found in dirty list.";

	// 4. Check Blob
	EXPECT_EQ(1UL, dataManager->getDirtyEntityInfos<Blob>({EntityChangeType::ADDED}).size());

	simulateSave();
	EXPECT_FALSE(dataManager->hasUnsavedChanges());
}

TEST_F(DataManagerTest, LargeEntityCounts) {
	constexpr int entityCount = 100;

	std::vector<Node> nodes;
	std::vector<int64_t> nodeIds;
	nodes.reserve(entityCount);
	nodeIds.reserve(entityCount);

	for (int i = 0; i < entityCount; i++) {
		auto node = createTestNode("LargeNode" + std::to_string(i));
		dataManager->addNode(node);
		nodes.push_back(node);
		nodeIds.push_back(node.getId());
	}

	for (int i = 0; i < entityCount - 1; i++) {
		auto edge = createTestEdge(nodes[i].getId(), nodes[i + 1].getId(), "NEXT");
		dataManager->addEdge(edge);
	}

	for (int i = 0; i < entityCount; i++) {
		std::unordered_map<std::string, PropertyValue> props = {
				{"index", PropertyValue(i)},
				{"name", PropertyValue("Node" + std::to_string(i))},
		};
		dataManager->addNodeProperties(nodes[i].getId(), props);
	}

	for (int i = 0; i < entityCount; i += 10) {
		int end = std::min(i + 10, entityCount);
		std::vector<int64_t> batchIds(nodeIds.begin() + i, nodeIds.begin() + end);
		auto batchNodes = dataManager->getNodeBatch(batchIds);
		EXPECT_EQ(batchIds.size(), batchNodes.size());
	}

	dataManager->clearCache();

	const int middleIndex = entityCount / 2;
	auto retrievedNode = dataManager->getNode(nodes[middleIndex].getId());
	EXPECT_EQ(nodes[middleIndex].getId(), retrievedNode.getId());

	auto props = dataManager->getNodeProperties(nodes[middleIndex].getId());
	EXPECT_EQ(middleIndex, std::get<int64_t>(props.at("index").getVariant()));
}
