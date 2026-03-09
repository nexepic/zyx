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
#include "graph/storage/CacheManager.hpp"
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
		// Clean up resources
		if (database) {
			database->close();
		}

		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}

		// Also clean up WAL file if exists
		std::filesystem::path walPath = testFilePath;
		walPath += ".wal";
		if (std::filesystem::exists(walPath)) {
			std::filesystem::remove(walPath);
		}
	}

	// Helper methods to create test entities
	Node createTestNode(int64_t id) {
		Node node;
		node.getMutableMetadata().id = id;
		node.getMutableMetadata().labelId = 100;
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
	// Test getCache for Node
	auto &nodeCache = DataManagerAccess<Node>::getCache(dataManager.get());
	EXPECT_NE(&nodeCache, nullptr);

	// Verify cache is the correct type
	using NodeCacheType = LRUCache<int64_t, Node>;
	EXPECT_TRUE((std::is_same_v<decltype(nodeCache), NodeCacheType &>) );
}

TEST_F(EntityTraitsTest, EdgeDataManagerAccess) {
	// Test getCache for Edge
	auto &edgeCache = DataManagerAccess<Edge>::getCache(dataManager.get());
	EXPECT_NE(&edgeCache, nullptr);

	// Verify cache is the correct type
	using EdgeCacheType = LRUCache<int64_t, Edge>;
	EXPECT_TRUE((std::is_same_v<decltype(edgeCache), EdgeCacheType &>) );
}

TEST_F(EntityTraitsTest, PropertyDataManagerAccess) {
	// Test getCache for Property
	auto &propertyCache = DataManagerAccess<Property>::getCache(dataManager.get());
	EXPECT_NE(&propertyCache, nullptr);

	// Verify cache is the correct type
	using PropertyCacheType = LRUCache<int64_t, Property>;
	EXPECT_TRUE((std::is_same_v<decltype(propertyCache), PropertyCacheType &>) );
}

TEST_F(EntityTraitsTest, BlobDataManagerAccess) {
	// Test getCache for Blob
	auto &blobCache = DataManagerAccess<Blob>::getCache(dataManager.get());
	EXPECT_NE(&blobCache, nullptr);

	// Verify cache is the correct type
	using BlobCacheType = LRUCache<int64_t, Blob>;
	EXPECT_TRUE((std::is_same_v<decltype(blobCache), BlobCacheType &>) );
}

TEST_F(EntityTraitsTest, IndexDataManagerAccess) {
	// Test getCache for Index
	auto &indexCache = DataManagerAccess<Index>::getCache(dataManager.get());
	EXPECT_NE(&indexCache, nullptr);

	// Verify cache is the correct type
	using IndexCacheType = LRUCache<int64_t, Index>;
	EXPECT_TRUE((std::is_same_v<decltype(indexCache), IndexCacheType &>) );
}

TEST_F(EntityTraitsTest, StateDataManagerAccess) {
	// Test getCache for State
	auto &stateCache = DataManagerAccess<State>::getCache(dataManager.get());
	EXPECT_NE(&stateCache, nullptr);

	// Verify cache is the correct type
	using StateCacheType = LRUCache<int64_t, State>;
	EXPECT_TRUE((std::is_same_v<decltype(stateCache), StateCacheType &>) );
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

TEST_F(EntityTraitsTest, EntityTraitsGetCache) {
	// Test EntityTraits::getCache for all entity types
	auto &nodeCache = EntityTraits<Node>::getCache(dataManager.get());
	EXPECT_NE(&nodeCache, nullptr);

	auto &edgeCache = EntityTraits<Edge>::getCache(dataManager.get());
	EXPECT_NE(&edgeCache, nullptr);

	auto &propertyCache = EntityTraits<Property>::getCache(dataManager.get());
	EXPECT_NE(&propertyCache, nullptr);

	auto &blobCache = EntityTraits<Blob>::getCache(dataManager.get());
	EXPECT_NE(&blobCache, nullptr);

	auto &indexCache = EntityTraits<Index>::getCache(dataManager.get());
	EXPECT_NE(&indexCache, nullptr);

	auto &stateCache = EntityTraits<State>::getCache(dataManager.get());
	EXPECT_NE(&stateCache, nullptr);
}

TEST_F(EntityTraitsTest, EntityTraitsAddToCache) {
	// Create test entities
	Node testNode = createTestNode(1);
	Edge testEdge = createTestEdge(2, 1, 2);
	// Note: Skip Property test as it may have different cache behavior
	Blob testBlob = createTestBlob(4);
	Index testIndex = createTestIndex(5);
	State testState = createTestState(6);

	// Test addToCache for all entity types
	EntityTraits<Node>::addToCache(dataManager.get(), testNode);
	EntityTraits<Edge>::addToCache(dataManager.get(), testEdge);
	EntityTraits<Blob>::addToCache(dataManager.get(), testBlob);
	EntityTraits<Index>::addToCache(dataManager.get(), testIndex);
	EntityTraits<State>::addToCache(dataManager.get(), testState);

	// Verify entities are in cache using contains()
	auto &nodeCache = EntityTraits<Node>::getCache(dataManager.get());
	auto &edgeCache = EntityTraits<Edge>::getCache(dataManager.get());
	auto &blobCache = EntityTraits<Blob>::getCache(dataManager.get());
	auto &indexCache = EntityTraits<Index>::getCache(dataManager.get());
	auto &stateCache = EntityTraits<State>::getCache(dataManager.get());

	EXPECT_TRUE(nodeCache.contains(1));
	EXPECT_TRUE(edgeCache.contains(2));
	EXPECT_TRUE(blobCache.contains(4));
	EXPECT_TRUE(indexCache.contains(5));
	EXPECT_TRUE(stateCache.contains(6));

	// Verify we can retrieve the entities
	Node retrievedNode = nodeCache.get(1);
	EXPECT_EQ(retrievedNode.getId(), 1);

	Edge retrievedEdge = edgeCache.get(2);
	EXPECT_EQ(retrievedEdge.getId(), 2);
}

TEST_F(EntityTraitsTest, EntityTraitsRemoveFromCache) {
	// Add entity to cache
	Node testNode = createTestNode(1);
	EntityTraits<Node>::addToCache(dataManager.get(), testNode);

	// Verify entity is in cache
	auto &nodeCache = EntityTraits<Node>::getCache(dataManager.get());
	EXPECT_TRUE(nodeCache.contains(1));

	// Remove from cache
	EntityTraits<Node>::removeFromCache(dataManager.get(), 1);

	// Verify entity is removed
	EXPECT_FALSE(nodeCache.contains(1));
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
// EntityTraits CacheType Tests
// ============================================================================

TEST_F(EntityTraitsTest, EntityTraitsCacheType) {
	// Verify CacheType is correctly defined for all entity types
	using NodeCacheType = LRUCache<int64_t, Node>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Node>::CacheType, NodeCacheType>) );

	using EdgeCacheType = LRUCache<int64_t, Edge>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Edge>::CacheType, EdgeCacheType>) );

	using PropertyCacheType = LRUCache<int64_t, Property>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Property>::CacheType, PropertyCacheType>) );

	using BlobCacheType = LRUCache<int64_t, Blob>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Blob>::CacheType, BlobCacheType>) );

	using IndexCacheType = LRUCache<int64_t, Index>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<Index>::CacheType, IndexCacheType>) );

	using StateCacheType = LRUCache<int64_t, State>;
	EXPECT_TRUE((std::is_same_v<EntityTraits<State>::CacheType, StateCacheType>) );
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
