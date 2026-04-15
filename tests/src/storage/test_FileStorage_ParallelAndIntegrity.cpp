/**
 * @file test_FileStorage_ParallelAndIntegrity.cpp
 * @brief Branch coverage tests for FileStorage — covers verifyIntegrity,
 *        parallel save with ThreadPool, and additional edge cases.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"

class FileStorageParallelTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
			("test_fs_parallel_" + to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
	}

	void TearDown() override {
		fileStorage.reset();
		database->close();
		database.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
};

// ============================================================================
// verifyIntegrity (lines 881-921)
// ============================================================================

TEST_F(FileStorageParallelTest, VerifyIntegrity_EmptyDatabase) {
	// On a fresh database, verifyIntegrity should return allPassed true
	// (no segments with mismatched CRCs)
	auto result = fileStorage->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
}

TEST_F(FileStorageParallelTest, VerifyIntegrity_AfterNodeCreation) {
	auto dm = fileStorage->getDataManager();

	// Create several nodes to establish segments
	for (int i = 0; i < 10; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	auto result = fileStorage->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
	EXPECT_FALSE(result.segments.empty());

	// Verify segment results have valid data
	for (const auto &seg : result.segments) {
		EXPECT_NE(seg.offset, 0ULL);
		EXPECT_GT(seg.capacity, 0U);
	}
}

TEST_F(FileStorageParallelTest, VerifyIntegrity_AfterMixedOperations) {
	auto dm = fileStorage->getDataManager();

	// Create nodes
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	// Create edges
	graph::Edge e(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e);

	// Create property entity
	graph::Property p;
	p.setId(0);
	dm->addPropertyEntity(p);

	fileStorage->flush();

	auto result = fileStorage->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);

	// Should have segments for multiple entity types
	EXPECT_GE(result.segments.size(), 2UL);
}

TEST_F(FileStorageParallelTest, VerifyIntegrity_AfterModifyAndDelete) {
	auto dm = fileStorage->getDataManager();

	// Create, modify, and delete to exercise all segment states
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Delete some
	auto nodes = dm->getNodesInRange(1, 5, 10);
	if (nodes.size() >= 2) {
		dm->deleteNode(nodes[0]);
		dm->deleteNode(nodes[1]);
	}
	fileStorage->flush();

	auto result = fileStorage->verifyIntegrity();
	// After modifications, integrity should still hold
	EXPECT_TRUE(result.allPassed);
}

// ============================================================================
// Parallel save path with ThreadPool (lines 239-286, 363-418)
// ============================================================================

TEST_F(FileStorageParallelTest, Save_WithThreadPool_AllEntityTypes) {
	// Set thread pool on FileStorage to enable parallel save paths
	graph::concurrent::ThreadPool pool(4);
	fileStorage->setThreadPool(&pool);

	auto dm = fileStorage->getDataManager();

	// Create nodes
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}

	// Create edges
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	graph::Edge e(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e);

	// Create property
	graph::Property p;
	p.setId(0);
	dm->addPropertyEntity(p);

	// Create blob
	graph::Blob b;
	b.setId(0);
	b.setData("test");
	dm->addBlobEntity(b);

	// Create index entity
	graph::Index idx;
	idx.setId(0);
	dm->addIndexEntity(idx);

	// Create state
	graph::State s;
	s.setId(0);
	s.setKey("key");
	dm->addStateEntity(s);

	// Save exercises the parallel preparation path (lines 239-286)
	EXPECT_NO_THROW(fileStorage->save());

	fileStorage->setThreadPool(nullptr);
}

TEST_F(FileStorageParallelTest, Save_WithThreadPool_ModifyAndDelete) {
	graph::concurrent::ThreadPool pool(4);
	fileStorage->setThreadPool(&pool);

	auto dm = fileStorage->getDataManager();

	// Add entities and flush
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	graph::Edge e(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e);

	graph::Property p;
	p.setId(0);
	dm->addPropertyEntity(p);

	graph::Blob b;
	b.setId(0);
	b.setData("data");
	dm->addBlobEntity(b);

	graph::Index idx;
	idx.setId(0);
	dm->addIndexEntity(idx);

	graph::State s;
	s.setId(0);
	s.setKey("stkey");
	dm->addStateEntity(s);

	fileStorage->save();

	// Now modify and delete to exercise parallel I/O path (lines 363-418)
	n1.setLabelId(42);
	dm->updateNode(n1);

	e.setTypeId(42);
	dm->updateEdge(e);

	p.setEntityInfo(99, 0);
	dm->updatePropertyEntity(p);

	b.setData("updated");
	dm->updateBlobEntity(b);

	idx.setParentId(10);
	dm->updateIndexEntity(idx);

	s.setKey("updated");
	dm->updateStateEntity(s);

	// This save should hit the parallel modify path
	EXPECT_NO_THROW(fileStorage->save());

	// Now delete entities to hit parallel delete path
	dm->deleteNode(n2);
	dm->deleteEdge(e);
	dm->deleteProperty(p);
	dm->deleteBlob(b);
	dm->deleteIndex(idx);
	dm->deleteState(s);

	EXPECT_NO_THROW(fileStorage->save());

	fileStorage->setThreadPool(nullptr);
}

TEST_F(FileStorageParallelTest, Save_WithThreadPool_OnlyNodes) {
	graph::concurrent::ThreadPool pool(4);
	fileStorage->setThreadPool(&pool);

	auto dm = fileStorage->getDataManager();

	// Only nodes — exercises the nodes-only parallel prep branch
	for (int i = 0; i < 10; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}

	EXPECT_NO_THROW(fileStorage->save());

	fileStorage->setThreadPool(nullptr);
}

TEST_F(FileStorageParallelTest, Save_WithThreadPool_OnlyEdges) {
	graph::concurrent::ThreadPool pool(4);
	fileStorage->setThreadPool(&pool);

	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	for (int i = 0; i < 10; ++i) {
		graph::Edge e(0, n1.getId(), n2.getId(), 0);
		dm->addEdge(e);
	}

	EXPECT_NO_THROW(fileStorage->save());

	fileStorage->setThreadPool(nullptr);
}

TEST_F(FileStorageParallelTest, Save_WithSingleThreadPool_Fallback) {
	// Single-threaded pool should fall back to sequential path
	graph::concurrent::ThreadPool pool(1);
	fileStorage->setThreadPool(&pool);

	auto dm = fileStorage->getDataManager();

	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}

	EXPECT_NO_THROW(fileStorage->save());

	fileStorage->setThreadPool(nullptr);
}

// ============================================================================
// Flush with ThreadPool and compaction
// ============================================================================

TEST_F(FileStorageParallelTest, Flush_WithThreadPool_AndCompaction) {
	graph::concurrent::ThreadPool pool(4);
	fileStorage->setThreadPool(&pool);
	fileStorage->setCompactionEnabled(true);

	auto dm = fileStorage->getDataManager();

	// Create many nodes
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 20; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Delete most to trigger compaction
	for (int i = 0; i < 15; ++i) {
		dm->deleteNode(nodes[i]);
	}

	EXPECT_NO_THROW(fileStorage->flush());

	fileStorage->setCompactionEnabled(false);
	fileStorage->setThreadPool(nullptr);
}

// ============================================================================
// writeSegmentData pwrite path (lines 605-616)
// ============================================================================

TEST_F(FileStorageParallelTest, WriteSegmentData_PwritePath) {
	// The pwrite path is exercised by saveData when writeFd_ is valid
	// (which it always is after open())
	auto dm = fileStorage->getDataManager();

	std::vector<graph::Node> data;
	for (int64_t i = 1; i <= 20; ++i) {
		data.push_back(graph::Node(i, 100));
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data, segHead, 100);

	EXPECT_NE(segHead, 0u);

	// Verify written data
	auto tracker = fileStorage->getSegmentTracker();
	auto header = tracker->getSegmentHeader(segHead);
	EXPECT_GE(header.used, 20u);
}

// ============================================================================
// Close/reopen after parallel save
// ============================================================================

TEST_F(FileStorageParallelTest, ParallelSave_ThenReopenAndVerify) {
	graph::concurrent::ThreadPool pool(4);
	fileStorage->setThreadPool(&pool);

	auto dm = fileStorage->getDataManager();

	// Create entities with parallel save
	int64_t lbl = dm->getOrCreateTokenId("ParallelNode");
	graph::Node n(0, lbl);
	dm->addNode(n);
	int64_t nId = n.getId();

	graph::Edge e(0, nId, nId, 0);
	dm->addEdge(e);
	int64_t eId = e.getId();

	fileStorage->save();
	fileStorage->setThreadPool(nullptr);

	// Close and reopen
	database->close();
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	dm = fileStorage->getDataManager();

	// Verify data persisted correctly
	dm->clearCache();
	auto loadedNode = dm->loadNodeFromDisk(nId);
	EXPECT_EQ(loadedNode.getId(), nId);

	auto loadedEdge = dm->loadEdgeFromDisk(eId);
	EXPECT_EQ(loadedEdge.getId(), eId);
}
