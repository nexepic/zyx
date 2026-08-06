/**
 * @file test_FileStorage_StateTransitions.cpp
 * @brief Branch coverage tests for FileStorage.cpp targeting:
 *        - open() OPEN_EXISTING_FILE with missing file (line 58)
 *        - open() OPEN_CREATE_NEW_FILE with existing file (line 61)
 *        - open() already open (line 52)
 *        - save() when not open (line 264)
 *        - save() with no unsaved changes (line 266)
 *        - flush() concurrent lock contention (line 391)
 *        - flush() compaction disabled path (line 417)
 *        - close() when not open
 *        - verifyBitmapConsistency, clearCache, verifyIntegrity
 *        - registerEventListener + expired listener cleanup
 **/

#include "storage/FileStorageTestFixture.hpp"

#include <memory>
#include <thread>
#include "graph/storage/IStorageEventListener.hpp"

class FileStorageStateTransitionsTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_fs_branch_" + to_string(uuid) + ".dat");
	}

	void TearDown() override {
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	[[nodiscard]] std::shared_ptr<graph::storage::FileStorage> openLegacyStorage() const {
		auto storage = std::make_shared<graph::storage::FileStorage>(
				testFilePath.string(), 16U, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		return storage;
	}
};

// ============================================================================
// open() with OPEN_EXISTING_FILE when file doesn't exist
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Open_ExistingFile_Missing) {
	auto fs = std::make_shared<graph::storage::FileStorage>(
		testFilePath.string(), 16, graph::storage::OpenMode::OPEN_EXISTING_FILE);
	EXPECT_THROW(fs->open(), std::runtime_error);
}

// ============================================================================
// open() with OPEN_CREATE_NEW_FILE when file already exists
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Open_CreateNewFile_AlreadyExists) {
	// Create the file first
	{
		std::ofstream ofs(testFilePath, std::ios::binary);
		ofs << "data";
	}
	auto fs = std::make_shared<graph::storage::FileStorage>(
		testFilePath.string(), 16, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
	EXPECT_THROW(fs->open(), std::runtime_error);
}

// ============================================================================
// open() when already open (returns early)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Open_AlreadyOpen) {
	auto fs = openLegacyStorage();
	// Second open should be a no-op
	EXPECT_NO_THROW(fs->open());
	fs->close();
}

// ============================================================================
// save() when not open
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Save_NotOpen) {
	auto fs = std::make_shared<graph::storage::FileStorage>(testFilePath.string(), 16);
	EXPECT_THROW(fs->save(), std::runtime_error);
}

// ============================================================================
// save() with no unsaved changes (returns early)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Save_NoChanges) {
	auto fs = openLegacyStorage();
	EXPECT_NO_THROW(fs->save());
	fs->close();
}

// ============================================================================
// flush() with compaction disabled
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Flush_CompactionDisabled) {
	auto fs = openLegacyStorage();

	fs->setCompactionEnabled(false);

	auto dm = fs->getDataManager();
	graph::Node n(0, 1);
	dm->addNode(n);

	// Flush with compaction disabled
	EXPECT_NO_THROW(fs->flush());
	fs->close();
}

// ============================================================================
// flush() with delete + compaction disabled
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Flush_DeleteNoCompact) {
	auto fs = openLegacyStorage();

	fs->setCompactionEnabled(false);

	auto dm = fs->getDataManager();
	graph::Node n(0, 1);
	dm->addNode(n);
	fs->flush();

	dm->deleteNode(n);
	EXPECT_NO_THROW(fs->flush());
	fs->close();
}

// ============================================================================
// clearCache
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, ClearCache) {
	auto fs = openLegacyStorage();
	EXPECT_NO_THROW(fs->clearCache());
	fs->close();
}

// ============================================================================
// verifyIntegrity
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, VerifyIntegrity) {
	auto fs = openLegacyStorage();
	auto result = fs->verifyIntegrity();
	EXPECT_TRUE(result.allPassed);
	fs->close();
}

// ============================================================================
// registerEventListener with valid and expired listeners
// ============================================================================

class TestStorageListener : public graph::storage::IStorageEventListener {
public:
	int flushCount = 0;
	void onStorageFlush() override { flushCount++; }
};

TEST_F(FileStorageStateTransitionsTest, RegisterEventListener_FlushNotifies) {
	auto fs = openLegacyStorage();

	auto listener = std::make_shared<TestStorageListener>();
	fs->registerEventListener(listener);

	auto dm = fs->getDataManager();
	graph::Node n(0, 1);
	dm->addNode(n);

	fs->flush();
	EXPECT_GE(listener->flushCount, 1);
	fs->close();
}

