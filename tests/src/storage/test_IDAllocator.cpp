/**
 * @file test_IDAllocator.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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

// ============================================================================
// RecoverGapIds, ResetAfterCompaction, DiskScan & Remaining Tests
// ============================================================================

TEST_F(IDAllocatorTest, SequentialAllocation) {
	// Verify that IDs increment sequentially when no holes exist.
	graph::Node n1 = insertNode("A");
	graph::Node n2 = insertNode("B");

	EXPECT_EQ(n1.getId(), 1);
	EXPECT_EQ(n2.getId(), 2);
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 2);
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
	nodeAllocator->clearPersistedCaches();

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
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 3);
}

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
	nodeAllocator->clearPersistedCaches();

	// 4. Allocate -> Should trigger fetchInactiveIdsFromDisk and find ID 2
	graph::Node nLazy = insertNode("Lazy");

	EXPECT_EQ(nLazy.getId(), 2) << "Failed to lazy-load ID 2 from disk bitmap";
}

TEST_F(IDAllocatorTest, DiskExhaustionOptimization) {
	// 1. Clean state
	nodeAllocator->clearPersistedCaches();

	// 2. Allocate sequential (Trigger disk scan -> finds nothing -> set exhausted = true)
	graph::Node n1 = insertNode("1");
	EXPECT_EQ(n1.getId(), 1);

	// 3. Allocate again
	// This should NOT trigger a disk scan (internal optimization verification)
	// It should directly allocate ID 2
	graph::Node n2 = insertNode("2");
	EXPECT_EQ(n2.getId(), 2);
}

TEST_F(IDAllocatorTest, L1CacheOverflowToL2) {
	// L1 = 100, L2 = 10
	nodeAllocator->setCacheLimits(100, 10, 50000);

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
	constexpr int COUNT = 200;
	for (int i = 0; i < COUNT; ++i)
		(void) insertNode("N");
	fileStorage->flush();

	// Delete nodes in blocks to trigger merges
	// Delete 100..150
	for (int i = 100; i <= 150; ++i)
		deleteNode(i);

	// Delete 151..200 (Should merge with previous interval in L2)
	for (int i = 151; i <= COUNT; ++i)
		deleteNode(i);

	// Now re-allocate.
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < 101; ++i) {
		graph::Node n = insertNode("R");
		reusedIds.insert(n.getId());
	}

	// Verify we got ids from the range [100, 200]
	bool hitRange = false;
	for (int64_t id: reusedIds) {
		if (id >= 100 && id <= 200)
			hitRange = true;
	}
	EXPECT_TRUE(hitRange) << "Failed to reuse IDs from merged intervals";
}

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
	nodeAllocator->free(1);

	// 4. Verification
	// The first allocation should get 1.
	int64_t id1 = nodeAllocator->allocate();
	EXPECT_EQ(id1, 1);

	// The second allocation should NOT get 1.
	int64_t id2 = nodeAllocator->allocate();
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

	for (int i = 0; i < 200; ++i) {
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
	EXPECT_LT(nodeAllocator->getCurrentMaxId(), 200);
}

TEST_F(IDAllocatorTest, ThreadSafetyConcurrency) {
	constexpr int NUM_THREADS = 4;
	constexpr int OPS_PER_THREAD = 50;

	std::vector<std::future<std::vector<int64_t>>> futures;
	futures.reserve(NUM_THREADS);

	for (int t = 0; t < NUM_THREADS; ++t) {
		futures.push_back(std::async(std::launch::async, [this]() {
			std::vector<int64_t> myIds;
			myIds.reserve(OPS_PER_THREAD);
			for (int i = 0; i < OPS_PER_THREAD; ++i) {
				myIds.push_back(nodeAllocator->allocate());
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
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);

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
		auto alloc2 = store2->getIDAllocator(graph::EntityType::Node);
		auto dataManager2 = store2->getDataManager();

		// CHECK: Max ID should be recovered correctly from header
		// If the "Capacity vs Used" bug exists, this might be incorrect (e.g. 1024).
		EXPECT_EQ(alloc2->getCurrentMaxId(), 3) << "MaxNodeId recovery error.";

		// Insert new node.
		// IF the bug exists, it would return 1025 or overlap.
		// IF fixed, it should return 4.
		int64_t id4 = alloc2->allocate();
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
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
	dataManager = fileStorage->getDataManager();

	// 4. Verify Max ID Recovery
	// Buggy behavior: recoverGapIds calculates physicalMaxId = 1 + 1024 - 1 = 1024.
	//                 logicalMaxId is then "corrected" to 1024.
	// Correct behavior: physicalMaxId = 1 + 3 - 1 = 3.
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 3)
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
	int64_t id4 = nodeAllocator->allocate();
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
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);

	// 5. Verify Gap Recovery
	// Physical Max is 3 (from segments). Logical Max is 4 (from header).
	// Gap [4, 4] should be recovered into Volatile Cache.

	// Allocate should pop 4 from volatile cache.
	int64_t recoveredId = nodeAllocator->allocate();
	EXPECT_EQ(recoveredId, 4) << "Failed to recover gap ID 4 from volatile cache";

	// Next should be 5
	EXPECT_EQ(nodeAllocator->allocate(), 5);
}

TEST_F(IDAllocatorTest, ExplicitCacheClearing) {
	// 1. Allocate ID 1
	int64_t id1 = nodeAllocator->allocate();
	EXPECT_EQ(id1, 1);

	// 2. Free ID 1 (Volatile)
	nodeAllocator->free(id1);

	// 3. Reset
	nodeAllocator->resetAfterCompaction();

	// 4. Allocate Next
	int64_t id2 = nodeAllocator->allocate();

	// Debug print
	if (id2 == id1) {
		std::cerr << "FAILURE: Got " << id2 << " again. MaxID is " << nodeAllocator->getCurrentMaxId() << std::endl;
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
	nodeAllocator->clearPersistedCaches();

	// 3. To verify, we'd ideally check internal state.
	// Indirectly: Allocate should assume cache miss and go to disk.
	// Since this is hard to distinguish from outcome (both return 1),
	// we primarily rely on this test execution hitting the lines of code.
	// The previous test already proved Hot Cache logic works.
	// Calling the function ensures coverage.

	int64_t id = nodeAllocator->allocate();
	EXPECT_EQ(id, 1); // Should still get 1 (via disk scan now)
}

TEST_F(IDAllocatorTest, VolatileCacheOverflow) {
	// Test volatile cache overflow (volatileMaxIntervals_)
	// Set small limits to trigger overflow
	nodeAllocator->setCacheLimits(10, 5, 5); // L1=10, L2=5 intervals, Volatile=5 intervals

	// 1. Allocate many IDs without persisting
	constexpr int COUNT = 100;
	std::vector<int64_t> ids;
	ids.reserve(COUNT);
for (int i = 0; i < COUNT; ++i) {
		ids.push_back(nodeAllocator->allocate());
	}

	// 2. Free all IDs - should fill volatile cache and potentially overflow
	for (int64_t id: ids) {
		nodeAllocator->free(id);
	}

	// 3. Re-allocate - should reuse from volatile cache up to capacity
	// Some IDs might be dropped due to overflow
	std::unordered_set<int64_t> reusedIds;
	for (int i = 0; i < COUNT; ++i) {
		int64_t id = nodeAllocator->allocate();
		reusedIds.insert(id);
	}

	// At least some IDs should be reused
	EXPECT_GT(reusedIds.size(), 0u);
	EXPECT_LE(*reusedIds.begin(), COUNT);
}

TEST_F(IDAllocatorTest, L2CacheOverflow) {
	// Test L2 cache overflow (l2MaxIntervals_)
	// Set small limits
	nodeAllocator->setCacheLimits(10, 3, 50000); // L1=10, L2=3 intervals, Volatile=large

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
		int64_t id = nodeAllocator->allocate();
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
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);

	// 3. Should recover correctly
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 0);

	// 4. First allocation should work
	int64_t id = nodeAllocator->allocate();
	EXPECT_EQ(id, 1);
}

TEST_F(IDAllocatorTest, DiskScan_Wrapping) {
	// Test disk scan wrapping behavior
	// This tests cursor.wrappedAround logic in fetchInactiveIdsFromDisk

	// 1. Create many nodes across multiple segments
	const int COUNT = static_cast<int>(graph::storage::NODES_PER_SEGMENT) + 50;
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete nodes from the beginning (creating gaps at start)
	for (int i = 1; i <= 50; ++i) {
		deleteNode(i);
	}

	// 3. Clear caches to force disk scan
	nodeAllocator->clearPersistedCaches();

	// 4. Allocate - should scan from start and wrap if needed
	int64_t id = nodeAllocator->allocate();
	EXPECT_LE(id, 50);
}

TEST_F(IDAllocatorTest, SegmentHeader_IdOutsideRange) {
	// Test freeId when ID is outside segment's capacity range
	// This tests the else branch at line 341 where isDirtyInPreAlloc = true

	// 1. Allocate an ID but don't insert into dataManager
	int64_t id = nodeAllocator->allocate();

	// 2. Free it - should go to volatile cache since it's not in any segment
	nodeAllocator->free(id);

	// 3. Should be reusable
	int64_t reusedId = nodeAllocator->allocate();
	EXPECT_EQ(reusedId, id);
}

TEST_F(IDAllocatorTest, DiskExhaustedFlag) {
	// Test that diskExhausted flag prevents repeated scans
	// This tests line 222: if (!cursor.diskExhausted) return false early

	// 1. Start with fresh allocator
	nodeAllocator->clearPersistedCaches();

	// 2. Allocate without any persisted deletions - will trigger disk scan
	int64_t id1 = nodeAllocator->allocate();
	EXPECT_EQ(id1, 1);

	// 3. Allocate again - should use exhausted flag optimization
	int64_t id2 = nodeAllocator->allocate();
	EXPECT_EQ(id2, 2);

	// 4. No disk scan should occur on second allocation
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 2);
}

TEST_F(IDAllocatorTest, ResetAfterCompaction_VolatileCleared) {
	// Test that resetAfterCompaction clears volatile cache
	// This is distinct from clearAllCaches which preserves volatile cache

	// 1. Allocate dirty ID
	int64_t id1 = nodeAllocator->allocate();

	// 2. Free to volatile cache
	nodeAllocator->free(id1);

	// 3. Simulate compaction reset
	nodeAllocator->resetAfterCompaction();

	// 4. Next allocation should NOT reuse id1 (volatile was cleared)
	// Since volatile cache is cleared and max is still 1, next allocation should be 2
	int64_t id2 = nodeAllocator->allocate();
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
		logicalMax = nodeAllocator->allocate();
	}
	EXPECT_EQ(logicalMax, 103);

	// 3. Simulate restart - gap [4, 103] should be recovered to volatile cache
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);

	// 4. Next allocations should reuse from gap
	std::unordered_set<int64_t> gapIds;
	for (int i = 0; i < 100; ++i) {
		int64_t id = nodeAllocator->allocate();
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
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);

	// Should recover correctly from physical max
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 2);
}

TEST_F(IDAllocatorTest, IntervalSet_MergeWithMultipleIntervals) {
	// Test IDIntervalSet merging with multiple existing intervals
	// This tests the while loop at line 70: merging with subsequent intervals

	nodeAllocator->setCacheLimits(100, 100, 50000);

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
		int64_t id = nodeAllocator->allocate();
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

TEST_F(IDAllocatorTest, DiskScan_MaxScansPerCall) {
	// Test that disk scan respects MAX_SCANS_PER_CALL limit (line 391)
	// Create enough nodes across many segments to exercise multi-segment scanning
	nodeAllocator->setCacheLimits(5, 100, 50000); // Small L1 to trigger disk scan sooner

	constexpr int COUNT = 30;
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// Delete some to create inactive entries
	for (int i = 1; i <= 5; ++i) {
		deleteNode(i);
	}
	fileStorage->save();

	// Clear caches to force disk scan
	nodeAllocator->clearPersistedCaches();

	// Allocate - triggers disk scan
	for (int i = 0; i < 5; ++i) {
		int64_t id = nodeAllocator->allocate();
		EXPECT_GT(id, 0);
		EXPECT_LE(id, COUNT);
	}
}

TEST_F(IDAllocatorTest, RecoverGapIds_PhysicalEqualsLogical) {
	// Test recoverGapIds when physicalMaxId == logicalMaxId (neither CASE A nor CASE B)
	// This is the normal case where no gaps or stale header exist

	for (int i = 0; i < 3; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// Restart - physical == logical, no gaps to recover
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);

	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 3);
	// Next should be 4 (no gap recovery needed)
	int64_t next = nodeAllocator->allocate();
	EXPECT_EQ(next, 4);
}

TEST_F(IDAllocatorTest, ClearAllCaches_PreservesVolatile) {
	// Test that clearAllCaches does NOT clear volatile cache
	// (volatile cache stores dirty IDs that would be lost)

	// Allocate and free ID without persisting -> volatile cache
	int64_t id1 = nodeAllocator->allocate();
	nodeAllocator->free(id1);

	// clearAllCaches should NOT clear volatile
	nodeAllocator->clearPersistedCaches();

	// Should still get id1 back from volatile
	int64_t reused = nodeAllocator->allocate();
	EXPECT_EQ(reused, id1) << "clearAllCaches should preserve volatile cache";
}

TEST_F(IDAllocatorTest, Initialize_RecoverGaps_AllEntityTypes) {
	// Test that initialize() calls recoverGapIds for all entity types
	// Create IDs for multiple types, flush, allocate more without flushing, restart

	(void)insertNode("N");
	int64_t edgeId = edgeAllocator->allocate();
	auto propAllocator = fileStorage->getIDAllocator(graph::EntityType::Property);
	auto blobAllocator = fileStorage->getIDAllocator(graph::EntityType::Blob);
	int64_t propId = propAllocator->allocate();
	int64_t blobId = blobAllocator->allocate();
	(void)edgeId;
	(void)propId;
	(void)blobId;

	fileStorage->flush();

	// Allocate more without flushing (creates gaps on restart)
	int64_t gapNode = nodeAllocator->allocate();
	int64_t gapEdge = edgeAllocator->allocate();
	(void)gapNode;
	(void)gapEdge;

	// Restart
	database->close();
	database.reset();
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
	edgeAllocator = fileStorage->getIDAllocator(graph::EntityType::Edge);
	dataManager = fileStorage->getDataManager();

	// Gap IDs should be recoverable via volatile cache
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 2);
	EXPECT_EQ(edgeAllocator->getCurrentMaxId(), 2);
}

TEST_F(IDAllocatorTest, RecoverGapIds_SegmentWithZeroUsed) {
	// Covers line 129: header.used == 0 (false branch)
	// When a segment has 0 used items, physicalMaxId stays 0

	// Allocate a node (creates segment with used > 0)
	(void)insertNode("N");
	fileStorage->flush();

	// Now close and reopen - initialize will scan segments
	database->close();
	database.reset();
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
	dataManager = fileStorage->getDataManager();

	// Verify allocator is functional
	int64_t id = nodeAllocator->allocate();
	EXPECT_GT(id, 0);
}

TEST_F(IDAllocatorTest, ResetAfterCompaction_EmptyScanCursors) {
	// Covers line 184: loop over scanCursors_ after clearing
	// The loop should never execute because scanCursors_ was just cleared

	nodeAllocator->resetAfterCompaction();

	// Verify allocator is functional after reset
	int64_t id = nodeAllocator->allocate();
	EXPECT_EQ(id, 1) << "After reset, sequential allocation should start from 1";
}

TEST_F(IDAllocatorTest, RecoverGapIds_CaseA_LogicalGtPhysical) {
	// Targets line 140: logicalMaxId > physicalMaxId (CASE A gap recovery)
	// Strategy: Persist nodes, allocate more without persisting, close DB,
	// then reopen. The header will have a higher logical max than physical.

	// 1. Create and persist 3 nodes
	(void)insertNode("A"); // ID 1
	(void)insertNode("B"); // ID 2
	(void)insertNode("C"); // ID 3
	fileStorage->save(); // Physical max = 3 on disk

	// 2. Allocate more IDs WITHOUT persisting them to segments
	// These only increment the logical max counter
	int64_t id4 = nodeAllocator->allocate(); // ID 4
	int64_t id5 = nodeAllocator->allocate(); // ID 5
	EXPECT_EQ(id4, 4);
	EXPECT_EQ(id5, 5);

	// 3. Close and reopen the database
	// FileHeaderManager saves logicalMaxId=5 on close, but physical max on disk=3
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
	dataManager = fileStorage->getDataManager();

	// 4. initialize() -> recoverGapIds should detect logicalMax(5) > physicalMax(3)
	// Gap [4, 5] should be in volatile cache
	// First allocations should reuse gap IDs
	std::unordered_set<int64_t> recoveredIds;
	recoveredIds.insert(nodeAllocator->allocate());
	recoveredIds.insert(nodeAllocator->allocate());

	// Should recover IDs 4 and 5 from the gap
	EXPECT_TRUE(recoveredIds.count(4) > 0) << "Gap ID 4 not recovered";
	EXPECT_TRUE(recoveredIds.count(5) > 0) << "Gap ID 5 not recovered";
}

TEST_F(IDAllocatorTest, DiskScanExhaustion_FallsBackToSequential) {
	// Targets line 371: cursor.diskExhausted returns true
	// Strategy: Create a few IDs, free them, flush to disk, clear caches,
	// then allocate until the disk scanner exhausts all segments.
	// After exhaustion, subsequent allocations should get sequential IDs.

	nodeAllocator->setCacheLimits(2, 2, 50);

	// 1. Create nodes and flush them to disk
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 5; i++) {
		nodes.push_back(insertNode("TestLabel"));
	}
	fileStorage->flush();

	// 2. Delete some nodes to create inactive entries on disk
	for (int i = 0; i < 3; i++) {
		dataManager->deleteNode(nodes[i]);
	}
	fileStorage->flush();

	// 3. Clear all caches so the next allocations must scan disk
	nodeAllocator->clearPersistedCaches();

	// 4. Allocate IDs - first few should recover from disk scan
	std::vector<int64_t> allocated;
	for (int i = 0; i < 10; i++) {
		int64_t id = nodeAllocator->allocate();
		EXPECT_GT(id, 0);
		allocated.push_back(id);
	}

	// 5. Clear caches again and allocate more - disk should now be exhausted
	nodeAllocator->clearPersistedCaches();
	for (int i = 0; i < 10; i++) {
		int64_t id = nodeAllocator->allocate();
		EXPECT_GT(id, 0);
	}
}

TEST_F(IDAllocatorTest, ResetAfterCompaction_ClearsState) {
	// Targets line 184: for loop over scanCursors_ (dead code path - scanCursors_
	// is cleared before the loop, so this tests the function still works correctly)

	// 1. Allocate some IDs to establish state
	for (int i = 0; i < 5; i++) {
		(void)nodeAllocator->allocate();
	}

	// 2. Call resetAfterCompaction
	nodeAllocator->resetAfterCompaction();

	// 3. Verify allocator still works
	int64_t id = nodeAllocator->allocate();
	EXPECT_GT(id, 0);
}

TEST_F(IDAllocatorTest, RecoverGapIds_SegmentWithZeroUsed_PhysicalMaxStaysZero) {
	// Targets line 129 False branch: header.used == 0
	// When a segment has used=0, we skip the physicalMaxId update.
	// Strategy: Create a DB where segments exist but have 0 used count.
	// This happens when we allocate segments but never write entities to them.

	// 1. Open a fresh DB - no nodes inserted, so segments may not exist
	// 2. Force flush to create file header state
	fileStorage->flush();

	// 3. Close and reopen
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
	edgeAllocator = fileStorage->getIDAllocator(graph::EntityType::Edge);
	dataManager = fileStorage->getDataManager();

	// Edge type should have no segments at all, meaning chain head is 0
	// and the while loop in recoverGapIds never executes.
	// For node type with no data, same applies.
	EXPECT_EQ(edgeAllocator->getCurrentMaxId(), 0);
	EXPECT_EQ(nodeAllocator->getCurrentMaxId(), 0);

	// First allocation should be 1
	int64_t id = edgeAllocator->allocate();
	EXPECT_EQ(id, 1);
}

TEST_F(IDAllocatorTest, RecoverGapIds_MultipleSegments_UsedZeroInSome) {
	// Targets line 129 False branch more directly: a segment with used=0
	// among segments that have used>0.
	// This is hard to create with normal API since segments auto-fill.
	// But we can create a scenario where after deletions + compaction,
	// some segments end up with used=0.

	// 1. Create enough nodes to span at least one segment
	constexpr int COUNT = 20;
	for (int i = 0; i < COUNT; ++i) {
		(void)insertNode("N");
	}
	fileStorage->flush();

	// 2. Delete all nodes - makes segments have all inactive entries
	for (int i = 1; i <= COUNT; ++i) {
		deleteNode(i);
	}
	fileStorage->flush();

	// 3. Close and reopen to trigger recoverGapIds
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	nodeAllocator = fileStorage->getIDAllocator(graph::EntityType::Node);
	dataManager = fileStorage->getDataManager();

	// 4. Allocator should still work correctly
	int64_t id = nodeAllocator->allocate();
	EXPECT_GT(id, 0);
}
