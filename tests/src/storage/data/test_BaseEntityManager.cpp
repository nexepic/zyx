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
		// Release shared_ptrs before closing database
		edgeManager.reset();
		nodeManager.reset();
		dataManager.reset();
		fileStorage.reset();

		if (database) {
			database->close();
		}
		database.reset();

		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
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
	auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED,
													 graph::storage::EntityChangeType::CHANGE_MODIFIED,
													 graph::storage::EntityChangeType::CHANGE_DELETED});

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
	auto addedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED});
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
	auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_MODIFIED});
	EXPECT_EQ(modifiedNodes.size(), 0UL);


	// Since 'removedNode' was only in memory (ADDED), deleting it simply removes it
	// from the dirty registry completely. It does NOT create a DELETED record.
	auto deletedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_DELETED});
	// In current implementation, we upsert a DELETED record.
	EXPECT_EQ(deletedNodes.size(), 0UL);

	// Get all dirty nodes. Total = 3 (2 Added + 1 Deleted)
	auto allDirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED,
														graph::storage::EntityChangeType::CHANGE_MODIFIED,
														graph::storage::EntityChangeType::CHANGE_DELETED});
	EXPECT_EQ(allDirtyNodes.size(), 2UL);
}

TEST_F(BaseEntityManagerTest, ExplicitCacheOperations) {
	// 1. Setup: Create and add a node (goes to dirty registry)
	graph::Node node = createTestNode(dataManager, "CacheTestNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Entity should be retrievable (from dirty registry)
	graph::Node retrieved = nodeManager->get(nodeId);
	EXPECT_EQ(retrieved.getId(), nodeId);
	EXPECT_TRUE(retrieved.isActive());

	// 2. Test clearCache(): clears the page buffer pool
	nodeManager->clearCache();
	EXPECT_EQ(dataManager->getPagePool().size(), 0UL);

	// 3. Entity is still retrievable after cache clear (from dirty registry)
	retrieved = nodeManager->get(nodeId);
	EXPECT_EQ(retrieved.getId(), nodeId);
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
	auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED});
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
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED});
    size_t previousCount = dirtyNodes.size();

    nodeManager->addBatch(emptyNodes);

    dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED});
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
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_MODIFIED});
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
// Tests branch at line 108-110: dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED
TEST_F(BaseEntityManagerTest, UpdatePreservesAddedState) {
    graph::Node node = createTestNode(dataManager, "AddedStateTest");
    nodeManager->add(node);

    // Update before flushing - should stay in ADDED state
    node.setLabelId(dataManager->getOrCreateLabelId("Updated"));
    nodeManager->update(node);

    // Verify still in ADDED state (not MODIFIED)
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED});
    bool found = false;
    for (const auto &dn: dirtyNodes) {
        if (dn.getId() == node.getId()) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Node should still be in ADDED state after update";

    auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_MODIFIED});
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
// Tests else branch at line 111-114: EntityChangeType::CHANGE_MODIFIED
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
    auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_MODIFIED});
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

// Test addToCache directly (now a no-op with PageBufferPool, but entity is still retrievable)
TEST_F(BaseEntityManagerTest, AddToCacheDirect) {
    graph::Node node = createTestNode(dataManager, "DirectCache");
    node.setId(12345); // Manual ID

    nodeManager->addToCache(node);

    // addToCache is a no-op with PageBufferPool, so entity won't be found
    // unless it's in the dirty registry or on disk
    graph::Node retrieved = nodeManager->get(12345);
    // Entity was not added to dirty registry, so it won't be found
    EXPECT_EQ(retrieved.getId(), 0);
}

