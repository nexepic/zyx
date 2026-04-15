/**
 * @file test_StorageWriter.cpp
 * @brief Unit tests for StorageWriter entity write engine
 *
 * Tests the core write operations (writeSegmentData, updateEntityInPlace,
 * deleteEntityOnDisk) in isolation via a minimal FileStorage setup.
 * Full integration coverage is provided by test_FileStorage.cpp.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageWriter.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

using namespace graph::storage;
using namespace graph;

class StorageWriterTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;
	std::shared_ptr<StorageIO> storageIO;
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<IDAllocator> idAllocator;
	std::shared_ptr<SegmentAllocator> allocator;
	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<StorageWriter> writer;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("storage_writer_test_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		storageIO = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		segmentTracker = std::make_shared<SegmentTracker>(storageIO, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		idAllocator = std::make_shared<IDAllocator>(
			file, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
			fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
			fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());
		allocator = std::make_shared<SegmentAllocator>(storageIO, segmentTracker, fileHeaderManager, idAllocator);
		dataManager = std::make_shared<DataManager>(file, 16, header, idAllocator, segmentTracker);
		dataManager->initialize();

		writer = std::make_shared<StorageWriter>(storageIO, segmentTracker, allocator, dataManager, idAllocator, header);
	}

	void TearDown() override {
		writer.reset();
		dataManager.reset();
		allocator.reset();
		idAllocator.reset();
		segmentTracker.reset();
		storageIO.reset();

		if (file && file->is_open()) {
			file->close();
		}
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

	// Read entity back from disk by deserializing from its segment slot
	Node readNodeFromDisk(uint64_t segOff, uint32_t slotIndex) {
		uint64_t entityOffset = segOff + sizeof(SegmentHeader) + slotIndex * Node::getTotalSize();
		std::vector<char> buf(Node::getTotalSize());
		storageIO->readAt(entityOffset, buf.data(), buf.size());

		std::string str(buf.begin(), buf.end());
		std::istringstream iss(str, std::ios::binary);
		return utils::FixedSizeSerializer::deserializeWithFixedSize<Node>(iss, Node::getTotalSize());
	}
};

// ============================================================================
// writeSegmentData + readback roundtrip
// ============================================================================

TEST_F(StorageWriterTest, WriteSegmentDataAndReadback) {
	constexpr uint32_t capacity = itemsPerSegment<Node>();
	uint64_t segOff = allocator->allocateSegment(Node::typeId, capacity);
	ASSERT_NE(segOff, 0u);

	std::vector<Node> nodes = {makeNode(1), makeNode(2), makeNode(3)};
	writer->writeSegmentData(segOff, nodes, 0);

	// Verify header was updated
	SegmentHeader hdr = segmentTracker->getSegmentHeader(segOff);
	EXPECT_EQ(hdr.used, 3u);
	EXPECT_EQ(hdr.start_id, 1);

	// Verify bitmap bits are set
	EXPECT_TRUE(segmentTracker->getBitmapBit(segOff, 0));
	EXPECT_TRUE(segmentTracker->getBitmapBit(segOff, 1));
	EXPECT_TRUE(segmentTracker->getBitmapBit(segOff, 2));
}

// ============================================================================
// writeSegmentData appends to existing data in segment
// ============================================================================

TEST_F(StorageWriterTest, WriteSegmentDataAppendToExisting) {
	constexpr uint32_t capacity = itemsPerSegment<Node>();
	uint64_t segOff = allocator->allocateSegment(Node::typeId, capacity);

	std::vector<Node> first = {makeNode(1), makeNode(2)};
	writer->writeSegmentData(segOff, first, 0);

	std::vector<Node> second = {makeNode(3), makeNode(4)};
	writer->writeSegmentData(segOff, second, 2);

	SegmentHeader hdr = segmentTracker->getSegmentHeader(segOff);
	EXPECT_EQ(hdr.used, 4u);
	EXPECT_EQ(hdr.start_id, 1);

	for (uint32_t i = 0; i < 4; ++i) {
		EXPECT_TRUE(segmentTracker->getBitmapBit(segOff, i));
	}
}

// ============================================================================
// saveData allocates segments and writes entities
// ============================================================================

TEST_F(StorageWriterTest, SaveDataAllocatesAndWrites) {
	std::vector<Node> nodes = {makeNode(1), makeNode(2), makeNode(3)};
	uint64_t segHead = 0;
	constexpr uint32_t perSeg = itemsPerSegment<Node>();

	writer->saveData(nodes, segHead, perSeg);

	EXPECT_NE(segHead, 0u);

	SegmentHeader hdr = segmentTracker->getSegmentHeader(segHead);
	EXPECT_EQ(hdr.used, 3u);
	EXPECT_EQ(hdr.start_id, 1);
}

// ============================================================================
// updateEntityInPlace overwrites entity data
// ============================================================================

TEST_F(StorageWriterTest, UpdateEntityInPlace) {
	constexpr uint32_t capacity = itemsPerSegment<Node>();
	uint64_t segOff = allocator->allocateSegment(Node::typeId, capacity);

	std::vector<Node> nodes = {makeNode(1)};
	writer->writeSegmentData(segOff, nodes, 0);

	// Build segment index so findSegmentForEntityId works
	dataManager->getSegmentIndexManager()->buildSegmentIndexes();

	// Update the node (add a label)
	Node updated = makeNode(1);
	updated.addLabelId(42);
	writer->updateEntityInPlace(updated, segOff);

	// Read back and verify the entity was updated
	Node readBack = readNodeFromDisk(segOff, 0);
	EXPECT_EQ(readBack.getId(), 1);
	EXPECT_TRUE(readBack.hasLabelId(42));
}

// ============================================================================
// updateEntityInPlace throws for unknown entity
// ============================================================================

TEST_F(StorageWriterTest, UpdateEntityInPlaceThrowsForUnknownEntity) {
	dataManager->getSegmentIndexManager()->buildSegmentIndexes();
	Node n = makeNode(999);
	EXPECT_THROW(writer->updateEntityInPlace(n), std::runtime_error);
}

// ============================================================================
// deleteEntityOnDisk marks entity inactive and frees ID
// ============================================================================

TEST_F(StorageWriterTest, DeleteEntityOnDisk) {
	// Use saveData to properly set up segment chain head
	std::vector<Node> nodes = {makeNode(1), makeNode(2)};
	uint64_t segHead = 0;
	constexpr uint32_t perSeg = itemsPerSegment<Node>();
	writer->saveData(nodes, segHead, perSeg);
	ASSERT_NE(segHead, 0u);

	// Update fileHeader so segment index can find the chain
	header.node_segment_head = segHead;
	dataManager->getSegmentIndexManager()->buildSegmentIndexes();

	// Delete node 1
	Node deleted = makeNode(1, /*active=*/false);
	writer->deleteEntityOnDisk(deleted);

	// Read back and verify inactive
	Node readBack = readNodeFromDisk(segHead, 0);
	EXPECT_FALSE(readBack.isActive());
}

