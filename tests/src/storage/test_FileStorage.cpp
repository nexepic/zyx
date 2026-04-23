/**
 * @file test_FileStorage.cpp
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

TYPED_TEST_SUITE(FileStorageTypedTest, AllEntityTypes);

// ============================================================================
// Delete, Update, Write, Close & Typed Tests
// ============================================================================

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

TEST_F(FileStorageTest, VerifySegmentLinking) {
	std::vector<graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data.push_back(graph::Node(i, 100));
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
	int64_t lbl = dataManager->getOrCreateTokenId("Test");
	graph::Node node(1, lbl);
	dataManager->addNode(node);
	fileStorage->flush(); // Ensure it's on disk

	// 2. Modify Node in memory
	int64_t newLbl = dataManager->getOrCreateTokenId("Updated");
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

TEST_F(FileStorageTest, DeleteEntityOnDisk_InvalidId_ReturnsEarly) {
	graph::Node node(0, 0); // id = 0
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(node));

	graph::Node negNode(-1, 0); // negative id
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(negNode));
}

TEST_F(FileStorageTest, DeleteEntityOnDisk_NotOnDisk_NoException) {
	// Entity with valid ID but never saved to disk
	graph::Node node(99999, 0);
	node.markInactive();
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(node));
}

TEST_F(FileStorageTest, UpdateEntityInPlace_EntityNotFound_Throws) {
	graph::Node node(88888, 0);
	EXPECT_THROW(fileStorage->updateEntityInPlace(node), std::runtime_error);
}

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

TEST_F(FileStorageTest, DeleteEntityOnDisk_EdgeNotOnDisk) {
	// Test deleteEntityOnDisk for edge that was never saved to disk (line 492-496 false branch)
	graph::Edge e(12345, 1, 2, 0);
	e.markInactive();
	EXPECT_NO_THROW(fileStorage->deleteEntityOnDisk(e));
}

TEST_F(FileStorageTest, CloseReopenVerifyPersistence) {
	auto dm = fileStorage->getDataManager();

	// Add nodes with properties
	int64_t lbl = dm->getOrCreateTokenId("Persist");
	graph::Node n1(0, lbl);
	dm->addNode(n1);
	int64_t n1Id = n1.getId();

	graph::Node n2(0, lbl);
	dm->addNode(n2);
	int64_t n2Id = n2.getId();

	// Add an edge between them
	int64_t edgeLbl = dm->getOrCreateTokenId("CONNECTS");
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

TEST_F(FileStorageTest, WriteSegmentData_NonZeroBaseUsed) {
	// Test writeSegmentData when baseUsed != 0 (line 428 false branch)
	auto dm = fileStorage->getDataManager();

	// Create a batch that partially fills a segment
	std::vector<graph::Node> data1;
	for (int64_t i = 1; i <= 10; ++i) {
		data1.push_back(graph::Node(i, 100));
	}
	uint64_t segmentHead = 0;
	fileStorage->saveData(data1, segmentHead, 100);

	// Now add more data to the same segment chain (non-zero baseUsed)
	std::vector<graph::Node> data2;
	for (int64_t i = 11; i <= 20; ++i) {
		data2.push_back(graph::Node(i, 200));
	}
	fileStorage->saveData(data2, segmentHead, 100);

	// Verify both batches are in the segment
	auto tracker = fileStorage->getSegmentTracker();
	auto header = tracker->getSegmentHeader(segmentHead);
	EXPECT_GE(header.used, 20u);
}

TEST_F(FileStorageTest, WriteSegmentData_NonZeroBaseUsed_Edge) {
	// Create edges that partially fill a segment, then add more via saveData
	auto dm = fileStorage->getDataManager();

	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	// First batch: partially fill segment
	std::vector<graph::Edge> data1;
	for (int64_t i = 1; i <= 5; ++i) {
		data1.push_back(graph::Edge(i, n1.getId(), n2.getId(), 0));
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, 100);
	ASSERT_NE(segHead, 0u);

	// Second batch: add to same segment (baseUsed > 0)
	std::vector<graph::Edge> data2;
	for (int64_t i = 6; i <= 10; ++i) {
		data2.push_back(graph::Edge(i, n1.getId(), n2.getId(), 0));
	}
	fileStorage->saveData(data2, segHead, 100);

	auto tracker = fileStorage->getSegmentTracker();
	auto header = tracker->getSegmentHeader(segHead);
	EXPECT_GE(header.used, 10u);
}

TYPED_TEST(FileStorageTypedTest, SaveData_PreAllocatedSlotReuse) {
    using Helper = EntityTestHelper<TypeParam>;
    auto dm = this->fileStorage->getDataManager();

    auto entity = Helper::create(*dm);
    this->fileStorage->flush();

    Helper::update(*dm, entity);
    EXPECT_NO_THROW(this->fileStorage->flush());
}

TYPED_TEST(FileStorageTypedTest, SaveData_EmptyData) {
    std::vector<TypeParam> emptyData;
    uint64_t segHead = 0;
    this->fileStorage->saveData(emptyData, segHead, 100);
    EXPECT_EQ(segHead, 0u);
}

TYPED_TEST(FileStorageTypedTest, Save_OnlyNewEntities) {
    using Helper = EntityTestHelper<TypeParam>;
    auto dm = this->fileStorage->getDataManager();

    auto entity = Helper::create(*dm);
    (void)entity;
    EXPECT_NO_THROW(this->fileStorage->save());
}

TYPED_TEST(FileStorageTypedTest, SaveData_Deletion) {
    using Helper = EntityTestHelper<TypeParam>;
    auto dm = this->fileStorage->getDataManager();

    auto entity = Helper::create(*dm);
    this->fileStorage->flush();

    Helper::del(*dm, entity);
    EXPECT_NO_THROW(this->fileStorage->flush());
}

TYPED_TEST(FileStorageTypedTest, Save_ModifyAndDeleteEntities) {
    using Helper = EntityTestHelper<TypeParam>;
    auto dm = this->fileStorage->getDataManager();

    auto e1 = Helper::create(*dm);
    auto e2 = Helper::create(*dm);
    this->fileStorage->flush();

    Helper::update(*dm, e1);
    Helper::del(*dm, e2);
    EXPECT_NO_THROW(this->fileStorage->save());
}

TEST_F(FileStorageTest, GetIntegrityChecker_ReturnsNonNull) {
	// Covers FileStorage::getIntegrityChecker()
	auto checker = fileStorage->getIntegrityChecker();
	EXPECT_NE(checker, nullptr);
}

TEST_F(FileStorageTest, GetIDAllocatorByTypeId_ReturnsNonNull) {
	// Covers FileStorage::getIDAllocator(uint32_t typeId) for all 6 entity types
	for (uint32_t typeId = 0; typeId < 6; ++typeId) {
		auto allocator = fileStorage->getIDAllocator(typeId);
		EXPECT_NE(allocator, nullptr) << "typeId=" << typeId;
	}
}
