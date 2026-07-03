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
#include <array>
#include <memory>
#include <span>
#include <vector>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
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
		manager->upsert(DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, n));
	}

	// Helper to add an edge
	void addEdge(int64_t id) const {
		Edge e;
		e.setId(id);
		manager->upsert(DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_ADDED, e));
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

TEST_F(PersistenceManagerTest, VisitDirtyInfosRoutesToTypedRegistry) {
	addNode(100);
	addNode(101);
	const std::array<int64_t, 3> ids{100, 999, 101};
	std::vector<int64_t> visitedIds;
	std::vector<int64_t> foundIds;

	manager->visitDirtyInfos<Node>(
			std::span<const int64_t>(ids), [&](int64_t id, const DirtyEntityInfo<Node> *info) {
				visitedIds.push_back(id);
				if (info && info->backup.has_value()) {
					foundIds.push_back(info->backup->getId());
				}
			});

	EXPECT_EQ(visitedIds, (std::vector<int64_t>{100, 999, 101}));
	EXPECT_EQ(foundIds, (std::vector<int64_t>{100, 101}));
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

TEST_F(PersistenceManagerTest, SnapshotViewKeepsFlushingStateUntilCommit) {
	addNode(1);

	auto snapshot = manager->createSnapshotView();
	ASSERT_NE(snapshot.nodes, nullptr);
	EXPECT_FALSE(snapshot.isEmpty());
	EXPECT_EQ(snapshot.nodes->size(), 1UL);
	EXPECT_TRUE(snapshot.nodes->contains(1));

	// The non-owning flush view must still protect flushing entities from
	// being removed while the storage writer is using them.
	manager->remove<Node>(1);
	EXPECT_TRUE(manager->isDirty<Node>(1));

	// A second snapshot while flushing is non-empty merges active changes into
	// the same flushing layer instead of replacing it.
	addNode(2);
	auto merged = manager->createSnapshotView();
	ASSERT_NE(merged.nodes, nullptr);
	EXPECT_EQ(merged.nodes->size(), 2UL);
	EXPECT_TRUE(merged.nodes->contains(1));
	EXPECT_TRUE(merged.nodes->contains(2));

	manager->commitSnapshot();
	EXPECT_FALSE(manager->hasUnsavedChanges());
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
	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, p));
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
	manager->upsertBatch(edges, EntityChangeType::CHANGE_ADDED);

	auto edgeInfos = manager->getAllDirtyInfos<Edge>();
	EXPECT_EQ(edgeInfos.size(), 5UL);

	// 2. Property Batch
	std::vector<Property> props;
	for (int i = 0; i < 5; ++i) {
		Property p;
		p.setId(i + 1);
		props.push_back(p);
	}
	manager->upsertBatch(props, EntityChangeType::CHANGE_ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<Property>().size(), 5UL);

	// 3. Blob Batch
	std::vector<Blob> blobs;
	for (int i = 0; i < 5; ++i) {
		Blob b;
		b.setId(i + 1);
		blobs.push_back(b);
	}
	manager->upsertBatch(blobs, EntityChangeType::CHANGE_ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<Blob>().size(), 5UL);

	// 4. Index Batch
	std::vector<Index> indexes;
	for (int i = 0; i < 5; ++i) {
		Index idx;
		idx.setId(i + 1);
		indexes.push_back(idx);
	}
	manager->upsertBatch(indexes, EntityChangeType::CHANGE_ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<Index>().size(), 5UL);

	// 5. State Batch
	std::vector<State> states;
	for (int i = 0; i < 5; ++i) {
		State s;
		s.setId(i + 1);
		states.push_back(s);
	}
	manager->upsertBatch(states, EntityChangeType::CHANGE_ADDED);
	EXPECT_EQ(manager->getAllDirtyInfos<State>().size(), 5UL);

	// 6. Empty Batch (Coverage for early return)
	std::vector<Node> emptyNodes;
	manager->upsertBatch(emptyNodes, EntityChangeType::CHANGE_ADDED);
	// Should verify dirty count didn't change (still 0 nodes)
	EXPECT_EQ(manager->getAllDirtyInfos<Node>().size(), 0UL);
}