// Test clearCache clears the page buffer pool
TEST_F(BaseEntityManagerTest, ClearCacheRemovesAll) {
    // Add multiple nodes (goes to dirty registry, not page pool)
    std::vector<int64_t> nodeIds;
    for (int i = 0; i < 10; i++) {
        graph::Node node = createTestNode(dataManager, "ClearTest" + std::to_string(i));
        nodeManager->add(node);
        nodeIds.push_back(node.getId());
    }

    // Clear cache
    nodeManager->clearCache();

    // Page pool is empty
    EXPECT_EQ(dataManager->getPagePool().size(), 0UL);

    // Entities are still retrievable from dirty registry
    for (int64_t id: nodeIds) {
        graph::Node retrieved = nodeManager->get(id);
        EXPECT_EQ(retrieved.getId(), id);
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
    auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_ADDED});
    int countFound = 0;
    for (const auto& dn : dirtyNodes) {
        for (const auto& original : nodes) {
            if (dn.getId() == original.getId()) countFound++;
        }
    }
    EXPECT_EQ(countFound, 3);
}

// =========================================================================
// Additional Branch Coverage Tests
// =========================================================================

// Test add with entity that already has a non-zero ID
// Tests branch at line 35: if (entity.getId() == 0) - False branch
TEST_F(BaseEntityManagerTest, AddEntityWithExistingId) {
    graph::Node node = createTestNode(dataManager, "PreIdNode");
    node.setId(50000); // Manually set a non-zero ID

    nodeManager->add(node);

    // ID should remain the same (no allocation)
    EXPECT_EQ(node.getId(), 50000);

    // Should be retrievable
    graph::Node retrieved = nodeManager->get(50000);
    EXPECT_EQ(retrieved.getId(), 50000);
    EXPECT_TRUE(retrieved.isActive());
}

// Test edge operations through BaseEntityManager
TEST_F(BaseEntityManagerTest, EdgePropertyManagement) {
    // Create source and target nodes
    graph::Node source = createTestNode(dataManager, "EdgePropSource");
    graph::Node target = createTestNode(dataManager, "EdgePropTarget");
    nodeManager->add(source);
    nodeManager->add(target);

    // Create edge
    graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "HAS_PROP");
    edgeManager->add(edge);
    int64_t edgeId = edge.getId();

    // Add properties to edge
    std::unordered_map<std::string, graph::PropertyValue> props;
    props["weight"] = 42;
    props["label"] = std::string("test");
    edgeManager->addProperties(edgeId, props);

    // Get properties
    auto retrievedProps = edgeManager->getProperties(edgeId);
    EXPECT_EQ(retrievedProps.size(), 2UL);
    EXPECT_EQ(std::get<int64_t>(retrievedProps["weight"].getVariant()), 42);

    // Remove a property
    edgeManager->removeProperty(edgeId, "weight");
    retrievedProps = edgeManager->getProperties(edgeId);
    EXPECT_EQ(retrievedProps.size(), 1UL);
    EXPECT_EQ(retrievedProps.count("weight"), 0UL);
}

// Test getBatch with all inactive entities
TEST_F(BaseEntityManagerTest, GetBatchAllInactive) {
    // Create and remove all nodes
    std::vector<int64_t> nodeIds;
    for (int i = 0; i < 3; i++) {
        graph::Node node = createTestNode(dataManager, "AllInactive" + std::to_string(i));
        nodeManager->add(node);
        nodeIds.push_back(node.getId());
    }

    // Remove all
    for (int64_t id : nodeIds) {
        graph::Node node = nodeManager->get(id);
        nodeManager->remove(node);
    }

    // getBatch should return empty since all are inactive
    auto nodes = nodeManager->getBatch(nodeIds);
    EXPECT_TRUE(nodes.empty());
}

// Test edge getBatch
TEST_F(BaseEntityManagerTest, EdgeGetBatch) {
    graph::Node source = createTestNode(dataManager, "BatchEdgeSource");
    graph::Node target = createTestNode(dataManager, "BatchEdgeTarget");
    nodeManager->add(source);
    nodeManager->add(target);

    std::vector<int64_t> edgeIds;
    for (int i = 0; i < 3; i++) {
        graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(),
                                           "BATCH_EDGE" + std::to_string(i));
        edgeManager->add(edge);
        edgeIds.push_back(edge.getId());
    }

    auto edges = edgeManager->getBatch(edgeIds);
    EXPECT_EQ(edges.size(), 3UL);
    for (const auto &edge : edges) {
        EXPECT_TRUE(edge.isActive());
    }
}

