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
#include <gtest/gtest.h>
#include <zlib.h>
#include "graph/core/Database.hpp"

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
		// Clean up using Database
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
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
				graph::storage::FileStorage fs(dirPath.string(), 1024, graph::storage::OpenMode::CREATE_NEW_FILE);
				fs.open();
			},
			std::runtime_error);

	std::filesystem::remove(dirPath);
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
	std::filesystem::remove(lockedPath);
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

	std::filesystem::remove(dirPath);
}
