/**
 * @file test_BaseEntityManager.cpp
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
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/BaseEntityManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/NodeManager.hpp"

class BaseEntityManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_baseEntityManager_" + to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Get the entity managers
		nodeManager = dataManager->getNodeManager();
		edgeManager = dataManager->getEdgeManager();
	}

	void TearDown() override {
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// Helper to create a Node using the correct API
	static graph::Node createTestNode(const std::shared_ptr<graph::storage::DataManager> &dm, const std::string &label) {
		graph::Node node;
		node.setLabelId(dm->getOrCreateLabelId(label));
		return node;
	}

	// Helper to create an Edge using the correct API
	static graph::Edge createTestEdge(const std::shared_ptr<graph::storage::DataManager> &dm, int64_t sourceId, int64_t targetId, const std::string &label) {
		graph::Edge edge;
		edge.setSourceNodeId(sourceId);
		edge.setTargetNodeId(targetId);
		edge.setLabelId(dm->getOrCreateLabelId(label));
		return edge;
	}

	// New Helper: Get dirty entities from DataManager using new API
	template<typename T>
	std::vector<T> getDirtyEntities(const std::vector<graph::storage::EntityChangeType> &types) {
		auto infos = dataManager->getDirtyEntityInfos<T>(types);
		std::vector<T> result;
		for (const auto &info: infos) {
			if (info.backup.has_value()) {
				result.push_back(*info.backup);
			}
		}
		return result;
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::NodeManager> nodeManager;
	std::shared_ptr<graph::storage::EdgeManager> edgeManager;
};

// Tests for add method
TEST_F(BaseEntityManagerTest, AddNodeEntity) {
	graph::Node node = createTestNode(dataManager, "TestNode");
	nodeManager->add(node);

	EXPECT_NE(node.getId(), 0);

	graph::Node retrievedNode = nodeManager->get(node.getId());
	EXPECT_EQ(retrievedNode.getId(), node.getId());

	EXPECT_EQ(retrievedNode.getLabelId(), node.getLabelId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedNode.getLabelId()), "TestNode");

	EXPECT_TRUE(retrievedNode.isActive());
}

// Tests for update method
TEST_F(BaseEntityManagerTest, UpdateNodeEntity) {
	graph::Node node = createTestNode(dataManager, "OriginalLabel");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedLabel"));
	nodeManager->update(node);

	graph::Node retrievedNode = nodeManager->get(nodeId);
	EXPECT_EQ(dataManager->resolveLabel(retrievedNode.getLabelId()), "UpdatedLabel");
}

// Tests for remove method
TEST_F(BaseEntityManagerTest, RemoveNodeEntity) {
	// Create and add a node
	graph::Node node = createTestNode(dataManager, "NodeToRemove");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Remove the node
	nodeManager->remove(node);

	// Test retrieval from memory (Dirty Layer)
	// The entity should still be retrievable from dirty memory but marked inactive
	// OR depending on implementation, get() might return empty/inactive if DELETED.
	// PersistenceManager::get() returns backup if present.
	// Let's check if it's considered "deleted" by checking active status
	graph::Node retrievedNode = nodeManager->get(nodeId);
	EXPECT_EQ(retrievedNode.getId(), 0) << "Removed node should return empty entity (ID 0) from memory.";

	// Test retrieval from disk after cache clear
	// Since we haven't flushed, disk should have nothing or old state.
	// But since it's a new node never saved, disk has nothing.
	dataManager->clearCache();
	graph::Node retrievedFromDisk = dataManager->loadNodeFromDisk(nodeId);
	EXPECT_EQ(retrievedFromDisk.getId(), 0) << "Deleted node should not be retrievable from disk.";
}

// Tests for get method
TEST_F(BaseEntityManagerTest, GetNonExistentNode) {
	graph::Node node = nodeManager->get(999999);

	// Should return a default-constructed node with ID 0
	EXPECT_EQ(node.getId(), 0);
}

// Tests for getBatch method
TEST_F(BaseEntityManagerTest, GetBatchOfNodes) {
	// Create and add multiple nodes
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 5; i++) {
		graph::Node node = createTestNode(dataManager, "BatchNode" + std::to_string(i));
		nodeManager->add(node);
		nodeIds.push_back(node.getId());
	}

	// Mark one node as inactive
	graph::Node nodeToRemove = nodeManager->get(nodeIds[2]);
	nodeManager->remove(nodeToRemove);

	// Get batch of nodes
	std::vector<graph::Node> nodes = nodeManager->getBatch(nodeIds);

	// Should only return active nodes
	EXPECT_EQ(nodes.size(), 4UL);

	// Verify correct nodes were returned
	for (const auto &node: nodes) {
		EXPECT_TRUE(node.isActive());
		EXPECT_NE(node.getId(), nodeIds[2]);
	}
}

// Tests for getInRange method
TEST_F(BaseEntityManagerTest, GetNodesInRange) {
	std::vector<int64_t> nodeIds;
	int64_t startId = 0;
	for (int i = 0; i < 10; i++) {
		graph::Node node = createTestNode(dataManager, "RangeNode" + std::to_string(i));
		nodeManager->add(node);
		nodeIds.push_back(node.getId());
		if (i == 0)
			startId = node.getId();
	}

	// Get nodes in range
	std::vector<graph::Node> nodes = nodeManager->getInRange(startId + 2, startId + 6, 10);

	// Verify correct range was returned
	EXPECT_EQ(nodes.size(), 5UL);
	for (const auto &node: nodes) {
		EXPECT_GE(node.getId(), startId + 2);
		EXPECT_LE(node.getId(), startId + 6);
	}

	// Test with limit
	nodes = nodeManager->getInRange(startId + 2, startId + 6, 2);
	EXPECT_EQ(nodes.size(), 2UL);
}

// Tests for property management
TEST_F(BaseEntityManagerTest, NodePropertyManagement) {
	graph::Node node = createTestNode(dataManager, "PropertyNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["name"] = std::string("TestName");
	props["age"] = 30;
	props["active"] = true;
	nodeManager->addProperties(nodeId, props);

	auto retrievedProps = nodeManager->getProperties(nodeId);

	EXPECT_EQ(retrievedProps.size(), 3UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "TestName");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["age"].getVariant()), 30);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);

	nodeManager->removeProperty(nodeId, "age");

	retrievedProps = nodeManager->getProperties(nodeId);
	EXPECT_EQ(retrievedProps.size(), 2UL);
	EXPECT_EQ(retrievedProps.count("age"), 0UL);
}

// Tests for Edge entity
TEST_F(BaseEntityManagerTest, EdgeEntityOperations) {
	graph::Node sourceNode = createTestNode(dataManager, "Source");
	graph::Node targetNode = createTestNode(dataManager, "Target");
	nodeManager->add(sourceNode);
	nodeManager->add(targetNode);

	graph::Edge edge = createTestEdge(dataManager, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);

	graph::Edge retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedEdge.getLabelId()), "CONNECTS_TO");

	edge.setLabelId(dataManager->getOrCreateLabelId("RELATED_TO"));
	edgeManager->update(edge);

	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedEdge.getLabelId()), "RELATED_TO");

	edgeManager->remove(edge);

	// Check deletion status
	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), 0) << "Removed edge should return empty entity (ID 0).";

	dataManager->clearCache();
	auto retrievedFromDisk = dataManager->loadEdgeFromDisk(edge.getId());
	EXPECT_EQ(retrievedFromDisk.getId(), 0);
}

// Tests for PersistenceManager Snapshot Commit (Replacing MarkAllSaved)
TEST_F(BaseEntityManagerTest, SnapshotCommitClearsDirty) {
	// Create and add multiple nodes
	for (int i = 0; i < 3; i++) {
		graph::Node node = createTestNode(dataManager, "SavedNode" + std::to_string(i));
		nodeManager->add(node);
	}

	// 1. Prepare Snapshot (Move to Flushing Layer)
	auto snapshot = dataManager->prepareFlushSnapshot();

	// 2. Commit Snapshot (Clear Flushing Layer)
	dataManager->commitFlushSnapshot();

	// Verify dirty list is empty
	// Using new API helper getDirtyEntities
	auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED,
													 graph::storage::EntityChangeType::MODIFIED,
													 graph::storage::EntityChangeType::DELETED});

	EXPECT_TRUE(dirtyNodes.empty());
}

// Tests for getDirtyEntityInfos (Replacing getDirtyWithChangeTypes)
TEST_F(BaseEntityManagerTest, GetDirtyWithChangeTypes) {
	// --- Setup ---

	// 1. ADDED
    graph::Node addedNode = createTestNode(dataManager, "AddedNode");
    nodeManager->add(addedNode);

    // 2. ADDED then MODIFIED
    graph::Node modifiedNode = createTestNode(dataManager, "OriginalLabel");
    nodeManager->add(modifiedNode);

    modifiedNode.setLabelId(dataManager->getOrCreateLabelId("ModifiedLabel"));
    nodeManager->update(modifiedNode);

    // 3. ADDED then REMOVED
    graph::Node removedNode = createTestNode(dataManager, "RemovedNode");
    nodeManager->add(removedNode);
    nodeManager->remove(removedNode);

	// --- Assertions ---

	// Get only ADDED nodes
	auto addedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
	// Depending on strict logic:
	// addedNode -> ADDED
	// modifiedNode -> ADDED (because updateEntityImpl preserved ADDED state)
	EXPECT_EQ(addedNodes.size(), 2UL);

	// Verify IDs
	std::set<int64_t> addedIds;
	for (auto &n: addedNodes)
		addedIds.insert(n.getId());
	EXPECT_TRUE(addedIds.count(addedNode.getId()));
	EXPECT_TRUE(addedIds.count(modifiedNode.getId()));


	// Get only MODIFIED nodes
	auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
	EXPECT_EQ(modifiedNodes.size(), 0UL);


	// Since 'removedNode' was only in memory (ADDED), deleting it simply removes it
	// from the dirty registry completely. It does NOT create a DELETED record.
	auto deletedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::DELETED});
	// In current implementation, we upsert a DELETED record.
	EXPECT_EQ(deletedNodes.size(), 0UL);

	// Get all dirty nodes. Total = 3 (2 Added + 1 Deleted)
	auto allDirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED,
														graph::storage::EntityChangeType::MODIFIED,
														graph::storage::EntityChangeType::DELETED});
	EXPECT_EQ(allDirtyNodes.size(), 2UL);
}

TEST_F(BaseEntityManagerTest, ExplicitCacheOperations) {
	// 1. Setup: Create and add a node (implicitly adds to cache)
	graph::Node node = createTestNode(dataManager, "CacheTestNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Verify it is currently in the cache (DataManager exposes specific caches)
	EXPECT_TRUE(dataManager->getNodeCache().contains(nodeId));

	// 2. Test clearCache(): Should remove all items from the specific entity cache
	nodeManager->clearCache();

	// Verify cache is empty
	EXPECT_FALSE(dataManager->getNodeCache().contains(nodeId));
	EXPECT_EQ(dataManager->getNodeCache().size(), 0UL);

	// 3. Test addToCache(): Should manually put the entity back
	nodeManager->addToCache(node);

	// Verify it is back in cache
	EXPECT_TRUE(dataManager->getNodeCache().contains(nodeId));
	EXPECT_EQ(dataManager->getNodeCache().size(), 1UL);
}

TEST_F(BaseEntityManagerTest, AddBatchNodes) {
	// Prepare a vector of nodes without IDs
	std::vector<graph::Node> nodes;
	constexpr int BATCH_SIZE = 5;
	for (int i = 0; i < BATCH_SIZE; ++i) {
		nodes.push_back(createTestNode(dataManager, "Batch" + std::to_string(i)));
	}

	// Execute Batch Add
	nodeManager->addBatch(nodes);

	// Verify IDs were assigned sequentially and entities persist
	int64_t prevId = 0;
	for (const auto& node : nodes) {
		EXPECT_NE(node.getId(), 0);
		EXPECT_GT(node.getId(), prevId); // Sequential check
		prevId = node.getId();

		// Verify retrieval
		graph::Node retrieved = nodeManager->get(node.getId());
		EXPECT_EQ(retrieved.getId(), node.getId());
		EXPECT_TRUE(retrieved.isActive());
	}

	// Verify they are in the dirty list (ADDED state)
	auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
	// + any previous tests' nodes if not cleared, but at least these 5 should exist
	int countFound = 0;
	for (const auto& dn : dirtyNodes) {
		for (const auto& original : nodes) {
			if (dn.getId() == original.getId()) countFound++;
		}
	}
	EXPECT_EQ(countFound, BATCH_SIZE);
}

// NOTE: ZombieManagerSafety test has been removed because it's no longer applicable.
// The new design uses assert() to guarantee DataManager validity, making null DataManager
// scenarios impossible by design. DataManager's lifecycle is guaranteed to be longer than
// all EntityManager instances by the ownership hierarchy (DataManager owns EntityManager).
//
// If DataManager could be null, it indicates a programming error that should trigger the
// assertion for early detection in debug builds, rather than silently returning default values.
//
// The old ZombieManagerSafety test checked defensive code paths that are intentionally
// removed to simplify the code and improve coverage (untestable branches).

// Additional tests to improve branch coverage

// Test addBatch with empty vector
// Tests branch at line 50: if (entities.empty()) return;
TEST_F(BaseEntityManagerTest, AddBatchEmptyVector) {
    std::vector<graph::Node> emptyNodes;
    EXPECT_NO_THROW(nodeManager->addBatch(emptyNodes));

    // Verify no nodes were added
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
    size_t previousCount = dirtyNodes.size();

    nodeManager->addBatch(emptyNodes);

    dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
    EXPECT_EQ(dirtyNodes.size(), previousCount);
}

// Test update with entity that has ID = 0
// Tests branch at line 96-98: if (entity.getId() == 0) return;
TEST_F(BaseEntityManagerTest, UpdateEntityWithZeroId) {
    graph::Node node;
    node.setId(0); // Explicitly set to zero
    node.setLabelId(dataManager->getOrCreateLabelId("Test"));

    EXPECT_NO_THROW(nodeManager->update(node));

    // Node should not have been added to dirty map
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
    for (const auto &dn: dirtyNodes) {
        EXPECT_NE(dn.getLabelId(), node.getLabelId());
    }
}

// Test update with inactive entity throws exception
// Tests branch at line 100-101: if (!entity.isActive()) throw
TEST_F(BaseEntityManagerTest, UpdateInactiveEntityThrows) {
    graph::Node node = createTestNode(dataManager, "InactiveNode");
    nodeManager->add(node);

    // Mark node as inactive
    node.markInactive();

    EXPECT_THROW(nodeManager->update(node), std::runtime_error);
}

// Test remove with entity that has ID = 0
// Tests branch at line 121-123: if (entity.getId() == 0 || !entity.isActive()) return;
TEST_F(BaseEntityManagerTest, RemoveEntityWithZeroId) {
    graph::Node node;
    node.setId(0);

    EXPECT_NO_THROW(nodeManager->remove(node));

    // Should not affect deletion flag
    // (This tests the early return path)
}

// Test remove with inactive entity
// Tests branch at line 121-123: inactive check
TEST_F(BaseEntityManagerTest, RemoveInactiveEntity) {
    graph::Node node = createTestNode(dataManager, "AlreadyInactive");
    node.markInactive(); // Mark as inactive before adding

    EXPECT_NO_THROW(nodeManager->remove(node));

    // Should not have been added or marked for deletion
}

// Test getBatch with non-existent IDs
// Tests branch at line 154: if (entity.getId() != 0 && entity.isActive())
TEST_F(BaseEntityManagerTest, GetBatchWithNonExistentIds) {
    std::vector<int64_t> nonExistentIds = {99990, 99991, 99992, 99993, 99994};

    auto nodes = nodeManager->getBatch(nonExistentIds);

    EXPECT_TRUE(nodes.empty());
}

// Test getBatch with mix of existent and non-existent IDs
TEST_F(BaseEntityManagerTest, GetBatchMixedIds) {
    // Create some nodes
    std::vector<int64_t> existingIds;
    for (int i = 0; i < 3; i++) {
        graph::Node node = createTestNode(dataManager, "Mixed" + std::to_string(i));
        nodeManager->add(node);
        existingIds.push_back(node.getId());
    }

    // Mix with non-existent IDs
    std::vector<int64_t> mixedIds = {
        existingIds[0],
        99999, // non-existent
        existingIds[1],
        88888, // non-existent
        existingIds[2]
    };

    auto nodes = nodeManager->getBatch(mixedIds);

    // Should only return existing (active) nodes
    EXPECT_EQ(nodes.size(), 3UL);
    for (const auto &node: nodes) {
        EXPECT_TRUE(node.isActive());
    }
}

// Test getInRange with no results
TEST_F(BaseEntityManagerTest, GetInRangeNoResults) {
    // Request range where no nodes exist
    auto nodes = nodeManager->getInRange(50000, 50100, 100);

    EXPECT_TRUE(nodes.empty());
}

// Test getInRange with startId > endId
TEST_F(BaseEntityManagerTest, GetInRangeInvalidRange) {
    auto nodes = nodeManager->getInRange(100, 50, 10);

    EXPECT_TRUE(nodes.empty());
}

// Test getProperties with non-existent entity
// Tests branch at line 193-194: if (!dataManager) return {}
TEST_F(BaseEntityManagerTest, GetPropertiesNonExistent) {
    auto props = nodeManager->getProperties(99999);

    EXPECT_TRUE(props.empty());
}

// Test addProperties with non-existent entity
TEST_F(BaseEntityManagerTest, AddPropertiesNonExistent) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    props["test"] = 42;

    // PropertyManager throws for non-existent entities
    EXPECT_THROW(nodeManager->addProperties(99999, props), std::runtime_error);
}

// Test removeProperty from non-existent entity
TEST_F(BaseEntityManagerTest, RemovePropertyNonExistent) {
    // Should not throw, just no-op
    EXPECT_NO_THROW(nodeManager->removeProperty(99999, "anyKey"));
}

// Test removeProperty with non-existent property
TEST_F(BaseEntityManagerTest, RemoveNonExistentProperty) {
    graph::Node node = createTestNode(dataManager, "PropertyTest");
    nodeManager->add(node);

    // Remove property that doesn't exist - should not throw
    EXPECT_NO_THROW(nodeManager->removeProperty(node.getId(), "nonExistentKey"));
}

// Test update when entity is in ADDED state (preserves ADDED)
// Tests branch at line 108-110: dirtyInfo->changeType == EntityChangeType::ADDED
TEST_F(BaseEntityManagerTest, UpdatePreservesAddedState) {
    graph::Node node = createTestNode(dataManager, "AddedStateTest");
    nodeManager->add(node);

    // Update before flushing - should stay in ADDED state
    node.setLabelId(dataManager->getOrCreateLabelId("Updated"));
    nodeManager->update(node);

    // Verify still in ADDED state (not MODIFIED)
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
    bool found = false;
    for (const auto &dn: dirtyNodes) {
        if (dn.getId() == node.getId()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Node should still be in ADDED state after update";

    auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
    found = false;
    for (const auto &dn: modifiedNodes) {
        if (dn.getId() == node.getId()) {
            found = true;
            break;
        }
    }
    EXPECT_FALSE(found) << "Node should not be in MODIFIED state";
}

// Test update when entity is in MODIFIED state
// Tests else branch at line 111-114: EntityChangeType::MODIFIED
TEST_F(BaseEntityManagerTest, UpdateCreatesModifiedState) {
    graph::Node node = createTestNode(dataManager, "ModifiedStateTest");
    nodeManager->add(node);

    // Flush to move from ADDED to persistent state
    dataManager->prepareFlushSnapshot();
    dataManager->commitFlushSnapshot();

    // Now update - should create MODIFIED state
    node.setLabelId(dataManager->getOrCreateLabelId("Modified"));
    nodeManager->update(node);

    // Verify in MODIFIED state
    auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
    bool found = false;
    for (const auto &dn: modifiedNodes) {
        if (dn.getId() == node.getId()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Node should be in MODIFIED state after flush and update";
}

// Test getBatch filters out inactive entities
// Tests branch at line 154: if (entity.getId() != 0 && entity.isActive())
TEST_F(BaseEntityManagerTest, GetBatchFiltersInactive) {
    // Create nodes
    std::vector<int64_t> nodeIds;
    for (int i = 0; i < 5; i++) {
        graph::Node node = createTestNode(dataManager, "FilterTest" + std::to_string(i));
        nodeManager->add(node);
        nodeIds.push_back(node.getId());
    }

    // Mark some as inactive
    graph::Node node2 = nodeManager->get(nodeIds[1]);
    nodeManager->remove(node2);

    graph::Node node4 = nodeManager->get(nodeIds[3]);
    nodeManager->remove(node4);

    // Get batch - should only return active nodes
    auto nodes = nodeManager->getBatch(nodeIds);

    EXPECT_EQ(nodes.size(), 3UL);
    for (const auto &node: nodes) {
        EXPECT_TRUE(node.isActive());
        EXPECT_NE(node.getId(), nodeIds[1]);
        EXPECT_NE(node.getId(), nodeIds[3]);
    }
}

// Test addToCache directly
TEST_F(BaseEntityManagerTest, AddToCacheDirect) {
    graph::Node node = createTestNode(dataManager, "DirectCache");
    node.setId(12345); // Manual ID

    nodeManager->addToCache(node);

    // Verify in cache
    EXPECT_TRUE(dataManager->getNodeCache().contains(12345));

    // Can retrieve it
    graph::Node retrieved = nodeManager->get(12345);
    EXPECT_EQ(retrieved.getId(), 12345);
}

// Test clearCache clears all entities
TEST_F(BaseEntityManagerTest, ClearCacheRemovesAll) {
    // Add multiple nodes
    std::vector<int64_t> nodeIds;
    for (int i = 0; i < 10; i++) {
        graph::Node node = createTestNode(dataManager, "ClearTest" + std::to_string(i));
        nodeManager->add(node);
        nodeIds.push_back(node.getId());
    }

    // Verify all in cache
    for (int64_t id: nodeIds) {
        EXPECT_TRUE(dataManager->getNodeCache().contains(id));
    }

    // Clear cache
    nodeManager->clearCache();

    // Verify none in cache
    EXPECT_EQ(dataManager->getNodeCache().size(), 0UL);
    for (int64_t id: nodeIds) {
        EXPECT_FALSE(dataManager->getNodeCache().contains(id));
    }
}

// =========================================================================
// Coverage Improvement Test to Reach 85%
// =========================================================================

// Test addBatch with entities that already have IDs
// Tests uncovered branches:
//   Line 59: if (entity.getId() == 0) - False branch (entity has non-zero ID)
//   Line 66: if (!newEntities.empty()) - False branch (no entities need new IDs)
TEST_F(BaseEntityManagerTest, AddBatchWithExistingIds) {
    // Create nodes with pre-existing IDs (simulating re-adding existing entities)
    std::vector<graph::Node> nodes;
    nodes.reserve(3);

    // Create nodes with explicit non-zero IDs
    for (int i = 0; i < 3; ++i) {
        graph::Node node = createTestNode(dataManager, "ExistingId" + std::to_string(i));
        node.setId(1000 + i); // Explicitly set non-zero IDs
        nodes.push_back(node);
    }

    // Execute Batch Add with entities that already have IDs
    // This should skip ID allocation (line 66 False branch)
    // because all entities have non-zero IDs (line 59 False branch)
    EXPECT_NO_THROW(nodeManager->addBatch(nodes));

    // Verify IDs were NOT changed (no new allocation occurred)
    for (size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_EQ(nodes[i].getId(), 1000 + static_cast<int64_t>(i));

        // Verify they are accessible
        graph::Node retrieved = nodeManager->get(nodes[i].getId());
        EXPECT_EQ(retrieved.getId(), nodes[i].getId());
        EXPECT_TRUE(retrieved.isActive());
    }

    // Verify they are in the dirty list (ADDED state)
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
    int countFound = 0;
    for (const auto& dn : dirtyNodes) {
        for (const auto& original : nodes) {
            if (dn.getId() == original.getId()) countFound++;
        }
    }
    EXPECT_EQ(countFound, 3);
}