TEST_F(PersistenceManagerTest, UpsertBatchEmptyInputsAreIgnoredForAllEntityTypes) {
	std::vector<Node> nodes;
	std::vector<Edge> edges;
	std::vector<Property> properties;
	std::vector<Blob> blobs;
	std::vector<Index> indexes;
	std::vector<State> states;

	manager->upsertBatch(nodes, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(edges, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(properties, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(blobs, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(indexes, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(states, EntityChangeType::CHANGE_ADDED);

	EXPECT_FALSE(manager->hasUnsavedChanges());
}

TEST_F(PersistenceManagerTest, UpsertBatchSelectedEmptyInputsAreIgnoredForAllEntityTypes) {
	std::vector<Node> nodes;
	std::vector<Edge> edges;
	std::vector<Property> properties;
	std::vector<Blob> blobs;
	std::vector<Index> indexes;
	std::vector<State> states;

	manager->upsertBatchSelected(nodes, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(edges, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(properties, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(blobs, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(indexes, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(states, {0}, EntityChangeType::CHANGE_ADDED);

	nodes.emplace_back();
	edges.emplace_back();
	properties.emplace_back();
	blobs.emplace_back();
	indexes.emplace_back();
	states.emplace_back();

	manager->upsertBatchSelected(nodes, {}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(edges, {}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(properties, {}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(blobs, {}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(indexes, {}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(states, {}, EntityChangeType::CHANGE_ADDED);

	EXPECT_FALSE(manager->hasUnsavedChanges());
}

TEST_F(PersistenceManagerTest, TransactionalBatchUpsertsDeferAutoFlushForAllEntityTypes) {
	int flushCount = 0;
	manager->setMaxDirtyEntities(1);
	manager->setAutoFlushCallback([&flushCount]() { ++flushCount; });
	manager->setTransactionActive(true);

	std::vector<Node> nodes(1);
	nodes[0].setId(1);
	std::vector<Edge> edges(1);
	edges[0].setId(2);
	std::vector<Property> properties(1);
	properties[0].setId(3);
	std::vector<Blob> blobs(1);
	blobs[0].setId(4);
	std::vector<Index> indexes(1);
	indexes[0].setId(5);
	std::vector<State> states(1);
	states[0].setId(6);

	manager->upsertBatch(nodes, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(edges, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(properties, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(blobs, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(indexes, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatch(states, EntityChangeType::CHANGE_ADDED);

	EXPECT_EQ(flushCount, 0);
	EXPECT_TRUE(manager->isDirty<Node>(1));
	EXPECT_TRUE(manager->isDirty<Edge>(2));
	EXPECT_TRUE(manager->isDirty<Property>(3));
	EXPECT_TRUE(manager->isDirty<Blob>(4));
	EXPECT_TRUE(manager->isDirty<Index>(5));
	EXPECT_TRUE(manager->isDirty<State>(6));
}

TEST_F(PersistenceManagerTest, TransactionalSelectedBatchUpsertsDeferAutoFlushForAllEntityTypes) {
	int flushCount = 0;
	manager->setMaxDirtyEntities(1);
	manager->setAutoFlushCallback([&flushCount]() { ++flushCount; });
	manager->setTransactionActive(true);

	std::vector<Node> nodes(1);
	nodes[0].setId(11);
	std::vector<Edge> edges(1);
	edges[0].setId(12);
	std::vector<Property> properties(1);
	properties[0].setId(13);
	std::vector<Blob> blobs(1);
	blobs[0].setId(14);
	std::vector<Index> indexes(1);
	indexes[0].setId(15);
	std::vector<State> states(1);
	states[0].setId(16);

	manager->upsertBatchSelected(nodes, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(edges, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(properties, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(blobs, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(indexes, {0}, EntityChangeType::CHANGE_ADDED);
	manager->upsertBatchSelected(states, {0}, EntityChangeType::CHANGE_ADDED);

	EXPECT_EQ(flushCount, 0);
	EXPECT_TRUE(manager->isDirty<Node>(11));
	EXPECT_TRUE(manager->isDirty<Edge>(12));
	EXPECT_TRUE(manager->isDirty<Property>(13));
	EXPECT_TRUE(manager->isDirty<Blob>(14));
	EXPECT_TRUE(manager->isDirty<Index>(15));
	EXPECT_TRUE(manager->isDirty<State>(16));
}

TEST_F(PersistenceManagerTest, HasDirtyInfoOfTypesRoutesAllEntityTypes) {
	Node node;
	node.setId(1);
	Edge edge;
	edge.setId(2);
	Property property;
	property.setId(3);
	Blob blob;
	blob.setId(4);
	Index index;
	index.setId(5);
	State state;
	state.setId(6);

	const std::vector<EntityChangeType> added = {EntityChangeType::CHANGE_ADDED};
	const std::vector<EntityChangeType> deleted = {EntityChangeType::CHANGE_DELETED};

	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Node>(added));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Edge>(added));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Property>(added));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Blob>(added));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Index>(added));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<State>(added));

	manager->upsert(DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, node));
	manager->upsert(DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_ADDED, edge));
	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, property));
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED, blob));
	manager->upsert(DirtyEntityInfo<Index>(EntityChangeType::CHANGE_ADDED, index));
	manager->upsert(DirtyEntityInfo<State>(EntityChangeType::CHANGE_ADDED, state));

	EXPECT_TRUE(manager->hasDirtyInfoOfTypes<Node>(added));
	EXPECT_TRUE(manager->hasDirtyInfoOfTypes<Edge>(added));
	EXPECT_TRUE(manager->hasDirtyInfoOfTypes<Property>(added));
	EXPECT_TRUE(manager->hasDirtyInfoOfTypes<Blob>(added));
	EXPECT_TRUE(manager->hasDirtyInfoOfTypes<Index>(added));
	EXPECT_TRUE(manager->hasDirtyInfoOfTypes<State>(added));

	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Node>(deleted));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Edge>(deleted));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Property>(deleted));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Blob>(deleted));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<Index>(deleted));
	EXPECT_FALSE(manager->hasDirtyInfoOfTypes<State>(deleted));
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

	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, p));
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED, b));
	manager->upsert(DirtyEntityInfo<Index>(EntityChangeType::CHANGE_ADDED, idx));
	manager->upsert(DirtyEntityInfo<State>(EntityChangeType::CHANGE_ADDED, s));

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
	manager->upsert(DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, n));
	Edge e;
	e.setId(2);
	manager->upsert(DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_ADDED, e));
	Blob b;
	b.setId(3);
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED, b));

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
	manager->upsert(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, p));

	Blob b;
	b.setId(20);
	manager->upsert(DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED, b));

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
	manager->upsertBatch(batch, EntityChangeType::CHANGE_ADDED);

	// Should have triggered flush at least once during batch processing
	EXPECT_GT(flushCount, 0) << "Should have triggered intermediate flush during batch";
}

