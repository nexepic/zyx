/**
 * @file test_FileStorage_FlushPaths.cpp
 * @brief Coverage tests for FileStorage::flush() exception, listener, and
 *        compaction branches.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <thread>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;

class FileStorageFlushTest : public ::testing::Test {
protected:
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_flush_" + to_string(uuid) + ".dat");
	}

	void TearDown() override {
		std::error_code ec;
		fs::remove(testFilePath, ec);
	}
};

// Test listener notification and expired listener cleanup during flush
class TestStorageEventListener : public IStorageEventListener {
public:
	int flushCount = 0;
	void onStorageFlush() override { flushCount++; }
};

TEST_F(FileStorageFlushTest, FlushNotifiesListeners) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	auto listener = std::make_shared<TestStorageEventListener>();
	storage->registerEventListener(listener);

	Node n(0, 0);
	dm->addNode(n);
	storage->flush();

	EXPECT_GE(listener->flushCount, 1);
	db.close();
}

TEST_F(FileStorageFlushTest, FlushRemovesExpiredListeners) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	{
		auto listener = std::make_shared<TestStorageEventListener>();
		storage->registerEventListener(listener);
		// listener goes out of scope here
	}

	Node n(0, 0);
	dm->addNode(n);
	// Should not crash when the listener weak_ptr has expired
	EXPECT_NO_THROW(storage->flush());
	db.close();
}

TEST_F(FileStorageFlushTest, FlushWithCompactionAfterDelete) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	// Add many nodes and flush to disk
	std::vector<Node> nodes;
	for (int i = 0; i < 20; ++i) {
		Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	storage->flush();

	// Delete most nodes to trigger compaction
	for (int i = 0; i < 15; ++i) {
		dm->deleteNode(nodes[i]);
	}
	// Compaction is enabled by default, so flush after delete triggers compaction path
	storage->flush();

	db.close();
}

TEST_F(FileStorageFlushTest, CloseFlushesFileHeader) {
	{
		Database db(testFilePath.string());
		db.open();
		auto storage = db.getStorage();
		auto dm = storage->getDataManager();

		Node n(0, 0);
		dm->addNode(n);
		// close() should flush pending changes + file header
		db.close();
	}

	// Reopen and verify data persisted
	Database db2(testFilePath.string());
	db2.open();
	auto dm2 = db2.getStorage()->getDataManager();
	Node loaded = dm2->getNode(1);
	EXPECT_TRUE(loaded.isActive());
	db2.close();
}

TEST_F(FileStorageFlushTest, VerifyIntegrity) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	Node n(0, 0);
	dm->addNode(n);
	storage->flush();

	auto result = storage->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
	db.close();
}

TEST_F(FileStorageFlushTest, FlushAfterMinimalDeleteSkipsCompaction) {
	// Fill all entity segments above 70% capacity so getTotalFragmentationRatio() < 0.3
	// This makes shouldCompact() return false, covering the False branch at line 374.
	auto storage = std::make_shared<graph::storage::FileStorage>(testFilePath.string(), 64);
	storage->open();
	storage->setCompactionEnabled(true);
	auto dm = storage->getDataManager();

	// Fill nodes (capacity=512, need 359+ per segment for <30% frag)
	std::vector<Node> nodes;
	for (int i = 0; i < 400; ++i) {
		Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}

	// Fill index segment (capacity varies, add enough to dominate)
	for (int i = 0; i < 400; ++i) {
		graph::Index idx;
		idx.setId(0);
		idx.getMutableMetadata().isActive = true;
		dm->addIndexEntity(idx);
	}

	// Fill state segment
	for (int i = 0; i < 400; ++i) {
		graph::State s;
		s.setId(0);
		s.setKey("k" + std::to_string(i));
		dm->addStateEntity(s);
	}

	storage->flush();

	// Delete only 1 node — overall fragmentation stays low
	dm->deleteNode(nodes[0]);
	storage->flush();

	storage->close();
}

TEST_F(FileStorageFlushTest, ConcurrentFlushReturnsEarly) {
	// Exercise the flush lock contention path (!lock.owns_lock())
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	// Add some data so flush has work to do
	for (int i = 0; i < 50; ++i) {
		Node n(0, 0);
		dm->addNode(n);
	}

	// Launch multiple concurrent flushes — some will fail to acquire the lock
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i) {
		threads.emplace_back([&storage]() { storage->flush(); });
	}
	for (auto &t : threads) {
		t.join();
	}

	db.close();
}

TEST_F(FileStorageFlushTest, VerifyBitmapConsistency) {
	Database db(testFilePath.string());
	db.open();
	auto storage = db.getStorage();
	auto dm = storage->getDataManager();

	Node n(0, 0);
	dm->addNode(n);
	storage->flush();

	// Get segment offset for node segment
	auto segments = storage->getSegmentTracker()->getSegmentsByType(Node::typeId);
	ASSERT_FALSE(segments.empty());
	EXPECT_TRUE(storage->verifyBitmapConsistency(segments[0].file_offset));
	db.close();
}
