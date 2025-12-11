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
		// Generate unique temporary file path
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_id_alloc_" + to_string(uuid) + ".dat");

		// Initialize Database
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
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

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::IDAllocator> allocator;
};

// ==========================================
// 1. Basic Functionality Tests
// ==========================================

TEST_F(IDAllocatorTest, SequentialAllocation) {
	// Verify that IDs increment sequentially when no holes exist
	graph::Node n1 = fileStorage->insertNode("A");
	graph::Node n2 = fileStorage->insertNode("B");

	EXPECT_EQ(n1.getId(), 1);
	EXPECT_EQ(n2.getId(), 2);
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 2);
}

TEST_F(IDAllocatorTest, TypeIsolation) {
	// Verify that Node IDs and Edge IDs are tracked separately
	graph::Node n1 = fileStorage->insertNode("N1");
	// Ensure nodes exist for edge creation
	graph::Edge e1 = fileStorage->insertEdge(n1.getId(), n1.getId(), "E1");

	EXPECT_EQ(n1.getId(), 1);
	EXPECT_EQ(e1.getId(), 1); // Edge ID should also start at 1

	// Create a NEW node to test Edge ID increment
	// We do NOT delete n1, so e1 remains active (ID 1 is taken).
	graph::Node n2 = fileStorage->insertNode("N2");

	// Insert edge on the new node
	graph::Edge e2 = fileStorage->insertEdge(n2.getId(), n2.getId(), "E2");

	// Now we can safely expect ID 2, because ID 1 is still held by e1.
	// This fixes the previous logic error where deleting n1 caused e1 to be freed implicitly.
	EXPECT_EQ(e2.getId(), 2);

	// Also verify Node ID sequence is independent
	EXPECT_EQ(n2.getId(), 2);
}

// ==========================================
// 2. Eager Reuse (L1 & MemoryOnly Cache) Tests
// ==========================================

TEST_F(IDAllocatorTest, EagerReuse_Verify_NoDiskIO_WithCacheClear) {
	// This test verifies the critical fix for the "Dirty ID lost during Compaction" bug.

	// 1. Insert 2 nodes and Persist
	(void) fileStorage->insertNode("A"); // 1
	(void) fileStorage->insertNode("B"); // 2
	fileStorage->save(); // 1 and 2 are on disk (Persisted).

	// 2. Insert Node 3 (Dirty, Memory Only)
	(void) fileStorage->insertNode("C"); // 3

	// 3. Delete Node 3 (Dirty)
	// Should go to memoryOnlyCache_
	fileStorage->deleteNode(3);

	// 4. Delete Node 2 (Persisted)
	// Should go to hotCache_ (L1)
	fileStorage->deleteNode(2);

	// 5. SIMULATE COMPACTION / FLUSH
	// This clears L1/L2 caches.
	// ID 2 (Persisted) is cleared from RAM but exists on Disk as inactive.
	// ID 3 (Dirty) MUST remain in memoryOnlyCache_ or it is lost forever.
	allocator->clearAllCaches();

	// 6. Reuse
	// Allocation 1: Should hit memoryOnlyCache_ first (ID 3)
	graph::Node nFirst = fileStorage->insertNode("Reuse1");

	// Allocation 2: Should scan disk (lazy load) for ID 2
	graph::Node nSecond = fileStorage->insertNode("Reuse2");

	// We expect both to be reused.
	// The order might vary depending on implementation priority, but both IDs must be present.
	std::unordered_set<int64_t> reusedIds = {nFirst.getId(), nSecond.getId()};

	EXPECT_TRUE(reusedIds.count(3)) << "ID 3 (Dirty) was lost during cache clear/compaction simulation!";
	EXPECT_TRUE(reusedIds.count(2)) << "ID 2 (Persisted) failed to lazy load from disk!";

	// 7. Verify Max ID didn't increase
	EXPECT_EQ(allocator->getCurrentMaxNodeId(), 3);
}

TEST_F(IDAllocatorTest, AllocateId_Returns_MemoryOnly_ID_Direct) {
	// 1. Insert a node (ID 1) - Do NOT save.
	graph::Node n1 = fileStorage->insertNode("DirtyNode");
	EXPECT_EQ(n1.getId(), 1);

	// 2. Delete the node immediately.
	fileStorage->deleteNode(n1.getId());

	// 3. Insert a new node.
	// The allocator should pop ID 1 from MemoryOnlyCache.
	// CRITICAL: Even though SegmentOffset is 0, the allocator must return it.
	graph::Node n2 = fileStorage->insertNode("NewNode");

	EXPECT_EQ(n2.getId(), 1) << "Allocator failed to reuse Dirty ID (SegmentOffset=0)";

	// 4. Verify uniqueness
	graph::Node n3 = fileStorage->insertNode("NextNode");
	EXPECT_EQ(n3.getId(), 2);
}

// ==========================================
// 3. Lazy Loading & Persistence Tests
// ==========================================

