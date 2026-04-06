/**
 * @file test_EntityTraits.cpp
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
#include <gtest/gtest.h>
#include <random>
#include "graph/core/Blob.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/PageBufferPool.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"

using namespace graph;
using namespace graph::storage;

/**
 * Test fixture for EntityTraits tests
 * Sets up a Database instance with FileStorage and DataManager
 */
class EntityTraitsTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file for each test using UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_entityTraits_" + to_string(uuid) + ".dat");

		// Initialize Database and get the DataManager instance
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
	}

	void TearDown() override {
		// Release shared_ptrs before closing database
		dataManager.reset();
		fileStorage.reset();

		if (database) {
			database->close();
		}
		database.reset();

		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);

		// Also clean up WAL file if exists
		std::filesystem::path walPath = testFilePath;
		walPath += ".wal";
		std::filesystem::remove(walPath, ec);
	}

	// Helper methods to create test entities
	Node createTestNode(int64_t id) {
		Node node;
		node.getMutableMetadata().id = id;
		node.getMutableMetadata().labelIds[0] = 100;
		node.getMutableMetadata().labelCount = 1;
		node.getMutableMetadata().isActive = true;
		return node;
	}

	Edge createTestEdge(int64_t id, int64_t sourceId, int64_t targetId) {
		Edge edge;
		edge.getMutableMetadata().id = id;
		edge.getMutableMetadata().sourceNodeId = sourceId;
		edge.getMutableMetadata().targetNodeId = targetId;
		edge.getMutableMetadata().labelId = 200;
		edge.getMutableMetadata().isActive = true;
		return edge;
	}

	Property createTestProperty(int64_t id) {
		Property property;
		property.getMutableMetadata().entityId = id;
		property.getMutableMetadata().entityType = static_cast<uint32_t>(EntityType::Node);
		property.getMutableMetadata().isActive = true;
		return property;
	}

	Blob createTestBlob(int64_t id) {
		Blob blob;
		blob.getMutableMetadata().id = id;
		blob.getMutableMetadata().isActive = true;
		return blob;
	}

	Index createTestIndex(int64_t id) {
		Index index;
		index.getMutableMetadata().id = id;
		index.getMutableMetadata().indexType = 0; // Just set a basic value
		index.getMutableMetadata().isActive = true;
		return index;
	}

	State createTestState(int64_t id) {
		State state;
		state.getMutableMetadata().id = id;
		state.getMutableMetadata().isActive = true;
		return state;
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<FileStorage> fileStorage;
	std::shared_ptr<DataManager> dataManager;
};

// ============================================================================
// DataManagerAccess Specialization Tests
// ============================================================================

TEST_F(EntityTraitsTest, NodeDataManagerAccess) {
	// Test loadFromDisk for Node (returns default entity since no data on disk)
	auto result = DataManagerAccess<Node>::loadFromDisk(dataManager.get(), 999);
	EXPECT_EQ(result.getId(), 0); // No entity on disk
}

TEST_F(EntityTraitsTest, EdgeDataManagerAccess) {
	auto result = DataManagerAccess<Edge>::loadFromDisk(dataManager.get(), 999);
	EXPECT_EQ(result.getId(), 0);
}

TEST_F(EntityTraitsTest, PropertyDataManagerAccess) {
	auto result = DataManagerAccess<Property>::loadFromDisk(dataManager.get(), 999);
	EXPECT_EQ(result.getId(), 0);
}

TEST_F(EntityTraitsTest, BlobDataManagerAccess) {
	auto result = DataManagerAccess<Blob>::loadFromDisk(dataManager.get(), 999);
	EXPECT_EQ(result.getId(), 0);
}

TEST_F(EntityTraitsTest, IndexDataManagerAccess) {
	auto result = DataManagerAccess<Index>::loadFromDisk(dataManager.get(), 999);
	EXPECT_EQ(result.getId(), 0);
}

TEST_F(EntityTraitsTest, StateDataManagerAccess) {
	auto result = DataManagerAccess<State>::loadFromDisk(dataManager.get(), 999);
	EXPECT_EQ(result.getId(), 0);
}