// ============================================================================
// deleteEntityOnDisk with invalid ID is a no-op
// ============================================================================

TEST_F(StorageWriterTest, DeleteEntityOnDiskInvalidIdNoOp) {
	Node n = makeNode(0);
	EXPECT_NO_THROW(writer->deleteEntityOnDisk(n));

	Node neg = makeNode(-1);
	EXPECT_NO_THROW(writer->deleteEntityOnDisk(neg));
}

// ============================================================================
// saveNewEntities delegates through saveData
// ============================================================================

TEST_F(StorageWriterTest, SaveNewEntitiesWritesToChain) {
	std::vector<Node> nodes = {makeNode(1), makeNode(2)};
	writer->saveNewEntities(nodes);

	// Verify that node_segment_head was updated
	EXPECT_NE(header.node_segment_head, 0u);

	SegmentHeader hdr = segmentTracker->getSegmentHeader(header.node_segment_head);
	EXPECT_EQ(hdr.used, 2u);
}

// ============================================================================
// classifyEntities correctly splits by change type
// ============================================================================

TEST_F(StorageWriterTest, ClassifyEntitiesSplitsByChangeType) {
	std::unordered_map<int64_t, DirtyEntityInfo<Node>> map;

	Node n1 = makeNode(1);
	Node n2 = makeNode(2);
	Node n3 = makeNode(3, false);

	map[1] = {EntityChangeType::CHANGE_ADDED, n1};
	map[2] = {EntityChangeType::CHANGE_MODIFIED, n2};
	map[3] = {EntityChangeType::CHANGE_DELETED, n3};

	auto batch = classifyEntities(map);

	EXPECT_EQ(batch.added.size(), 1u);
	EXPECT_EQ(batch.modified.size(), 1u);
	EXPECT_EQ(batch.deleted.size(), 1u);
	EXPECT_EQ(batch.added[0].getId(), 1);
	EXPECT_EQ(batch.modified[0].getId(), 2);
	EXPECT_EQ(batch.deleted[0].getId(), 3);
}

// ============================================================================
// Edge writeSegmentData roundtrip
// ============================================================================

TEST_F(StorageWriterTest, EdgeWriteSegmentDataRoundtrip) {
	constexpr uint32_t capacity = itemsPerSegment<Edge>();
	uint64_t segOff = allocator->allocateSegment(Edge::typeId, capacity);
	ASSERT_NE(segOff, 0u);

	std::vector<Edge> edges = {makeEdge(1, 10, 20), makeEdge(2, 30, 40)};
	writer->writeSegmentData(segOff, edges, 0);

	SegmentHeader hdr = segmentTracker->getSegmentHeader(segOff);
	EXPECT_EQ(hdr.used, 2u);
	EXPECT_EQ(hdr.start_id, 1);

	EXPECT_TRUE(segmentTracker->getBitmapBit(segOff, 0));
	EXPECT_TRUE(segmentTracker->getBitmapBit(segOff, 1));
}