TEST_F(PersistenceManagerTest, EdgeBatchIntermediateFlushUsesTypedRegistryCounts) {
	int flushCount = 0;
	manager->setMaxDirtyEntities(1);
	manager->setAutoFlushCallback([&flushCount]() { ++flushCount; });

	std::vector<Edge> edges;
	for (int i = 0; i < 2; ++i) {
		Edge edge;
		edge.setId(100 + i);
		edges.push_back(edge);
	}

	manager->upsertBatch(edges, EntityChangeType::CHANGE_ADDED);

	EXPECT_GE(flushCount, 1);
	EXPECT_TRUE(manager->isDirty<Edge>(100));
	EXPECT_TRUE(manager->isDirty<Edge>(101));
}

TEST_F(PersistenceManagerTest, BatchAutoFlushCallbackCanDeferFinalRecheck) {
	int flushCount = 0;
	manager->setMaxDirtyEntities(1);
	manager->setAutoFlushCallback([&]() {
		++flushCount;
		manager->setTransactionActive(true);
	});

	Node node;
	node.setId(77);
	manager->upsertBatch(std::vector<Node>{node}, EntityChangeType::CHANGE_ADDED);

	EXPECT_EQ(flushCount, 1);
	EXPECT_TRUE(manager->isTransactionActive());
	EXPECT_TRUE(manager->isDirty<Node>(77));
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

// Test isTransactionActive before and after setTransactionActive
TEST_F(PersistenceManagerTest, IsTransactionActive_DefaultFalse) {
	// Initially transaction should not be active
	EXPECT_FALSE(manager->isTransactionActive());
}

TEST_F(PersistenceManagerTest, IsTransactionActive_SetTrue) {
	manager->setTransactionActive(true);
	EXPECT_TRUE(manager->isTransactionActive());
}

TEST_F(PersistenceManagerTest, IsTransactionActive_SetFalseAfterTrue) {
	manager->setTransactionActive(true);
	EXPECT_TRUE(manager->isTransactionActive());
	manager->setTransactionActive(false);
	EXPECT_FALSE(manager->isTransactionActive());
}

// --- FlushSnapshot::isEmpty() branch coverage ---
// Tests each short-circuit branch in the && chain

TEST(FlushSnapshotTest, EmptySnapshotIsEmpty) {
	FlushSnapshot snapshot;
	EXPECT_TRUE(snapshot.isEmpty());
}

TEST(FlushSnapshotTest, NonEmptyNodes) {
	FlushSnapshot snapshot;
	Node n;
	n.setId(1);
	snapshot.nodes.emplace(1, DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, n));
	EXPECT_FALSE(snapshot.isEmpty());
}

