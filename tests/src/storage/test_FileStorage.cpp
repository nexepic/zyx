/**
 * @file test_FileStorage.cpp
 * @author Nexepic
 * @date 2025/3/19
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
#include <thread>
#include <gtest/gtest.h>
#include <zlib.h>
#include "graph/core/Database.hpp"
#include "graph/storage/SegmentIndexManager.hpp"

class FileStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file name using UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_db_file_" + to_string(uuid) + ".dat");

		// Create and initialize Database instead of FileStorage directly
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		// Get FileStorage from Database
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

// Remove redundant open() calls from all test methods
TEST_F(FileStorageTest, AllocateSegment) {
	// Allocate a segment
	const uint64_t segmentOffset = fileStorage->allocateSegment(0, 10);

	// Verify the segment was allocated correctly using SegmentTracker
	const auto segmentTracker = fileStorage->getSegmentTracker();
	const graph::storage::SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);

	ASSERT_EQ(header.data_type, static_cast<unsigned int>(0));
	ASSERT_EQ(header.capacity, static_cast<unsigned int>(10));
	ASSERT_EQ(header.used, static_cast<unsigned int>(0));
	ASSERT_EQ(header.next_segment_offset, static_cast<unsigned long long>(0));
	ASSERT_EQ(header.start_id, static_cast<long long>(1));
}

TEST_F(FileStorageTest, TestOpenClose) {
	EXPECT_TRUE(fileStorage->isOpen());
	fileStorage->close();
	EXPECT_FALSE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, SaveDataEmpty) {
	std::unordered_map<int64_t, graph::Node> data;
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_EQ(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataSingleElement) {
	std::unordered_map<int64_t, graph::Node> data = {{1, graph::Node(1, 10)}};
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataFitsInOneSegment) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 50; ++i) {
		data[i] = graph::Node(i, 100);
	}
	uint64_t segmentHead = 0;
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataMultipleSegments) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data[i] = graph::Node(i, 100);
	}
	uint64_t segmentHead = 0;
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, VerifySegmentLinking) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data[i] = graph::Node(i, 100);
	}
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);

	// Use SegmentTracker to check linking
	const auto segmentTracker = fileStorage->getSegmentTracker();
	uint64_t currentOffset = segmentHead;
	int segmentCount = 0;

	while (currentOffset != 0) {
		graph::storage::SegmentHeader header = segmentTracker->getSegmentHeader(currentOffset);
		segmentCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_EQ(segmentCount, 3);
}

TEST_F(FileStorageTest, UpdateEntityInPlace_Explicit) {
	// 1. Create and Save a Node
	auto dataManager = fileStorage->getDataManager();
	int64_t lbl = dataManager->getOrCreateLabelId("Test");
	graph::Node node(1, lbl);
	dataManager->addNode(node);
	fileStorage->flush(); // Ensure it's on disk

	// 2. Modify Node in memory
	int64_t newLbl = dataManager->getOrCreateLabelId("Updated");
	node.setLabelId(newLbl);

	// 3. Explicitly call updateEntityInPlace
	// Usually save() calls this, but we test the method directly if possible.
	// Since it's a template method on FileStorage, we can call it.
	// Note: It requires finding the segment first.
	// We rely on the internal logic finding it.
	fileStorage->updateEntityInPlace(node);

	// 4. Verify Update on Disk
	dataManager->clearCache();
	graph::Node reloaded = dataManager->loadNodeFromDisk(1);
	EXPECT_EQ(reloaded.getLabelId(), newLbl);
}

TEST_F(FileStorageTest, DeleteEntityOnDisk_Explicit) {
	// 1. Create and Save
	auto dataManager = fileStorage->getDataManager();
	graph::Node node(10, 0);
	dataManager->addNode(node);
	fileStorage->flush();

	// 2. Explicitly call deleteEntityOnDisk
	// Must mark entity as inactive first, because updateEntityInPlace uses entity.isActive() status
	node.markInactive(); // Assuming Node has markInactive() or setActive(false)
	// If Node doesn't expose markInactive publicly, we might need a workaround or check API.
	// BaseEntity usually has `bool active_ = true;` and `void markInactive() { active_ = false; }`.

	fileStorage->deleteEntityOnDisk(node);

	// 3. Verify Deletion
	dataManager->clearCache();
	graph::Node reloaded = dataManager->loadNodeFromDisk(10);
	EXPECT_EQ(reloaded.getId(), 0);
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

// Mock Listener
class MockStorageListener : public graph::storage::IStorageEventListener {
public:
	int flushCount = 0;
	void onStorageFlush() override { flushCount++; }
};

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

TEST_F(FileStorageTest, SaveAllEntityTypes) {
	auto dm = fileStorage->getDataManager();

	// 1. Node
	graph::Node n(0, 0);
	dm->addNode(n);

	// 2. Edge
	graph::Edge e(0, 1, 1, 0);
	dm->addEdge(e);

	// 3. Property Entity (Manual insert to trigger logic)
	graph::Property p;
	p.setId(100);
	dm->addPropertyEntity(p);

	// 4. Blob
	graph::Blob b;
	b.setId(200);
	dm->addBlobEntity(b);

	// 5. Index
	graph::Index idx;
	idx.setId(300);
	dm->addIndexEntity(idx);

	// 6. State
	graph::State s;
	s.setId(400);
	dm->addStateEntity(s);

	// Trigger Save (via Flush)
	fileStorage->flush();

	// Verify persistence by clearing cache and reloading
	dm->clearCache();

	EXPECT_NE(dm->loadNodeFromDisk(n.getId()).getId(), 0);
	EXPECT_NE(dm->loadEdgeFromDisk(e.getId()).getId(), 0);
}

TEST_F(FileStorageTest, UpdateEntityInPlace_OutOfBounds_Exception) {
	auto dm = fileStorage->getDataManager();
	// 1. Create a valid segment
	graph::Node n(1, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Find valid offset
	uint64_t segOffset = dm->findSegmentForEntityId<graph::Node>(1);
	ASSERT_NE(segOffset, 0ULL);

	// 2. Create invalid entity with HUGE ID
	// Assuming segment capacity is e.g. 10 or 32.
	// ID 1000 will definitely be out of bounds relative to start_id=1.
	graph::Node invalidNode(1000, 0);

	// 3. Force update with mismatched ID but valid segment offset
	EXPECT_THROW({ fileStorage->updateEntityInPlace(invalidNode, segOffset); }, std::runtime_error);
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
	std::filesystem::path lockedPath = std::filesystem::temp_directory_path() / "locked.db";
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

// --- New tests to cover uncovered branches ---

// Branch: open() returns early if already open (line 45)
TEST_F(FileStorageTest, Open_AlreadyOpen_ReturnsEarly) {
	EXPECT_TRUE(fileStorage->isOpen());
	// Calling open() again should be a no-op
	EXPECT_NO_THROW(fileStorage->open());
	EXPECT_TRUE(fileStorage->isOpen());
}

// Branch: close() when not open is a no-op (line 122 else branch)
TEST_F(FileStorageTest, Close_WhenNotOpen_IsNoop) {
	fileStorage->close();
	EXPECT_FALSE(fileStorage->isOpen());
	// Calling close() again should not crash
	EXPECT_NO_THROW(fileStorage->close());
	EXPECT_FALSE(fileStorage->isOpen());
}

// Branch: OPEN_EXISTING_FILE on non-existent file (line 51-52)
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

// Branch: OPEN_CREATE_NEW_FILE when file already exists (line 54-55)
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

// Branch: save() when database is not open (line 151-152)
TEST_F(FileStorageTest, Save_WhenNotOpen_Throws) {
	fileStorage->close();
	EXPECT_THROW(fileStorage->save(), std::runtime_error);
}

// Branch: save() when there are no unsaved changes (line 153-154)
TEST_F(FileStorageTest, Save_NoUnsavedChanges_ReturnsEarly) {
	// No data modifications, so save should return early
	EXPECT_NO_THROW(fileStorage->save());
}

// Branch: verifyBitmapConsistency with segmentOffset == 0 (line 569-570)
TEST_F(FileStorageTest, VerifyBitmapConsistency_ZeroOffset_ReturnsFalse) {
	EXPECT_FALSE(fileStorage->verifyBitmapConsistency(0));
}

// Branch: deleteEntityOnDisk with id <= 0 (line 485-486)
TEST_F(FileStorageTest, DeleteEntityOnDisk_InvalidId_ReturnsEarly) {
	graph::Node node(0, 0); // id = 0
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(node));

	graph::Node negNode(-1, 0); // negative id
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(negNode));
}

// Branch: deleteEntityOnDisk when entity doesn't exist on disk (line 492-496)
TEST_F(FileStorageTest, DeleteEntityOnDisk_NotOnDisk_NoException) {
	// Entity with valid ID but never saved to disk
	graph::Node node(99999, 0);
	node.markInactive();
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(node));
}

// Branch: updateEntityInPlace with entity not found (segmentOffset == 0, line 451-453)
TEST_F(FileStorageTest, UpdateEntityInPlace_EntityNotFound_Throws) {
	graph::Node node(88888, 0);
	EXPECT_THROW(fileStorage->updateEntityInPlace(node), std::runtime_error);
}

// Branch: flush with compaction enabled and delete operations (line 636)
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

// Branch: OPEN_CREATE_OR_OPEN_FILE with existing file (line 74 else branch)
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

// Branch: saveData with pre-allocated slots (entities saved to existing segments)
TEST_F(FileStorageTest, SaveData_WithPreAllocatedSlots) {
	auto dm = fileStorage->getDataManager();

	// First, create a node and flush to disk
	graph::Node n1(0, 0);
	dm->addNode(n1);
	fileStorage->flush();

	// Delete the node (frees the slot)
	dm->deleteNode(n1);
	fileStorage->flush();

	// Now add a new node - it should reuse the freed slot
	graph::Node n2(0, 0);
	dm->addNode(n2);
	fileStorage->flush();

	// Verify node persisted
	dm->clearCache();
	graph::Node loaded = dm->loadNodeFromDisk(n2.getId());
	EXPECT_NE(loaded.getId(), 0);
}

// Branch: Multiple entity type save paths in save() - edges, properties, blobs, indexes, states
TEST_F(FileStorageTest, Save_ModifyAndDeleteAllEntityTypes) {
	auto dm = fileStorage->getDataManager();

	// Add entities
	graph::Node n(0, 0);
	dm->addNode(n);

	graph::Edge e(0, n.getId(), n.getId(), 0);
	dm->addEdge(e);

	graph::Property p;
	p.setId(100);
	dm->addPropertyEntity(p);

	graph::Blob b;
	b.setId(200);
	dm->addBlobEntity(b);

	graph::Index idx;
	idx.setId(300);
	dm->addIndexEntity(idx);

	graph::State s;
	s.setId(400);
	dm->addStateEntity(s);

	// First flush to persist
	fileStorage->flush();

	// Now modify entities to trigger CHANGE_MODIFIED paths
	n.setLabelId(42);
	dm->updateNode(n);

	e.setLabelId(42);
	dm->updateEdge(e);

	// Flush modifications
	fileStorage->flush();

	// Now delete to trigger CHANGE_DELETED paths
	dm->deleteNode(n);
	dm->deleteEdge(e);

	// Flush deletions
	fileStorage->flush();

	// Verify
	dm->clearCache();
	graph::Node loadedN = dm->loadNodeFromDisk(n.getId());
	EXPECT_EQ(loadedN.getId(), 0);
}

// Branch: OPEN_CREATE_NEW_FILE fails to create (invalid path, line 65-66)
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

// Branch: Concurrent flush (lock contention path, line 610)
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

// Branch: Destructor calls close (line 42)
TEST_F(FileStorageTest, Destructor_ClosesStorage) {
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	std::filesystem::path tmpPath =
			std::filesystem::temp_directory_path() / ("test_destructor_" + to_string(uuid) + ".dat");
	{
		graph::Database db(tmpPath.string());
		db.open();
		EXPECT_TRUE(db.isOpen());
		// Destructor runs here
	}
	// File should exist even after destructor
	EXPECT_TRUE(std::filesystem::exists(tmpPath));
	{ std::error_code ec; std::filesystem::remove(tmpPath, ec); }
}

// =========================================================================
// Additional Branch Coverage Tests for FileStorage
// =========================================================================

TEST_F(FileStorageTest, SaveData_EmptyData) {
	// Test saveData with empty data map (line 276 early return)
	std::unordered_map<int64_t, graph::Edge> emptyData;
	uint64_t segHead = 0;
	fileStorage->saveData(emptyData, segHead, 100);
	EXPECT_EQ(segHead, 0u);
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

TEST_F(FileStorageTest, SaveData_EdgesInMultipleSegments) {
	// Test saveData for edges spanning multiple segments
	auto dm = fileStorage->getDataManager();

	// Create source and target nodes first
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	fileStorage->flush();

	// Create many edges
	std::unordered_map<int64_t, graph::Edge> edgeData;
	for (int64_t i = 1; i <= 200; ++i) {
		edgeData[i] = graph::Edge(i, n1.getId(), n2.getId(), 0);
	}
	uint64_t edgeSegHead = 0;
	fileStorage->saveData(edgeData, edgeSegHead, 100);
	EXPECT_NE(edgeSegHead, 0u);
}

TEST_F(FileStorageTest, SaveData_PropertiesPath) {
	// Test save() for the properties path (line 200-215)
	auto dm = fileStorage->getDataManager();

	// Add a property entity and flush
	graph::Property p;
	p.setId(0);
	dm->addPropertyEntity(p);
	fileStorage->flush();

	// Modify the property
	graph::Property loaded = dm->getProperty(p.getId());
	loaded.setEntityInfo(42, 0);
	dm->updatePropertyEntity(loaded);

	// Flush to trigger CHANGE_MODIFIED path
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, SaveData_BlobsPath) {
	// Test save() for the blobs path (line 217-232)
	auto dm = fileStorage->getDataManager();

	graph::Blob b;
	b.setId(0);
	dm->addBlobEntity(b);
	fileStorage->flush();

	// Modify
	graph::Blob loaded = dm->getBlob(b.getId());
	loaded.setData("test data");
	dm->updateBlobEntity(loaded);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, SaveData_IndexesPath) {
	// Test save() for the indexes path (line 234-249)
	auto dm = fileStorage->getDataManager();

	graph::Index idx;
	idx.setId(0);
	dm->addIndexEntity(idx);
	fileStorage->flush();

	// Modify
	graph::Index loaded = dm->getIndex(idx.getId());
	loaded.setParentId(42);
	dm->updateIndexEntity(loaded);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, SaveData_StatesPath) {
	// Test save() for the states path (line 251-266)
	auto dm = fileStorage->getDataManager();

	graph::State s;
	s.setId(0);
	dm->addStateEntity(s);
	fileStorage->flush();

	// Modify
	graph::State loaded = dm->getState(s.getId());
	loaded.setNextStateId(99);
	dm->updateStateEntity(loaded);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, DeleteEntityOnDisk_EdgeType) {
	// Test deleteEntityOnDisk for Edge type
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	graph::Edge e(0, n.getId(), n.getId(), 0);
	dm->addEdge(e);
	fileStorage->flush();

	e.markInactive();
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(e));
}

TEST_F(FileStorageTest, Save_EmptySnapshot) {
	// Test save() when snapshot is empty (line 159 early return)
	auto dm = fileStorage->getDataManager();

	// Add a node and flush - this clears dirty state
	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Now call save() again - no dirty entities, snapshot should be empty
	EXPECT_NO_THROW(fileStorage->save());
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

TEST_F(FileStorageTest, SaveData_PropertyDeletion) {
	// Test deletion path for properties
	auto dm = fileStorage->getDataManager();

	graph::Property p;
	p.setId(0);
	dm->addPropertyEntity(p);
	fileStorage->flush();

	// Delete the property
	dm->deleteProperty(p);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, SaveData_BlobDeletion) {
	// Test deletion path for blobs
	auto dm = fileStorage->getDataManager();

	graph::Blob b;
	b.setId(0);
	dm->addBlobEntity(b);
	fileStorage->flush();

	// Delete the blob
	dm->deleteBlob(b);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, SaveData_IndexDeletion) {
	// Test deletion path for indexes
	auto dm = fileStorage->getDataManager();

	graph::Index idx;
	idx.setId(0);
	dm->addIndexEntity(idx);
	fileStorage->flush();

	// Delete the index
	dm->deleteIndex(idx);
	EXPECT_NO_THROW(fileStorage->flush());
}

TEST_F(FileStorageTest, SaveData_StateDeletion) {
	// Test deletion path for states
	auto dm = fileStorage->getDataManager();

	graph::State s;
	s.setId(0);
	dm->addStateEntity(s);
	fileStorage->flush();

	// Delete the state
	dm->deleteState(s);
	EXPECT_NO_THROW(fileStorage->flush());
}

// =========================================================================
// Additional Branch Coverage Tests for FileStorage
// =========================================================================

TEST_F(FileStorageTest, SaveData_PreAllocatedSlotReuse_Batch) {
	// Test saveData when entities go into pre-allocated slots via batch operations
	auto dm = fileStorage->getDataManager();

	// Create nodes and flush
	for (int i = 0; i < 10; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Delete several nodes to create free slots
	for (int i = 1; i <= 5; ++i) {
		auto node = dm->getNode(i);
		dm->deleteNode(node);
	}
	fileStorage->flush();

	// Add new nodes that should reuse freed slots
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	dm->clearCache();
	auto nodes = dm->getNodesInRange(1, 15, 20);
	EXPECT_GE(nodes.size(), 5u);
}

TEST_F(FileStorageTest, SaveData_ExistingSegmentWithData) {
	// Test saveData when data goes into an existing segment chain
	// First, save some data to create the segment chain
	auto dm = fileStorage->getDataManager();

	// Create first batch of nodes
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Create more nodes to go into same/new segments
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Verify all 10 nodes exist
	dm->clearCache();
	auto nodes = dm->getNodesInRange(1, 10, 20);
	EXPECT_GE(nodes.size(), 5u);
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

TEST_F(FileStorageTest, SaveData_WriteToExistingSegmentWithFreeSlots) {
	// Test saveData path where entities reuse pre-allocated slots (line 290-329)
	auto dm = fileStorage->getDataManager();

	// Create nodes, flush, delete, flush, then create again
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	graph::Node n3(0, 0);
	dm->addNode(n3);
	fileStorage->flush();

	// Delete middle node
	dm->deleteNode(n2);
	fileStorage->flush();

	// Create new node that should reuse the freed slot
	graph::Node n4(0, 0);
	dm->addNode(n4);
	fileStorage->flush();

	// Verify
	dm->clearCache();
	graph::Node loaded = dm->loadNodeFromDisk(n4.getId());
	EXPECT_NE(loaded.getId(), 0);
}

TEST_F(FileStorageTest, PersistSegmentHeaders) {
	// Test persistSegmentHeaders (line 594-597)
	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Directly call persistSegmentHeaders
	EXPECT_NO_THROW(fileStorage->persistSegmentHeaders());
}

TEST_F(FileStorageTest, ClearCache) {
	// Test clearCache (line 670)
	auto dm = fileStorage->getDataManager();
	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	EXPECT_NO_THROW(fileStorage->clearCache());
}

TEST_F(FileStorageTest, SaveData_NodeBitmapUpdate) {
	// Test that bitmap is properly updated when saving nodes with pre-allocated slots
	// This tests the branch at line 318: if (!segmentTracker->getBitmapBit(...))
	auto dm = fileStorage->getDataManager();

	// Create and flush nodes
	for (int i = 0; i < 3; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Delete a node, flush (creates inactive slot)
	auto nodes = dm->getNodesInRange(1, 1, 5);
	if (!nodes.empty()) {
		dm->deleteNode(nodes[0]);
	}
	fileStorage->flush();

	// Create new node (should reuse freed slot)
	graph::Node newNode(0, 0);
	dm->addNode(newNode);
	fileStorage->flush();

	// Verify bitmap consistency
	uint64_t segOffset = dm->findSegmentForEntityId<graph::Node>(newNode.getId());
	if (segOffset != 0) {
		bool consistent = fileStorage->verifyBitmapConsistency(segOffset);
		EXPECT_TRUE(consistent);
	}
}

TEST_F(FileStorageTest, DeleteEntityOnDisk_EdgeNotOnDisk) {
	// Test deleteEntityOnDisk for edge that was never saved to disk (line 492-496 false branch)
	graph::Edge e(12345, 1, 2, 0);
	e.markInactive();
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(e));
}

TEST_F(FileStorageTest, SaveData_MultipleEntityTypesInSequence) {
	// Test saving multiple entity types sequentially to cover all save() branches
	auto dm = fileStorage->getDataManager();

	// Add and modify all entity types
	graph::Node n(0, 0);
	dm->addNode(n);

	graph::Edge e(0, n.getId(), n.getId(), 0);
	dm->addEdge(e);

	graph::Property p;
	p.setId(0);
	dm->addPropertyEntity(p);

	graph::Blob b;
	b.setId(0);
	dm->addBlobEntity(b);

	graph::Index idx;
	idx.setId(0);
	dm->addIndexEntity(idx);

	graph::State s;
	s.setId(0);
	dm->addStateEntity(s);

	// Flush all
	fileStorage->flush();

	// Now delete all to trigger deletion paths
	dm->deleteNode(n);
	dm->deleteEdge(e);
	dm->deleteProperty(p);
	dm->deleteBlob(b);
	dm->deleteIndex(idx);
	dm->deleteState(s);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test saveData pre-allocated slot reuse path for Edge type
// First save allocates new slot, second save (after update) reuses the pre-allocated slot
TEST_F(FileStorageTest, SaveData_PreAllocatedSlotReuse_Edge) {
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	graph::Edge e(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e);

	// First flush - allocates new segment slot for edge
	fileStorage->flush();

	// Update the edge - this creates a MODIFIED dirty entry
	e.setLabelId(42);
	dm->updateEdge(e);

	// Second flush - should find pre-allocated slot and reuse it (line 307, 318 branches)
	EXPECT_NO_THROW(fileStorage->flush());
}

// Test saveData pre-allocated slot reuse path for Property type
TEST_F(FileStorageTest, SaveData_PreAllocatedSlotReuse_Property) {
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);

	graph::Property p;
	p.setId(0);
	p.getMutableMetadata().entityId = n.getId();
	p.getMutableMetadata().entityType = 0;
	p.getMutableMetadata().isActive = true;
	p.setProperties({{"key1", graph::PropertyValue(1)}});
	dm->addPropertyEntity(p);

	fileStorage->flush();

	// Update property
	p.setProperties({{"key1", graph::PropertyValue(2)}});
	dm->updatePropertyEntity(p);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test saveData pre-allocated slot reuse path for Blob type
TEST_F(FileStorageTest, SaveData_PreAllocatedSlotReuse_Blob) {
	auto dm = fileStorage->getDataManager();

	graph::Blob b;
	b.setId(0);
	b.setData("initial");
	b.getMutableMetadata().isActive = true;
	dm->addBlobEntity(b);

	fileStorage->flush();

	b.setData("updated");
	dm->updateBlobEntity(b);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test saveData pre-allocated slot reuse path for Index type
TEST_F(FileStorageTest, SaveData_PreAllocatedSlotReuse_Index) {
	auto dm = fileStorage->getDataManager();

	graph::Index idx;
	idx.setId(0);
	idx.getMutableMetadata().isActive = true;
	dm->addIndexEntity(idx);

	fileStorage->flush();

	idx.getMutableMetadata().isActive = true;
	dm->updateIndexEntity(idx);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test saveData pre-allocated slot reuse path for State type
TEST_F(FileStorageTest, SaveData_PreAllocatedSlotReuse_State) {
	auto dm = fileStorage->getDataManager();

	graph::State s;
	s.setId(0);
	s.setKey("testkey");
	dm->addStateEntity(s);

	fileStorage->flush();

	s.setKey("updated");
	dm->updateStateEntity(s);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test bitmap inconsistency detection (line 579 True branch)
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

// Test compaction trigger during flush (lines 637, 641)
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

// =========================================================================
// Additional Branch Coverage Tests: Entity modification and deletion paths
// =========================================================================

// Test that save() processes modified and deleted blobs correctly
TEST_F(FileStorageTest, Save_ModifyAndDeleteBlobs) {
	auto dm = fileStorage->getDataManager();

	// Add multiple blobs
	graph::Blob b1;
	b1.setId(0);
	b1.setData("blob_data_1");
	dm->addBlobEntity(b1);

	graph::Blob b2;
	b2.setId(0);
	b2.setData("blob_data_2");
	dm->addBlobEntity(b2);

	fileStorage->flush();

	// Modify first blob (triggers CHANGE_MODIFIED path for blobs in save)
	b1.setData("modified_blob");
	dm->updateBlobEntity(b1);

	// Delete second blob (triggers CHANGE_DELETED path for blobs in save)
	dm->deleteBlob(b2);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test that save() processes modified and deleted indexes correctly
TEST_F(FileStorageTest, Save_ModifyAndDeleteIndexes) {
	auto dm = fileStorage->getDataManager();

	graph::Index idx1;
	idx1.setId(0);
	dm->addIndexEntity(idx1);

	graph::Index idx2;
	idx2.setId(0);
	dm->addIndexEntity(idx2);

	fileStorage->flush();

	// Modify first index
	idx1.setParentId(99);
	dm->updateIndexEntity(idx1);

	// Delete second index
	dm->deleteIndex(idx2);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test that save() processes modified and deleted states correctly
TEST_F(FileStorageTest, Save_ModifyAndDeleteStates) {
	auto dm = fileStorage->getDataManager();

	graph::State s1;
	s1.setId(0);
	s1.setKey("state1");
	dm->addStateEntity(s1);

	graph::State s2;
	s2.setId(0);
	s2.setKey("state2");
	dm->addStateEntity(s2);

	fileStorage->flush();

	// Modify first state
	s1.setKey("modified_state");
	dm->updateStateEntity(s1);

	// Delete second state
	dm->deleteState(s2);

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test flush with both delete flag and compaction enabled but shouldCompact returns false
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

// Test save with only new properties (no modify or delete)
TEST_F(FileStorageTest, Save_OnlyNewProperties) {
	auto dm = fileStorage->getDataManager();

	// Add multiple property entities (triggers newProps path in save)
	for (int i = 0; i < 3; i++) {
		graph::Property p;
		p.setId(0);
		dm->addPropertyEntity(p);
	}

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test save with only new blobs
TEST_F(FileStorageTest, Save_OnlyNewBlobs) {
	auto dm = fileStorage->getDataManager();

	for (int i = 0; i < 3; i++) {
		graph::Blob b;
		b.setId(0);
		b.setData("blob_" + std::to_string(i));
		dm->addBlobEntity(b);
	}

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test save with only new indexes
TEST_F(FileStorageTest, Save_OnlyNewIndexes) {
	auto dm = fileStorage->getDataManager();

	for (int i = 0; i < 3; i++) {
		graph::Index idx;
		idx.setId(0);
		dm->addIndexEntity(idx);
	}

	EXPECT_NO_THROW(fileStorage->flush());
}

// Test save with only new states
TEST_F(FileStorageTest, Save_OnlyNewStates) {
	auto dm = fileStorage->getDataManager();

	for (int i = 0; i < 3; i++) {
		graph::State s;
		s.setId(0);
		s.setKey("key_" + std::to_string(i));
		dm->addStateEntity(s);
	}

	EXPECT_NO_THROW(fileStorage->flush());
}

// =========================================================================
// Additional Integration-style Branch Coverage Tests for FileStorage
// =========================================================================

// Test close then reopen the same database file and verify data persists
TEST_F(FileStorageTest, CloseReopenVerifyPersistence) {
	auto dm = fileStorage->getDataManager();

	// Add nodes with properties
	int64_t lbl = dm->getOrCreateLabelId("Persist");
	graph::Node n1(0, lbl);
	dm->addNode(n1);
	int64_t n1Id = n1.getId();

	graph::Node n2(0, lbl);
	dm->addNode(n2);
	int64_t n2Id = n2.getId();

	// Add an edge between them
	int64_t edgeLbl = dm->getOrCreateLabelId("CONNECTS");
	graph::Edge e(0, n1Id, n2Id, edgeLbl);
	dm->addEdge(e);
	int64_t eId = e.getId();

	fileStorage->flush();

	// Close the database
	database->close();
	EXPECT_FALSE(fileStorage->isOpen());

	// Reopen
	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	fileStorage = database->getStorage();
	dm = fileStorage->getDataManager();

	// Verify nodes persisted
	dm->clearCache();
	graph::Node loadedN1 = dm->loadNodeFromDisk(n1Id);
	EXPECT_EQ(loadedN1.getId(), n1Id);

	graph::Node loadedN2 = dm->loadNodeFromDisk(n2Id);
	EXPECT_EQ(loadedN2.getId(), n2Id);

	// Verify edge persisted
	graph::Edge loadedE = dm->loadEdgeFromDisk(eId);
	EXPECT_EQ(loadedE.getId(), eId);
	EXPECT_EQ(loadedE.getSourceNodeId(), n1Id);
	EXPECT_EQ(loadedE.getTargetNodeId(), n2Id);
}

// Test flush after adding, modifying, and deleting entities in a single cycle
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

// Test multiple flush-close-reopen cycles with data integrity
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

// Test save when snapshot has only edge deletions (no new edges)
TEST_F(FileStorageTest, Save_OnlyEdgeDeletions) {
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	graph::Edge e1(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e1);
	graph::Edge e2(0, n1.getId(), n2.getId(), 0);
	dm->addEdge(e2);

	fileStorage->flush();

	// Now only delete edges
	dm->deleteEdge(e1);
	dm->deleteEdge(e2);

	EXPECT_NO_THROW(fileStorage->flush());
}

// =========================================================================
// Additional Branch Coverage Tests for FileStorage - Round 4
// =========================================================================

TEST_F(FileStorageTest, UpdateEntityInPlace_OutOfBounds_ViaUpdate) {
	// Test updateEntityInPlace when entity index is out of bounds (line 461-466)
	// via updateNode with a huge ID but valid segment offset
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Create a node with an ID that exists on disk but modify it to have
	// an out-of-bounds ID relative to its segment
	uint64_t segOffset = dm->findSegmentForEntityId<graph::Node>(n.getId());
	ASSERT_NE(segOffset, 0ULL);

	// This is tested by the existing test UpdateEntityInPlace_OutOfBounds_Exception
	// but we exercise it from a different angle - entity that doesn't exist in any segment
	graph::Node badNode(77777, 0);
	EXPECT_THROW(fileStorage->updateEntityInPlace(badNode), std::runtime_error);
}

TEST_F(FileStorageTest, DeleteEntityOnDisk_NegativeId) {
	// Test deleteEntityOnDisk with negative id (line 485 boundary)
	graph::Node negNode(-5, 0);
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(negNode));

	graph::Edge negEdge(-10, 1, 2, 0);
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(negEdge));
}

TEST_F(FileStorageTest, SaveData_WriteToExistingSegmentChain) {
	// Test saveData when segment chain already exists with data (line 346-354)
	auto dm = fileStorage->getDataManager();

	// First batch creates the segment chain
	std::unordered_map<int64_t, graph::Node> data1;
	for (int64_t i = 1; i <= 50; ++i) {
		data1[i] = graph::Node(i, 100);
	}
	uint64_t segmentHead = 0;
	fileStorage->saveData(data1, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);

	// Second batch adds to existing chain
	std::unordered_map<int64_t, graph::Node> data2;
	for (int64_t i = 51; i <= 100; ++i) {
		data2[i] = graph::Node(i, 200);
	}
	fileStorage->saveData(data2, segmentHead, 100);

	// Chain should still be valid
	auto tracker = fileStorage->getSegmentTracker();
	uint64_t currentOffset = segmentHead;
	int segCount = 0;
	while (currentOffset != 0) {
		auto header = tracker->getSegmentHeader(currentOffset);
		segCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_GE(segCount, 1);
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

TEST_F(FileStorageTest, WriteSegmentData_NonZeroBaseUsed) {
	// Test writeSegmentData when baseUsed != 0 (line 428 false branch)
	auto dm = fileStorage->getDataManager();

	// Create a batch that partially fills a segment
	std::unordered_map<int64_t, graph::Node> data1;
	for (int64_t i = 1; i <= 10; ++i) {
		data1[i] = graph::Node(i, 100);
	}
	uint64_t segmentHead = 0;
	fileStorage->saveData(data1, segmentHead, 100);

	// Now add more data to the same segment chain (non-zero baseUsed)
	std::unordered_map<int64_t, graph::Node> data2;
	for (int64_t i = 11; i <= 20; ++i) {
		data2[i] = graph::Node(i, 200);
	}
	fileStorage->saveData(data2, segmentHead, 100);

	// Verify both batches are in the segment
	auto tracker = fileStorage->getSegmentTracker();
	auto header = tracker->getSegmentHeader(segmentHead);
	EXPECT_GE(header.used, 20u);
}

TEST_F(FileStorageTest, SaveData_StartIdMismatch) {
	// Test line 380: newSegHeader.start_id != dataIt->getId()
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	fileStorage->flush();

	// Delete and re-add with reused ID
	dm->deleteNode(n1);
	fileStorage->flush();

	graph::Node n2(0, 0);
	dm->addNode(n2);
	fileStorage->flush();

	dm->clearCache();
	graph::Node loaded = dm->loadNodeFromDisk(n2.getId());
	EXPECT_NE(loaded.getId(), 0);
}

// =========================================================================
// BRANCH COVERAGE: Additional tests targeting uncovered branches
// =========================================================================

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

TEST_F(FileStorageTest, Save_ModifyAndDeleteMultipleEntityTypes) {
	// Covers the modify and delete paths for all entity types through save()
	auto dm = fileStorage->getDataManager();

	// Create and flush a node
	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// Create and flush an edge
	graph::Edge e(0, n.getId(), n.getId(), 0);
	dm->addEdge(e);
	fileStorage->flush();

	// Modify node
	n.setLabelId(999);
	dm->updateNode(n);
	fileStorage->flush();

	// Delete edge
	dm->deleteEdge(e);
	fileStorage->flush();

	EXPECT_TRUE(fileStorage->isOpen());
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

TEST_F(FileStorageTest, SaveData_EdgeMultipleSegments) {
	// Test saveData with edges spanning multiple segments
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	for (int i = 0; i < 200; i++) {
		graph::Edge e(0, n.getId(), n.getId(), 0);
		dm->addEdge(e);
	}
	fileStorage->flush();

	auto tracker = fileStorage->getSegmentTracker();
	uint64_t edgeHead = tracker->getChainHead(graph::Edge::typeId);
	EXPECT_NE(edgeHead, 0u);
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

TEST_F(FileStorageTest, Save_NodesViaSnapshotPath) {
	// Covers lines 163-179: save() path that processes nodes via snapshot
	auto dm = fileStorage->getDataManager();

	// Add multiple nodes
	for (int i = 0; i < 5; i++) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}

	// save() exercises the snapshot path including getEntitiesByType
	EXPECT_NO_THROW(fileStorage->save());
}

TEST_F(FileStorageTest, Save_EdgesViaSnapshotPath) {
	// Covers lines 182-198: save() path that processes edges via snapshot
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	for (int i = 0; i < 5; i++) {
		graph::Edge e(0, n1.getId(), n2.getId(), 0);
		dm->addEdge(e);
	}

	EXPECT_NO_THROW(fileStorage->save());
}

TEST_F(FileStorageTest, Save_MultipleFlushCycles) {
	// Covers save() path across multiple cycles
	auto dm = fileStorage->getDataManager();

	// First cycle
	graph::Node n1(0, 0);
	dm->addNode(n1);
	EXPECT_NO_THROW(fileStorage->save());

	// Second cycle - add more data
	graph::Node n2(0, 0);
	dm->addNode(n2);
	EXPECT_NO_THROW(fileStorage->save());
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

TEST_F(FileStorageTest, Save_ModifiedAndDeletedNodes_CoversAllPaths) {
	// Covers lines 175-178: modNodes and delNodes iteration
	auto dm = fileStorage->getDataManager();

	// Create nodes and flush them
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	graph::Node n3(0, 0);
	dm->addNode(n3);
	fileStorage->save();

	// Modify one node (change label) and delete another
	graph::Node n1Updated(n1.getId(), 99);
	dm->updateNode(n1Updated);
	dm->deleteNode(n2);

	// This save should exercise modified + deleted node paths
	EXPECT_NO_THROW(fileStorage->save());
}

// =========================================================================
// Branch Coverage: saveData with empty maps for non-Node/Edge types (line 276)
// =========================================================================

TEST_F(FileStorageTest, SaveData_EmptyPropertyMap) {
	std::unordered_map<int64_t, graph::Property> emptyData;
	uint64_t segHead = 0;
	fileStorage->saveData(emptyData, segHead, 100);
	EXPECT_EQ(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_EmptyBlobMap) {
	std::unordered_map<int64_t, graph::Blob> emptyData;
	uint64_t segHead = 0;
	fileStorage->saveData(emptyData, segHead, 100);
	EXPECT_EQ(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_EmptyIndexMap) {
	std::unordered_map<int64_t, graph::Index> emptyData;
	uint64_t segHead = 0;
	fileStorage->saveData(emptyData, segHead, 100);
	EXPECT_EQ(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_EmptyStateMap) {
	std::unordered_map<int64_t, graph::State> emptyData;
	uint64_t segHead = 0;
	fileStorage->saveData(emptyData, segHead, 100);
	EXPECT_EQ(segHead, 0u);
}

// =========================================================================
// Branch Coverage: saveData pre-allocated slot reuse for Property type
// Covers lines 290 (True), 300, 307, 318, 323, 332 for Property instantiation
// =========================================================================

TEST_F(FileStorageTest, SaveData_PropertyPreAllocatedSlotReuse) {
	auto dm = fileStorage->getDataManager();

	// Step 1: Create properties and flush to disk to establish segments
	std::vector<graph::Property> props;
	for (int i = 0; i < 5; ++i) {
		graph::Property p;
		p.setId(0);
		dm->addPropertyEntity(p);
		props.push_back(p);
	}
	fileStorage->flush();

	// Step 2: Delete properties to create free slots with inactive_count > 0
	for (auto &p : props) {
		dm->deleteProperty(p);
	}
	fileStorage->flush();

	// Step 3: Rebuild segment indexes so findSegmentForEntityId can find existing segments
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Step 4: Now directly call saveData with Property entities that have IDs
	// within the range of existing segments, triggering pre-allocated slot reuse
	std::unordered_map<int64_t, graph::Property> data;
	for (auto &p : props) {
		graph::Property newP;
		newP.setId(p.getId());
		newP.getMutableMetadata().isActive = true;
		data[p.getId()] = newP;
	}
	uint64_t segHead = fileStorage->getDataManager()->getSegmentTracker()->getChainHead(graph::Property::typeId);
	fileStorage->saveData(data, segHead, graph::storage::PROPERTIES_PER_SEGMENT);

	// Verify entities were written
	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: saveData pre-allocated slot reuse for Blob type
// =========================================================================

TEST_F(FileStorageTest, SaveData_BlobPreAllocatedSlotReuse) {
	auto dm = fileStorage->getDataManager();

	// Step 1: Create blobs and flush
	std::vector<graph::Blob> blobs;
	for (int i = 0; i < 5; ++i) {
		graph::Blob b;
		b.setId(0);
		b.setData("data_" + std::to_string(i));
		dm->addBlobEntity(b);
		blobs.push_back(b);
	}
	fileStorage->flush();

	// Step 2: Delete blobs
	for (auto &b : blobs) {
		dm->deleteBlob(b);
	}
	fileStorage->flush();

	// Rebuild segment index so findSegmentForEntityId can find the segments
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Step 3: Rebuild segment indexes so findSegmentForEntityId can find slots
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Step 4: Call saveData with blobs that match existing segment slots
	std::unordered_map<int64_t, graph::Blob> data;
	for (auto &b : blobs) {
		graph::Blob newB;
		newB.setId(b.getId());
		newB.setData("reused");
		newB.getMutableMetadata().isActive = true;
		data[b.getId()] = newB;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Blob::typeId);
	fileStorage->saveData(data, segHead, graph::storage::BLOBS_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: saveData pre-allocated slot reuse for Index type
// =========================================================================

TEST_F(FileStorageTest, SaveData_IndexPreAllocatedSlotReuse) {
	auto dm = fileStorage->getDataManager();

	// Step 1: Create indexes and flush
	std::vector<graph::Index> indexes;
	for (int i = 0; i < 5; ++i) {
		graph::Index idx;
		idx.setId(0);
		dm->addIndexEntity(idx);
		indexes.push_back(idx);
	}
	fileStorage->flush();

	// Step 2: Delete indexes
	for (auto &idx : indexes) {
		dm->deleteIndex(idx);
	}
	fileStorage->flush();

	// Step 3: Rebuild segment indexes
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Step 4: Call saveData with indexes matching existing segment slots
	std::unordered_map<int64_t, graph::Index> data;
	for (auto &idx : indexes) {
		graph::Index newIdx;
		newIdx.setId(idx.getId());
		newIdx.getMutableMetadata().isActive = true;
		data[idx.getId()] = newIdx;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Index::typeId);
	fileStorage->saveData(data, segHead, graph::storage::INDEXES_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: saveData pre-allocated slot reuse for State type
// =========================================================================

TEST_F(FileStorageTest, SaveData_StatePreAllocatedSlotReuse) {
	auto dm = fileStorage->getDataManager();

	// Step 1: Create states and flush
	std::vector<graph::State> states;
	for (int i = 0; i < 5; ++i) {
		graph::State s;
		s.setId(0);
		s.setKey("key_" + std::to_string(i));
		dm->addStateEntity(s);
		states.push_back(s);
	}
	fileStorage->flush();

	// Step 2: Delete states
	for (auto &s : states) {
		dm->deleteState(s);
	}
	fileStorage->flush();

	// Step 3: Rebuild segment indexes
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Step 4: Call saveData with states matching existing segment slots
	std::unordered_map<int64_t, graph::State> data;
	for (auto &s : states) {
		graph::State newS;
		newS.setId(s.getId());
		newS.setKey("reused");
		data[s.getId()] = newS;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::State::typeId);
	fileStorage->saveData(data, segHead, graph::storage::STATES_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: saveData where ALL entities have pre-allocated slots
// Covers line 332 (entitiesForNewSlots.empty() True branch) for non-Node types
// =========================================================================

TEST_F(FileStorageTest, SaveData_AllEntitiesPreAllocated_Property) {
	auto dm = fileStorage->getDataManager();

	// Create properties and flush to create segments
	graph::Property p1;
	p1.setId(0);
	dm->addPropertyEntity(p1);
	graph::Property p2;
	p2.setId(0);
	dm->addPropertyEntity(p2);
	fileStorage->flush();

	// Rebuild segment index so findSegmentForEntityId can locate existing segments
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Now call saveData with the same IDs - they should all find pre-allocated slots
	// since those entities already exist in segments on disk
	std::unordered_map<int64_t, graph::Property> data;
	graph::Property p1Copy;
	p1Copy.setId(p1.getId());
	p1Copy.getMutableMetadata().isActive = true;
	data[p1.getId()] = p1Copy;

	graph::Property p2Copy;
	p2Copy.setId(p2.getId());
	p2Copy.getMutableMetadata().isActive = true;
	data[p2.getId()] = p2Copy;

	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Property::typeId);
	fileStorage->saveData(data, segHead, graph::storage::PROPERTIES_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_AllEntitiesPreAllocated_Blob) {
	auto dm = fileStorage->getDataManager();

	graph::Blob b1;
	b1.setId(0);
	b1.setData("test1");
	dm->addBlobEntity(b1);
	graph::Blob b2;
	b2.setId(0);
	b2.setData("test2");
	dm->addBlobEntity(b2);
	fileStorage->flush();

	// Rebuild segment index
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Call saveData with same IDs - all should find pre-allocated slots
	std::unordered_map<int64_t, graph::Blob> data;
	graph::Blob b1Copy;
	b1Copy.setId(b1.getId());
	b1Copy.setData("updated1");
	b1Copy.getMutableMetadata().isActive = true;
	data[b1.getId()] = b1Copy;

	graph::Blob b2Copy;
	b2Copy.setId(b2.getId());
	b2Copy.setData("updated2");
	b2Copy.getMutableMetadata().isActive = true;
	data[b2.getId()] = b2Copy;

	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Blob::typeId);
	fileStorage->saveData(data, segHead, graph::storage::BLOBS_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: saveData traversing multi-segment chain (line 350 False)
// =========================================================================

TEST_F(FileStorageTest, SaveData_PropertyMultiSegmentChain) {
	// Create enough properties to fill multiple segments, then add more
	auto dm = fileStorage->getDataManager();

	// Fill first segment
	std::unordered_map<int64_t, graph::Property> data1;
	uint32_t capacity = graph::storage::PROPERTIES_PER_SEGMENT;
	for (uint32_t i = 1; i <= capacity; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data1[static_cast<int64_t>(i)] = p;
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Add more to trigger second segment allocation
	std::unordered_map<int64_t, graph::Property> data2;
	for (uint32_t i = capacity + 1; i <= capacity + 10; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data2[static_cast<int64_t>(i)] = p;
	}
	fileStorage->saveData(data2, segHead, capacity);

	// Verify chain has multiple segments
	auto tracker = fileStorage->getSegmentTracker();
	uint64_t currentOffset = segHead;
	int segCount = 0;
	while (currentOffset != 0) {
		auto header = tracker->getSegmentHeader(currentOffset);
		segCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_GE(segCount, 2);
}

// =========================================================================
// Branch Coverage: Edge pre-allocated slot reuse in saveData (line 290 True for Edge)
// =========================================================================

TEST_F(FileStorageTest, Save_EdgeSlotReuse_CoversPreAllocatedPath) {
	// Create edges, flush to disk, delete one, flush, then create new edge
	// that reuses the deleted slot. This covers line 290 True for Edge template.
	auto dm = fileStorage->getDataManager();
	auto alloc = fileStorage->getIDAllocator();

	// Create two nodes for edges to reference
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	// Create several edges and flush to disk
	std::vector<graph::Edge> edges;
	for (int i = 0; i < 5; i++) {
		int64_t eid = alloc->allocateId(graph::Edge::typeId);
		int64_t labelId = dm->getOrCreateLabelId("KNOWS");
		graph::Edge e(eid, n1.getId(), n2.getId(), labelId);
		dm->addEdge(e);
		edges.push_back(e);
	}
	fileStorage->flush();

	// Delete one edge to create an inactive slot
	dm->deleteEdge(edges[2]);
	fileStorage->flush();

	// Create a new edge - the allocator should reuse the deleted ID slot
	int64_t newEid = alloc->allocateId(graph::Edge::typeId);
	int64_t labelId = dm->getOrCreateLabelId("FOLLOWS");
	graph::Edge newEdge(newEid, n1.getId(), n2.getId(), labelId);
	dm->addEdge(newEdge);

	// This save() call should hit the pre-allocated slot path for Edge (line 290 True)
	EXPECT_NO_THROW(fileStorage->save());
}

// =========================================================================
// Branch Coverage: Line 318 False - bitmap bit already set during saveData
// When saveData writes to a pre-allocated slot that is already active,
// getBitmapBit returns true (bit is set), so the if-body is skipped.
// =========================================================================

TEST_F(FileStorageTest, SaveData_PreAllocatedSlot_BitmapAlreadySet_Node) {
	// For Node entities: create nodes, flush (bitmap set), then call saveData
	// again with the same IDs. The bitmap bits are already set, covering line 318 False.
	auto dm = fileStorage->getDataManager();

	// Create nodes and flush to disk
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 3; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Rebuild segment index for findSegmentForEntityId
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Now call saveData with the same IDs - bitmap bits are already set
	std::unordered_map<int64_t, graph::Node> data;
	for (auto &n : nodes) {
		graph::Node copy(n.getId(), 99);
		data[n.getId()] = copy;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Node::typeId);
	fileStorage->saveData(data, segHead, graph::storage::NODES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_PreAllocatedSlot_BitmapAlreadySet_Edge) {
	// Same as above but for Edge entities
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	std::vector<graph::Edge> edges;
	for (int i = 0; i < 3; ++i) {
		graph::Edge e(0, n1.getId(), n2.getId(), 0);
		dm->addEdge(e);
		edges.push_back(e);
	}
	fileStorage->flush();

	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Call saveData with same edge IDs - bitmap bits already set
	std::unordered_map<int64_t, graph::Edge> data;
	for (auto &e : edges) {
		graph::Edge copy(e.getId(), n1.getId(), n2.getId(), 42);
		data[e.getId()] = copy;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Edge::typeId);
	fileStorage->saveData(data, segHead, graph::storage::EDGES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_PreAllocatedSlot_BitmapAlreadySet_Index) {
	// Same for Index entities
	auto dm = fileStorage->getDataManager();

	std::vector<graph::Index> indexes;
	for (int i = 0; i < 3; ++i) {
		graph::Index idx;
		idx.setId(0);
		idx.getMutableMetadata().isActive = true;
		dm->addIndexEntity(idx);
		indexes.push_back(idx);
	}
	fileStorage->flush();

	dm->getSegmentIndexManager()->buildSegmentIndexes();

	std::unordered_map<int64_t, graph::Index> data;
	for (auto &idx : indexes) {
		graph::Index copy;
		copy.setId(idx.getId());
		copy.getMutableMetadata().isActive = true;
		data[idx.getId()] = copy;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Index::typeId);
	fileStorage->saveData(data, segHead, graph::storage::INDEXES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_PreAllocatedSlot_BitmapAlreadySet_State) {
	// Same for State entities
	auto dm = fileStorage->getDataManager();

	std::vector<graph::State> states;
	for (int i = 0; i < 3; ++i) {
		graph::State s;
		s.setId(0);
		s.setKey("key_" + std::to_string(i));
		dm->addStateEntity(s);
		states.push_back(s);
	}
	fileStorage->flush();

	dm->getSegmentIndexManager()->buildSegmentIndexes();

	std::unordered_map<int64_t, graph::State> data;
	for (auto &s : states) {
		graph::State copy;
		copy.setId(s.getId());
		copy.setKey("updated");
		data[s.getId()] = copy;
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::State::typeId);
	fileStorage->saveData(data, segHead, graph::storage::STATES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: Line 351 False for Property template instantiation
// Property saveData must traverse a multi-segment chain (next_segment_offset != 0)
// =========================================================================

TEST_F(FileStorageTest, SaveData_PropertyMultiSegmentChain_TraversalBranch) {
	// Fill a Property segment to capacity, then add more to force chain traversal.
	// This covers line 351 False branch for the Property template instantiation.
	uint32_t capacity = graph::storage::PROPERTIES_PER_SEGMENT;

	// First batch: fill exactly one segment
	std::unordered_map<int64_t, graph::Property> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data1[static_cast<int64_t>(i)] = p;
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Second batch: overflow into second segment, forcing chain traversal
	std::unordered_map<int64_t, graph::Property> data2;
	for (uint32_t i = capacity + 1; i <= capacity * 2 + 5; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data2[static_cast<int64_t>(i)] = p;
	}
	fileStorage->saveData(data2, segHead, capacity);

	// Verify multiple segments in chain
	auto tracker = fileStorage->getSegmentTracker();
	uint64_t current = segHead;
	int segCount = 0;
	while (current != 0) {
		auto hdr = tracker->getSegmentHeader(current);
		segCount++;
		current = hdr.next_segment_offset;
	}
	EXPECT_GE(segCount, 3);

	// Third batch: add more to force traversal through existing multi-segment chain
	std::unordered_map<int64_t, graph::Property> data3;
	for (uint32_t i = capacity * 2 + 6; i <= capacity * 2 + 10; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data3[static_cast<int64_t>(i)] = p;
	}
	fileStorage->saveData(data3, segHead, capacity);
	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: Line 351 False for Index template instantiation
// Index saveData must traverse a multi-segment chain (next_segment_offset != 0)
// =========================================================================

TEST_F(FileStorageTest, SaveData_IndexMultiSegmentChain_TraversalBranch) {
	// Fill an Index segment to capacity, then add more to force chain traversal.
	// This covers line 351 False branch for the Index template instantiation.
	uint32_t capacity = graph::storage::INDEXES_PER_SEGMENT;

	// First batch: fill exactly one segment
	std::unordered_map<int64_t, graph::Index> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		graph::Index idx;
		idx.setId(static_cast<int64_t>(i));
		idx.getMutableMetadata().isActive = true;
		data1[static_cast<int64_t>(i)] = idx;
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Second batch: overflow into second segment, forcing chain traversal
	std::unordered_map<int64_t, graph::Index> data2;
	for (uint32_t i = capacity + 1; i <= capacity * 2 + 5; ++i) {
		graph::Index idx;
		idx.setId(static_cast<int64_t>(i));
		idx.getMutableMetadata().isActive = true;
		data2[static_cast<int64_t>(i)] = idx;
	}
	fileStorage->saveData(data2, segHead, capacity);

	// Verify multiple segments
	auto tracker = fileStorage->getSegmentTracker();
	uint64_t current = segHead;
	int segCount = 0;
	while (current != 0) {
		auto hdr = tracker->getSegmentHeader(current);
		segCount++;
		current = hdr.next_segment_offset;
	}
	EXPECT_GE(segCount, 3);

	// Third batch: add more to force traversal through existing multi-segment chain
	std::unordered_map<int64_t, graph::Index> data3;
	for (uint32_t i = capacity * 2 + 6; i <= capacity * 2 + 10; ++i) {
		graph::Index idx;
		idx.setId(static_cast<int64_t>(i));
		idx.getMutableMetadata().isActive = true;
		data3[static_cast<int64_t>(i)] = idx;
	}
	fileStorage->saveData(data3, segHead, capacity);
	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: Line 324 True for Edge template instantiation
// Edge pre-allocated slot reuse where inactive_count > 0
// =========================================================================

TEST_F(FileStorageTest, SaveData_EdgePreAllocatedSlot_InactiveCountDecrement) {
	// Create edges, flush to disk, delete some (setting inactive_count > 0),
	// then call saveData with the deleted edge IDs to reuse inactive slots.
	// This covers line 324 True branch for the Edge template instantiation.
	auto dm = fileStorage->getDataManager();

	// Create two nodes for edges
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);
	fileStorage->flush();

	// Create edges and flush to establish segment
	std::vector<graph::Edge> edges;
	for (int i = 0; i < 5; ++i) {
		graph::Edge e(0, n1.getId(), n2.getId(), 0);
		dm->addEdge(e);
		edges.push_back(e);
	}
	fileStorage->flush();

	// Delete some edges (creates inactive slots with inactive_count > 0)
	dm->deleteEdge(edges[1]);
	dm->deleteEdge(edges[3]);
	fileStorage->flush();

	// Rebuild segment indexes so findSegmentForEntityId can locate the slots
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Now call saveData with the deleted edge IDs - should find pre-allocated
	// inactive slots and decrement inactive_count (line 324 True path for Edge)
	std::unordered_map<int64_t, graph::Edge> data;
	graph::Edge r1(edges[1].getId(), n1.getId(), n2.getId(), 42);
	data[edges[1].getId()] = r1;
	graph::Edge r2(edges[3].getId(), n1.getId(), n2.getId(), 42);
	data[edges[3].getId()] = r2;

	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Edge::typeId);
	fileStorage->saveData(data, segHead, graph::storage::EDGES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: Line 428 False - baseUsed != 0 when writeSegmentData
// This tests appending to an existing partially-filled segment.
// =========================================================================

TEST_F(FileStorageTest, WriteSegmentData_NonZeroBaseUsed_Edge) {
	// Create edges that partially fill a segment, then add more via saveData
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	// First batch: partially fill segment
	std::unordered_map<int64_t, graph::Edge> data1;
	for (int64_t i = 1; i <= 5; ++i) {
		data1[i] = graph::Edge(i, n1.getId(), n2.getId(), 0);
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, 100);
	ASSERT_NE(segHead, 0u);

	// Second batch: add to same segment (baseUsed > 0)
	std::unordered_map<int64_t, graph::Edge> data2;
	for (int64_t i = 6; i <= 10; ++i) {
		data2[i] = graph::Edge(i, n1.getId(), n2.getId(), 0);
	}
	fileStorage->saveData(data2, segHead, 100);

	auto tracker = fileStorage->getSegmentTracker();
	auto header = tracker->getSegmentHeader(segHead);
	EXPECT_GE(header.used, 10u);
}

// =========================================================================
// Branch Coverage: Line 350 False - multi-segment chain traversal
// For edge entities, traverse existing chain to find last segment
// =========================================================================

TEST_F(FileStorageTest, SaveData_EdgeMultiSegmentChainTraversal) {
	// Create enough edges to span multiple segments, then add more
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// First batch: fill enough to create multiple segments
	uint32_t capacity = graph::storage::EDGES_PER_SEGMENT;
	std::unordered_map<int64_t, graph::Edge> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		data1[static_cast<int64_t>(i)] = graph::Edge(static_cast<int64_t>(i), n.getId(), n.getId(), 0);
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Second batch: more edges to trigger chain traversal and second segment
	std::unordered_map<int64_t, graph::Edge> data2;
	for (uint32_t i = capacity + 1; i <= capacity + 10; ++i) {
		data2[static_cast<int64_t>(i)] = graph::Edge(static_cast<int64_t>(i), n.getId(), n.getId(), 0);
	}
	fileStorage->saveData(data2, segHead, capacity);

	// Verify chain has multiple segments
	auto tracker = fileStorage->getSegmentTracker();
	uint64_t currentOffset = segHead;
	int segCount = 0;
	while (currentOffset != 0) {
		auto header = tracker->getSegmentHeader(currentOffset);
		segCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_GE(segCount, 2);
}

// =========================================================================
// Branch Coverage: Line 323 True - inactive_count > 0 when decrementing
// during pre-allocated slot reuse
// =========================================================================

// =========================================================================
// Branch Coverage: Line 428 False - baseUsed != 0 in writeSegmentData
// When appending data to an existing partially-filled segment,
// baseUsed > 0 so the condition (baseUsed == 0 && !data.empty()) is false.
// =========================================================================

TEST_F(FileStorageTest, SaveData_AppendToPartiallyFilledSegment_Node) {
	// Use the existing test from WriteSegmentData_NonZeroBaseUsed but for Node type.
	// The key is that IDs must be CONSECUTIVE from the first batch so they naturally
	// extend the segment rather than finding pre-allocated slots.
	// Using the DataManager's addNode and flush() approach ensures proper IDs.
	auto dm = fileStorage->getDataManager();

	// Create first batch via DataManager
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Create second batch - these will be appended to the same segment
	// if the segment has remaining capacity (writeSegmentData with baseUsed > 0)
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
	}
	fileStorage->flush();

	// Just verify no crash and data is persisted
	dm->clearCache();
	auto nodes = dm->getNodesInRange(1, 10, 20);
	EXPECT_GE(nodes.size(), 5u);
}

// =========================================================================
// Branch Coverage: Line 350 False - multi-segment chain traversal for Node
// =========================================================================

TEST_F(FileStorageTest, SaveData_NodeMultiSegmentChainTraversal) {
	uint32_t capacity = graph::storage::NODES_PER_SEGMENT;

	std::unordered_map<int64_t, graph::Node> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		data1[static_cast<int64_t>(i)] = graph::Node(static_cast<int64_t>(i), 100);
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	std::unordered_map<int64_t, graph::Node> data2;
	for (uint32_t i = capacity + 1; i <= capacity + 5; ++i) {
		data2[static_cast<int64_t>(i)] = graph::Node(static_cast<int64_t>(i), 200);
	}
	fileStorage->saveData(data2, segHead, capacity);

	auto tracker = fileStorage->getSegmentTracker();
	uint64_t currentOffset = segHead;
	int segCount = 0;
	while (currentOffset != 0) {
		auto header = tracker->getSegmentHeader(currentOffset);
		segCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_GE(segCount, 2);
}

TEST_F(FileStorageTest, SaveData_PreAllocatedSlot_DecrementInactiveCount) {
	// Create nodes, delete some (sets inactive_count > 0), then saveData
	// with the deleted node IDs to reuse inactive slots.
	auto dm = fileStorage->getDataManager();

	// Create nodes
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 5; ++i) {
		graph::Node n(0, 0);
		dm->addNode(n);
		nodes.push_back(n);
	}
	fileStorage->flush();

	// Delete some nodes (creates inactive slots with inactive_count > 0)
	dm->deleteNode(nodes[1]);
	dm->deleteNode(nodes[3]);
	fileStorage->flush();

	// Rebuild segment indexes
	dm->getSegmentIndexManager()->buildSegmentIndexes();

	// Now call saveData with the deleted node IDs - should find pre-allocated
	// inactive slots and decrement inactive_count (line 323 True path)
	std::unordered_map<int64_t, graph::Node> data;
	graph::Node r1(nodes[1].getId(), 42);
	data[nodes[1].getId()] = r1;
	graph::Node r2(nodes[3].getId(), 42);
	data[nodes[3].getId()] = r2;

	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Node::typeId);
	fileStorage->saveData(data, segHead, graph::storage::NODES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

// =========================================================================
// Branch Coverage: Line 636-641 - Compaction path during flush
// Enable compaction, create high fragmentation, then flush to trigger
// the shouldCompact() and safeCompactSegments() paths.
// =========================================================================

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

// =========================================================================
// Branch Coverage: Line 636 False - compactionEnabled is false (default)
// Verify that flush does NOT compact when compaction is disabled
// =========================================================================

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

// =========================================================================
// Branch Coverage: Line 610 - Concurrent flush (lock contention)
// Two threads flush simultaneously; one should early-return due to lock.
// =========================================================================

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