TEST_F(FileStorageStateTransitionsTest, RegisterEventListener_ExpiredListener) {
	auto fs = openLegacyStorage();

	{
		auto listener = std::make_shared<TestStorageListener>();
		fs->registerEventListener(listener);
		// listener goes out of scope here
	}

	auto dm = fs->getDataManager();
	graph::Node n(0, 1);
	dm->addNode(n);

	// Flush should clean up expired listener
	EXPECT_NO_THROW(fs->flush());
	fs->close();
}

// ============================================================================
// verifyBitmapConsistency
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, VerifyBitmapConsistency) {
	auto fs = openLegacyStorage();

	auto dm = fs->getDataManager();
	graph::Node n(0, 1);
	dm->addNode(n);
	fs->flush();

	// Get a segment offset to test bitmap consistency
	auto segTracker = fs->getSegmentTracker();
	auto segments = segTracker->getSegmentsByType(graph::Node::typeId);
	if (!segments.empty()) {
		bool consistent = fs->verifyBitmapConsistency(segments[0].file_offset);
		EXPECT_TRUE(consistent);
	}
	fs->close();
}

// ============================================================================
// close() when not open (no-op)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Close_NotOpen) {
	auto fs = std::make_shared<graph::storage::FileStorage>(testFilePath.string(), 16);
	// close() when not open should be a no-op
	EXPECT_NO_THROW(fs->close());
}

// ============================================================================
// flush() concurrent: flushInProgress already true (line 391 second branch)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Flush_Concurrent) {
	auto fs = openLegacyStorage();

	auto dm = fs->getDataManager();

	// Add data so flush has something to do
	for (int i = 0; i < 50; ++i) {
		graph::Node n(0, 1);
		dm->addNode(n);
	}

	// Launch multiple concurrent flushes to trigger lock contention
	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i) {
		threads.emplace_back([&fs]() {
			fs->flush();
		});
	}
	for (auto &t : threads) {
		t.join();
	}

	fs->close();
}

// ============================================================================
// save() with snapshot empty (line 274) - save after flush cleared dirty data
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Save_SnapshotEmpty) {
	auto fs = openLegacyStorage();

	auto dm = fs->getDataManager();
	graph::Node n(0, 1);
	dm->addNode(n);

	// First save processes dirty data
	fs->save();

	// Second save: no dirty data but hasUnsavedChanges might still be true
	// depending on implementation. If not, this is just a no-op coverage.
	EXPECT_NO_THROW(fs->save());
	fs->close();
}

// ============================================================================
// flush() with delete + compaction enabled (exercises shouldCompact true path)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Flush_DeleteWithCompaction) {
	auto fs = openLegacyStorage();

	fs->setCompactionEnabled(true);

	auto dm = fs->getDataManager();

	// Add and flush nodes to persist them
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 20; ++i) {
		graph::Node n(0, 1);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fs->flush();

	// Delete nodes to trigger compaction path
	for (auto &n : nodes) {
		dm->deleteNode(n);
	}
	EXPECT_NO_THROW(fs->flush());
	fs->close();
}

// ============================================================================
// flush() with compaction enabled but below threshold (shouldCompact() false)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Flush_DeleteBelowCompactionThreshold) {
	auto fs = openLegacyStorage();

	fs->setCompactionEnabled(true);

	auto dm = fs->getDataManager();

	// Add many nodes and flush to persist
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 20; ++i) {
		graph::Node n(0, 1);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fs->flush();

	// Delete only 1 node out of 20 (5% fragmentation, well below 30% threshold)
	dm->deleteNode(nodes[0]);

	// flush() enters compaction block (deleteOperationPerformed=true,
	// compactionEnabled=true) but shouldCompact() returns false.
	EXPECT_NO_THROW(fs->flush());
	fs->close();
}

// ============================================================================
// save() with empty snapshot (snapshot.isEmpty() == true)
// ============================================================================

TEST_F(FileStorageStateTransitionsTest, Save_EmptySnapshot) {
	auto fs = openLegacyStorage();

	auto dm = fs->getDataManager();

	// Add a node and flush to clear dirty state
	graph::Node n(0, 1);
	dm->addNode(n);
	fs->flush();

	// Now save() with no new dirty data — prepareFlushSnapshot returns empty
	EXPECT_NO_THROW(fs->save());
	fs->close();
}