TEST(FlushSnapshotTest, NonEmptyEdges) {
	FlushSnapshot snapshot;
	Edge e;
	e.setId(1);
	snapshot.edges.emplace(1, DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_ADDED, e));
	EXPECT_FALSE(snapshot.isEmpty());
}

TEST(FlushSnapshotTest, NonEmptyProperties) {
	FlushSnapshot snapshot;
	Property p;
	p.setId(1);
	snapshot.properties.emplace(1, DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, p));
	EXPECT_FALSE(snapshot.isEmpty());
}

TEST(FlushSnapshotTest, NonEmptyBlobs) {
	FlushSnapshot snapshot;
	Blob b;
	b.setId(1);
	snapshot.blobs.emplace(1, DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED, b));
	EXPECT_FALSE(snapshot.isEmpty());
}

TEST(FlushSnapshotTest, NonEmptyIndexes) {
	FlushSnapshot snapshot;
	Index idx;
	idx.setId(1);
	snapshot.indexes.emplace(1, DirtyEntityInfo<Index>(EntityChangeType::CHANGE_ADDED, idx));
	EXPECT_FALSE(snapshot.isEmpty());
}

TEST(FlushSnapshotTest, NonEmptyStates) {
	FlushSnapshot snapshot;
	State s;
	s.setId(1);
	snapshot.states.emplace(1, DirtyEntityInfo<State>(EntityChangeType::CHANGE_ADDED, s));
	EXPECT_FALSE(snapshot.isEmpty());
}

TEST(FlushSnapshotViewTest, NullPointersAreEmpty) {
	FlushSnapshotView view;
	EXPECT_TRUE(view.isEmpty());
}

TEST(FlushSnapshotViewTest, EmptyMapsAreEmpty) {
	DirtyEntityRegistry<Node>::DirtyMap nodes;
	DirtyEntityRegistry<Edge>::DirtyMap edges;
	DirtyEntityRegistry<Property>::DirtyMap properties;
	DirtyEntityRegistry<Blob>::DirtyMap blobs;
	DirtyEntityRegistry<Index>::DirtyMap indexes;
	DirtyEntityRegistry<State>::DirtyMap states;

	FlushSnapshotView view{&nodes, &edges, &properties, &blobs, &indexes, &states};
	EXPECT_TRUE(view.isEmpty());
}

TEST(FlushSnapshotViewTest, EachEntityMapMakesViewNonEmpty) {
	Node node;
	node.setId(1);
	Edge edge;
	edge.setId(2);
	Property property;
	property.setId(3);
	Blob blob;
	blob.setId(4);
	Index index;
	index.setId(5);
	State state;
	state.setId(6);

	DirtyEntityRegistry<Node>::DirtyMap nodes;
	nodes.emplace(node.getId(), DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, node));
	DirtyEntityRegistry<Edge>::DirtyMap edges;
	edges.emplace(edge.getId(), DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_ADDED, edge));
	DirtyEntityRegistry<Property>::DirtyMap properties;
	properties.emplace(property.getId(), DirtyEntityInfo<Property>(EntityChangeType::CHANGE_ADDED, property));
	DirtyEntityRegistry<Blob>::DirtyMap blobs;
	blobs.emplace(blob.getId(), DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_ADDED, blob));
	DirtyEntityRegistry<Index>::DirtyMap indexes;
	indexes.emplace(index.getId(), DirtyEntityInfo<Index>(EntityChangeType::CHANGE_ADDED, index));
	DirtyEntityRegistry<State>::DirtyMap states;
	states.emplace(state.getId(), DirtyEntityInfo<State>(EntityChangeType::CHANGE_ADDED, state));

	EXPECT_FALSE((FlushSnapshotView{&nodes, nullptr, nullptr, nullptr, nullptr, nullptr}).isEmpty());
	EXPECT_FALSE((FlushSnapshotView{nullptr, &edges, nullptr, nullptr, nullptr, nullptr}).isEmpty());
	EXPECT_FALSE((FlushSnapshotView{nullptr, nullptr, &properties, nullptr, nullptr, nullptr}).isEmpty());
	EXPECT_FALSE((FlushSnapshotView{nullptr, nullptr, nullptr, &blobs, nullptr, nullptr}).isEmpty());
	EXPECT_FALSE((FlushSnapshotView{nullptr, nullptr, nullptr, nullptr, &indexes, nullptr}).isEmpty());
	EXPECT_FALSE((FlushSnapshotView{nullptr, nullptr, nullptr, nullptr, nullptr, &states}).isEmpty());
}
