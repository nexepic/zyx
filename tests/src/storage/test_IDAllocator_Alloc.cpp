/**
 * @file test_IDAllocator_Alloc.cpp
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

#include "storage/IDAllocatorTestFixture.hpp"

// ==========================================
// AllocateId_* Tests
// ==========================================

TEST_F(IDAllocatorTest, AllocateId_Returns_MemoryOnly_ID_Direct) {
	// 1. Insert a node (ID 1) - Do NOT save.
	graph::Node n1 = insertNode("DirtyNode");
	EXPECT_EQ(n1.getId(), 1);

	// 2. Delete the node immediately.
	deleteNode(n1.getId());

	// 3. Insert a new node.
	// The allocator should pop ID 1 from VolatileCache immediately.
	graph::Node n2 = insertNode("NewNode");

	EXPECT_EQ(n2.getId(), 1) << "Allocator failed to reuse Dirty ID";

	// 4. Verify uniqueness
	graph::Node n3 = insertNode("NextNode");
	EXPECT_EQ(n3.getId(), 2);
}

TEST_F(IDAllocatorTest, AllocateId_BasicFunctionality) {
	// Test that per-type allocators produce valid IDs
	int64_t nodeId = nodeAllocator->allocate();
	EXPECT_GT(nodeId, 0);

	int64_t edgeId = edgeAllocator->allocate();
	EXPECT_GT(edgeId, 0);
}

TEST_F(IDAllocatorTest, AllocateId_L2ColdCachePath) {
	// Test allocateId when L1 is empty but L2 has IDs (line 214-217)
	nodeAllocator->setCacheLimits(1, 100, 50000); // Tiny L1 = 1

	// Create and persist nodes
	for (int i = 0; i < 10; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// Delete nodes - first goes to L1, rest overflow to L2
	for (int i = 1; i <= 10; ++i) {
		deleteNode(i);
	}

	// Allocate - should drain L1 first (1 item), then hit L2
	int64_t id1 = nodeAllocator->allocate();
	EXPECT_LE(id1, 10);

	int64_t id2 = nodeAllocator->allocate();
	EXPECT_LE(id2, 10);
}

TEST_F(IDAllocatorTest, AllocateId_VolatileCacheFirst) {
	// Test that volatile cache is checked first (line 199-203)
	// Use direct allocator API to avoid DataManager interactions

	// Allocate IDs 1, 2, 3 without persisting
	int64_t id1 = nodeAllocator->allocate();
	int64_t id2 = nodeAllocator->allocate();
	int64_t id3 = nodeAllocator->allocate();
	EXPECT_EQ(id1, 1);
	EXPECT_EQ(id2, 2);
	EXPECT_EQ(id3, 3);

	// Free id1 -> goes to volatile cache (not persisted)
	nodeAllocator->free(id1);

	// Next allocate should return id1 from volatile cache
	int64_t recycled = nodeAllocator->allocate();
	EXPECT_EQ(recycled, id1) << "Should return volatile cache ID first";
}

TEST_F(IDAllocatorTest, AllocateId_FallbackToSequentialWhenDiskExhausted) {
	// Test the full fallback path: volatile empty -> L1 empty -> L2 empty ->
	// disk exhausted -> new sequential ID (lines 220-241)
	nodeAllocator->clearPersistedCaches();

	// Create nodes and persist (no deletions, so disk has no inactive IDs)
	for (int i = 0; i < 3; ++i) {
		(void)insertNode("N");
	}
	fileStorage->save();

	// Clear all caches to force going through all layers
	nodeAllocator->clearPersistedCaches();

	// Allocate - volatile is empty, L1 is empty, L2 is empty,
	// disk scan finds nothing (all active) -> exhausted -> sequential
	int64_t id = nodeAllocator->allocate();
	EXPECT_EQ(id, 4); // Next sequential after 3

	// Second allocation should also be sequential (disk already exhausted)
	int64_t id2 = nodeAllocator->allocate();
	EXPECT_EQ(id2, 5);
}

TEST_F(IDAllocatorTest, AllocateId_DiskScan_FoundButHotEmpty) {
	// Covers line 224: hot.empty() == false after fetchInactiveIdsFromDisk
	// Create inactive IDs on disk, clear caches, allocate to trigger disk scan

	nodeAllocator->setCacheLimits(5, 100, 100);

	// Create and persist many nodes
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 20; i++) {
		nodes.push_back(insertNode("N"));
	}
	fileStorage->flush();

	// Delete some nodes to create inactive IDs on disk
	for (int i = 0; i < 15; i++) {
		deleteNode(nodes[i].getId());
	}
	fileStorage->flush();

	// Clear all caches to force disk scan path
	nodeAllocator->clearPersistedCaches();

	// Allocate should trigger disk scan and find inactive IDs
	int64_t id = nodeAllocator->allocate();
	EXPECT_GT(id, 0);
	// The ID should be a recycled one (from 1-15 range)
	EXPECT_LE(id, 15) << "Should get recycled ID from disk scan";
}

TEST_F(IDAllocatorTest, AllocateId_DiskScanFindsNothing_HotStillEmpty) {
	// Targets line 224 False branch: hot.empty() after fetchInactiveIdsFromDisk
	// Strategy: Create nodes (all active), clear caches, allocate.
	// Disk scan finds no inactive IDs, so hot stays empty after fetch.

	// 1. Create active nodes (no deletions)
	for (int i = 0; i < 5; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Clear all caches
	nodeAllocator->clearPersistedCaches();

	// 3. Allocate - disk scan finds all active entities, hot remains empty
	// Falls through to allocateNewSequentialId
	int64_t id = nodeAllocator->allocate();
	EXPECT_EQ(id, 6); // Next sequential after 5
}

TEST_F(IDAllocatorTest, AllocateId_L2ColdCacheHit) {
	// Targets line 214: !cold.empty() == true (L2 cold cache path)
	// Strategy: Set tiny L1, persist many nodes, delete them (overflow L1 -> L2),
	// then consume all L1 entries, so next allocate hits L2.

	// Set L1 to 2 entries only, L2 to 100 intervals
	nodeAllocator->setCacheLimits(2, 100, 50000);

	// 1. Create and persist nodes
	for (int i = 0; i < 20; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete 10 nodes - first 2 go to L1, rest overflow to L2 cold cache
	for (int i = 1; i <= 10; ++i) {
		deleteNode(i);
	}

	// 3. Drain L1 (2 entries)
	int64_t fromL1a = nodeAllocator->allocate();
	int64_t fromL1b = nodeAllocator->allocate();
	EXPECT_LE(fromL1a, 10);
	EXPECT_LE(fromL1b, 10);

	// 4. Next allocation should hit L2 cold cache (line 214)
	int64_t fromL2 = nodeAllocator->allocate();
	EXPECT_GE(fromL2, 1);
	EXPECT_LE(fromL2, 10) << "Should get ID from L2 cold cache";
}

// ==========================================
// BatchAllocation_* Tests
// ==========================================

TEST_F(IDAllocatorTest, BatchAllocation_AllTypes) {
	constexpr size_t BATCH = 5;

	// 1. Edge
	int64_t startEdge = edgeAllocator->allocateBatch(BATCH);
	EXPECT_GT(startEdge, 0);
	// Verify counter incremented
	EXPECT_EQ(static_cast<uint64_t>(edgeAllocator->getCurrentMaxId()), static_cast<uint64_t>(startEdge + BATCH - 1));

	// 2. Property
	auto propAllocator = fileStorage->getIDAllocator(graph::EntityType::Property);
	int64_t startProp = propAllocator->allocateBatch(BATCH);
	EXPECT_GT(startProp, 0);
	EXPECT_EQ(static_cast<uint64_t>(propAllocator->getCurrentMaxId()), static_cast<uint64_t>(startProp + BATCH - 1));

	// 3. Blob
	auto blobAllocator = fileStorage->getIDAllocator(graph::EntityType::Blob);
	int64_t startBlob = blobAllocator->allocateBatch(BATCH);
	EXPECT_GT(startBlob, 0);
	EXPECT_EQ(static_cast<uint64_t>(blobAllocator->getCurrentMaxId()), static_cast<uint64_t>(startBlob + BATCH - 1));

	// 4. Index
	auto indexAllocator = fileStorage->getIDAllocator(graph::EntityType::Index);
	int64_t startIndex = indexAllocator->allocateBatch(BATCH);
	EXPECT_GT(startIndex, 0);
	EXPECT_EQ(static_cast<uint64_t>(indexAllocator->getCurrentMaxId()), static_cast<uint64_t>(startIndex + BATCH - 1));

	// 5. State
	auto stateAllocator = fileStorage->getIDAllocator(graph::EntityType::State);
	int64_t startState = stateAllocator->allocateBatch(BATCH);
	EXPECT_GT(startState, 0);
	EXPECT_EQ(static_cast<uint64_t>(stateAllocator->getCurrentMaxId()), static_cast<uint64_t>(startState + BATCH - 1));
}

TEST_F(IDAllocatorTest, BatchAllocation_ZeroCount) {
	// Test allocateIdBatch with count == 0
	int64_t startId = nodeAllocator->allocateBatch(0);
	EXPECT_EQ(startId, 0);
}

TEST_F(IDAllocatorTest, BatchAllocation_LargeBatch) {
	// Test allocateIdBatch with a large batch size
	constexpr size_t LARGE_BATCH = 100;
	int64_t startId = nodeAllocator->allocateBatch(LARGE_BATCH);
	EXPECT_EQ(startId, 1);
	EXPECT_EQ(static_cast<size_t>(nodeAllocator->getCurrentMaxId()), LARGE_BATCH);
}

TEST_F(IDAllocatorTest, BatchAllocation_NodeType) {
	// Test allocateIdBatch for Node type specifically (line 257-260)
	int64_t startNode = nodeAllocator->allocateBatch(10);
	EXPECT_GT(startNode, 0);
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), startNode + 9);
}

TEST_F(IDAllocatorTest, AllocateIdBatch_MultipleTypesIndependently) {
	// Test batch allocation for multiple types independently
	constexpr size_t BATCH = 10;

	int64_t startNode = nodeAllocator->allocateBatch(BATCH);
	EXPECT_EQ(startNode, 1);

	int64_t startEdge = edgeAllocator->allocateBatch(BATCH);
	EXPECT_EQ(startEdge, 1);

	// Node and Edge should have independent counters
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), static_cast<int64_t>(BATCH));
	EXPECT_EQ(edgeAllocator->getCurrentMaxId(), static_cast<int64_t>(BATCH));
}

TEST_F(IDAllocatorTest, MixedSequentialAndBatchAllocation) {
	// Test mixing sequential and batch allocation
	// 1. Sequential allocation
	int64_t id1 = nodeAllocator->allocate();
	EXPECT_EQ(id1, 1);

	// 2. Batch allocation
	constexpr size_t BATCH = 5;
	int64_t batchStart = nodeAllocator->allocateBatch(BATCH);
	EXPECT_EQ(batchStart, 2); // Should start from 2

	// 3. Sequential again
	int64_t id2 = nodeAllocator->allocate();
	EXPECT_EQ(id2, 2 + static_cast<int64_t>(BATCH)); // Should be after batch

	// Max should be 1 (first alloc) + 5 (batch) + 1 (second alloc) = 7
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 1 + static_cast<int64_t>(BATCH) + 1);
}

// ==========================================
// AllocateNewSequentialId_* Tests
// ==========================================

TEST_F(IDAllocatorTest, AllocateNewSequentialId_AllTypes) {
	// Test allocateNewSequentialId for all entity types (lines 435-447)
	// Each type should increment independently
	int64_t nodeId = nodeAllocator->allocate();
	EXPECT_GT(nodeId, 0);

	int64_t edgeId = edgeAllocator->allocate();
	EXPECT_GT(edgeId, 0);

	auto propAllocator = fileStorage->getIDAllocator(graph::EntityType::Property);
	int64_t propId = propAllocator->allocate();
	EXPECT_GT(propId, 0);

	auto blobAllocator = fileStorage->getIDAllocator(graph::EntityType::Blob);
	int64_t blobId = blobAllocator->allocate();
	EXPECT_GT(blobId, 0);

	auto indexAllocator = fileStorage->getIDAllocator(graph::EntityType::Index);
	int64_t indexId = indexAllocator->allocate();
	EXPECT_GT(indexId, 0);

	auto stateAllocator = fileStorage->getIDAllocator(graph::EntityType::State);
	int64_t stateId = stateAllocator->allocate();
	EXPECT_GT(stateId, 0);

	// Allocating again should give the next sequential ID
	int64_t nodeId2 = nodeAllocator->allocate();
	EXPECT_EQ(nodeId2, nodeId + 1);
}

TEST_F(IDAllocatorTest, AllocateNewSequentialId_AllNonNodeTypes) {
	// Test allocateNewSequentialId for Blob, Index, and State types

	const std::vector<graph::EntityType> types = {graph::EntityType::Blob, graph::EntityType::Index, graph::EntityType::State};
	for (auto entityType : types) {
		auto alloc = fileStorage->getIDAllocator(entityType);
		int64_t id1 = alloc->allocate();
		EXPECT_GT(id1, 0);
		int64_t id2 = alloc->allocate();
		EXPECT_EQ(id2, id1 + 1) << "Failed for entityType=" << static_cast<uint32_t>(entityType);
	}
}

// ==========================================
// PerTypeAllocators_* Tests
// ==========================================

TEST_F(IDAllocatorTest, PerTypeAllocators_NotNull) {
	// Verify that per-type allocators are always obtainable
	EXPECT_NE(fileStorage->getIDAllocator(graph::EntityType::Node), nullptr);
	EXPECT_NE(fileStorage->getIDAllocator(graph::EntityType::Edge), nullptr);
	EXPECT_NE(fileStorage->getIDAllocator(graph::EntityType::Property), nullptr);
	EXPECT_NE(fileStorage->getIDAllocator(graph::EntityType::Blob), nullptr);
	EXPECT_NE(fileStorage->getIDAllocator(graph::EntityType::Index), nullptr);
	EXPECT_NE(fileStorage->getIDAllocator(graph::EntityType::State), nullptr);
}

TEST_F(IDAllocatorTest, PerTypeAllocators_IndependentCounters) {
	// Verify that each per-type allocator maintains independent counters
	int64_t nodeId = nodeAllocator->allocate();
	int64_t edgeId = edgeAllocator->allocate();
	EXPECT_EQ(nodeId, 1);
	EXPECT_EQ(edgeId, 1); // Independent from node counter
}

// ==========================================
// AllEntityTypes_* Tests
// ==========================================

TEST_F(IDAllocatorTest, AllEntityTypes_BatchAndSequential) {
	// Test batch and sequential allocation for all entity types
	struct TypeInfo {
		graph::EntityType entityType;
		std::string name;
	};

	std::vector<TypeInfo> types = {
		{graph::EntityType::Node, "Node"},
		{graph::EntityType::Edge, "Edge"},
		{graph::EntityType::Property, "Property"},
		{graph::EntityType::Blob, "Blob"},
		{graph::EntityType::Index, "Index"},
		{graph::EntityType::State, "State"},
	};

	for (const auto &typeInfo: types) {
		auto alloc = fileStorage->getIDAllocator(typeInfo.entityType);
		// Sequential
		int64_t seqId = alloc->allocate();
		EXPECT_GT(seqId, 0) << "Sequential allocation failed for " << typeInfo.name;

		// Batch
		constexpr size_t BATCH = 3;
		int64_t batchStart = alloc->allocateBatch(BATCH);
		EXPECT_GT(batchStart, 0) << "Batch allocation failed for " << typeInfo.name;
	}
}
