/**
 * @file test_StorageWriter_WritePaths.cpp
 * @brief Branch coverage tests for StorageWriter.cpp targeting:
 *        - classifyEntities with no backup (line 56 continue)
 *        - writeSnapshot with thread pool (parallel path lines 105-119)
 *        - writeSnapshot without thread pool (sequential path lines 120-122)
 *        - updateEntityInPlace error paths (lines 360-375)
 *        - deleteEntityOnDisk zero/negative id (line 391)
 *        - saveNewEntities empty (line 143)
 *        - saveModifiedAndDeleted empty
 *        - saveData with pre-allocated entities (line 171)
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageWriter.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EntityChangeType.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"
#include "graph/concurrent/ThreadPool.hpp"

using namespace graph::storage;
using namespace graph;

class StorageWriterWritePathsTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;
	std::shared_ptr<StorageIO> storageIO;
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	IDAllocators allocators;
	std::shared_ptr<SegmentAllocator> allocator;
	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<StorageWriter> writer;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("sw_branch_test_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		storageIO = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		segmentTracker = std::make_shared<SegmentTracker>(storageIO, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		std::pair<EntityType, int64_t *> typeMaxPairs[] = {
			{EntityType::Node, &fileHeaderManager->getMaxNodeIdRef()},
			{EntityType::Edge, &fileHeaderManager->getMaxEdgeIdRef()},
			{EntityType::Property, &fileHeaderManager->getMaxPropIdRef()},
			{EntityType::Blob, &fileHeaderManager->getMaxBlobIdRef()},
			{EntityType::Index, &fileHeaderManager->getMaxIndexIdRef()},
			{EntityType::State, &fileHeaderManager->getMaxStateIdRef()},
		};
		for (auto &[type, maxIdPtr] : typeMaxPairs) {
			allocators[static_cast<size_t>(type)] =
				std::make_shared<IDAllocator>(type, segmentTracker, *maxIdPtr);
		}
		allocator = std::make_shared<SegmentAllocator>(storageIO, segmentTracker, fileHeaderManager);
		dataManager = std::make_shared<DataManager>(file, 16, header, allocators, segmentTracker);
		dataManager->initialize();

		writer = std::make_shared<StorageWriter>(storageIO, segmentTracker, allocator, dataManager, allocators, header);
	}

	void TearDown() override {
		writer.reset();
		dataManager.reset();
		allocator.reset();
		for (auto &alloc : allocators) alloc.reset();
		segmentTracker.reset();
		storageIO.reset();
		if (file && file->is_open()) file->close();
		file.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	Node makeNode(int64_t id, bool active = true) {
		Node n;
		n.setId(id);
		if (!active) n.markInactive();
		return n;
	}

	Edge makeEdge(int64_t id, int64_t src = 1, int64_t dst = 2, bool active = true) {
		Edge e;
		e.setId(id);
		e.setSourceNodeId(src);
		e.setTargetNodeId(dst);
		if (!active) e.markInactive();
		return e;
	}
};

// ============================================================================
// classifyEntities: entry with no backup — should be skipped (continue)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, ClassifyEntities_NoBackup_Skipped) {
	std::unordered_map<int64_t, DirtyEntityInfo<Node>> map;

	// Entry without backup (default-constructed optional is empty)
	DirtyEntityInfo<Node> info;
	info.changeType = EntityChangeType::CHANGE_ADDED;
	// info.backup is std::nullopt by default
	map[1] = info;

	auto batch = classifyEntities(map);
	EXPECT_EQ(batch.added.size(), 0u);
	EXPECT_EQ(batch.modified.size(), 0u);
	EXPECT_EQ(batch.deleted.size(), 0u);
}

TEST_F(StorageWriterWritePathsTest, ClassifyEntities_MixedBackupAndNoBackup) {
	std::unordered_map<int64_t, DirtyEntityInfo<Node>> map;

	// One with backup, one without
	map[1] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, makeNode(1));
	DirtyEntityInfo<Node> noBackup;
	noBackup.changeType = EntityChangeType::CHANGE_MODIFIED;
	map[2] = noBackup;

	auto batch = classifyEntities(map);
	EXPECT_EQ(batch.added.size(), 1u);
	EXPECT_EQ(batch.modified.size(), 0u);
}

// ============================================================================
// saveNewEntities: empty vector returns early
// ============================================================================

TEST_F(StorageWriterWritePathsTest, SaveNewEntities_Empty) {
	std::vector<Node> empty;
	EXPECT_NO_THROW(writer->saveNewEntities(empty));
}

// ============================================================================
// saveModifiedAndDeleted: empty vectors
// ============================================================================

TEST_F(StorageWriterWritePathsTest, SaveModifiedAndDeleted_Empty) {
	std::vector<Node> emptyMod, emptyDel;
	EXPECT_NO_THROW(writer->saveModifiedAndDeleted(emptyMod, emptyDel));
}

// ============================================================================
// deleteEntityOnDisk: zero and negative IDs return early
// ============================================================================

TEST_F(StorageWriterWritePathsTest, DeleteEntityOnDisk_ZeroId) {
	Node n = makeNode(0);
	EXPECT_NO_THROW(writer->deleteEntityOnDisk(n));
}

TEST_F(StorageWriterWritePathsTest, DeleteEntityOnDisk_NegativeId) {
	Node n = makeNode(-5);
	EXPECT_NO_THROW(writer->deleteEntityOnDisk(n));
}

// ============================================================================
// deleteEntityOnDisk: entity not found in segment (segmentOffset == 0)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, DeleteEntityOnDisk_NotInSegment) {
	Node n = makeNode(999, false);
	EXPECT_NO_THROW(writer->deleteEntityOnDisk(n));
}

// ============================================================================
// deleteEntityOnDisk for Edge type
// ============================================================================

TEST_F(StorageWriterWritePathsTest, DeleteEntityOnDisk_Edge) {
	Edge e = makeEdge(999, 1, 2, false);
	EXPECT_NO_THROW(writer->deleteEntityOnDisk(e));
}

// ============================================================================
// updateEntityInPlace: entity not found (segmentOffset == 0)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, UpdateEntityInPlace_NotFound) {
	dataManager->getSegmentIndexManager()->buildSegmentIndexes();
	Node n = makeNode(999);
	EXPECT_THROW(writer->updateEntityInPlace(n), std::runtime_error);
}

// ============================================================================
// saveData with empty data
// ============================================================================

TEST_F(StorageWriterWritePathsTest, SaveData_EmptyData) {
	std::vector<Node> empty;
	uint64_t segHead = 0;
	EXPECT_NO_THROW(writer->saveData(empty, segHead, itemsPerSegment<Node>()));
	EXPECT_EQ(segHead, 0u);
}

// ============================================================================
// saveData with pre-allocated entities (found in existing segment)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, SaveData_PreAllocatedEntities) {
	// First write some nodes to create a segment
	std::vector<Node> nodes = {makeNode(1), makeNode(2), makeNode(3)};
	uint64_t segHead = 0;
	writer->saveData(nodes, segHead, itemsPerSegment<Node>());
	ASSERT_NE(segHead, 0u);

	header.node_segment_head = segHead;
	dataManager->getSegmentIndexManager()->buildSegmentIndexes();

	// Now save again with same IDs — should go through writePreAllocatedEntities path
	std::vector<Node> updated = {makeNode(1), makeNode(2)};
	updated[0].addLabelId(42);
	writer->saveData(updated, segHead, itemsPerSegment<Node>());
}

// ============================================================================
// writeSnapshot with null thread pool (sequential path)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, WriteSnapshot_NullPool) {
	FlushSnapshot snapshot;

	Node n = makeNode(1);
	snapshot.nodes[1] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, n);

	EXPECT_NO_THROW(writer->writeSnapshot(snapshot, nullptr));
}

// ============================================================================
// writeSnapshot with single-threaded pool (sequential path via isSingleThreaded)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, WriteSnapshot_SingleThreadPool) {
	FlushSnapshot snapshot;

	Node n = makeNode(1);
	snapshot.nodes[1] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, n);

	concurrent::ThreadPool pool(1);
	EXPECT_NO_THROW(writer->writeSnapshot(snapshot, &pool));
}

// ============================================================================
// writeSnapshot with multi-threaded pool (parallel path)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, WriteSnapshot_MultiThreadPool) {
	// Set up some data first
	std::vector<Node> nodes = {makeNode(1), makeNode(2)};
	uint64_t segHead = 0;
	writer->saveData(nodes, segHead, itemsPerSegment<Node>());
	header.node_segment_head = segHead;
	dataManager->getSegmentIndexManager()->buildSegmentIndexes();

	FlushSnapshot snapshot;

	// Add a new node
	Node n3 = makeNode(3);
	snapshot.nodes[3] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_ADDED, n3);

	// Modify an existing node
	Node n1mod = makeNode(1);
	n1mod.addLabelId(42);
	snapshot.nodes[1] = DirtyEntityInfo<Node>(EntityChangeType::CHANGE_MODIFIED, n1mod);

	concurrent::ThreadPool pool(4);
	EXPECT_NO_THROW(writer->writeSnapshot(snapshot, &pool));
}

// ============================================================================
// writeSegmentData with empty data (exercises data.empty() guard)
// ============================================================================

TEST_F(StorageWriterWritePathsTest, WriteSegmentData_EmptyData) {
	constexpr uint32_t capacity = itemsPerSegment<Node>();
	uint64_t segOff = allocator->allocateSegment(Node::typeId, capacity);
	ASSERT_NE(segOff, 0u);

	std::vector<Node> empty;
	writer->writeSegmentData(segOff, empty, 0);

	SegmentHeader hdr = segmentTracker->getSegmentHeader(segOff);
	EXPECT_EQ(hdr.used, 0u);
}

// ============================================================================
// saveData with non-consecutive IDs triggers new segment allocation
// ============================================================================

TEST_F(StorageWriterWritePathsTest, SaveData_NonConsecutiveIds) {
	std::vector<Node> nodes = {makeNode(1), makeNode(2), makeNode(100)};
	uint64_t segHead = 0;
	writer->saveData(nodes, segHead, itemsPerSegment<Node>());
	EXPECT_NE(segHead, 0u);
}
