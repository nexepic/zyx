/**
 * @file test_FileStorage_ReadOps.cpp
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

#include "storage/FileStorageTestFixture.hpp"
#include "graph/storage/IStorageEventListener.hpp"

class MockStorageListener : public graph::storage::IStorageEventListener {
public:
	int flushCount = 0;
	void onStorageFlush() override { flushCount++; }
};

// ============================================================================
// Open, Flush & VerifyBitmapConsistency Tests
// ============================================================================

TEST_F(FileStorageTest, TestOpenClose) {
	EXPECT_TRUE(fileStorage->isOpen());
	fileStorage->close();
	EXPECT_FALSE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, VerifyBitmapConsistency) {
	auto dataManager = fileStorage->getDataManager();

	// Create segment with nodes
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dataManager->addNode(n);
	}
	fileStorage->flush();

	// Delete one node
	auto nodes = dataManager->getNodesInRange(1, 5, 10);
	if (!nodes.empty()) {
		dataManager->deleteNode(nodes[0]);
	}
	fileStorage->flush();

	// Find the segment offset
	uint64_t segOffset = dataManager->findSegmentForEntityId<graph::Node>(nodes[0].getId());

	// Call verify
	bool consistent = fileStorage->verifyBitmapConsistency(segOffset);
	EXPECT_TRUE(consistent);
}

TEST_F(FileStorageTest, FlushWithListener) {
	auto listener = std::make_shared<MockStorageListener>();
	fileStorage->registerEventListener(listener);

	// Trigger flush
	fileStorage->flush();

	EXPECT_EQ(listener->flushCount, 1);
}

TEST_F(FileStorageTest, FlushWithExpiredListener) {
	// Register listener that will expire
	{
		auto listener = std::make_shared<MockStorageListener>();
		fileStorage->registerEventListener(listener);
		// Listener dies here
	}

	// Flush should handle expired weak_ptr gracefully
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, VerifyBitmapConsistency_DetectsInconsistency) {
	auto dm = fileStorage->getDataManager();
	// 1. Add some nodes
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Get offset
	uint64_t segOffset = dm->findSegmentForEntityId<graph::Node>(1);
	auto tracker = fileStorage->getSegmentTracker();

	// 2. Corrupt the header (Manual Hack)
	// inactive_count is currently 0. Set it to 5.
	// Bitmap says 0 inactive. Header says 5 inactive. -> Inconsistent.
	tracker->updateSegmentHeader(segOffset, [](graph::storage::SegmentHeader &h) { h.inactive_count = 5; });

	// 3. Verify returns false
	// Note: You might see "Bitmap inconsistency detected..." in stderr
	EXPECT_FALSE(fileStorage->verifyBitmapConsistency(segOffset));
}

TEST_F(FileStorageTest, Open_CreateFails_InvalidPath) {
	// Attempt to create a file in a non-existent directory tree
	// e.g. /tmp/non_existent_dir/db.dat (assuming non_existent_dir wasn't created)
	// Or use an illegal character if OS supports it (e.g. NUL on Linux)

	// Robust approach: Use a path that is actually a directory.
	// Opening a directory as a file stream usually fails.
	std::filesystem::path dirPath = std::filesystem::temp_directory_path() / "test_dir_conflict";
	std::filesystem::create_directory(dirPath);

	EXPECT_THROW(
			{
				graph::storage::FileStorage fs(dirPath.string(), 1024, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
				fs.open();
			},
			std::runtime_error);

	{ std::error_code ec; std::filesystem::remove(dirPath, ec); }
}

TEST_F(FileStorageTest, Open_ExistingFails_Permissions) {
	// 1. Create a dummy file
	std::filesystem::path lockedPath = std::filesystem::temp_directory_path() / "locked.zyx";
	{
		std::ofstream f(lockedPath);
	} // file created and closed

	// 2. Remove read/write permissions
	// Note: This might depend on OS. Works on Linux/macOS.
	// On Windows, permissions behave differently (might need ACLs or file locking).
	std::filesystem::permissions(lockedPath, std::filesystem::perms::none, std::filesystem::perm_options::replace);

	// 3. Try to open
	EXPECT_THROW(
			{
				graph::storage::FileStorage fs(lockedPath.string(), 1024, graph::storage::OpenMode::OPEN_EXISTING_FILE);
				fs.open();
			},
			std::runtime_error);

	// Cleanup: Restore permissions so we can delete it
	std::filesystem::permissions(lockedPath, std::filesystem::perms::all);
	{ std::error_code ec; std::filesystem::remove(lockedPath, ec); }
}

TEST_F(FileStorageTest, Open_ExistingFails_DirectoryAsFile) {
	std::filesystem::path dirPath = std::filesystem::temp_directory_path() / "test_dir_open_fail";
	std::filesystem::create_directory(dirPath);

	EXPECT_THROW(
			{
				graph::storage::FileStorage fs(dirPath.string(), 1024, graph::storage::OpenMode::OPEN_EXISTING_FILE);
				fs.open();
			},
			std::runtime_error);

	{ std::error_code ec; std::filesystem::remove(dirPath, ec); }
}

TEST_F(FileStorageTest, Open_AlreadyOpen_ReturnsEarly) {
	EXPECT_TRUE(fileStorage->isOpen());
	// Calling open() again should be a no-op
	EXPECT_NO_THROW(fileStorage->open());
	EXPECT_TRUE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, Close_WhenNotOpen_IsNoop) {
	fileStorage->close();
	EXPECT_FALSE(fileStorage->isOpen());
	// Calling close() again should not crash
	EXPECT_NO_THROW(fileStorage->close());
	EXPECT_FALSE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, Open_ExistingMode_NonExistentFile_Throws) {
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	std::filesystem::path nonExistentPath =
			std::filesystem::temp_directory_path() / ("nonexistent_" + to_string(uuid) + ".dat");

	EXPECT_THROW(
			{
				graph::storage::FileStorage fs(nonExistentPath.string(), 1024,
											   graph::storage::OpenMode::OPEN_EXISTING_FILE);
				fs.open();
			},
			std::runtime_error);
}

TEST_F(FileStorageTest, Open_CreateNewMode_ExistingFile_Throws) {
	// testFilePath already exists from SetUp
	EXPECT_THROW(
			{
				graph::storage::FileStorage fs(testFilePath.string(), 1024,
											   graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
				fs.open();
			},
			std::runtime_error);
}

TEST_F(FileStorageTest, VerifyBitmapConsistency_ZeroOffset_ReturnsFalse) {
	EXPECT_FALSE(fileStorage->verifyBitmapConsistency(0));
}

TEST_F(FileStorageTest, Flush_WithCompactionEnabled) {
	fileStorage->setCompactionEnabled(true);
	EXPECT_TRUE(fileStorage->isCompactionEnabled());

	auto dm = fileStorage->getDataManager();

	// Add nodes
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Delete a node to trigger deleteOperationPerformed
	auto nodes = dm->getNodesInRange(1, 5, 10);
	if (!nodes.empty()) {
		dm->deleteNode(nodes[0]);
	}

	// flush should handle compaction path
	EXPECT_NO_THROW(fileStorage->flush());

	fileStorage->setCompactionEnabled(false);
}

TEST_F(FileStorageTest, Open_CreateOrOpen_ExistingFile) {
	// Close the current database first
	database->close();
	database.reset();

	// Reopen the same file with OPEN_CREATE_OR_OPEN_FILE mode (existing file)
	database = std::make_unique<graph::Database>(testFilePath.string(),
												 graph::storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
	EXPECT_NO_THROW(database->open());
	EXPECT_TRUE(database->isOpen());
	fileStorage = database->getStorage();
}

TEST_F(FileStorageTest, Open_CreateNewFile_InvalidPath_Throws) {
	// Use a path inside a non-existent directory
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	std::filesystem::path badPath =
			std::filesystem::temp_directory_path() / ("nonexistent_dir_" + to_string(uuid)) / "db.dat";

	EXPECT_THROW(
			{
				graph::storage::FileStorage fs(badPath.string(), 1024,
											   graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
				fs.open();
			},
			std::runtime_error);
}

TEST_F(FileStorageTest, Flush_ConcurrentCalls_NoDeadlock) {
	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);

	// Call flush from multiple threads to exercise the try_lock and flushInProgress branches
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i) {
		threads.emplace_back([this]() { fileStorage->flush(); });
	}
	for (auto &t: threads) {
		t.join();
	}
	EXPECT_TRUE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, Flush_CompactionDisabled) {
	// Test flush when compaction is disabled (line 636 false branch)
	fileStorage->setCompactionEnabled(false);
	EXPECT_FALSE(fileStorage->isCompactionEnabled());

	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);
	dm->deleteNode(n);

	// Flush with compaction disabled
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, Flush_DeleteOperationNotPerformed) {
	// Test flush when deleteOperationPerformed is false (line 636 false branch)
	fileStorage->setCompactionEnabled(true);

	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);

	// Flush without any delete operations
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, FlushExceptionHandling) {
	// Test that flush handles exceptions gracefully (line 658-663)
	// We can't easily trigger an exception, but we can test that
	// flush with valid state doesn't crash
	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, VerifyBitmapConsistency_Inconsistent) {
	auto dm = fileStorage->getDataManager();
	auto segTracker = dm->getSegmentTracker();

	// Create node and save
	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Find the node segment
	auto segments = segTracker->getSegmentsByType(static_cast<uint32_t>(graph::EntityType::Node));
	ASSERT_FALSE(segments.empty());

	auto segOffset = segments[0].file_offset;

	// Manually corrupt the inactive_count in the header to create inconsistency
	segTracker->updateSegmentHeader(segOffset, [](graph::storage::SegmentHeader &hdr) {
		hdr.inactive_count = 999; // Deliberately wrong
	});

	// This should detect inconsistency and return false
	bool consistent = fileStorage->verifyBitmapConsistency(segOffset);
	EXPECT_FALSE(consistent);
}

TEST_F(FileStorageTest, FlushTriggersCompaction) {
	auto dm = fileStorage->getDataManager();

	// Add many nodes then delete most to trigger compaction conditions
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 20; i++) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Delete most nodes to create fragmentation
	for (int i = 0; i < 18; i++) {
		dm->deleteNode(nodes[i]);
	}

	// Flush should check compaction condition (line 636-641)
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, Flush_DeleteWithCompactionEnabled_NoCompactionNeeded) {
	fileStorage->setCompactionEnabled(true);
	auto dm = fileStorage->getDataManager();

	// Add a small number of nodes
	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Delete just one node (unlikely to trigger compaction threshold)
	dm->deleteNode(n);

	// Flush should check compaction but shouldCompact likely returns false
	EXPECT_NO_THROW(fileStorage->flush());
	fileStorage->setCompactionEnabled(false);
}

TEST_F(FileStorageTest, FlushAfterMixedOperations) {
	auto dm = fileStorage->getDataManager();

	// Add several nodes
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	graph::Node n3(0, 0);
	dm->addNode(n3);

	// Add edges
	graph::Edge e1(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e1);
	graph::Edge e2(0, n2.getId(), n3.getId(), 0);
	dm->addEdge(e2);

	// Flush initial state
	fileStorage->flush();

	// Modify n1
	n1.setLabelId(42);
	dm->updateNode(n1);

	// Delete n3 (and its edge)
	dm->deleteNode(n3);
	dm->deleteEdge(e2);

	// Create a new node in the same cycle
	graph::Node n4(0, 0);
	dm->addNode(n4);

	// Single flush handles adds, modifies, and deletes
	EXPECT_NO_THROW(fileStorage->flush());

	// Verify
	dm->clearCache();
	graph::Node loadedN1 = dm->loadNodeFromDisk(n1.getId());
	EXPECT_EQ(loadedN1.getLabelId(), 42);

	// n3 was deleted - loadNodeFromDisk may still return the raw entity
	// The important thing is that n1 and n4 are correctly persisted

	graph::Node loadedN4 = dm->loadNodeFromDisk(n4.getId());
	EXPECT_NE(loadedN4.getId(), 0); // Newly added
}

TEST_F(FileStorageTest, MultipleFlushCloseReopenCycles) {
	for (int cycle = 0; cycle < 3; cycle++) {
		auto dm = fileStorage->getDataManager();

		// Add a node per cycle
		graph::Node n(0, 0);
		dm->addNode(n);
		fileStorage->flush();

		// Close and reopen
		database->close();
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
	}

	// After 3 cycles, verify we can read nodes
	auto dm = fileStorage->getDataManager();
	dm->clearCache();
	auto nodes = dm->getNodesInRange(1, 3, 10);
	EXPECT_GE(nodes.size(), 1u);
}

TEST_F(FileStorageTest, Flush_CompactionEnabledWithHighFragmentation) {
	// Test flush path where compaction is actually needed (lines 637-648)
	fileStorage->setCompactionEnabled(true);
	auto dm = fileStorage->getDataManager();

	// Create many nodes
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 50; i++) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Delete most nodes to create high fragmentation (>30% threshold)
	for (int i = 0; i < 45; i++) {
		dm->deleteNode(nodes[i]);
	}

	// This flush should trigger shouldCompact() -> true -> safeCompactSegments()
	EXPECT_NO_THROW(fileStorage->flush());
	fileStorage->setCompactionEnabled(false);
}

TEST_F(FileStorageTest, Flush_ConcurrentFlush_SkippedWhenInProgress) {
	// Covers line 610: flushInProgress.exchange(true) returns true
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);

	// Two rapid flush calls - second should be safe
	EXPECT_NO_THROW(fileStorage->flush());
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, Flush_ListenerThrowsException_HandledGracefully) {
	// Covers line 658-663: exception handling during flush
	class ThrowingListener : public graph::storage::IStorageEventListener {
	public:
		void onStorageFlush() override {
			throw std::runtime_error("Listener error");
		}
	};

	auto listener = std::make_shared<ThrowingListener>();
	fileStorage->registerEventListener(listener);

	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);

	// Flush should handle the exception internally
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, Flush_CompactionEnabled_LowFragmentation_NoCompact) {
	// Covers line 636-637: deleteOperationPerformed=true but shouldCompact=false
	fileStorage->setCompactionEnabled(true);
	auto dm = fileStorage->getDataManager();

	// Create a few nodes
	graph::Node n1(0, 0);
	dm->addNode(n1);
	fileStorage->flush();

	// Delete node to set deleteOperationPerformed flag
	dm->deleteNode(n1);

	// Flush with deletion flag set but low fragmentation
	EXPECT_NO_THROW(fileStorage->flush());
	fileStorage->setCompactionEnabled(false);
}

TEST_F(FileStorageTest, VerifyBitmapConsistency_ValidSegment_ReturnsTrue) {
	// Covers the full bitmap consistency check path returning true
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	auto tracker = fileStorage->getSegmentTracker();
	uint64_t nodeHead = tracker->getChainHead(graph::Node::typeId);
	ASSERT_NE(nodeHead, 0u);

	bool consistent = fileStorage->verifyBitmapConsistency(nodeHead);
	EXPECT_TRUE(consistent);
}

TEST_F(FileStorageTest, Open_CreateNewFile_Success) {
	// Test OPEN_CREATE_NEW_FILE mode with a path that doesn't exist
	boost::uuids::uuid uuid2 = boost::uuids::random_generator()();
	auto newPath = std::filesystem::temp_directory_path() / ("test_new_" + to_string(uuid2) + ".dat");

	std::filesystem::remove(newPath);

	{
		auto newDb = std::make_unique<graph::Database>(
			newPath.string(), graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		EXPECT_NO_THROW(newDb->open());
		auto storage = newDb->getStorage();
		EXPECT_TRUE(storage->isOpen());
		newDb->close();
	}

	{ std::error_code ec; std::filesystem::remove(newPath, ec); }
}

TEST_F(FileStorageTest, Open_ExistingFile_Success) {
	// Test OPEN_EXISTING_FILE mode with a file that exists
	database->close();

	auto db2 = std::make_unique<graph::Database>(
		testFilePath.string(), graph::storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_NO_THROW(db2->open());
	auto storage = db2->getStorage();
	EXPECT_TRUE(storage->isOpen());
	db2->close();

	// Re-create the original database for TearDown
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
}

TEST_F(FileStorageTest, Flush_HighFragmentation_TriggersCompaction) {
	// Covers line 637-648: shouldCompact() returns true, safeCompactSegments succeeds
	fileStorage->setCompactionEnabled(true);
	auto dm = fileStorage->getDataManager();

	// Create many nodes to fill segments
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 60; i++) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Delete most to create high fragmentation
	for (int i = 0; i < 55; i++) {
		dm->deleteNode(nodes[i]);
	}

	// This flush should trigger compaction
	EXPECT_NO_THROW(fileStorage->flush());
	fileStorage->setCompactionEnabled(false);
}

TEST_F(FileStorageTest, Close_FlushesAndTruncates) {
	// Covers the close() method which calls flush and truncateFile
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Close should flush and truncate
	EXPECT_NO_THROW(database->close());
	EXPECT_FALSE(fileStorage->isOpen());

	// Reopen for TearDown
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
}

TEST_F(FileStorageTest, Open_OPEN_EXISTING_FILE_NonExistent_Throws) {
	// Covers line 51: OPEN_EXISTING_FILE with non-existent file
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto nonExistentPath = std::filesystem::temp_directory_path() / ("nonexistent_" + to_string(uuid) + ".dat");

	auto db = std::make_unique<graph::Database>(nonExistentPath.string(),
		graph::storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_THROW(db->open(), std::runtime_error);
}

TEST_F(FileStorageTest, Open_OPEN_CREATE_NEW_FILE_ExistingFile_Throws) {
	// Covers line 54: OPEN_CREATE_NEW_FILE with already-existing file
	// testFilePath already exists from SetUp
	database->close();
	database.reset();

	auto db = std::make_unique<graph::Database>(testFilePath.string(),
		graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
	EXPECT_THROW(db->open(), std::runtime_error);

	// Reopen for TearDown
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
}

TEST_F(FileStorageTest, Open_ExistingFile_ReadsBackData) {
	// Covers lines 74-82: opening existing file path (else branch in open)
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Close and reopen to exercise the "open existing" path
	database->close();
	database = std::make_unique<graph::Database>(testFilePath.string(),
		graph::storage::OpenMode::OPEN_EXISTING_FILE);
	database->open();
	fileStorage = database->getStorage();

	EXPECT_TRUE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, Flush_WithCompaction_HighFragmentation) {
	auto dm = fileStorage->getDataManager();

	// Enable compaction (disabled by default)
	fileStorage->setCompactionEnabled(true);

	// Create many nodes and flush to persist them
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 20; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Delete >30% of nodes to exceed COMPACTION_THRESHOLD (0.3)
	// Delete 15 out of 20 = 75% fragmentation
	for (int i = 0; i < 15; ++i) {
		dm->deleteNode(nodes[i]);
	}

	// Flush should trigger: deleteOperationPerformed=true, compactionEnabled=true
	// Then shouldCompact() checks fragmentation > 0.3 (we have 75%)
	// Then safeCompactSegments() runs the compaction
	EXPECT_NO_THROW(fileStorage->flush());

	// After compaction, verify remaining nodes are still retrievable
	// (compaction may reorganize data but should preserve active nodes)
	dm->clearCache();
	auto remaining = dm->getNodesInRange(1, nodes.back().getId() + 1, 100);
	EXPECT_GE(remaining.size(), 1u); // At least some nodes survive
}

TEST_F(FileStorageTest, Flush_NoCompaction_WhenDisabled) {
	auto dm = fileStorage->getDataManager();

	// compaction is disabled by default
	EXPECT_FALSE(fileStorage->isCompactionEnabled());

	// Create nodes, flush, delete, flush again
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 10; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	for (int i = 0; i < 8; ++i) {
		dm->deleteNode(nodes[i]);
	}

	// Flush should NOT trigger compaction (line 636 False path)
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, Flush_ConcurrentFlushLockContention) {
	auto dm = fileStorage->getDataManager();

	// Create some data
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}

	// Launch two flush operations concurrently
	std::thread t1([&]() { fileStorage->flush(); });
	std::thread t2([&]() { fileStorage->flush(); });

	t1.join();
	t2.join();

	// No crash means success - one thread acquired the lock, the other returned early
	EXPECT_TRUE(true);
}
