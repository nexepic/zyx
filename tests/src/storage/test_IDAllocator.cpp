/**
 * @file test_IDAllocator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/2
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <unordered_set>
#include <future>
#include <random>

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
        database->close();
        database.reset();
        std::filesystem::remove(testFilePath);
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

    // Deleting node shouldn't affect edge allocation
    fileStorage->deleteNode(n1.getId());

    graph::Edge e2 = fileStorage->insertEdge(n1.getId(), n1.getId(), "E2");
    EXPECT_EQ(e2.getId(), 2); // Should be 2, not reusing Node ID 1
}

// ==========================================
// 2. Eager Reuse (L1 Cache) Tests
// ==========================================

TEST_F(IDAllocatorTest, EagerReuseWithoutFlush) {
    // 1. Insert 3 nodes
    (void)fileStorage->insertNode("A"); // 1
    (void)fileStorage->insertNode("B"); // 2
    (void)fileStorage->insertNode("C"); // 3
    fileStorage->save(); // Persist segments

    // 2. Delete ID 2
    // DeletionManager should trigger Eager Free, putting ID 2 into L1 Cache immediately.
    // We DO NOT call save() here to prove it works in memory.
    fileStorage->deleteNode(2);

    // 3. Insert new node -> Should assume ID 2 immediately
    graph::Node nReuse = fileStorage->insertNode("D");
    EXPECT_EQ(nReuse.getId(), 2) << "Failed to eagerly reuse ID from DeletionManager";

    // 4. Insert next -> Should be 4
    graph::Node nNew = fileStorage->insertNode("E");
    EXPECT_EQ(nNew.getId(), 4);
}

// ==========================================
// 3. Lazy Loading & Persistence Tests
// ==========================================

TEST_F(IDAllocatorTest, LazyLoadFromDiskAfterRestart) {
    // 1. Setup Data
    (void)fileStorage->insertNode("A"); // 1
    (void)fileStorage->insertNode("B"); // 2
    (void)fileStorage->insertNode("C"); // 3
    fileStorage->save();

    // 2. Delete and Persist
    // Calling save() ensures the Bitmap on disk is updated to '0' (inactive)
    fileStorage->deleteNode(2);
    fileStorage->save();

    // 3. Simulate Restart/OOM
    // This wipes L1 and L2 caches.
    allocator->clearAllCaches();

    // 4. Allocate -> Should trigger fetchInactiveIdsFromDisk and find ID 2
    // Note: We use fileStorage to ensure full integration
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
    // Behaviorally, we just ensure it gets the next correct ID.
    graph::Node n2 = fileStorage->insertNode("2");
    EXPECT_EQ(n2.getId(), 2);
}

// ==========================================
// 4. Tiered Cache (L1 Overflow & L2) Tests
// ==========================================

TEST_F(IDAllocatorTest, L1CacheOverflowToL2) {
    // L1 limit is 1024. We will delete 1500 items to force L2 usage.
    const int COUNT = 1500;
    std::vector<int64_t> ids;

    // 1. Insert 1500 nodes
    for(int i=0; i<COUNT; ++i) {
        (void)fileStorage->insertNode("N");
    }
    fileStorage->save(); // Create segments

    // 2. Delete all 1500 nodes
    // The first 1024 will fill L1. The remaining 476 should go to L2 (or vice versa depending on logic).
    for(int i=1; i<=COUNT; ++i) {
        fileStorage->deleteNode(i);
    }
    // We don't necessarily need to save(), memory reuse should work.

    // 3. Re-allocate 1500 nodes
    // Should reuse ALL IDs without increasing max ID > 1500
    for(int i=0; i<COUNT; ++i) {
        graph::Node n = fileStorage->insertNode("Re");
        EXPECT_LE(n.getId(), COUNT) << "ID leaked! Allocated " << n.getId() << " but should be <= " << COUNT;
    }

    // 4. Verify next is new
    graph::Node nNext = fileStorage->insertNode("New");
    EXPECT_EQ(nNext.getId(), COUNT + 1);
}

TEST_F(IDAllocatorTest, L2IntervalMerging) {
    // This tests if L2 efficiently handles sequential deletes.
    // Insert 2000 nodes.
    const int COUNT = 2000;
    for(int i=0; i<COUNT; ++i) (void)fileStorage->insertNode("N");
    fileStorage->save();

    // Delete nodes in blocks to trigger merges
    // Delete 1000..1500
    for(int i=1000; i<=1500; ++i) fileStorage->deleteNode(i);

    // Delete 1501..2000 (Should merge with previous interval in L2)
    for(int i=1501; i<=COUNT; ++i) fileStorage->deleteNode(i);

    // Now re-allocate.
    // If merging works, memory usage is low and all IDs are available.
    std::unordered_set<int64_t> reusedIds;
    for(int i=0; i<1001; ++i) {
        graph::Node n = fileStorage->insertNode("R");
        reusedIds.insert(n.getId());
    }

    // Verify we got ids from the range [1000, 2000]
    bool hitRange = false;
    for(int64_t id : reusedIds) {
        if(id >= 1000 && id <= 2000) hitRange = true;
    }
    EXPECT_TRUE(hitRange);
}

// ==========================================
// 5. Integration & Robustness Tests
// ==========================================

TEST_F(IDAllocatorTest, DeleteAndSaveIdempotency) {
    // Verify that calling freeId twice (once via DeletionManager, once via Save) is safe

    (void)fileStorage->insertNode("A"); // 1
    (void)fileStorage->insertNode("B"); // 2
    fileStorage->save();

    // 1. Delete Node 2
    // DeletionManager calls idAllocator->freeId(2).
    // Bitmap -> 0. L1 -> [2].
    fileStorage->deleteNode(2);

    // 2. Insert Node C -> Should reuse 2 immediately
    graph::Node nC = fileStorage->insertNode("C");
    EXPECT_EQ(nC.getId(), 2);
    // Bitmap -> 1. L1 -> [].

    // 3. Delete Node C (ID 2) again
    // Bitmap -> 0. L1 -> [2].
    fileStorage->deleteNode(2);

    // 4. Save
    // This triggers FileStorage::deleteEntityOnDisk(2).
    // It calls idAllocator->freeId(2).
    // Allocator checks: Is Bitmap Active? No (it is 0).
    // Returns safely. State is preserved.
    fileStorage->save();

    // 5. Verify ID 2 is still available
    graph::Node nD = fileStorage->insertNode("D");
    EXPECT_EQ(nD.getId(), 2);
}

TEST_F(IDAllocatorTest, MixedOperationsStress) {
    // Randomly Insert and Delete to simulate real workload
    std::vector<int64_t> activeIds;
    std::mt19937 gen(42);

    for(int i=0; i<1000; ++i) {
        if(activeIds.empty() || (gen() % 3 != 0)) { // 66% Insert
            graph::Node n = fileStorage->insertNode("R");
            activeIds.push_back(n.getId());
        } else { // 33% Delete
            std::uniform_int_distribution<> dis(0, activeIds.size() - 1);
            int idx = dis(gen);
            int64_t idToDelete = activeIds[idx];

            fileStorage->deleteNode(idToDelete);

            // Remove from tracking vector
            activeIds[idx] = activeIds.back();
            activeIds.pop_back();
        }
    }

    // Ensure data is consistent
    fileStorage->save();

    // Check internal consistency (Max ID shouldn't be astronomically high)
    // We performed ~660 inserts. Max ID should be around there, not 1000.
    EXPECT_LT(allocator->getCurrentMaxNodeId(), 800);
}

// ==========================================
// 6. Concurrency Tests
// ==========================================

TEST_F(IDAllocatorTest, ThreadSafetyConcurrency) {
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 200;

    // Use allocator directly to stress test the internal mutexes
    // Note: We only test uniqueness of allocation here.
    // Testing insert/delete concurrently requires DB-level locking which is outside Allocator scope.

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

    for (auto& f : futures) {
        std::vector<int64_t> ids = f.get();
        for (int64_t id : ids) {
            EXPECT_TRUE(uniqueIds.find(id) == uniqueIds.end())
                << "Duplicate ID generated during concurrency test: " << id;
            uniqueIds.insert(id);
        }
        totalCount += ids.size();
    }

    EXPECT_EQ(totalCount, NUM_THREADS * OPS_PER_THREAD);
}

// ==========================================
// 7. Clear Cache Tests
// ==========================================

TEST_F(IDAllocatorTest, ClearAllCachesMultipleTypes) {
    // 1. Setup
    (void)fileStorage->insertNode("N1");
    (void)fileStorage->insertEdge(1, 1, "E1");
    fileStorage->save();

    // 2. Delete and Free
    fileStorage->deleteNode(1);
    fileStorage->deleteEdge(1);
    fileStorage->save();

    // 3. Clear
    allocator->clearAllCaches();

    // 4. Validate Reuse (Should Lazy Load both types)
    EXPECT_EQ(allocator->allocateId(graph::Node::typeId), 1);
    EXPECT_EQ(allocator->allocateId(graph::Edge::typeId), 1);
}