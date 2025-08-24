/**
 * @file test_DataManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

/**
 * @file test_DataManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
#include "graph/query/indexes/IEntityObserver.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityChangeType.hpp"

namespace graph::storage::test {

	// Test observer to track notifications from DataManager
	class TestEntityObserver : public IEntityObserver {
	public:
		void onNodeAdded(const Node &node) override { addedNodes.push_back(node); }

		void onNodeUpdated(const Node &oldNode, const Node &newNode) override {
			updatedNodes.push_back(std::make_pair(oldNode, newNode));
		}

		void onNodeDeleted(const Node &node) override { deletedNodes.push_back(node); }

		void onEdgeAdded(const Edge &edge) override { addedEdges.push_back(edge); }

		void onEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) override {
			updatedEdges.push_back(std::make_pair(oldEdge, newEdge));
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

		// Helper methods to create test entities.
		// Entities are active by default upon creation, so calls to set them active are not needed.
		Node createTestNode(const std::string &label = "TestNode") {
			Node node;
			node.setLabel(label);
			// node.isActive() is true by default
			return node;
		}

		Edge createTestEdge(int64_t sourceId, int64_t targetId, const std::string &label = "TestEdge") {
			Edge edge;
			edge.setSourceNodeId(sourceId);
			edge.setTargetNodeId(targetId);
			edge.setLabel(label);
			// edge.isActive() is true by default
			return edge;
		}

		Property createTestProperty(int64_t entityId, uint32_t entityType,
									const std::unordered_map<std::string, PropertyValue> &properties) {
			Property property;
			property.getMutableMetadata().entityId = entityId;
			property.getMutableMetadata().entityType = entityType;
			property.setProperties(properties);
			property.getMutableMetadata().isActive = true; // Explicitly set for clarity
			return property;
		}

		Blob createTestBlob(const std::string &data) {
			Blob blob;
			blob.setData(data);
			blob.getMutableMetadata().isActive = true; // Explicitly set for clarity
			return blob;
		}

		Index createTestIndex(Index::NodeType nodeType, uint32_t indexType) {
			// ID is 0 before being added to the database
			Index index(0, nodeType, indexType);
			index.getMutableMetadata().isActive = true; // Explicitly set for clarity
			return index;
		}

		State createTestState(const std::string &key) {
			State state;
			state.setKey(key);
			// state.isActive() is true by default
			return state;
		}

		std::filesystem::path testFilePath;
		std::unique_ptr<graph::Database> database;
		std::shared_ptr<graph::storage::FileStorage> fileStorage;
		std::shared_ptr<graph::storage::DataManager> dataManager;
		std::shared_ptr<TestEntityObserver> observer;
	};

	TEST_F(DataManagerTest, Initialization) {
		// Verify that all managers are properly initialized
		ASSERT_NE(nullptr, dataManager);
		EXPECT_NE(nullptr, dataManager->getNodeManager());
		EXPECT_NE(nullptr, dataManager->getEdgeManager());
		EXPECT_NE(nullptr, dataManager->getPropertyManager());
		EXPECT_NE(nullptr, dataManager->getBlobManager());
		EXPECT_NE(nullptr, dataManager->getSegmentIndexManager());

		// Verify the file header has been written with correct values
		auto header = dataManager->getFileHeader();
		// Compare magic string properly - it should match FILE_HEADER_MAGIC_STRING
		EXPECT_STREQ(graph::storage::FILE_HEADER_MAGIC_STRING, header.magic);
		EXPECT_NE(0U, header.version); // Use 0U for unsigned comparison
	}

	TEST_F(DataManagerTest, NodeCRUD) {
		// 1. Create
		auto node = createTestNode("Person");
		dataManager->addNode(node);
		EXPECT_NE(0, node.getId()) << "Node should have a valid ID after being added.";
		ASSERT_EQ(1, observer->addedNodes.size()) << "Observer should be notified of node addition.";
		EXPECT_EQ(node.getId(), observer->addedNodes[0].getId());

		// 2. Retrieve
		auto retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ(node.getId(), retrievedNode.getId());
		EXPECT_EQ("Person", retrievedNode.getLabel());
		EXPECT_TRUE(retrievedNode.isActive());

		// 3. Update (while still in 'ADDED' state)
		// According to the source code logic, updating a node that is still marked as 'ADDED'
		// should NOT trigger an 'onNodeUpdated' notification.
		node.setLabel("StagingPerson");
		dataManager->updateNode(node);
		retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ("StagingPerson", retrievedNode.getLabel());
		ASSERT_EQ(0, observer->updatedNodes.size())
				<< "Observer should NOT be notified of an update when the entity is still in ADDED state.";

		// 4. Simulate a save and then update an existing entity
		dataManager->markAllSaved(); // This clears the dirty map, node is no longer 'ADDED'
		observer->reset(); // Reset observer for a clean test of the update notification

		Node oldNode = retrievedNode; // Capture state before update
		node.setLabel("UpdatedPerson");
		dataManager->updateNode(node);
		retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ("UpdatedPerson", retrievedNode.getLabel());

		// NOW, the update notification should be triggered.
		ASSERT_EQ(1, observer->updatedNodes.size())
				<< "Observer should be notified of node update for a persisted entity.";
		EXPECT_EQ(oldNode.getLabel(), observer->updatedNodes[0].first.getLabel());
		EXPECT_EQ("UpdatedPerson", observer->updatedNodes[0].second.getLabel());

		// 5. Delete
		dataManager->deleteNode(node);
		dataManager->clearCache(); // Clear cache to ensure we are not getting a stale in-memory copy
		retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ(0, retrievedNode.getId()) << "Deleted node should not be retrievable.";
		// Note: The observer was reset, so we expect exactly 1 deleted notification since the reset.
		ASSERT_EQ(1, observer->deletedNodes.size()) << "Observer should be notified of node deletion.";
		EXPECT_EQ(node.getId(), observer->deletedNodes[0].getId());
	}

	TEST_F(DataManagerTest, NodeProperties) {
		// Create a node
		auto node = createTestNode("Person");
		dataManager->addNode(node);

		// Add a map of properties
		std::unordered_map<std::string, PropertyValue> properties = {{"name", PropertyValue("John Doe")},
																	 {"age", PropertyValue(30)},
																	 {"active", PropertyValue(true)},
																	 {"score", PropertyValue(98.5)}};
		dataManager->addNodeProperties(node.getId(), properties);

		// Retrieve properties and verify them
		auto retrievedProps = dataManager->getNodeProperties(node.getId());
		EXPECT_EQ(4, retrievedProps.size());
		EXPECT_EQ("John Doe", std::get<std::string>(retrievedProps.at("name").getVariant()));
		EXPECT_EQ(30, std::get<int64_t>(retrievedProps.at("age").getVariant()));
		EXPECT_TRUE(std::get<bool>(retrievedProps.at("active").getVariant()));
		EXPECT_DOUBLE_EQ(98.5, std::get<double>(retrievedProps.at("score").getVariant()));

		// Remove a property
		dataManager->removeNodeProperty(node.getId(), "age");
		retrievedProps = dataManager->getNodeProperties(node.getId());
		EXPECT_EQ(3, retrievedProps.size());
		EXPECT_EQ(retrievedProps.find("age"), retrievedProps.end()) << "Property 'age' should have been removed.";
	}

	TEST_F(DataManagerTest, EdgeCRUD) {
		// Create source and target nodes for the edge
		auto sourceNode = createTestNode("Source");
		auto targetNode = createTestNode("Target");
		dataManager->addNode(sourceNode);
		dataManager->addNode(targetNode);
		observer->reset(); // Reset observer after setup

		// 1. Create
		auto edge = createTestEdge(sourceNode.getId(), targetNode.getId(), "KNOWS");
		dataManager->addEdge(edge);
		EXPECT_NE(0, edge.getId()) << "Edge should have a valid ID after being added.";
		ASSERT_EQ(1, observer->addedEdges.size()) << "Observer should be notified of edge addition.";

		// 2. Retrieve
		auto retrievedEdge = dataManager->getEdge(edge.getId());
		EXPECT_EQ(edge.getId(), retrievedEdge.getId());
		EXPECT_EQ("KNOWS", retrievedEdge.getLabel());
		EXPECT_TRUE(retrievedEdge.isActive());

		// 3. Simulate a save to move the edge out of the 'ADDED' state
		dataManager->markAllSaved();
		observer->reset();

		// 4. Update (now that the edge is considered 'persisted')
		Edge oldEdge = retrievedEdge; // Capture state before update
		edge.setLabel("UPDATED_KNOWS");
		dataManager->updateEdge(edge);
		retrievedEdge = dataManager->getEdge(edge.getId());
		EXPECT_EQ("UPDATED_KNOWS", retrievedEdge.getLabel());

		// The update notification should now be triggered.
		ASSERT_EQ(1, observer->updatedEdges.size()) << "Observer should be notified of edge update.";
		EXPECT_EQ(oldEdge.getLabel(), observer->updatedEdges[0].first.getLabel());
		EXPECT_EQ("UPDATED_KNOWS", observer->updatedEdges[0].second.getLabel());


		// 5. Delete
		dataManager->deleteEdge(edge);
		dataManager->clearCache(); // Ensure we read from disk
		retrievedEdge = dataManager->getEdge(edge.getId());
		EXPECT_EQ(0, retrievedEdge.getId()) << "Deleted edge should not be retrievable.";
		ASSERT_EQ(1, observer->deletedEdges.size()) << "Observer should be notified of edge deletion.";
		EXPECT_EQ(edge.getId(), observer->deletedEdges[0].getId());
	}

	TEST_F(DataManagerTest, EdgeProperties) {
		// Create nodes and an edge
		auto sourceNode = createTestNode("Source");
		auto targetNode = createTestNode("Target");
		dataManager->addNode(sourceNode);
		dataManager->addNode(targetNode);
		auto edge = createTestEdge(sourceNode.getId(), targetNode.getId(), "CONNECTS");
		dataManager->addEdge(edge);

		// Add properties to the edge
		std::unordered_map<std::string, PropertyValue> properties = {
				{"weight", PropertyValue(2.5)}, {"since", PropertyValue(2023)}, {"active", PropertyValue(true)}};
		dataManager->addEdgeProperties(edge.getId(), properties);

		// Retrieve and verify properties
		auto retrievedProps = dataManager->getEdgeProperties(edge.getId());
		EXPECT_EQ(3, retrievedProps.size());
		EXPECT_DOUBLE_EQ(2.5, std::get<double>(retrievedProps.at("weight").getVariant()));
		EXPECT_EQ(2023, std::get<int64_t>(retrievedProps.at("since").getVariant()));
		EXPECT_TRUE(std::get<bool>(retrievedProps.at("active").getVariant()));

		// Remove a property
		dataManager->removeEdgeProperty(edge.getId(), "since");
		retrievedProps = dataManager->getEdgeProperties(edge.getId());
		EXPECT_EQ(2, retrievedProps.size());
		EXPECT_EQ(retrievedProps.find("since"), retrievedProps.end());
	}

	TEST_F(DataManagerTest, FindEdgesByNode) {
		// Create a small graph
		auto node1 = createTestNode("Node1");
		auto node2 = createTestNode("Node2");
		auto node3 = createTestNode("Node3");
		dataManager->addNode(node1);
		dataManager->addNode(node2);
		dataManager->addNode(node3);
		auto edge1 = createTestEdge(node1.getId(), node2.getId(), "CONNECTS_TO"); // node1 -> node2
		auto edge2 = createTestEdge(node2.getId(), node3.getId(), "CONNECTS_TO"); // node2 -> node3
		auto edge3 = createTestEdge(node3.getId(), node1.getId(), "CONNECTS_TO"); // node3 -> node1
		dataManager->addEdge(edge1);
		dataManager->addEdge(edge2);
		dataManager->addEdge(edge3);

		// Test finding outgoing edges for node1
		auto outEdges = dataManager->findEdgesByNode(node1.getId(), "out");
		ASSERT_EQ(1, outEdges.size());
		EXPECT_EQ(edge1.getId(), outEdges[0].getId());
		EXPECT_EQ(node2.getId(), outEdges[0].getTargetNodeId());

		// Test finding incoming edges for node1
		auto inEdges = dataManager->findEdgesByNode(node1.getId(), "in");
		ASSERT_EQ(1, inEdges.size());
		EXPECT_EQ(edge3.getId(), inEdges[0].getId());
		EXPECT_EQ(node3.getId(), inEdges[0].getSourceNodeId());

		// Test finding all connected edges for node1
		auto allEdges = dataManager->findEdgesByNode(node1.getId(), "both");
		EXPECT_EQ(2, allEdges.size());
	}

	TEST_F(DataManagerTest, PropertyEntityCRUD) {
		// Create a node to associate the property with
		auto node = createTestNode("PropertyHolder");
		dataManager->addNode(node);

		// 1. Create a Property entity
		std::unordered_map<std::string, PropertyValue> props = {{"test_key", PropertyValue("test_value")}};
		auto property = createTestProperty(node.getId(), Node::typeId, props);
		dataManager->addPropertyEntity(property);
		EXPECT_NE(0, property.getId()) << "Property should have a valid ID after being added.";

		// 2. Retrieve the Property entity
		auto retrievedProperty = dataManager->getProperty(property.getId());
		EXPECT_EQ(property.getId(), retrievedProperty.getId());
		EXPECT_EQ(node.getId(), retrievedProperty.getEntityId());
		ASSERT_TRUE(retrievedProperty.hasPropertyValue("test_key"));
		EXPECT_EQ("test_value",
				  std::get<std::string>(retrievedProperty.getPropertyValues().at("test_key").getVariant()));

		// 3. Update the Property entity
		props["test_key"] = PropertyValue(42);
		property.setProperties(props);
		dataManager->updatePropertyEntity(property);
		retrievedProperty = dataManager->getProperty(property.getId());
		EXPECT_EQ(42, std::get<int64_t>(retrievedProperty.getPropertyValues().at("test_key").getVariant()));

		// 4. Delete the Property entity
		dataManager->deleteProperty(property);
		dataManager->clearCache();
		retrievedProperty = dataManager->getProperty(property.getId());
		EXPECT_EQ(0, retrievedProperty.getId()) << "Deleted property should not be retrievable.";
	}

	TEST_F(DataManagerTest, BlobEntityCRUD) {
		// 1. Create
		std::string blobDataStr = "12345";
		auto blob = createTestBlob(blobDataStr);
		dataManager->addBlobEntity(blob);
		EXPECT_NE(0, blob.getId()) << "Blob should have a valid ID after being added.";

		// 2. Retrieve
		auto retrievedBlob = dataManager->getBlob(blob.getId());
		EXPECT_EQ(blob.getId(), retrievedBlob.getId());
		EXPECT_EQ(blobDataStr, retrievedBlob.getDataAsString());

		// 3. Update
		std::string newBlobDataStr = "67890";
		blob.setData(newBlobDataStr);
		dataManager->updateBlobEntity(blob);
		retrievedBlob = dataManager->getBlob(blob.getId());
		EXPECT_EQ(newBlobDataStr, retrievedBlob.getDataAsString());

		// 4. Delete
		dataManager->deleteBlob(blob);
		dataManager->clearCache();
		retrievedBlob = dataManager->getBlob(blob.getId());
		EXPECT_EQ(0, retrievedBlob.getId()) << "Deleted blob should not be retrievable.";
	}

	TEST_F(DataManagerTest, IndexEntityCRUD) {
		// 1. Create
		auto index = createTestIndex(Index::NodeType::LEAF, 1);
		dataManager->addIndexEntity(index);
		EXPECT_NE(0, index.getId()) << "Index should have a valid ID after being added.";

		// 2. Retrieve
		auto retrievedIndex = dataManager->getIndex(index.getId());
		EXPECT_EQ(index.getId(), retrievedIndex.getId());
		EXPECT_EQ(1U, retrievedIndex.getIndexType());
		EXPECT_EQ(Index::NodeType::LEAF, retrievedIndex.getNodeType());

		// 3. Update
		index.setLevel(5); // An arbitrary field to update
		dataManager->updateIndexEntity(index);
		retrievedIndex = dataManager->getIndex(index.getId());
		EXPECT_EQ(5, retrievedIndex.getLevel());

		// 4. Delete
		dataManager->deleteIndex(index);
		dataManager->clearCache();
		retrievedIndex = dataManager->getIndex(index.getId());
		EXPECT_EQ(0, retrievedIndex.getId()) << "Deleted index should not be retrievable.";
	}

	TEST_F(DataManagerTest, StateEntityCRUD) {
		// 1. Create
		auto state = createTestState("test.state.key");
		dataManager->addStateEntity(state);
		EXPECT_NE(0, state.getId()) << "State should have a valid ID after being added.";

		// 2. Retrieve
		auto retrievedState = dataManager->getState(state.getId());
		EXPECT_EQ(state.getId(), retrievedState.getId());
		EXPECT_EQ("test.state.key", retrievedState.getKey());

		// Test retrieval by key
		auto foundState = dataManager->findStateByKey("test.state.key");
		EXPECT_EQ(state.getId(), foundState.getId());

		// 3. Update
		state.setKey("updated.state.key");
		dataManager->updateStateEntity(state);
		// After updating the key, the old key should not find it anymore
		auto notFoundState = dataManager->findStateByKey("test.state.key");
		EXPECT_EQ(0, notFoundState.getId());
		// The new key should find it
		foundState = dataManager->findStateByKey("updated.state.key");
		EXPECT_EQ(state.getId(), foundState.getId());
		EXPECT_EQ("updated.state.key", foundState.getKey());

		// 4. Delete
		dataManager->deleteState(state);
		dataManager->clearCache();
		retrievedState = dataManager->getState(state.getId());
		EXPECT_EQ(0, retrievedState.getId()) << "Deleted state should not be retrievable.";
	}

	TEST_F(DataManagerTest, StateProperties) {
		// Create a state
		auto state = createTestState("config.state");
		dataManager->addStateEntity(state);

		// Add properties to the state
		std::unordered_map<std::string, PropertyValue> properties = {{"setting1", PropertyValue("value1")},
																	 {"setting2", PropertyValue(42)},
																	 {"setting3", PropertyValue(true)}};
		dataManager->addStateProperties("config.state", properties);

		// Retrieve properties and verify
		auto retrievedProps = dataManager->getStateProperties("config.state");
		EXPECT_EQ(3, retrievedProps.size());
		EXPECT_EQ("value1", std::get<std::string>(retrievedProps.at("setting1").getVariant()));
		EXPECT_EQ(42, std::get<int64_t>(retrievedProps.at("setting2").getVariant()));
		EXPECT_TRUE(std::get<bool>(retrievedProps.at("setting3").getVariant()));

		// Test removeState, which should also remove its properties
		dataManager->removeState("config.state");
		auto foundState = dataManager->findStateByKey("config.state");
		EXPECT_EQ(0, foundState.getId()) << "State should have been removed.";

		auto emptyProps = dataManager->getStateProperties("config.state");
		EXPECT_TRUE(emptyProps.empty()) << "State properties should have been removed with the state.";
	}

	TEST_F(DataManagerTest, BatchOperations) {
		// Create multiple nodes
		std::vector<int64_t> nodeIds;
		for (int i = 0; i < 10; i++) {
			auto node = createTestNode("BatchNode" + std::to_string(i));
			dataManager->addNode(node);
			nodeIds.push_back(node.getId());
		}

		// Test getNodeBatch
		auto nodes = dataManager->getNodeBatch(nodeIds);
		ASSERT_EQ(10, nodes.size());
		for (size_t i = 0; i < nodes.size(); i++) {
			EXPECT_EQ(nodeIds[i], nodes[i].getId());
			EXPECT_EQ("BatchNode" + std::to_string(i), nodes[i].getLabel());
		}

		// Test getNodesInRange with a limit
		auto rangeNodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 5);
		EXPECT_EQ(5, rangeNodes.size());

		// Test getNodesInRange for all nodes in the range
		rangeNodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 20);
		EXPECT_EQ(10, rangeNodes.size());
	}

	TEST_F(DataManagerTest, CacheManagement) {
		// Add an entity to populate the cache
		auto node = createTestNode("CacheNode");
		dataManager->addNode(node);

		// Retrieving it should hit the cache
		auto retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ(node.getId(), retrievedNode.getId()) << "Node should be in cache after creation.";

		// Clear the cache
		dataManager->clearCache();

		// The entity should still be retrievable, this time loading from disk
		retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ(node.getId(), retrievedNode.getId()) << "Node should be retrievable from disk after cache clear.";
	}

	TEST_F(DataManagerTest, DirtyTracking) {
		// Add a node, which should mark it as a dirty "ADDED" entity
		auto node = createTestNode("DirtyNode");
		dataManager->addNode(node);
		EXPECT_TRUE(dataManager->hasUnsavedChanges());

		// Verify it's in the dirty map with the correct change type
		auto dirtyNodes = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::ADDED});
		ASSERT_EQ(1, dirtyNodes.size());
		EXPECT_EQ(EntityChangeType::ADDED, dirtyNodes[0].changeType);
		EXPECT_EQ(node.getId(), dirtyNodes[0].backup->getId());

		// Simulate a save operation
		dataManager->markAllSaved();
		EXPECT_FALSE(dataManager->hasUnsavedChanges());

		// Update the node, which should mark it as a dirty "MODIFIED" entity
		node.setLabel("UpdatedDirtyNode");
		dataManager->updateNode(node);
		EXPECT_TRUE(dataManager->hasUnsavedChanges());

		// Verify it's in the dirty map again with the "MODIFIED" change type
		dirtyNodes = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::MODIFIED});
		ASSERT_EQ(1, dirtyNodes.size());
		EXPECT_EQ(EntityChangeType::MODIFIED, dirtyNodes[0].changeType);
	}

	TEST_F(DataManagerTest, AutoFlush) {
		bool autoFlushCalled = false;

		// Set up auto-flush callback and a low threshold
		dataManager->setMaxDirtyEntities(5);
		dataManager->setAutoFlushCallback([&autoFlushCalled]() { autoFlushCalled = true; });

		// Add entities until the dirty entity count is equal to the threshold
		for (int i = 0; i < 4; i++) {
			auto node = createTestNode("AutoFlushNode" + std::to_string(i));
			dataManager->addNode(node);
		}
		// The 4th entity (index 3) brings the count to 4. Still no flush.
		EXPECT_FALSE(autoFlushCalled);

		// The 5th entity addition should NOT trigger the flush yet (count == threshold)
		// because the check is `dirtyNodes_.size() >= maxDirtyEntities_`
		auto node = createTestNode("ThresholdNode");
		dataManager->addNode(node);
		EXPECT_TRUE(autoFlushCalled) << "Auto-flush should have been triggered when dirty count reached the max.";
	}

	TEST_F(DataManagerTest, ObserverNotifications) {
		observer->reset(); // Start with a clean slate

		// --- PHASE 1: ADDITIONS ---
		auto source = createTestNode("Source");
		auto target = createTestNode("Target");
		dataManager->addNode(source);
		dataManager->addNode(target);
		auto edge = createTestEdge(source.getId(), target.getId(), "RELATES_TO");
		dataManager->addEdge(edge);

		// Verify notifications for additions
		EXPECT_EQ(2, observer->addedNodes.size());
		EXPECT_EQ(1, observer->addedEdges.size());
		EXPECT_EQ(0, observer->updatedNodes.size());
		EXPECT_EQ(0, observer->updatedEdges.size());

		// --- PHASE 2: UPDATES (User-initiated) ---
		dataManager->markAllSaved();
		observer->reset();

		// Update entities directly
		Node oldSource = dataManager->getNode(source.getId());
		source.setLabel("UpdatedSource");
		dataManager->updateNode(source);

		Edge oldEdge = dataManager->getEdge(edge.getId());
		edge.setLabel("UPDATED_RELATION");
		dataManager->updateEdge(edge);

		// Verify notifications for direct updates
		EXPECT_EQ(1, observer->updatedNodes.size());
		EXPECT_EQ(1, observer->updatedEdges.size());

		// --- PHASE 3: DELETIONS (and resulting system-initiated updates) ---
		observer->reset();

		// Delete entities
		dataManager->deleteEdge(edge);
		dataManager->deleteNode(target);

		// Verify notifications for deletions
		EXPECT_EQ(1, observer->deletedNodes.size()) << "Should be notified of the target node's deletion.";
		EXPECT_EQ(target.getId(), observer->deletedNodes[0].getId());

		EXPECT_EQ(1, observer->deletedEdges.size()) << "Should be notified of the edge's deletion.";
		EXPECT_EQ(edge.getId(), observer->deletedEdges[0].getId());

		// According to the design principle that "any update must be notified",
		// we now EXPECT two node updates as a result of the edge deletion.
		// This test now correctly validates the implemented behavior.
		EXPECT_EQ(2, observer->updatedNodes.size()) << "Deleting an edge MUST trigger update notifications for its "
													   "connected nodes, as per the 'Total Transparency' design.";

		// We can even add more specific checks if needed, e.g., verifying the IDs of the updated nodes.
		std::vector<int64_t> updatedNodeIds;
		for (const auto &pair: observer->updatedNodes) {
			updatedNodeIds.push_back(pair.second.getId());
		}
		ASSERT_TRUE(std::find(updatedNodeIds.begin(), updatedNodeIds.end(), source.getId()) != updatedNodeIds.end());
		ASSERT_TRUE(std::find(updatedNodeIds.begin(), updatedNodeIds.end(), target.getId()) != updatedNodeIds.end());

		// Verify no other unexpected notifications were triggered
		EXPECT_EQ(0, observer->addedNodes.size());
		EXPECT_EQ(0, observer->addedEdges.size());
		EXPECT_EQ(0, observer->updatedEdges.size());
	}

	TEST_F(DataManagerTest, MultipleEntityTypes) {
		// Create one of each entity type to ensure they can coexist in a dirty state
		auto node = createTestNode("MultiTypeNode");
		dataManager->addNode(node);

		auto state = createTestState("multi.type.state");
		dataManager->addStateEntity(state);

		auto index = createTestIndex(Index::NodeType::INTERNAL, 2);
		dataManager->addIndexEntity(index);

		std::string blobData = "some binary data";
		auto blob = createTestBlob(blobData);
		dataManager->addBlobEntity(blob);

		// Verify each can be retrieved correctly
		auto retrievedNode = dataManager->getNode(node.getId());
		EXPECT_EQ("MultiTypeNode", retrievedNode.getLabel());

		auto retrievedState = dataManager->findStateByKey("multi.type.state");
		EXPECT_EQ(state.getId(), retrievedState.getId());

		auto retrievedIndex = dataManager->getIndex(index.getId());
		EXPECT_EQ(2U, retrievedIndex.getIndexType());

		auto retrievedBlob = dataManager->getBlob(blob.getId());
		EXPECT_EQ(blobData, retrievedBlob.getDataAsString());

		// Verify dirty tracking for multiple entity types
		EXPECT_TRUE(dataManager->hasUnsavedChanges());
		EXPECT_EQ(1, dataManager->getDirtyEntityInfos<Node>({EntityChangeType::ADDED}).size());
		EXPECT_EQ(1, dataManager->getDirtyEntityInfos<State>({EntityChangeType::ADDED}).size());
		EXPECT_EQ(1, dataManager->getDirtyEntityInfos<Index>({EntityChangeType::ADDED}).size());
		EXPECT_EQ(1, dataManager->getDirtyEntityInfos<Blob>({EntityChangeType::ADDED}).size());

		// Mark all saved and verify state is clean
		dataManager->markAllSaved();
		EXPECT_FALSE(dataManager->hasUnsavedChanges());
	}


	TEST_F(DataManagerTest, LargeEntityCounts) {
		// Test performance and stability with a larger number of entities
		constexpr int entityCount = 100;

		std::vector<Node> nodes;
		std::vector<int64_t> nodeIds;
		nodes.reserve(entityCount);
		nodeIds.reserve(entityCount);

		// Create nodes
		for (int i = 0; i < entityCount; i++) {
			auto node = createTestNode("LargeNode" + std::to_string(i));
			dataManager->addNode(node);
			nodes.push_back(node);
			nodeIds.push_back(node.getId());
		}

		// Create edges between nodes
		for (int i = 0; i < entityCount - 1; i++) {
			auto edge = createTestEdge(nodes[i].getId(), nodes[i + 1].getId(), "NEXT");
			dataManager->addEdge(edge);
		}

		// Add properties to nodes
		for (int i = 0; i < entityCount; i++) {
			std::unordered_map<std::string, PropertyValue> props = {
					{"index", PropertyValue(i)},
					{"name", PropertyValue("Node" + std::to_string(i))},
			};
			dataManager->addNodeProperties(nodes[i].getId(), props);
		}

		// Retrieve in batches
		for (int i = 0; i < entityCount; i += 10) {
			int end = std::min(i + 10, entityCount);
			std::vector<int64_t> batchIds(nodeIds.begin() + i, nodeIds.begin() + end);
			auto batchNodes = dataManager->getNodeBatch(batchIds);
			EXPECT_EQ(batchIds.size(), batchNodes.size());
		}

		// Test clearing cache with many entities loaded
		dataManager->clearCache();

		// Verify we can still retrieve an entity from disk
		const int middleIndex = entityCount / 2;
		auto retrievedNode = dataManager->getNode(nodes[middleIndex].getId());
		EXPECT_EQ(nodes[middleIndex].getId(), retrievedNode.getId());

		// Verify we can still retrieve its properties
		auto props = dataManager->getNodeProperties(nodes[middleIndex].getId());
		EXPECT_EQ(middleIndex, std::get<int64_t>(props.at("index").getVariant()));
	}

} // namespace graph::storage::test