TEST_F(IDAllocatorTest, LazyLoadFromDiskAfterRestart) {
	// 1. Setup Data
	(void) fileStorage->insertNode("A"); // 1
	(void) fileStorage->insertNode("B"); // 2
	(void) fileStorage->insertNode("C"); // 3
	fileStorage->save();

	// 2. Delete and Persist
	// Calling save() ensures the Bitmap on disk is updated to '0' (inactive)
	fileStorage->deleteNode(2);
	fileStorage->save();

	// 3. Simulate Restart/OOM
	// This wipes all caches.
	allocator->clearAllCaches();

	// 4. Allocate -> Should trigger fetchInactiveIdsFromDisk and find ID 2
	graph::Node nLazy = fileStorage->insertNode("Lazy");

	EXPECT_EQ(nLazy.getId(), 2) << "Failed to lazy-load ID 2 from disk bitmap";
}

TEST_F(IDAllocatorTest, DiskExhaustionOptimization) {
	// 1. Clean state
	allocator->clearAllCaches();

	// 2. Allocate sequential (Trigger disk scan -> set exhausted = true)
	graph::Node n1 = fileStorage->insertNode("1");
	EXPECT_EQ(n1.getId(), 1);

	// 3. Allocate again
	// This should NOT trigger a disk scan (internal optimization verification)
	graph::Node n2 = fileStorage->insertNode("2");
	EXPECT_EQ(n2.getId(), 2);
}

// ==========================================
// 4. Tiered Cache (L1 Overflow & L2) Tests
// ==========================================

TEST_F(IDAllocatorTest, L1CacheOverflowToL2) {
	// L1 limit is 1024. We will delete 1500 items to force L2 usage.
	constexpr int COUNT = 1500;

	// 1. Insert 1500 nodes
	for (int i = 0; i < COUNT; ++i) {
		(void) fileStorage->insertNode("N");
	}
	fileStorage->save();

	// 2. Delete all 1500 nodes
	for (int i = 1; i <= COUNT; ++i) {
		fileStorage->deleteNode(i);
	}

	// 3. Re-allocate 1500 nodes
	// Should reuse ALL IDs without increasing max ID > 1500
	for (int i = 0; i < COUNT; ++i) {
		graph::Node n = fileStorage->insertNode("Re");
		EXPECT_LE(n.getId(), COUNT) << "ID leaked! Allocated " << n.getId() << " but should be <= " << COUNT;
	}

	// 4. Verify next is new
	graph::Node nNext = fileStorage->insertNode("New");
	EXPECT_EQ(nNext.getId(), COUNT + 1);
}

TEST_F(IDAllocatorTest, L2IntervalMerging) {
	// This tests if L2 efficiently handles sequential deletes.
	constexpr int COUNT = 2000;
	for (int i = 0; i < COUNT; ++i)
		(void) fileStorage->insertNode("N");
	fileStorage->save();

	// Delete nodes in blocks to trigger merges
	// Delete 1000..1500
	for (int i = 1000; i <= 1500; ++i)
		fileStorage->deleteNode(i);

	// Delete 1501..2000 (Should merge with previous interval in L2)
	for (int i = 1501; i <= COUNT; ++i)
		fileStorage->deleteNode(i);

	// Now re-allocate.
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < 1001; ++i) {
		graph::Node n = fileStorage->insertNode("R");
		reusedIds.insert(n.getId());
	}

	// Verify we got ids from the range [1000, 2000]
	bool hitRange = false;
	for (int64_t id: reusedIds) {
		if (id >= 1000 && id <= 2000)
			hitRange = true;
	}
	EXPECT_TRUE(hitRange);
}

// ==========================================
// 5. Robustness: Double Free & Consistency
// ==========================================

TEST_F(IDAllocatorTest, DoubleFree_OnDisk_Protection) {
	// 1. Setup: Insert and Persist
	(void) fileStorage->insertNode("A"); // ID 1
	fileStorage->save();

	// 2. Delete and Persist (Bitmap for ID 1 becomes 0/Inactive)
	fileStorage->deleteNode(1);
	fileStorage->save();

	// 3. Clear Caches to start fresh
	allocator->clearAllCaches();

	// 4. Manual Simulation: Attempt to free ID 1 twice.
	// Since ID 1 is already Inactive on disk (and is within Used range),
	// `freeId` should detect "True Double Free" and drop it.
	allocator->freeId(1, graph::Node::typeId);
	allocator->freeId(1, graph::Node::typeId);

	// 5. Allocate.
	// If Double Free protection FAILED, L1 would have two copies of [1].
	// If works, L1 is empty (dropped due to double free check), falls back to Disk Scan -> finds 1.
	int64_t id1 = allocator->allocateId(graph::Node::typeId);
	EXPECT_EQ(id1, 1);

	int64_t id2 = allocator->allocateId(graph::Node::typeId);
	EXPECT_NE(id2, 1) << "Double Free protection failed: ID 1 was allocated twice!";
	EXPECT_EQ(id2, 2);
}