TEST_F(EntityTraitsTest, GetSegmentIndex) {
	// Test getSegmentIndex for all entity types
	const auto &nodeIndex = DataManagerAccess<Node>::getSegmentIndex(dataManager.get());
	EXPECT_GE(nodeIndex.size(), 0u);

	const auto &edgeIndex = DataManagerAccess<Edge>::getSegmentIndex(dataManager.get());
	EXPECT_GE(edgeIndex.size(), 0u);

	const auto &propertyIndex = DataManagerAccess<Property>::getSegmentIndex(dataManager.get());
	EXPECT_GE(propertyIndex.size(), 0u);

	const auto &blobIndex = DataManagerAccess<Blob>::getSegmentIndex(dataManager.get());
	EXPECT_GE(blobIndex.size(), 0u);

	const auto &indexIndex = DataManagerAccess<Index>::getSegmentIndex(dataManager.get());
	EXPECT_GE(indexIndex.size(), 0u);

	const auto &stateIndex = DataManagerAccess<State>::getSegmentIndex(dataManager.get());
	EXPECT_GE(stateIndex.size(), 0u);
}

// ============================================================================
// EntityTraits Cache Management Tests
// ============================================================================

TEST_F(EntityTraitsTest, EntityTraitsPagePool) {
	// With PageBufferPool, the page pool is accessed via DataManager
	auto &pool = dataManager->getPagePool();
	EXPECT_EQ(pool.size(), 0u);
}

TEST_F(EntityTraitsTest, EntityTraitsAddToCache) {
	// addToCache is now a no-op with PageBufferPool
	Node testNode = createTestNode(1);
	Edge testEdge = createTestEdge(2, 1, 2);
	Blob testBlob = createTestBlob(4);
	Index testIndex = createTestIndex(5);
	State testState = createTestState(6);

	// These are no-ops — should not crash
	EntityTraits<Node>::addToCache(dataManager.get(), testNode);
	EntityTraits<Edge>::addToCache(dataManager.get(), testEdge);
	EntityTraits<Blob>::addToCache(dataManager.get(), testBlob);
	EntityTraits<Index>::addToCache(dataManager.get(), testIndex);
	EntityTraits<State>::addToCache(dataManager.get(), testState);

	// Page pool remains empty (addToCache is no-op)
	EXPECT_EQ(dataManager->getPagePool().size(), 0u);
}

TEST_F(EntityTraitsTest, EntityTraitsRemoveFromCache) {
	// removeFromCache is now a no-op with PageBufferPool — should not crash
	EntityTraits<Node>::removeFromCache(dataManager.get(), 1);
	EntityTraits<Edge>::removeFromCache(dataManager.get(), 2);
	EntityTraits<Property>::removeFromCache(dataManager.get(), 3);
	EntityTraits<Blob>::removeFromCache(dataManager.get(), 4);
	EntityTraits<Index>::removeFromCache(dataManager.get(), 5);
	EntityTraits<State>::removeFromCache(dataManager.get(), 6);
}

// ============================================================================
// EntityTraits Dirty Data Management Tests
// ============================================================================

TEST_F(EntityTraitsTest, EntityTraitsAddToDirty) {
	// Create test entities for dirty tracking
	Node testNode = createTestNode(1);
	Edge testEdge = createTestEdge(2, 1, 2);

	// Create dirty entity info for various entity types
	DirtyEntityInfo<Node> nodeDirtyInfo(EntityChangeType::CHANGE_ADDED, testNode);
	DirtyEntityInfo<Edge> edgeDirtyInfo(EntityChangeType::CHANGE_ADDED, testEdge);

	// Test addToDirty - should not throw
	EXPECT_NO_THROW(EntityTraits<Node>::addToDirty(dataManager.get(), nodeDirtyInfo));
	EXPECT_NO_THROW(EntityTraits<Edge>::addToDirty(dataManager.get(), edgeDirtyInfo));
}

TEST_F(EntityTraitsTest, EntityTraitsGetDirtyInfo) {
	// Create a test entity and dirty info
	Node testNode = createTestNode(1);
	DirtyEntityInfo<Node> dirtyInfo(EntityChangeType::CHANGE_ADDED, testNode);
	EntityTraits<Node>::addToDirty(dataManager.get(), dirtyInfo);

	// Get dirty info
	auto dirtyInfoOpt = EntityTraits<Node>::getDirtyInfo(dataManager.get(), 1);

	// Verify dirty info is present
	EXPECT_TRUE(dirtyInfoOpt.has_value());
	if (dirtyInfoOpt.has_value()) {
		EXPECT_EQ(dirtyInfoOpt->backup->getId(), 1);
		EXPECT_EQ(dirtyInfoOpt->changeType, EntityChangeType::CHANGE_ADDED);
	}
}

TEST_F(EntityTraitsTest, EntityTraitsGetDirtyInfoNotPresent) {
	// Try to get dirty info for an entity that was not marked as dirty
	auto dirtyInfoOpt = EntityTraits<Node>::getDirtyInfo(dataManager.get(), 999);

	// Should return empty optional
	EXPECT_FALSE(dirtyInfoOpt.has_value());
}

