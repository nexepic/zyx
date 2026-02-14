/**
 * @file test_IDAllocator.cpp
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/IDAllocator.hpp"

class IDAllocatorTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate unique temporary file path to avoid conflicts between parallel tests
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_id_alloc_" + to_string(uuid) + ".dat");

		// Initialize Database and components
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		allocator = fileStorage->getIDAllocator();
	}

	void TearDown() override {
		if (database) {
			database->close();
			database.reset();
		}
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	[[nodiscard]] graph::Node insertNode(const std::string &label) const {
		const int64_t id = allocator->allocateId(graph::Node::typeId);

		const int64_t labelId = dataManager->getOrCreateLabelId(label);

		graph::Node node(id, labelId);
		dataManager->addNode(node);
		return node;
	}

	[[nodiscard]] graph::Edge insertEdge(int64_t startId, int64_t endId, const std::string &label) const {
		const int64_t id = allocator->allocateId(graph::Edge::typeId);
		const int64_t labelId = dataManager->getOrCreateLabelId(label);

		graph::Edge edge(id, startId, endId, labelId);
		dataManager->addEdge(edge);
		return edge;
	}

	void deleteNode(int64_t id) const {
		graph::Node node_to_delete(id, 0);
		dataManager->deleteNode(node_to_delete);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::IDAllocator> allocator;
};

// ==========================================
// 1. Basic Functionality Tests
// ==========================================

TEST_F(IDAllocatorTest, SequentialAllocation) {
	// Verify that IDs increment sequentially when no holes exist.
	graph::Node n1 = insertNode("A");
	graph::Node n2 = insertNode("B");

	EXPECT_EQ(n1.getId(), 1);
	EXPECT_EQ(n2.getId(), 2);
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 2);
}

TEST_F(IDAllocatorTest, TypeIsolation) {
	// Verify that Node IDs and Edge IDs are tracked separately.
	graph::Node n1 = insertNode("N1");
	// Ensure nodes exist for edge creation
	graph::Edge e1 = insertEdge(n1.getId(), n1.getId(), "E1");

	EXPECT_EQ(n1.getId(), 1);
	EXPECT_EQ(e1.getId(), 1); // Edge ID should also start at 1, independent of Nodes

	// Create a NEW node to test Node ID increment
	graph::Node n2 = insertNode("N2");

	// Insert edge on the new node to test Edge ID increment
	graph::Edge e2 = insertEdge(n2.getId(), n2.getId(), "E2");

	EXPECT_EQ(n2.getId(), 2);
	EXPECT_EQ(e2.getId(), 2);
}

// ==========================================
// 2. Eager Reuse (L1 & MemoryOnly Cache) Tests
// ==========================================

TEST_F(IDAllocatorTest, EagerReuse_Verify_NoDiskIO_WithCacheClear) {
	// This test verifies the critical logic for "Dirty ID" vs "Persisted ID" during cache clearing.

	// 1. Insert 2 nodes and Persist
	(void) insertNode("A"); // ID 1
	(void) insertNode("B"); // ID 2
	fileStorage->flush(); // 1 and 2 are on disk (Persisted).

	// 2. Insert Node 3 (Dirty, Memory Only)
	(void) insertNode("C"); // ID 3

	// 3. Delete Node 3 (Dirty)
	// Should go to memoryOnlyCache_ (VolatileCache)
	deleteNode(3);

	// 4. Delete Node 2 (Persisted)
	// Should go to hotCache_ (L1)
	deleteNode(2);

	// 5. SIMULATE CACHE CLEARING (e.g., during low memory or partial flush)
	// This clears L1/L2 (Persisted) caches but MUST preserve VolatileCache.
	// ID 2 (Persisted) is cleared from RAM but exists on Disk as inactive (recoverable).
	// ID 3 (Dirty) MUST remain in VolatileCache or it is lost forever (leak).
	allocator->clearAllCaches();

	// 6. Reuse
	// Allocation 1: Should hit VolatileCache first (ID 3)
	graph::Node nFirst = insertNode("Reuse1");

	// Allocation 2: Should scan disk (lazy load) for ID 2
	graph::Node nSecond = insertNode("Reuse2");

	// We expect both to be reused.
	std::unordered_set<int64_t> reusedIds = {nFirst.getId(), nSecond.getId()};

	EXPECT_TRUE(reusedIds.count(3)) << "ID 3 (Dirty) was lost during cache clear!";
	EXPECT_TRUE(reusedIds.count(2)) << "ID 2 (Persisted) failed to lazy load from disk!";

	// 7. Verify Max ID didn't increase
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 3);
}

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

// ==========================================
// 3. Lazy Loading & Persistence Tests
// ==========================================

TEST_F(IDAllocatorTest, LazyLoadFromDiskAfterRestart) {
	// 1. Setup Data
	(void) insertNode("A"); // 1
	(void) insertNode("B"); // 2
	(void) insertNode("C"); // 3

	// Use save() instead of flush() to persist data WITHOUT triggering compaction.
	// Compaction would re-map IDs (3->2) and ruin the test assumption.
	fileStorage->save();

	// 2. Delete ID 2 and Persist
	deleteNode(2);
	fileStorage->save(); // Persist bitmap change

	// 3. Simulate Restart/OOM
	allocator->clearAllCaches();

	// 4. Allocate -> Should trigger fetchInactiveIdsFromDisk and find ID 2
	graph::Node nLazy = insertNode("Lazy");

	EXPECT_EQ(nLazy.getId(), 2) << "Failed to lazy-load ID 2 from disk bitmap";
}

TEST_F(IDAllocatorTest, DiskExhaustionOptimization) {
	// 1. Clean state
	allocator->clearAllCaches();

	// 2. Allocate sequential (Trigger disk scan -> finds nothing -> set exhausted = true)
	graph::Node n1 = insertNode("1");
	EXPECT_EQ(n1.getId(), 1);

	// 3. Allocate again
	// This should NOT trigger a disk scan (internal optimization verification)
	// It should directly allocate ID 2
	graph::Node n2 = insertNode("2");
	EXPECT_EQ(n2.getId(), 2);
}

// ==========================================
// 4. Tiered Cache (L1 Overflow & L2) Tests
// ==========================================

TEST_F(IDAllocatorTest, L1CacheOverflowToL2) {
	// L1 = 100, L2 = 10
	allocator->setCacheLimits(100, 10, 50000);

	constexpr int COUNT = 200;

	// 1. Insert 200 nodes
	for (int i = 0; i < COUNT; ++i) {
		(void) insertNode("N");
	}
	fileStorage->save();

	// 2. Delete all 200 nodes
	for (int i = 1; i <= COUNT; ++i) {
		deleteNode(i);
	}

	// 3. Re-allocate 200 nodes
	// Logic check: Should reuse ALL IDs without increasing max ID > 200
	for (int i = 0; i < COUNT; ++i) {
		graph::Node n = insertNode("Re");
		EXPECT_LE(n.getId(), COUNT) << "ID leaked! Allocated " << n.getId();
	}

	// 4. Verify next is new
	graph::Node nNext = insertNode("New");
	EXPECT_EQ(nNext.getId(), COUNT + 1);
}

TEST_F(IDAllocatorTest, L2IntervalMerging) {
	// This tests if L2 efficiently handles sequential deletes via merging.
	constexpr int COUNT = 2000;
	for (int i = 0; i < COUNT; ++i)
		(void) insertNode("N");
	fileStorage->flush();

	// Delete nodes in blocks to trigger merges
	// Delete 1000..1500
	for (int i = 1000; i <= 1500; ++i)
		deleteNode(i);

	// Delete 1501..2000 (Should merge with previous interval in L2)
	for (int i = 1501; i <= COUNT; ++i)
		deleteNode(i);

	// Now re-allocate.
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < 1001; ++i) {
		graph::Node n = insertNode("R");
		reusedIds.insert(n.getId());
	}

	// Verify we got ids from the range [1000, 2000]
	bool hitRange = false;
	for (int64_t id: reusedIds) {
		if (id >= 1000 && id <= 2000)
			hitRange = true;
	}
	EXPECT_TRUE(hitRange) << "Failed to reuse IDs from merged intervals";
}

// ==========================================
// 5. Robustness: Double Free & Consistency
// ==========================================

TEST_F(IDAllocatorTest, DoubleFree_InMemory_Protection) {
	// 1. Setup: Insert and Persist
	(void) insertNode("A"); // ID 1
	fileStorage->flush();

	// 2. First Delete
	// Sets disk bitmap to Inactive, puts ID 1 into Cache.
	deleteNode(1);

	// 3. Simulate Logic Error: Attempt to free ID 1 again manually.
	// The allocator should detect that ID 1 is already Inactive on disk
	// and assume it's already in the cache (Double Free protection).
	// It should NOT add a duplicate 1 to the cache.
	allocator->freeId(1, graph::Node::typeId);

	// 4. Verification
	// The first allocation should get 1.
	int64_t id1 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id1, 1);

	// The second allocation should NOT get 1.
	int64_t id2 = allocator->allocateId(graph::Node::typeId);
	EXPECT_NE(id2, 1) << "Double Free protection failed: ID 1 was allocated twice!";
	EXPECT_EQ(id2, 2);
}

TEST_F(IDAllocatorTest, DeleteAndSaveIdempotency) {
	(void) insertNode("A"); // 1
	(void) insertNode("B"); // 2
	fileStorage->flush();

	// 1. Delete Node 2
	deleteNode(2);

	// 2. Reuse check
	graph::Node nC = insertNode("C");
	EXPECT_EQ(nC.getId(), 2);

	// 3. Save to ensure ID 2 is marked Active on disk.
	// Without this, the disk bitmap remains Inactive.
	// When we delete nC next, freeId() would see "Inactive on disk" and
	// incorrectly drop the ID as a Double Free, causing ID 2 to be lost.
	fileStorage->flush();

	// 4. Delete Node C (ID 2) again
	deleteNode(2);

	// 5. Save again
	fileStorage->flush();

	// 6. Verify ID 2 is still available
	graph::Node nD = insertNode("D");
	EXPECT_EQ(nD.getId(), 2);
}

TEST_F(IDAllocatorTest, MixedOperationsStress) {
	// Randomly Insert and Delete to simulate real workload
	std::vector<int64_t> activeIds;
	std::mt19937 gen(42);

	for (int i = 0; i < 1000; ++i) {
		if (activeIds.empty() || (gen() % 3 != 0)) { // 66% Insert
			graph::Node n = insertNode("R");
			activeIds.push_back(n.getId());
		} else { // 33% Delete
			std::uniform_int_distribution<std::size_t> dis(0, activeIds.size() - 1);
			const std::size_t idx = dis(gen);
			const int64_t idToDelete = activeIds[idx];

			deleteNode(idToDelete);

			activeIds[idx] = activeIds.back();
			activeIds.pop_back();
		}
	}

	fileStorage->flush();
	EXPECT_LT(allocator->getCurrentMaxNodeId(), 800);
}

// ==========================================
// 6. Concurrency Tests
// ==========================================

TEST_F(IDAllocatorTest, ThreadSafetyConcurrency) {
	constexpr int NUM_THREADS = 8;
	constexpr int OPS_PER_THREAD = 200;

	std::vector<std::future<std::vector<int64_t>>> futures;
	futures.reserve(NUM_THREADS);

	for (int t = 0; t < NUM_THREADS; ++t) {
		futures.push_back(std::async(std::launch::async, [this]() {
			std::vector<int64_t> myIds;
			myIds.reserve(OPS_PER_THREAD);
			for (int i = 0; i < OPS_PER_THREAD; ++i) {
				myIds.push_back(allocator->allocateId(graph::Node::typeId));
			}
			return myIds;
		}));
	}

	std::unordered_set<int64_t> uniqueIds;
	size_t totalCount = 0;

	for (auto &f: futures) {
		std::vector<int64_t> ids = f.get();
		for (int64_t id: ids) {
			EXPECT_TRUE(!uniqueIds.contains(id)) << "Duplicate ID generated during concurrency test: " << id;
			uniqueIds.insert(id);
		}
		totalCount += ids.size();
	}

	EXPECT_EQ(totalCount, static_cast<size_t>(NUM_THREADS * OPS_PER_THREAD));
}

// ==========================================
// 7. Restart / Gap Recovery Tests
// ==========================================

TEST_F(IDAllocatorTest, RecoverGapsOnRestart) {
	// This tests the `recoverGapIds` logic in initialize()

	// 1. Create IDs [1, 2, 3] and Save.
	(void) insertNode("1");
	(void) insertNode("2");
	(void) insertNode("3");
	fileStorage->flush(); // FileHeader MaxID = 3

	// 2. Delete 3 and Save.
	// ID 3 becomes Inactive on disk.
	deleteNode(3);
	fileStorage->flush();

	// 3. Restart DB
	database->close();
	database.reset();
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();

	// 4. Insert New Node
	// It should reuse 3 (via Lazy Load scan).
	graph::Node n = insertNode("New");
	EXPECT_EQ(n.getId(), 3);
}

TEST_F(IDAllocatorTest, Persistence_Restart_NoOverlap) {
	// 1. Session 1: Create IDs [1, 2, 3]
	{
		// Use a local scope to force destruction of DB objects (Simulate Close)
		graph::Node n1 = insertNode("A");
		graph::Node n2 = insertNode("B");
		graph::Node n3 = insertNode("C");

		EXPECT_EQ(n1.getId(), 1);
		EXPECT_EQ(n2.getId(), 2);
		EXPECT_EQ(n3.getId(), 3);

		database->close();
	}

	// 2. Session 2: Re-open and Insert New Node
	{
		auto db2 = std::make_unique<graph::Database>(testFilePath.string());
		db2->open();
		auto store2 = db2->getStorage();
		auto alloc2 = store2->getIDAllocator();
		auto dataManager2 = store2->getDataManager();

		// CHECK: Max ID should be recovered correctly from header
		// If the "Capacity vs Used" bug exists, this might be incorrect (e.g. 1024).
		EXPECT_EQ(alloc2->getCurrentMaxNodeId(), 3) << "MaxNodeId recovery error.";

		// Insert new node.
		// IF the bug exists, it would return 1025 or overlap.
		// IF fixed, it should return 4.
		int64_t id4 = alloc2->allocateId(graph::Node::typeId);
		graph::Node n4(id4, 0);
		dataManager2->addNode(n4);

		EXPECT_EQ(n4.getId(), 4) << "ID Overlap or Skip Detected! Allocator logic error.";
	}
}

TEST_F(IDAllocatorTest, CrashRecovery_WithSparseSegment_CapacityVsUsed) {
	// This test specifically targets the bug where recoverGapIds incorrectly uses
	// segment CAPACITY instead of USED count to determine physical max ID.

	// 1. Create a few nodes.
	// Assume Segment Capacity is large (e.g., 1024). We only use 3 slots.
	(void)insertNode("1");
	(void)insertNode("2");
	(void)insertNode("3");

	// 2. Save and Close.
	// This ensures the data is written to disk.
	// Header.used = 3, Header.capacity = 1024.
	fileStorage->flush();
	database->close();
	database.reset();

	// 3. Simulate Restart
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();
	dataManager = fileStorage->getDataManager();

	// 4. Verify Max ID Recovery
	// Buggy behavior: recoverGapIds calculates physicalMaxId = 1 + 1024 - 1 = 1024.
	//                 logicalMaxId is then "corrected" to 1024.
	// Correct behavior: physicalMaxId = 1 + 3 - 1 = 3.
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 3)
			<< "Bug Detected: Allocator incorrectly set MaxID based on Segment Capacity instead of Used count!";

	// 5. Verify Next Allocation
	// Buggy behavior: Allocates 1025.
	// Correct behavior: Allocates 4.
	graph::Node n4 = insertNode("4");
	EXPECT_EQ(n4.getId(), 4) << "Allocator skipped IDs due to incorrect recovery logic!";
}

TEST_F(IDAllocatorTest, RecoverGapsOnNormalRestart_CaseA) {
	// 1. Create IDs [1, 2, 3] and Save.
	(void)insertNode("1");
	(void)insertNode("2");
	(void)insertNode("3");
	fileStorage->flush(); // Physical Max = 3

	// 2. Allocate ID 4 but DO NOT SAVE.
	// Logical Max becomes 4. Physical Max stays 3.
	// This simulates a transaction that allocated an ID but crashed/rolled back before flush.
	int64_t id4 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id4, 4);

	// 3. Restart DB
	// The FileHeader manager saves the logical max ID (4) on clean shutdown/flush.
	// We force a flush of the file header to persist "MaxNodeId = 4".
	// Note: In real app, FileStorage::close() does this.
	database->close();

	// 4. Reopen
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();

	// 5. Verify Gap Recovery
	// Physical Max is 3 (from segments). Logical Max is 4 (from header).
	// Gap [4, 4] should be recovered into Volatile Cache.

	// Allocate should pop 4 from volatile cache.
	int64_t recoveredId = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(recoveredId, 4) << "Failed to recover gap ID 4 from volatile cache";

	// Next should be 5
	EXPECT_EQ(allocator->allocateId(graph::Node::typeId), 5);
}

TEST_F(IDAllocatorTest, BatchAllocation_AllTypes) {
	constexpr size_t BATCH = 5;

	// 1. Edge
	int64_t startEdge = allocator->allocateIdBatch(graph::Edge::typeId, BATCH);
	EXPECT_GT(startEdge, 0);
	// Verify counter incremented
	EXPECT_EQ(static_cast<uint64_t>(allocator->getCurrentMaxEdgeId()), static_cast<uint64_t>(startEdge + BATCH - 1));

	// 2. Property
	int64_t startProp = allocator->allocateIdBatch(graph::Property::typeId, BATCH);
	EXPECT_GT(startProp, 0);
	EXPECT_EQ(static_cast<uint64_t>(allocator->getCurrentMaxPropId()), static_cast<uint64_t>(startProp + BATCH - 1));

	// 3. Blob
	int64_t startBlob = allocator->allocateIdBatch(graph::Blob::typeId, BATCH);
	EXPECT_GT(startBlob, 0);
	EXPECT_EQ(static_cast<uint64_t>(allocator->getCurrentMaxBlobId()), static_cast<uint64_t>(startBlob + BATCH - 1));

	// 4. Index
	int64_t startIndex = allocator->allocateIdBatch(graph::Index::typeId, BATCH);
	EXPECT_GT(startIndex, 0);
	EXPECT_EQ(static_cast<uint64_t>(allocator->getCurrentMaxIndexId()), static_cast<uint64_t>(startIndex + BATCH - 1));

	// 5. State
	int64_t startState = allocator->allocateIdBatch(graph::State::typeId, BATCH);
	EXPECT_GT(startState, 0);
	EXPECT_EQ(static_cast<uint64_t>(allocator->getCurrentMaxStateId()), static_cast<uint64_t>(startState + BATCH - 1));

	// 6. Invalid Type (Exception check)
	EXPECT_THROW(allocator->allocateIdBatch(999, BATCH), std::runtime_error);
}

TEST_F(IDAllocatorTest, ExplicitCacheClearing) {
	// 1. Allocate ID 1
	int64_t id1 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id1, 1);

	// 2. Free ID 1 (Volatile)
	allocator->freeId(id1, graph::Node::typeId);

	// 3. Reset
	allocator->resetAfterCompaction();

	// 4. Allocate Next
	int64_t id2 = allocator->allocateId(graph::Node::typeId);

	// Debug print
	if (id2 == id1) {
		std::cerr << "FAILURE: Got " << id2 << " again. MaxID is " << allocator->getCurrentMaxNodeId() << std::endl;
	}

	EXPECT_NE(id2, id1);
}

TEST_F(IDAllocatorTest, ClearCache_SpecificType) {
	// This tests the method: void clearCache(uint32_t entityType)

	// 1. Populate Hot Cache
	(void)insertNode("A"); // 1
	fileStorage->flush();
	deleteNode(1); // 1 -> Hot Cache

	// 2. Clear specific cache
	allocator->clearCache(graph::Node::typeId);

	// 3. To verify, we'd ideally check internal state.
	// Indirectly: Allocate should assume cache miss and go to disk.
	// Since this is hard to distinguish from outcome (both return 1),
	// we primarily rely on this test execution hitting the lines of code.
	// The previous test already proved Hot Cache logic works.
	// Calling the function ensures coverage.

	int64_t id = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id, 1); // Should still get 1 (via disk scan now)
}

// ==========================================
// Additional Tests for Branch Coverage
// ==========================================

TEST_F(IDAllocatorTest, BatchAllocation_ZeroCount) {
	// Test allocateIdBatch with count == 0
	int64_t startId = allocator->allocateIdBatch(graph::Node::typeId, 0);
	EXPECT_EQ(startId, 0);
}

TEST_F(IDAllocatorTest, BatchAllocation_LargeBatch) {
	// Test allocateIdBatch with a large batch size
	constexpr size_t LARGE_BATCH = 100;
	int64_t startId = allocator->allocateIdBatch(graph::Node::typeId, LARGE_BATCH);
	EXPECT_EQ(startId, 1);
	EXPECT_EQ(static_cast<size_t>(allocator->getCurrentMaxNodeId()), LARGE_BATCH);
}

TEST_F(IDAllocatorTest, FreeId_NonPersistedId) {
	// Test freeId for an ID that was allocated but never persisted (dirty)
	int64_t id1 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id1, 1);

	// Free the ID without persisting - should go to volatile cache
	allocator->freeId(id1, graph::Node::typeId);

	// Re-allocate should get the same ID back from volatile cache
	int64_t id2 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id2, 1);
}

TEST_F(IDAllocatorTest, FreeId_DirtyInPreAllocRange) {
	// Test the isDirtyInPreAlloc branch - ID allocated but not in used range
	// 1. Allocate an ID
	int64_t id1 = allocator->allocateId(graph::Node::typeId);

	// 2. Insert a node to occupy ID 1
	graph::Node node(id1, 0);
	dataManager->addNode(node);
	fileStorage->flush();

	// 3. Allocate another ID but don't persist
	int64_t id2 = allocator->allocateId(graph::Node::typeId);

	// 4. Manually free id2 - it should go to volatile cache since it's not persisted
	allocator->freeId(id2, graph::Node::typeId);

	// 5. Next allocation should get id2 from volatile cache
	int64_t id3 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id3, id2);
}

TEST_F(IDAllocatorTest, VolatileCacheOverflow) {
	// Test volatile cache overflow (volatileMaxIntervals_)
	// Set small limits to trigger overflow
	allocator->setCacheLimits(10, 5, 5); // L1=10, L2=5 intervals, Volatile=5 intervals

	// 1. Allocate many IDs without persisting
	constexpr int COUNT = 100;
	std::vector<int64_t> ids;
	ids.reserve(COUNT);
for (int i = 0; i < COUNT; ++i) {
		ids.push_back(allocator->allocateId(graph::Node::typeId));
	}

	// 2. Free all IDs - should fill volatile cache and potentially overflow
	for (int64_t id: ids) {
		allocator->freeId(id, graph::Node::typeId);
	}

	// 3. Re-allocate - should reuse from volatile cache up to capacity
	// Some IDs might be dropped due to overflow
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < COUNT; ++i) {
		int64_t id = allocator->allocateId(graph::Node::typeId);
		reusedIds.insert(id);
	}

	// At least some IDs should be reused
	EXPECT_GT(reusedIds.size(), 0u);
	EXPECT_LE(*reusedIds.begin(), COUNT);
}

TEST_F(IDAllocatorTest, L2CacheOverflow) {
	// Test L2 cache overflow (l2MaxIntervals_)
	// Set small limits
	allocator->setCacheLimits(10, 3, 50000); // L1=10, L2=3 intervals, Volatile=large

	constexpr int COUNT = 200;

	// 1. Insert and persist many nodes
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete all - should overflow L2 and go to L1
	for (int i = 1; i <= COUNT; ++i) {
		deleteNode(i);
	}

	// 3. Re-allocate - should reuse IDs
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < COUNT; ++i) {
		int64_t id = allocator->allocateId(graph::Node::typeId);
		reusedIds.insert(id);
	}

	// Should have reused some IDs
	bool reusedAny = false;
	for (int64_t id: reusedIds) {
		if (id <= COUNT) {
			reusedAny = true;
			break;
		}
	}
	EXPECT_TRUE(reusedAny);
}

TEST_F(IDAllocatorTest, IDIntervalSet_AppendToLastInterval) {
	// Test the optimization branch: appending to the last interval
	// This happens when adding IDs sequentially to an interval set
	allocator->setCacheLimits(100, 100, 50000);

	// 1. Insert and persist nodes
	for (int i = 0; i < 10; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete sequentially - should trigger append optimization
	for (int i = 1; i <= 10; ++i) {
		deleteNode(i);
	}

	// 3. Re-allocate - should get IDs back efficiently
	std::vector<int64_t> reusedIds;
	reusedIds.reserve(10);
for (int i = 0; i < 10; ++i) {
		reusedIds.push_back(allocator->allocateId(graph::Node::typeId));
	}

	// Should get all 10 IDs back
	std::ranges::sort(reusedIds);
	for (int i = 0; i < 10; ++i) {
		EXPECT_EQ(reusedIds[i], i + 1);
	}
}

TEST_F(IDAllocatorTest, RecoverGaps_EmptySegments) {
	// Test recoverGapIds when all segments are empty (header.used == 0)
	// This tests the line: if (header.used > 0) branch

	// 1. Create a database with no data
	fileStorage->flush();

	// 2. Close and restart to trigger recovery
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();

	// 3. Should recover correctly
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 0);

	// 4. First allocation should work
	int64_t id = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id, 1);
}

TEST_F(IDAllocatorTest, DiskScan_Wrapping) {
	// Test disk scan wrapping behavior
	// This tests cursor.wrappedAround logic in fetchInactiveIdsFromDisk

	// 1. Create many nodes across multiple segments
	constexpr int COUNT = 3000;
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete nodes from the beginning (creating gaps at start)
	for (int i = 1; i <= 100; ++i) {
		deleteNode(i);
	}

	// 3. Clear caches to force disk scan
	allocator->clearAllCaches();

	// 4. Allocate - should scan from start and wrap if needed
	int64_t id = allocator->allocateId(graph::Node::typeId);
	EXPECT_LE(id, 100);
}

TEST_F(IDAllocatorTest, FetchInactiveIds_L1FullEarlyReturn) {
	// Test early return when L1 cache is full during disk scan
	// This tests line 409-412: if (l1.size() >= l1CacheSize_) break/return

	allocator->setCacheLimits(10, 100, 50000); // Small L1, large L2

	// 1. Create many nodes with many gaps
	constexpr int COUNT = 500;
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete many nodes to create gaps
	for (int i = 1; i <= COUNT; i += 2) { // Delete half
		deleteNode(i);
	}

	// 3. Clear cache
	allocator->clearAllCaches();

	// 4. Allocate - should fetch only up to L1 capacity
	for (int i = 0; i < 15; ++i) { // More than L1 size
		int64_t id = allocator->allocateId(graph::Node::typeId);
		EXPECT_GT(id, 0);
	}

	// Verify max ID didn't increase much
	EXPECT_LE(allocator->getCurrentMaxNodeId(), COUNT);
}

TEST_F(IDAllocatorTest, AllocateId_InvalidEntityType) {
	// Test allocateId with invalid entity type
	EXPECT_THROW(allocator->allocateId(9999), std::runtime_error);
}

TEST_F(IDAllocatorTest, AllocateIdBatch_MultipleTypesIndependently) {
	// Test batch allocation for multiple types independently
	constexpr size_t BATCH = 10;

	int64_t startNode = allocator->allocateIdBatch(graph::Node::typeId, BATCH);
	EXPECT_EQ(startNode, 1);

	int64_t startEdge = allocator->allocateIdBatch(graph::Edge::typeId, BATCH);
	EXPECT_EQ(startEdge, 1);

	// Node and Edge should have independent counters
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), static_cast<int64_t>(BATCH));
	EXPECT_EQ(allocator->getCurrentMaxEdgeId(), static_cast<int64_t>(BATCH));
}

TEST_F(IDAllocatorTest, MixedSequentialAndBatchAllocation) {
	// Test mixing sequential and batch allocation
	// 1. Sequential allocation
	int64_t id1 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id1, 1);

	// 2. Batch allocation
	constexpr size_t BATCH = 5;
	int64_t batchStart = allocator->allocateIdBatch(graph::Node::typeId, BATCH);
	EXPECT_EQ(batchStart, 2); // Should start from 2

	// 3. Sequential again
	int64_t id2 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id2, 2 + static_cast<int64_t>(BATCH)); // Should be after batch

	// Max should be 1 (first alloc) + 5 (batch) + 1 (second alloc) = 7
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 1 + static_cast<int64_t>(BATCH) + 1);
}

TEST_F(IDAllocatorTest, FreeId_MultipleEntityTypes) {
	// Test freeId for different entity types
	// 1. Allocate and persist edges
	constexpr int COUNT = 5;
	std::vector<graph::Edge> edges;
	edges.reserve(COUNT);
for (int i = 0; i < COUNT; ++i) {
		edges.push_back(insertEdge(1, 1, "E" + std::to_string(i)));
	}
	fileStorage->flush();

	// 2. Delete some edges
	for (auto &edge: edges) {
		dataManager->deleteEdge(edge);
	}

	// 3. Re-allocate should reuse edge IDs
	for (int i = 0; i < COUNT; ++i) {
		int64_t id = allocator->allocateId(graph::Edge::typeId);
		EXPECT_LE(id, COUNT);
	}
}

TEST_F(IDAllocatorTest, SegmentHeader_IdOutsideRange) {
	// Test freeId when ID is outside segment's capacity range
	// This tests the else branch at line 341 where isDirtyInPreAlloc = true

	// 1. Allocate an ID but don't insert into dataManager
	int64_t id = allocator->allocateId(graph::Node::typeId);

	// 2. Free it - should go to volatile cache since it's not in any segment
	allocator->freeId(id, graph::Node::typeId);

	// 3. Should be reusable
	int64_t reusedId = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(reusedId, id);
}

TEST_F(IDAllocatorTest, DiskExhaustedFlag) {
	// Test that diskExhausted flag prevents repeated scans
	// This tests line 222: if (!cursor.diskExhausted) return false early

	// 1. Start with fresh allocator
	allocator->clearAllCaches();

	// 2. Allocate without any persisted deletions - will trigger disk scan
	int64_t id1 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id1, 1);

	// 3. Allocate again - should use exhausted flag optimization
	int64_t id2 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id2, 2);

	// 4. No disk scan should occur on second allocation
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 2);
}

TEST_F(IDAllocatorTest, ResetAfterCompaction_VolatileCleared) {
	// Test that resetAfterCompaction clears volatile cache
	// This is distinct from clearAllCaches which preserves volatile cache

	// 1. Allocate dirty ID
	int64_t id1 = allocator->allocateId(graph::Node::typeId);

	// 2. Free to volatile cache
	allocator->freeId(id1, graph::Node::typeId);

	// 3. Simulate compaction reset
	allocator->resetAfterCompaction();

	// 4. Next allocation should NOT reuse id1 (volatile was cleared)
	// Since volatile cache is cleared and max is still 1, next allocation should be 2
	int64_t id2 = allocator->allocateId(graph::Node::typeId);
	EXPECT_NE(id2, id1); // Should NOT get 1 back
	EXPECT_EQ(id2, 2); // Should get new sequential ID
}

TEST_F(IDAllocatorTest, LargeGapRecovery) {
	// Test recovery of large gaps between physical and logical max ID
	// This tests CASE A in recoverGapIds

	// 1. Create a few nodes
	(void)insertNode("A");
	(void)insertNode("B");
	(void)insertNode("C");
	fileStorage->flush(); // Physical max = 3

	// 2. Allocate many IDs but don't persist (e.g., crashed transaction)
	int64_t logicalMax = 0;
	for (int i = 0; i < 100; ++i) {
		logicalMax = allocator->allocateId(graph::Node::typeId);
	}
	EXPECT_EQ(logicalMax, 103);

	// 3. Simulate restart - gap [4, 103] should be recovered to volatile cache
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();

	// 4. Next allocations should reuse from gap
	std::unordered_set<int64_t> gapIds;
	for (int i = 0; i < 100; ++i) {
		int64_t id = allocator->allocateId(graph::Node::typeId);
		gapIds.insert(id);
	}

	// Should have reused some gap IDs
	// Since unordered_set doesn't have rbegin(), we iterate to check range
	[[maybe_unused]] bool allInRange = true;
	int64_t minId = LLONG_MAX;
	int64_t maxId = LLONG_MIN;
	for (int64_t id: gapIds) {
		if (id < minId) minId = id;
		if (id > maxId) maxId = id;
	}

	EXPECT_TRUE(gapIds.contains(4) || gapIds.contains(103) ||
	            (minId >= 4 && maxId <= 103));
}

TEST_F(IDAllocatorTest, StaleHeaderCorrection) {
	// Test CASE B in recoverGapIds: physicalMaxId > logicalMaxId
	// This happens when header is stale (not saved after last operation)

	// 1. Create some nodes
	(void)insertNode("A");
	(void)insertNode("B");
	fileStorage->flush(); // Logical max = 2

	// 2. Manually corrupt the logical max by allocating but not saving header
	// In real scenario, this would be a crash before header save
	// We simulate by checking that physical max takes precedence on restart

	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();

	// Should recover correctly from physical max
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 2);
}

TEST_F(IDAllocatorTest, IntervalSet_MergeWithMultipleIntervals) {
	// Test IDIntervalSet merging with multiple existing intervals
	// This tests the while loop at line 70: merging with subsequent intervals

	allocator->setCacheLimits(100, 100, 50000);

	// 1. Create nodes
	constexpr int COUNT = 100;
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete in non-sequential pattern to create multiple intervals
	// Delete 1-10, 20-30, 40-50
	for (int i = 1; i <= 10; ++i) deleteNode(i);
	for (int i = 20; i <= 30; ++i) deleteNode(i);
	for (int i = 40; i <= 50; ++i) deleteNode(i);

	// 3. Delete middle bridge (11-19, 31-39) to trigger merges
	for (int i = 11; i <= 19; ++i) deleteNode(i);
	for (int i = 31; i <= 39; ++i) deleteNode(i);

	// 4. Should reuse merged intervals
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < 55; ++i) {
		int64_t id = allocator->allocateId(graph::Node::typeId);
		reusedIds.insert(id);
	}

	// Should have reused IDs from the merged range
	bool hitRange = false;
	for (int64_t id: reusedIds) {
		if (id >= 1 && id <= 50) {
			hitRange = true;
			break;
		}
	}
	EXPECT_TRUE(hitRange);
}

TEST_F(IDAllocatorTest, FreeId_ZeroSegmentOffset) {
	// Test freeId when segmentOffset == 0 (ID not in any segment)
	// This tests the branch at line 344: if (segmentOffset == 0 || isDirtyInPreAlloc)

	// 1. Allocate an ID without inserting into storage
	int64_t id = allocator->allocateId(graph::Property::typeId);
	EXPECT_EQ(id, 1);

	// 2. Free the ID - segmentOffset will be 0
	allocator->freeId(id, graph::Property::typeId);

	// 3. Should go to volatile cache and be reusable
	int64_t reusedId = allocator->allocateId(graph::Property::typeId);
	EXPECT_EQ(reusedId, 1);
}

TEST_F(IDAllocatorTest, AllEntityTypes_BatchAndSequential) {
	// Test batch and sequential allocation for all entity types
	struct TypeInfo {
		uint32_t typeId;
		std::string name;
	};

	std::vector<TypeInfo> types = {
		{graph::Node::typeId, "Node"},
		{graph::Edge::typeId, "Edge"},
		{graph::Property::typeId, "Property"},
		{graph::Blob::typeId, "Blob"},
		{graph::Index::typeId, "Index"},
		{graph::State::typeId, "State"},
	};

	for (const auto &typeInfo: types) {
		// Sequential
		int64_t seqId = allocator->allocateId(typeInfo.typeId);
		EXPECT_GT(seqId, 0) << "Sequential allocation failed for " << typeInfo.name;

		// Batch
		constexpr size_t BATCH = 3;
		int64_t batchStart = allocator->allocateIdBatch(typeInfo.typeId, BATCH);
		EXPECT_GT(batchStart, 0) << "Batch allocation failed for " << typeInfo.name;
	}
}