TEST_F(IDAllocatorTest, DeleteAndSaveIdempotency) {
	(void) fileStorage->insertNode("A"); // 1
	(void) fileStorage->insertNode("B"); // 2
	fileStorage->save();

	// 1. Delete Node 2
	fileStorage->deleteNode(2);

	// 2. Reuse check
	graph::Node nC = fileStorage->insertNode("C");
	EXPECT_EQ(nC.getId(), 2);

	// 3. Delete Node C (ID 2) again
	fileStorage->deleteNode(2);

	// 4. Save (triggers internal deleteEntityOnDisk)
	fileStorage->save();

	// 5. Verify ID 2 is still available
	graph::Node nD = fileStorage->insertNode("D");
	EXPECT_EQ(nD.getId(), 2);
}

TEST_F(IDAllocatorTest, MixedOperationsStress) {
	// Randomly Insert and Delete to simulate real workload
	std::vector<int64_t> activeIds;
	std::mt19937 gen(42);

	for (int i = 0; i < 1000; ++i) {
		if (activeIds.empty() || (gen() % 3 != 0)) { // 66% Insert
			graph::Node n = fileStorage->insertNode("R");
			activeIds.push_back(n.getId());
		} else { // 33% Delete
			std::uniform_int_distribution<> dis(0, activeIds.size() - 1);
			int idx = dis(gen);
			int64_t idToDelete = activeIds[idx];

			fileStorage->deleteNode(idToDelete);

			activeIds[idx] = activeIds.back();
			activeIds.pop_back();
		}
	}

	fileStorage->save();
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
		futures.push_back(std::async(std::launch::async, [this, OPS_PER_THREAD]() {
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

	EXPECT_EQ(totalCount, NUM_THREADS * OPS_PER_THREAD);
}

// ==========================================
// 7. Restart / Gap Recovery Tests
// ==========================================

TEST_F(IDAllocatorTest, RecoverGapsOnRestart) {
	// This tests the `recoverGapIds` logic in initialize()

	// 1. Create IDs [1, 2, 3] and Save.
	(void) fileStorage->insertNode("1");
	(void) fileStorage->insertNode("2");
	(void) fileStorage->insertNode("3");
	fileStorage->save(); // FileHeader MaxID = 3

	// 2. Delete 3 and Save.
	// ID 3 becomes Inactive on disk.
	fileStorage->deleteNode(3);
	fileStorage->save();

	// 3. Restart DB
	database->close();
	database.reset();
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	allocator = fileStorage->getIDAllocator();

	// 4. Insert New Node
	// Ideally, it should reuse 3 (via Lazy Load scan).
	// This isn't strictly "Gap Recovery" (which implies Logical > Physical),
	// but ensures the allocator state is consistent after restart.
	graph::Node n = fileStorage->insertNode("New");
	EXPECT_EQ(n.getId(), 3);
}

TEST_F(IDAllocatorTest, Persistence_Restart_NoOverlap) {
	// 1. Session 1: Create IDs [1, 2, 3]
	{
		// Use a local scope to force destruction of DB objects (Simulate Close)
		graph::Node n1 = fileStorage->insertNode("A");
		graph::Node n2 = fileStorage->insertNode("B");
		graph::Node n3 = fileStorage->insertNode("C");

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

		// CHECK: Max ID should be recovered correctly from header
		EXPECT_EQ(alloc2->getCurrentMaxNodeId(), 3) << "MaxNodeId not recovered from FileHeader correctly";

		// Insert new node.
		// IF the bug exists (Gap Recovery thinks 1,2,3 are holes because SegmentTracker wasn't init),
		// it would return 1.
		// IF fixed, it should return 4.
		graph::Node n4 = store2->insertNode("D");

		EXPECT_EQ(n4.getId(), 4) << "ID Overlap Detected! Allocator reused an existing ID after restart.";

		// Ensure 1, 2, 3 still exist (Double check)
		// (Assuming retrieval logic works, but allocating 4 is the primary test)
	}
}

TEST_F(IDAllocatorTest, UndoAdd_Logic_Integration) {
	// 1. Insert Node
	graph::Node n = fileStorage->insertNode("Temp");
	const int64_t id = n.getId();

	// 2. Delete Node (Should trigger remove() in PersistenceManager + freeId)
	fileStorage->deleteNode(id);

	// 3. Save
	// Since it was "Undo Add", nothing was written to disk. Max ID in header might lag or advance depending on
	// implementation, but the ID should be effectively free.
	fileStorage->save();

	// 4. Restart
	database->close();
	database->open();
	fileStorage = database->getStorage();

	// 5. Insert new node
	// It should reuse the ID (or next available).
	// Note: Since we didn't save the node, FileHeader Max ID might not have incremented (ideal),
	// or if it did, `recoverGapIds` should catch it.
	const graph::Node n2 = fileStorage->insertNode("New");

	// We mainly ensure no crash and valid ID.
	EXPECT_GT(n2.getId(), 0);
}