// Test edge update
TEST_F(BaseEntityManagerTest, EdgeUpdate) {
    graph::Node source = createTestNode(dataManager, "UpdEdgeSrc");
    graph::Node target = createTestNode(dataManager, "UpdEdgeTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "ORIG_LABEL");
    edgeManager->add(edge);

    // Update the edge
    edge.setLabelId(dataManager->getOrCreateLabelId("NEW_LABEL"));
    edgeManager->update(edge);

    graph::Edge retrieved = edgeManager->get(edge.getId());
    EXPECT_EQ(dataManager->resolveLabel(retrieved.getLabelId()), "NEW_LABEL");
}

// Test edge clearCache and addToCache
TEST_F(BaseEntityManagerTest, EdgeCacheOperations) {
    graph::Node source = createTestNode(dataManager, "CacheEdgeSrc");
    graph::Node target = createTestNode(dataManager, "CacheEdgeTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "CACHE_EDGE");
    edgeManager->add(edge);
    int64_t edgeId = edge.getId();

    // Edge should be retrievable (from dirty registry)
    graph::Edge retrieved = edgeManager->get(edgeId);
    EXPECT_EQ(retrieved.getId(), edgeId);

    // Clear cache
    edgeManager->clearCache();
    EXPECT_EQ(dataManager->getPagePool().size(), 0UL);

    // Edge still retrievable from dirty registry
    retrieved = edgeManager->get(edgeId);
    EXPECT_EQ(retrieved.getId(), edgeId);
}

// Test addBatch for edges
TEST_F(BaseEntityManagerTest, EdgeAddBatch) {
    graph::Node source = createTestNode(dataManager, "BatchEdgeSrc");
    graph::Node target = createTestNode(dataManager, "BatchEdgeTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    std::vector<graph::Edge> edges;
    for (int i = 0; i < 5; ++i) {
        edges.push_back(createTestEdge(dataManager, source.getId(), target.getId(),
                                        "BATCH" + std::to_string(i)));
    }

    edgeManager->addBatch(edges);

    // Verify IDs assigned
    for (const auto &edge : edges) {
        EXPECT_NE(edge.getId(), 0);
        graph::Edge retrieved = edgeManager->get(edge.getId());
        EXPECT_EQ(retrieved.getId(), edge.getId());
        EXPECT_TRUE(retrieved.isActive());
    }
}

// ============================================================================
// Additional Branch Coverage Tests for BaseEntityManager.cpp
// ============================================================================

// Test addBatch with a mix of entities: some with existing IDs, some without
// This covers the case where newEntities is non-empty but smaller than entities
// Exercises: line 60 (getId() == 0) both True and False branches within the same call,
// and line 67 (newEntities not empty) True branch with partial allocation
TEST_F(BaseEntityManagerTest, AddBatchMixedIds) {
    std::vector<graph::Node> nodes;

    // Two nodes with zero IDs (need allocation)
    graph::Node nodeA = createTestNode(dataManager, "MixedA");
    graph::Node nodeB = createTestNode(dataManager, "MixedB");
    EXPECT_EQ(nodeA.getId(), 0);
    EXPECT_EQ(nodeB.getId(), 0);
    nodes.push_back(nodeA);
    nodes.push_back(nodeB);

    // One node with pre-set ID (no allocation needed)
    graph::Node nodeC = createTestNode(dataManager, "MixedC");
    nodeC.setId(60000);
    nodes.push_back(nodeC);

    // Another node with zero ID
    graph::Node nodeD = createTestNode(dataManager, "MixedD");
    EXPECT_EQ(nodeD.getId(), 0);
    nodes.push_back(nodeD);

    nodeManager->addBatch(nodes);

    // Verify: nodeA and nodeB got allocated IDs (non-zero)
    EXPECT_NE(nodes[0].getId(), 0) << "Node A should have an allocated ID";
    EXPECT_NE(nodes[1].getId(), 0) << "Node B should have an allocated ID";
    // nodeC kept its pre-set ID
    EXPECT_EQ(nodes[2].getId(), 60000) << "Node C should keep its pre-set ID";
    // nodeD got allocated ID
    EXPECT_NE(nodes[3].getId(), 0) << "Node D should have an allocated ID";

    // All should be retrievable
    for (const auto &node : nodes) {
        graph::Node retrieved = nodeManager->get(node.getId());
        EXPECT_EQ(retrieved.getId(), node.getId());
        EXPECT_TRUE(retrieved.isActive());
    }
}

// Test update on entity already persisted that has dirtyInfo present and is MODIFIED
// This specifically covers the else branch of dirtyInfo->changeType == CHANGE_ADDED
// at line 106-108, where an already-modified entity is updated again
TEST_F(BaseEntityManagerTest, UpdateModifiedEntityAgain) {
    // Create and add a node
    graph::Node node = createTestNode(dataManager, "DoubleModify");
    nodeManager->add(node);

    // Flush to persist
    dataManager->prepareFlushSnapshot();
    dataManager->commitFlushSnapshot();

    // First update creates MODIFIED state
    node.setLabelId(dataManager->getOrCreateLabelId("FirstMod"));
    nodeManager->update(node);

    // Second update should also use MODIFIED state (dirtyInfo exists, is MODIFIED)
    node.setLabelId(dataManager->getOrCreateLabelId("SecondMod"));
    nodeManager->update(node);

    // Verify in MODIFIED state
    auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::CHANGE_MODIFIED});
    bool found = false;
    for (const auto &dn : modifiedNodes) {
        if (dn.getId() == node.getId()) {
            found = true;
            EXPECT_EQ(dataManager->resolveLabel(dn.getLabelId()), "SecondMod");
            break;
        }
    }
    EXPECT_TRUE(found) << "Node should be in MODIFIED state after second update";
}

// Test getBatch where entity.getId() != 0 but entity.isActive() is false
// Covers the False path of entity.isActive() check at line 140
TEST_F(BaseEntityManagerTest, GetBatchIdNonZeroButInactive) {
    // Create a node, add it, then remove it
    graph::Node node = createTestNode(dataManager, "InactiveBatch");
    nodeManager->add(node);
    int64_t nodeId = node.getId();

    // Remove the node (marks it inactive/deleted)
    nodeManager->remove(node);

    // getBatch with the removed node's ID
    std::vector<int64_t> ids = {nodeId};
    auto nodes = nodeManager->getBatch(ids);

    // The node has a non-zero ID but is inactive, so it should be filtered out
    EXPECT_TRUE(nodes.empty()) << "Inactive node should be filtered from getBatch";
}

// Test add for edge with zero ID (exercises the add() id==0 branch for Edge type)
TEST_F(BaseEntityManagerTest, AddEdgeWithZeroId) {
    graph::Node source = createTestNode(dataManager, "ZeroEdgeSrc");
    graph::Node target = createTestNode(dataManager, "ZeroEdgeTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "ZERO_ID_EDGE");
    EXPECT_EQ(edge.getId(), 0) << "Edge should start with zero ID";

    edgeManager->add(edge);

    EXPECT_NE(edge.getId(), 0) << "Edge should get allocated ID";
    graph::Edge retrieved = edgeManager->get(edge.getId());
    EXPECT_EQ(retrieved.getId(), edge.getId());
}

// ============================================================================
// Additional Branch Coverage Tests for Edge addBatch
// ============================================================================

// Test edge addBatch with empty vector
// Covers: addBatch line 49 True branch for Edge template instantiation
TEST_F(BaseEntityManagerTest, EdgeAddBatchEmpty) {
    std::vector<graph::Edge> emptyEdges;
    EXPECT_NO_THROW(edgeManager->addBatch(emptyEdges));
}

// Test edge addBatch with pre-set IDs (non-zero)
// Covers: addBatch line 60 False branch (getId() != 0) for Edge template
// Covers: addBatch line 67 False branch (newEntities.empty()) for Edge template
TEST_F(BaseEntityManagerTest, EdgeAddBatchWithExistingIds) {
    graph::Node source = createTestNode(dataManager, "BatchIdSrc");
    graph::Node target = createTestNode(dataManager, "BatchIdTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    std::vector<graph::Edge> edges;
    for (int i = 0; i < 3; ++i) {
        graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(),
                                           "BATCH_ID_" + std::to_string(i));
        edge.setId(70000 + i); // Pre-set non-zero IDs
        edges.push_back(edge);
    }

    // All edges have non-zero IDs, so newEntities should be empty
    // This hits the False branch of !newEntities.empty() (line 67)
    EXPECT_NO_THROW(edgeManager->addBatch(edges));

    // Verify IDs were not changed
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(edges[i].getId(), 70000 + i);
        graph::Edge retrieved = edgeManager->get(edges[i].getId());
        EXPECT_EQ(retrieved.getId(), edges[i].getId());
        EXPECT_TRUE(retrieved.isActive());
    }
}

// Test edge addBatch with mixed IDs (some zero, some non-zero)
// Covers both True and False paths of getId() == 0 for Edge template (line 60)
TEST_F(BaseEntityManagerTest, EdgeAddBatchMixedIds) {
    graph::Node source = createTestNode(dataManager, "MixEdgeSrc");
    graph::Node target = createTestNode(dataManager, "MixEdgeTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    std::vector<graph::Edge> edges;

    // Edge with zero ID (needs allocation)
    graph::Edge e1 = createTestEdge(dataManager, source.getId(), target.getId(), "MIX_ZERO");
    EXPECT_EQ(e1.getId(), 0);
    edges.push_back(e1);

    // Edge with pre-set ID (no allocation needed)
    graph::Edge e2 = createTestEdge(dataManager, source.getId(), target.getId(), "MIX_SET");
    e2.setId(80000);
    edges.push_back(e2);

    // Another edge with zero ID
    graph::Edge e3 = createTestEdge(dataManager, source.getId(), target.getId(), "MIX_ZERO2");
    EXPECT_EQ(e3.getId(), 0);
    edges.push_back(e3);

    edgeManager->addBatch(edges);

    // e1 and e3 should get allocated IDs
    EXPECT_NE(edges[0].getId(), 0) << "Edge e1 should get allocated ID";
    // e2 should keep pre-set ID
    EXPECT_EQ(edges[1].getId(), 80000) << "Edge e2 should keep pre-set ID";
    // e3 should get allocated ID
    EXPECT_NE(edges[2].getId(), 0) << "Edge e3 should get allocated ID";

    // All should be retrievable
    for (const auto &edge : edges) {
        graph::Edge retrieved = edgeManager->get(edge.getId());
        EXPECT_EQ(retrieved.getId(), edge.getId());
        EXPECT_TRUE(retrieved.isActive());
    }
}

// Test edge update with zero ID
// Covers: update line 91 True branch for Edge template
TEST_F(BaseEntityManagerTest, EdgeUpdateWithZeroId) {
    graph::Edge edge;
    edge.setId(0);
    edge.setLabelId(dataManager->getOrCreateLabelId("ZeroIdUpdate"));

    // Should return early without error
    EXPECT_NO_THROW(edgeManager->update(edge));
}

// Test edge update with inactive entity
// Covers: update line 95-96 for Edge template
TEST_F(BaseEntityManagerTest, EdgeUpdateInactiveThrows) {
    graph::Node source = createTestNode(dataManager, "InactEdgeSrc");
    graph::Node target = createTestNode(dataManager, "InactEdgeTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "INACTIVE_UPD");
    edgeManager->add(edge);

    edge.markInactive();
    EXPECT_THROW(edgeManager->update(edge), std::runtime_error);
}

// Test edge remove with zero ID
// Covers: remove line 116 True branch for Edge template
TEST_F(BaseEntityManagerTest, EdgeRemoveWithZeroId) {
    graph::Edge edge;
    edge.setId(0);

    EXPECT_NO_THROW(edgeManager->remove(edge));
}

// Test edge remove with inactive entity
// Covers: remove line 116 !isActive() branch for Edge template
TEST_F(BaseEntityManagerTest, EdgeRemoveInactive) {
    graph::Node source = createTestNode(dataManager, "RemInactSrc");
    graph::Node target = createTestNode(dataManager, "RemInactTgt");
    nodeManager->add(source);
    nodeManager->add(target);

    graph::Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "REM_INACT");
    edge.markInactive();

    EXPECT_NO_THROW(edgeManager->remove(edge));
}

// ============================================================================
// Branch Coverage: addBatch where all entities already have IDs
// Covers: BaseEntityManager.cpp line 60: entity.getId() == 0 -> False (all have IDs)
// and line 67: !newEntities.empty() -> False (no new entities to allocate)
// ============================================================================

TEST_F(BaseEntityManagerTest, AddBatch_AllEntitiesHaveIds) {
	// Create nodes that already have IDs assigned
	// This should skip the ID allocation batch entirely
	std::vector<graph::Node> nodes;
	for (int i = 1; i <= 5; ++i) {
		graph::Node node;
		node.setId(static_cast<int64_t>(10000 + i));
		node.setLabelId(dataManager->getOrCreateLabelId("PreAssigned"));
		nodes.push_back(node);
	}

	EXPECT_NO_THROW(nodeManager->addBatch(nodes));

	// Verify nodes can be retrieved
	for (int i = 1; i <= 5; ++i) {
		graph::Node retrieved = nodeManager->get(10000 + i);
		EXPECT_EQ(retrieved.getId(), 10000 + i);
	}
}

// ============================================================================
// Branch Coverage: addBatch with mixed IDs (some pre-assigned, some need allocation)
// Covers: Both branches of entity.getId() == 0 in the addBatch loop (line 60)
// ============================================================================

TEST_F(BaseEntityManagerTest, AddBatch_MixedIds) {
	std::vector<graph::Node> nodes;

	// Node with pre-assigned ID
	graph::Node nodeWithId;
	nodeWithId.setId(20001);
	nodeWithId.setLabelId(dataManager->getOrCreateLabelId("Mixed"));
	nodes.push_back(nodeWithId);

	// Node without ID (needs allocation)
	graph::Node nodeWithoutId;
	nodeWithoutId.setLabelId(dataManager->getOrCreateLabelId("Mixed"));
	nodes.push_back(nodeWithoutId);

	EXPECT_NO_THROW(nodeManager->addBatch(nodes));

	// The pre-assigned node should still have its original ID
	graph::Node retrieved = nodeManager->get(20001);
	EXPECT_EQ(retrieved.getId(), 20001);
}

// ============================================================================
// Branch Coverage: addBatch for Edge entities (covers Edge template instantiation)
// Covers: BaseEntityManager.cpp line 49: entities.empty() -> False for Edge
// ============================================================================

TEST_F(BaseEntityManagerTest, EdgeAddBatch_AllWithIds) {
	graph::Node src = createTestNode(dataManager, "BSrc");
	graph::Node tgt = createTestNode(dataManager, "BTgt");
	nodeManager->add(src);
	nodeManager->add(tgt);

	std::vector<graph::Edge> edges;
	for (int i = 1; i <= 3; ++i) {
		graph::Edge edge;
		edge.setId(static_cast<int64_t>(30000 + i));
		edge.setSourceNodeId(src.getId());
		edge.setTargetNodeId(tgt.getId());
		edge.setLabelId(dataManager->getOrCreateLabelId("BATCH_EDGE"));
		edges.push_back(edge);
	}

	EXPECT_NO_THROW(edgeManager->addBatch(edges));
}

// ============================================================================
// Branch Coverage: addBatch with empty vector for Edge
// Covers: BaseEntityManager.cpp line 49: entities.empty() -> True for Edge
// ============================================================================

TEST_F(BaseEntityManagerTest, EdgeAddBatch_Empty) {
	std::vector<graph::Edge> emptyEdges;
	EXPECT_NO_THROW(edgeManager->addBatch(emptyEdges));
}