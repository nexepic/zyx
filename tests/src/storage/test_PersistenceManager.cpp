/**
 * @file test_PersistenceManager.cpp
 * @author Nexepic
 * @date 2025/12/2
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

#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/storage/PersistenceManager.hpp"

using namespace graph::storage;
using namespace graph;

class PersistenceManagerTest : public ::testing::Test {
protected:
	std::shared_ptr<PersistenceManager> manager;

	void SetUp() override { manager = std::make_shared<PersistenceManager>(); }

	// Helper to add a node
	void addNode(int64_t id) const {
		Node n;
		n.setId(id);
		manager->upsert(DirtyEntityInfo<Node>(EntityChangeType::ADDED, n));
	}

	// Helper to add an edge
	void addEdge(int64_t id) const {
		Edge e;
		e.setId(id);
		manager->upsert(DirtyEntityInfo<Edge>(EntityChangeType::ADDED, e));
	}
};

// 1. Type Routing (Ensure upsert<Node> goes to Node registry, etc.)
TEST_F(PersistenceManagerTest, TypeRouting) {
	addNode(100);
	addEdge(200);

	EXPECT_TRUE(manager->isDirty<Node>(100));
	EXPECT_FALSE(manager->isDirty<Edge>(100));

	EXPECT_TRUE(manager->isDirty<Edge>(200));
	EXPECT_FALSE(manager->isDirty<Node>(200));

	auto nodeInfo = manager->getDirtyInfo<Node>(100);
	ASSERT_TRUE(nodeInfo.has_value());
	ASSERT_TRUE(nodeInfo->backup.has_value());
	EXPECT_EQ(nodeInfo->backup->getId(), 100);
}

// 2. Snapshot Coordination
TEST_F(PersistenceManagerTest, SnapshotCoordination) {
	addNode(1);
	addEdge(2);

	EXPECT_TRUE(manager->hasUnsavedChanges());

	// Create Snapshot
	auto snapshot = manager->createSnapshot();

	// Verify snapshot contents
	EXPECT_FALSE(snapshot.isEmpty());
	EXPECT_EQ(snapshot.nodes.size(), 1UL);
	EXPECT_EQ(snapshot.edges.size(), 1UL);
	EXPECT_TRUE(snapshot.nodes.contains(1));
	EXPECT_TRUE(snapshot.edges.contains(2));

	// Since we only created a snapshot (Double Buffering), hasUnsavedChanges should still be true
	// because the data is now in the Flushing Layer, which is still "unsaved" from disk perspective.
	EXPECT_TRUE(manager->hasUnsavedChanges());

	// Commit Snapshot (Data flushed to disk)
	manager->commitSnapshot();

	// Now it should be empty
	EXPECT_FALSE(manager->hasUnsavedChanges());
	EXPECT_FALSE(manager->isDirty<Node>(1));
}

// 3. Auto-Flush Callback Logic
TEST_F(PersistenceManagerTest, AutoFlushTrigger) {
	int flushCount = 0;

	// Set threshold to 3 entities
	manager->setMaxDirtyEntities(3);
	manager->setAutoFlushCallback([&flushCount]() { flushCount++; });

	// Add 2 entities (Node + Edge) -> Total 2
	addNode(1);
	addEdge(2);
	EXPECT_EQ(flushCount, 0) << "Should not flush yet (2 < 3)";

	// Add 1 more entity -> Total 3
	addNode(3);
	EXPECT_EQ(flushCount, 1) << "Should flush now (3 >= 3)";

	// Add another entity -> Total 4
	// Note: In real app, flush() clears the flushing layer.
	// Here we strictly test the callback trigger logic based on size.
	addEdge(4);
	EXPECT_EQ(flushCount, 2) << "Should flush again";
}

// 4. GetAllDirtyInfos for Unified Query
TEST_F(PersistenceManagerTest, GetAllDirtyInfos) {
	// Add Node 1 (Active)
	addNode(1);

	// Create snapshot (Node 1 -> Flushing)
	(void) manager->createSnapshot();

	// Add Node 2 (Active)
	addNode(2);

	// Query all dirty nodes
	auto allNodes = manager->getAllDirtyInfos<Node>();

	EXPECT_EQ(allNodes.size(), 2UL);

	// Verify IDs present
	bool has1 = false, has2 = false;
	for (auto &info: allNodes) {
		if (info.backup->getId() == 1)
			has1 = true;
		if (info.backup->getId() == 2)
			has2 = true;
	}
	EXPECT_TRUE(has1 && has2);
}

// 5. Default Return Values (Edge Case for getEntity)
TEST_F(PersistenceManagerTest, GetNonExistentEntity) {
	auto info = manager->getDirtyInfo<Node>(999);
	EXPECT_FALSE(info.has_value());
}

// 6. Mixed Entity Types Count for AutoFlush
TEST_F(PersistenceManagerTest, AutoFlushCountsAllTypes) {
	int flushCount = 0;
	manager->setMaxDirtyEntities(2); // Flush at 2
	manager->setAutoFlushCallback([&flushCount]() { flushCount++; });

	// 1. Node (Count 1)
	addNode(1);
	EXPECT_EQ(flushCount, 0);

	// 2. Property (Count 2) - Should trigger
	Property p;
	p.getMutableMetadata().entityId = 1;
	p.setId(10);
	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::ADDED, p));
	EXPECT_EQ(flushCount, 1);
}

TEST_F(PersistenceManagerTest, UpsertBatch_AllEntityTypes) {
	// 1. Edge Batch
	std::vector<Edge> edges;
	for (int i = 0; i < 5; ++i) {
		Edge e;
		e.setId(i + 1);
		edges.push_back(e);
	}
	manager->upsertBatch(edges, EntityChangeType::ADDED);

	auto edgeInfos = manager->getAllDirtyInfos<Edge>();
	EXPECT_EQ(edgeInfos.size(), 5UL);

	// 2. Property Batch
	std::vector<Property> props;
	for (int i = 0; i < 5; ++i) {
		Property p;
		p.setId(i + 1);
		props.push_back(p);
	}
	manager->upsertBatch(props, EntityChangeType::ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<Property>().size(), 5UL);

	// 3. Blob Batch
	std::vector<Blob> blobs;
	for (int i = 0; i < 5; ++i) {
		Blob b;
		b.setId(i + 1);
		blobs.push_back(b);
	}
	manager->upsertBatch(blobs, EntityChangeType::ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<Blob>().size(), 5UL);

	// 4. Index Batch
	std::vector<Index> indexes;
	for (int i = 0; i < 5; ++i) {
		Index idx;
		idx.setId(i + 1);
		indexes.push_back(idx);
	}
	manager->upsertBatch(indexes, EntityChangeType::ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<Index>().size(), 5UL);

	// 5. State Batch
	std::vector<State> states;
	for (int i = 0; i < 5; ++i) {
		State s;
		s.setId(i + 1);
		states.push_back(s);
	}
	manager->upsertBatch(states, EntityChangeType::ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<State>().size(), 5UL);

	// 6. Empty Batch (Coverage for early return)
	std::vector<Node> emptyNodes;
	manager->upsertBatch(emptyNodes, EntityChangeType::ADDED);
	// Should verify dirty count didn't change (still 0 nodes)
	EXPECT_EQ(manager->getAllDirtyInfos<Node>().size(), 0UL);
}

TEST_F(PersistenceManagerTest, Accessors_AllEntityTypes) {
	// Setup one of each
	Property p;
	p.setId(10);
	Blob b;
	b.setId(20);
	Index idx;
	idx.setId(30);
	State s;
	s.setId(40);

	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::ADDED, p));
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::ADDED, b));
	manager->upsert(DirtyEntityInfo<Index>(EntityChangeType::ADDED, idx));
	manager->upsert(DirtyEntityInfo<State>(EntityChangeType::ADDED, s));

	// Test isDirty
	EXPECT_TRUE(manager->isDirty<Property>(10));
	EXPECT_TRUE(manager->isDirty<Blob>(20));
	EXPECT_TRUE(manager->isDirty<Index>(30));
	EXPECT_TRUE(manager->isDirty<State>(40));

	// Negative test
	EXPECT_FALSE(manager->isDirty<Property>(999));

	// Test getAllDirtyInfos
	auto allProps = manager->getAllDirtyInfos<Property>();
	ASSERT_EQ(allProps.size(), 1UL);
	EXPECT_EQ(allProps[0].backup->getId(), 10);

	auto allBlobs = manager->getAllDirtyInfos<Blob>();
	ASSERT_EQ(allBlobs.size(), 1UL);
	EXPECT_EQ(allBlobs[0].backup->getId(), 20);

	auto allIndexes = manager->getAllDirtyInfos<Index>();
	ASSERT_EQ(allIndexes.size(), 1UL);
	EXPECT_EQ(allIndexes[0].backup->getId(), 30);

	auto allStates = manager->getAllDirtyInfos<State>();
	ASSERT_EQ(allStates.size(), 1UL);
	EXPECT_EQ(allStates[0].backup->getId(), 40);
}

TEST_F(PersistenceManagerTest, ClearAllRegistries) {
	// Add various entities
	Node n;
	n.setId(1);
	manager->upsert(DirtyEntityInfo<Node>(EntityChangeType::ADDED, n));
	Edge e;
	e.setId(2);
	manager->upsert(DirtyEntityInfo<Edge>(EntityChangeType::ADDED, e));
	Blob b;
	b.setId(3);
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::ADDED, b));

	EXPECT_TRUE(manager->hasUnsavedChanges());

	// Execute ClearAll
	manager->clearAll();

	// Verify
	EXPECT_FALSE(manager->hasUnsavedChanges());
	EXPECT_FALSE(manager->isDirty<Node>(1));
	EXPECT_FALSE(manager->isDirty<Edge>(2));
	EXPECT_FALSE(manager->isDirty<Blob>(3));

	EXPECT_TRUE(manager->getAllDirtyInfos<Node>().empty());
}

TEST_F(PersistenceManagerTest, RemoveEntityFromRegistry) {
	// Add entities
	Property p;
	p.setId(10);
	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::ADDED, p));

	Blob b;
	b.setId(20);
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::ADDED, b));

	EXPECT_TRUE(manager->isDirty<Property>(10));
	EXPECT_TRUE(manager->isDirty<Blob>(20));

	// Remove
	manager->remove<Property>(10);
	manager->remove<Blob>(20);

	// Verify
	EXPECT_FALSE(manager->isDirty<Property>(10));
	EXPECT_FALSE(manager->isDirty<Blob>(20));

	// Ensure size decreased
	EXPECT_FALSE(manager->hasUnsavedChanges());
}

// Test auto-flush check without callback set
TEST_F(PersistenceManagerTest, AutoFlushNoCallback) {
	// Don't set any callback - checkAndTriggerAutoFlush should return early
	addNode(1);
	addEdge(2);

	// Verify dirty entities exist
	EXPECT_TRUE(manager->hasUnsavedChanges());

	// Create snapshot - this internally calls checkAndTriggerAutoFlush
	// Should not crash even without callback
	auto snapshot = manager->createSnapshot();
	EXPECT_FALSE(snapshot.isEmpty());
}

// Test intermediate flush during batch processing
TEST_F(PersistenceManagerTest, UpsertBatchIntermediateFlush) {
	int flushCount = 0;

	// Set a low threshold to trigger intermediate flush
	manager->setMaxDirtyEntities(3);
	manager->setAutoFlushCallback([&flushCount]() { flushCount++; });

	// Add some initial entities
	addNode(1);
	addNode(2);
	EXPECT_EQ(flushCount, 0) << "Should not flush yet (2 < 3)";

	// Create a batch that will trigger intermediate flush
	std::vector<Node> batch;
	for (int i = 0; i < 5; ++i) {
		Node n;
		n.setId(10 + i);
		batch.push_back(n);
	}

	// Upsert batch - should trigger flush multiple times during processing
	manager->upsertBatch(batch, EntityChangeType::ADDED);

	// Should have triggered flush at least once during batch processing
	EXPECT_GT(flushCount, 0) << "Should have triggered intermediate flush during batch";
}

// Test isDirty returns false for non-existent entities
TEST_F(PersistenceManagerTest, IsDirtyNonExistent) {
	EXPECT_FALSE(manager->isDirty<Node>(999));
	EXPECT_FALSE(manager->isDirty<Edge>(999));
	EXPECT_FALSE(manager->isDirty<Property>(999));
	EXPECT_FALSE(manager->isDirty<Blob>(999));
	EXPECT_FALSE(manager->isDirty<Index>(999));
	EXPECT_FALSE(manager->isDirty<State>(999));
}

// Test getDirtyInfo returns nullopt for non-existent entities
TEST_F(PersistenceManagerTest, GetDirtyInfoNonExistent) {
	EXPECT_FALSE(manager->getDirtyInfo<Node>(999).has_value());
	EXPECT_FALSE(manager->getDirtyInfo<Edge>(999).has_value());
	EXPECT_FALSE(manager->getDirtyInfo<Property>(999).has_value());
	EXPECT_FALSE(manager->getDirtyInfo<Blob>(999).has_value());
	EXPECT_FALSE(manager->getDirtyInfo<Index>(999).has_value());
	EXPECT_FALSE(manager->getDirtyInfo<State>(999).has_value());
}

// Test getAllDirtyInfos returns empty for non-existent types
TEST_F(PersistenceManagerTest, GetAllDirtyInfosEmpty) {
	EXPECT_TRUE(manager->getAllDirtyInfos<Node>().empty());
	EXPECT_TRUE(manager->getAllDirtyInfos<Edge>().empty());
	EXPECT_TRUE(manager->getAllDirtyInfos<Property>().empty());
	EXPECT_TRUE(manager->getAllDirtyInfos<Blob>().empty());
	EXPECT_TRUE(manager->getAllDirtyInfos<Index>().empty());
	EXPECT_TRUE(manager->getAllDirtyInfos<State>().empty());
}