TEST_F(EntityTraitsTest, EntityTraitsGetDirtyInfoModified) {
	// Create a test entity and mark it as modified
	Node testNode = createTestNode(1);
	DirtyEntityInfo<Node> dirtyInfo(EntityChangeType::CHANGE_MODIFIED, testNode);
	EntityTraits<Node>::addToDirty(dataManager.get(), dirtyInfo);

	// Get dirty info
	auto dirtyInfoOpt = EntityTraits<Node>::getDirtyInfo(dataManager.get(), 1);

	// Verify dirty info is present with MODIFIED type
	EXPECT_TRUE(dirtyInfoOpt.has_value());
	if (dirtyInfoOpt.has_value()) {
		EXPECT_EQ(dirtyInfoOpt->changeType, EntityChangeType::CHANGE_MODIFIED);
	}
}

TEST_F(EntityTraitsTest, EntityTraitsGetDirtyInfoDeleted) {
	// Create a test entity and mark it as deleted
	Node testNode = createTestNode(1);
	DirtyEntityInfo<Node> dirtyInfo(EntityChangeType::CHANGE_DELETED, testNode);
	EntityTraits<Node>::addToDirty(dataManager.get(), dirtyInfo);

	// Get dirty info
	auto dirtyInfoOpt = EntityTraits<Node>::getDirtyInfo(dataManager.get(), 1);

	// Verify dirty info is present with DELETED type
	EXPECT_TRUE(dirtyInfoOpt.has_value());
	if (dirtyInfoOpt.has_value()) {
		EXPECT_EQ(dirtyInfoOpt->changeType, EntityChangeType::CHANGE_DELETED);
	}
}

// ============================================================================
// EntityTraits Segment Index Access Tests
// ============================================================================

TEST_F(EntityTraitsTest, EntityTraitsGetSegmentIndex) {
	// Test getSegmentIndex for all entity types
	const auto &nodeIndex = EntityTraits<Node>::getSegmentIndex(dataManager.get());
	EXPECT_GE(nodeIndex.size(), 0u);

	const auto &edgeIndex = EntityTraits<Edge>::getSegmentIndex(dataManager.get());
	EXPECT_GE(edgeIndex.size(), 0u);

	const auto &propertyIndex = EntityTraits<Property>::getSegmentIndex(dataManager.get());
	EXPECT_GE(propertyIndex.size(), 0u);

	const auto &blobIndex = EntityTraits<Blob>::getSegmentIndex(dataManager.get());
	EXPECT_GE(blobIndex.size(), 0u);

	const auto &indexIndex = EntityTraits<Index>::getSegmentIndex(dataManager.get());
	EXPECT_GE(indexIndex.size(), 0u);

	const auto &stateIndex = EntityTraits<State>::getSegmentIndex(dataManager.get());
	EXPECT_GE(stateIndex.size(), 0u);
}

// ============================================================================
// EntityTraits TypeId Tests
// ============================================================================

TEST_F(EntityTraitsTest, EntityTraitsTypeId) {
	// Verify typeId values for all entity types
	EXPECT_EQ(EntityTraits<Node>::typeId, Node::typeId);
	EXPECT_EQ(EntityTraits<Edge>::typeId, Edge::typeId);
	EXPECT_EQ(EntityTraits<Property>::typeId, Property::typeId);
	EXPECT_EQ(EntityTraits<Blob>::typeId, Blob::typeId);
	EXPECT_EQ(EntityTraits<Index>::typeId, Index::typeId);
	EXPECT_EQ(EntityTraits<State>::typeId, State::typeId);
}

// ============================================================================
// EntityTraits Access Type Tests
// ============================================================================

TEST_F(EntityTraitsTest, EntityTraitsAccessType) {
	// Verify Access type is correctly defined for all entity types
	using NodeAccessType = DataManagerAccess<Node>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Node>::Access, NodeAccessType>) );

	using EdgeAccessType = DataManagerAccess<Edge>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Edge>::Access, EdgeAccessType>) );

	using PropertyAccessType = DataManagerAccess<Property>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Property>::Access, PropertyAccessType>) );

	using BlobAccessType = DataManagerAccess<Blob>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Blob>::Access, BlobAccessType>) );

	using IndexAccessType = DataManagerAccess<Index>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Index>::Access, IndexAccessType>) );

	using StateAccessType = DataManagerAccess<State>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<State>::Access, StateAccessType>) );
}
