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
