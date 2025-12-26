/**
 * @file test_IDAllocator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/2
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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

	graph::Node insertNode(const std::string &label) {
		int64_t id = allocator->allocateId(graph::Node::typeId);
		graph::Node node(id, label);
		dataManager->addNode(node);
		return node;
	}

	graph::Edge insertEdge(int64_t startId, int64_t endId, const std::string &label) {
		int64_t id = allocator->allocateId(graph::Edge::typeId);
		graph::Edge edge(id, startId, endId, label);
		dataManager->addEdge(edge);
		return edge;
	}

	void deleteNode(int64_t id) {
		graph::Node node_to_delete(id, "");
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
			std::uniform_int_distribution<> dis(0, activeIds.size() - 1);
			int idx = dis(gen);
			int64_t idToDelete = activeIds[idx];

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
		graph::Node n4(id4, "D");
		dataManager2->addNode(n4);

		EXPECT_EQ(n4.getId(), 4) << "ID Overlap or Skip Detected! Allocator logic error.";
	}
}

TEST_F(IDAllocatorTest, CrashRecovery_WithSparseSegment_CapacityVsUsed) {
	// This test specifically targets the bug where recoverGapIds incorrectly uses
	// segment CAPACITY instead of USED count to determine physical max ID.

	// 1. Create a few nodes.
	// Assume Segment Capacity is large (e.g., 1024). We only use 3 slots.
	insertNode("1");
	insertNode("2");
	insertNode("3");

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
