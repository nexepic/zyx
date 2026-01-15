/**
 * @file test_DeletionManager.cpp
 * @author Nexepic
 * @date 2025/12/4
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

#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/DeletionManager.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

using namespace graph::storage;
using namespace graph;

/**
 * @brief Test Fixture for DeletionManager.
 * Sets up a full storage engine environment to verify interactions between
 * deletions, ID allocation, and segment metadata.
 */
class DeletionManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;

	// Core Storage Components
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<IDAllocator> idAllocator;
	std::shared_ptr<SpaceManager> spaceManager;
	std::shared_ptr<EntityReferenceUpdater> refUpdater;

	// Data Management Components
	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<DeletionManager> deletionManager;
	std::shared_ptr<state::SystemStateManager> systemStateManager;

	void SetUp() override {
		// 1. Setup Temporary File
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("delmgr_test_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		// Write initial header
		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		// 2. Initialize Storage Layer Dependencies
		segmentTracker = std::make_shared<SegmentTracker>(file, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);

		idAllocator = std::make_shared<IDAllocator>(
				file, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
				fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
				fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());

		spaceManager = std::make_shared<SpaceManager>(file, testFilePath.string(), segmentTracker, fileHeaderManager,
													  idAllocator);
		spaceManager->setEntityReferenceUpdater(refUpdater);

		// 3. Initialize DataManager
		dataManager = std::make_shared<DataManager>(file,
													100, // Cache size
													header, idAllocator, segmentTracker, spaceManager);

		refUpdater = std::make_shared<EntityReferenceUpdater>(dataManager);

		// 4. Initialize System Under Test (DeletionManager)
		deletionManager = std::make_shared<DeletionManager>(dataManager, spaceManager, idAllocator);

		// Circular dependency resolution usually happens inside DataManager,
		// ensuring it is initialized.
		dataManager->initialize();

		systemStateManager = std::make_shared<state::SystemStateManager>(dataManager);
		dataManager->setSystemStateManager(systemStateManager);
	}

	void TearDown() override {
		deletionManager.reset();
		dataManager.reset();
		spaceManager.reset();
		refUpdater.reset();
		idAllocator.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open())
			file->close();
		std::filesystem::remove(testFilePath);
	}

	// --- Helper Functions to create entities and update Tracker Metadata ---

	[[nodiscard]] Node createNode(const std::string &label) const {
		int64_t id = idAllocator->allocateId(Node::typeId);

		uint64_t offset = segmentTracker->getSegmentOffsetForNodeId(id);
		if (offset == 0) {
			offset = spaceManager->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
		}

		int64_t labelId = dataManager->getOrCreateLabelId(label);
		Node node(id, labelId);

		dataManager->addNode(node);

		// CRITICAL: Manually update SegmentTracker state to simulate a valid active entity
		// This is required because DataManager::addNode typically just writes bytes,
		// while the tracker bitmap maintenance might be separate or handled by NodeManager.
		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		uint32_t idx = id - h.start_id;

		segmentTracker->setEntityActive(offset, idx, true);
		if (idx >= h.used) {
			segmentTracker->updateSegmentUsage(offset, idx + 1, h.inactive_count);
		}

		return node;
	}

	[[nodiscard]] Edge createEdge(int64_t srcId, int64_t dstId, const std::string &label) const {
		int64_t id = idAllocator->allocateId(Edge::typeId);

		uint64_t offset = segmentTracker->getSegmentOffsetForEdgeId(id);
		if (offset == 0) {
			offset = spaceManager->allocateSegment(Edge::typeId, EDGES_PER_SEGMENT);
		}

		int64_t labelId = dataManager->getOrCreateLabelId(label);
		Edge edge(id, srcId, dstId, labelId);

		dataManager->addEdge(edge);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		uint32_t idx = id - h.start_id;

		segmentTracker->setEntityActive(offset, idx, true);
		if (idx >= h.used) {
			segmentTracker->updateSegmentUsage(offset, idx + 1, h.inactive_count);
		}

		return edge;
	}

	[[nodiscard]] Property createProperty() const {
		int64_t id = idAllocator->allocateId(Property::typeId);
		uint64_t offset = spaceManager->allocateSegment(Property::typeId, PROPERTIES_PER_SEGMENT);

		Property prop;
		prop.setId(id);
		dataManager->addPropertyEntity(prop);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		uint32_t idx = id - h.start_id;
		segmentTracker->setEntityActive(offset, idx, true);
		segmentTracker->updateSegmentUsage(offset, idx + 1, 0);
		return prop;
	}

	[[nodiscard]] Blob createBlob() const {
		int64_t id = idAllocator->allocateId(Blob::typeId);
		uint64_t offset = spaceManager->allocateSegment(Blob::typeId, BLOBS_PER_SEGMENT);

		Blob blob;
		blob.setId(id);
		dataManager->addBlobEntity(blob);

		SegmentHeader h = segmentTracker->getSegmentHeader(offset);
		uint32_t idx = id - h.start_id;
		segmentTracker->setEntityActive(offset, idx, true);
		segmentTracker->updateSegmentUsage(offset, idx + 1, 0);
		return blob;
	}

	// Helper checks
	[[nodiscard]] bool isNodeDeleted(int64_t id) const {
		// 1. Check DeletionManager logic (Bitmap check)
		if (!deletionManager->isNodeActive(id))
			return true;
		return false;
	}

	[[nodiscard]] bool isEdgeDeleted(int64_t id) const {
		if (!deletionManager->isEdgeActive(id))
			return true;
		return false;
	}
};

// =========================================================================
// Tests for Node Deletion
// =========================================================================

TEST_F(DeletionManagerTest, DeleteNode_Basic) {
	Node node = createNode("User");
	int64_t id = node.getId();

	// Verify setup
	ASSERT_TRUE(deletionManager->isNodeActive(id));

	// Execute
	deletionManager->deleteNode(node);

	// Verify result
	EXPECT_FALSE(deletionManager->isNodeActive(id));
	EXPECT_TRUE(isNodeDeleted(id));
}

TEST_F(DeletionManagerTest, DeleteNode_CascadesToConnectedEdges) {
	Node nA = createNode("A");
	Node nB = createNode("B");
	// Connect A -> B
	Edge e1 = createEdge(nA.getId(), nB.getId(), "Knows");

	// Delete A
	deletionManager->deleteNode(nA);

	// A should be deleted
	EXPECT_TRUE(isNodeDeleted(nA.getId()));
	// B should remain
	EXPECT_FALSE(isNodeDeleted(nB.getId()));

	// Edge connected to A should be deleted
	// Note: This relies on DataManager::findEdgesByNode functioning correctly
	EXPECT_TRUE(isEdgeDeleted(e1.getId()));
}

TEST_F(DeletionManagerTest, DeleteNode_CascadesToPropertyEntity) {
	Node node = createNode("PropNode");
	Property prop = createProperty();

	// Link Node to Property Entity
	node.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateNode(node);

	// Delete Node
	deletionManager->deleteNode(node);

	// Verify Node is gone
	EXPECT_TRUE(isNodeDeleted(node.getId()));

	// Verify Property is gone (via tracker bitmap)
	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(pSeg, prop.getId() - h.start_id));
}

TEST_F(DeletionManagerTest, DeleteNode_CascadesToBlobEntity) {
	Node node = createNode("BlobNode");
	Blob blob = createBlob();

	// Link Node to Blob Entity
	node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateNode(node);

	// Delete Node
	deletionManager->deleteNode(node);

	// Verify Blob is gone
	uint64_t bSeg = spaceManager->getTracker()->getSegmentOffsetForBlobId(blob.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(bSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(bSeg, blob.getId() - h.start_id));
}

// =========================================================================
// Tests for Edge Deletion
// =========================================================================

TEST_F(DeletionManagerTest, DeleteEdge_Basic) {
	Node n1 = createNode("A");
	Node n2 = createNode("B");
	Edge edge = createEdge(n1.getId(), n2.getId(), "Link");
	int64_t id = edge.getId();

	ASSERT_TRUE(deletionManager->isEdgeActive(id));

	deletionManager->deleteEdge(edge);

	EXPECT_FALSE(deletionManager->isEdgeActive(id));
}

TEST_F(DeletionManagerTest, DeleteEdge_CascadesToProperty) {
	Node n1 = createNode("A");
	Node n2 = createNode("B");
	Edge edge = createEdge(n1.getId(), n2.getId(), "Link");
	Property prop = createProperty();

	edge.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateEdge(edge);

	deletionManager->deleteEdge(edge);

	EXPECT_TRUE(isEdgeDeleted(edge.getId()));

	// Check Property is deleted
	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(pSeg, prop.getId() - h.start_id));
}

// =========================================================================
// Tests for Other Entity Types
// =========================================================================

TEST_F(DeletionManagerTest, DeleteProperty_Direct) {
	Property prop = createProperty();
	ASSERT_GT(prop.getId(), 0);

	deletionManager->deleteProperty(prop);

	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(pSeg, prop.getId() - h.start_id));
}

TEST_F(DeletionManagerTest, DeleteBlob_Direct) {
	Blob blob = createBlob();
	ASSERT_GT(blob.getId(), 0);

	deletionManager->deleteBlob(blob);

	uint64_t bSeg = spaceManager->getTracker()->getSegmentOffsetForBlobId(blob.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(bSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(bSeg, blob.getId() - h.start_id));
}

TEST_F(DeletionManagerTest, DeleteIndex) {
	int64_t id = idAllocator->allocateId(Index::typeId);
	uint64_t offset = spaceManager->allocateSegment(Index::typeId, INDEXES_PER_SEGMENT);

	Index idx;
	idx.setId(id);
	dataManager->addIndexEntity(idx);

	// Manually activate
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);
	segmentTracker->setEntityActive(offset, id - h.start_id, true);
	segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, 0);

	deletionManager->deleteIndex(idx);

	EXPECT_FALSE(segmentTracker->isEntityActive(offset, id - h.start_id));
}

TEST_F(DeletionManagerTest, DeleteState) {
	int64_t id = idAllocator->allocateId(State::typeId);
	uint64_t offset = spaceManager->allocateSegment(State::typeId, STATES_PER_SEGMENT);

	State state;
	state.setId(id);
	dataManager->addStateEntity(state);

	SegmentHeader h = segmentTracker->getSegmentHeader(offset);
	segmentTracker->setEntityActive(offset, id - h.start_id, true);
	segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, 0);

	deletionManager->deleteState(state);

	EXPECT_FALSE(segmentTracker->isEntityActive(offset, id - h.start_id));
}

// =========================================================================
// Tests for Compaction Triggering
// =========================================================================

TEST_F(DeletionManagerTest, CompactionThreshold_Triggered) {
	// 1. Manually allocate a segment to strictly control capacity (10)
	uint32_t type = Node::typeId;
	uint32_t capacity = 10;
	uint64_t offset = spaceManager->allocateSegment(type, capacity);

	// Get the segment header
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);

	// CRITICAL CHECK: Ensure SpaceManager allocated a valid start_id >= 1
	// ID 0 is reserved/invalid. If this fails, SpaceManager allocation logic is flawed.
	ASSERT_GE(h.start_id, 1) << "Segment start_id must be >= 1. ID 0 is invalid.";

	int64_t startId = h.start_id;

	// 2. Setup Data
	// We explicitly call updateSegmentUsage FIRST.
	// This sets 'used' count to 10. The SegmentIndexManager listens to this
	// and registers the range [startId, startId + 9] as valid.
	segmentTracker->updateSegmentUsage(offset, 10, 0);

	std::vector<Node> nodes;
	for (int i = 0; i < 10; i++) {
		int64_t id = startId + i; // IDs will be e.g., 1, 2, 3... 10
		Node n(id, 10);

		// Write to DataManager so DeletionManager can read it back to verify existence
		dataManager->addNode(n);

		// Manually activate in tracker bitmap
		// index = id - startId = i (0 to 9)
		segmentTracker->setEntityActive(offset, i, true);

		nodes.push_back(n);
	}

	// 3. Pre-Deletion Verification
	// Verify everything is active and correctly mapped
	for (int i = 0; i < 10; i++) {
		ASSERT_TRUE(deletionManager->isNodeActive(nodes[i].getId()))
				<< "Setup Check Failed: Node " << i << " (ID: " << nodes[i].getId() << ") is inactive!";
	}

	// Verify Header Initial State
	SegmentHeader hBefore = segmentTracker->getSegmentHeader(offset);
	ASSERT_EQ(hBefore.used, 10U);
	ASSERT_EQ(hBefore.inactive_count, 0U);
	ASSERT_DOUBLE_EQ(hBefore.getFragmentationRatio(), 0.0);

	// 4. Delete 4 nodes
	for (int i = 0; i < 4; i++) {
		int64_t idToDelete = nodes[i].getId();

		// Double check active status before delete
		ASSERT_TRUE(deletionManager->isNodeActive(idToDelete))
				<< "Dependency Check: Node ID " << idToDelete << " became inactive unexpectedly!";

		deletionManager->deleteNode(nodes[i]);

		// Immediate post-check
		ASSERT_FALSE(deletionManager->isNodeActive(idToDelete))
				<< "Deletion Failed: Node ID " << idToDelete << " is still active!";
	}

	// 5. Final Verification
	SegmentHeader hAfter = segmentTracker->getSegmentHeader(offset);

	// Debug print if assertions fail
	if (hAfter.inactive_count != 4U) {
		std::cout << "[Debug] startId=" << startId << " Used=" << hAfter.used << " Inactive=" << hAfter.inactive_count
				  << std::endl;
	}

	EXPECT_EQ(hAfter.used, 10U);
	EXPECT_EQ(hAfter.inactive_count, 4U);

	// Ratio = 1.0 - ((10 - 4) / 10) = 0.4
	EXPECT_DOUBLE_EQ(hAfter.getFragmentationRatio(), 0.4);

	// Threshold is 0.3, so 0.4 should trigger compaction
	EXPECT_EQ(hAfter.needs_compaction, 1U);
}

TEST_F(DeletionManagerTest, CompactionThreshold_NotTriggered) {
	uint32_t type = Node::typeId;
	uint64_t offset = spaceManager->allocateSegment(type, 10);
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);

	// Fill 10 nodes
	std::vector<Node> nodes;
	for (int i = 0; i < 10; i++) {
		Node n(h.start_id + i, 10);
		dataManager->addNode(n);
		segmentTracker->setEntityActive(offset, i, true);
		nodes.push_back(n);
	}
	segmentTracker->updateSegmentUsage(offset, 10, 0);

	// Delete 1 node (10% < 30%)
	deletionManager->deleteNode(nodes[0]);

	SegmentHeader hAfter = segmentTracker->getSegmentHeader(offset);
	EXPECT_EQ(hAfter.needs_compaction, 0U);
	EXPECT_DOUBLE_EQ(hAfter.getFragmentationRatio(), 0.1);
}

// =========================================================================
// Tests for Fragmentation Analysis
// =========================================================================

TEST_F(DeletionManagerTest, AnalyzeFragmentation) {
	uint32_t type = Node::typeId;

	// Setup Segment 1: 50% Fragmented
	uint64_t seg1 = spaceManager->allocateSegment(type, 10);
	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	for (int i = 0; i < 10; i++) {
		segmentTracker->setEntityActive(seg1, i, true);
	}
	segmentTracker->updateSegmentUsage(seg1, 10, 0);

	// Manually deactivate 5 via tracker ( simulating deletions)
	for (int i = 0; i < 5; i++) {
		segmentTracker->setEntityActive(seg1, i, false);
	}

	// Setup Segment 2: 0% Fragmented
	uint64_t seg2 = spaceManager->allocateSegment(type, 10);
	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	for (int i = 0; i < 10; i++) {
		segmentTracker->setEntityActive(seg2, i, true);
	}
	segmentTracker->updateSegmentUsage(seg2, 10, 0);

	// Run Analysis
	auto fragMap = deletionManager->analyzeSegmentFragmentation(type);

	ASSERT_TRUE(fragMap.contains(seg1));
	ASSERT_TRUE(fragMap.contains(seg2));

	EXPECT_DOUBLE_EQ(fragMap[seg1], 0.5);
	EXPECT_DOUBLE_EQ(fragMap[seg2], 0.0);
}

// =========================================================================
// Edge Case Tests
// =========================================================================

TEST_F(DeletionManagerTest, DeleteZeroId) {
	// Should gracefully handle ID 0 (No-op)
	Node n(0, 10);
	deletionManager->deleteNode(n);

	Property p;
	p.setId(0);
	deletionManager->deleteProperty(p);

	// No assertions needed, just ensure no crash/exception
}

TEST_F(DeletionManagerTest, IsActive_NonExistentID) {
	// Check ID that hasn't been allocated
	EXPECT_FALSE(deletionManager->isNodeActive(99999));
	EXPECT_FALSE(deletionManager->isEdgeActive(99999));
}

TEST_F(DeletionManagerTest, DeletePropertyEntity_InvalidType) {
	// Pass an invalid storage type (not Property or Blob)
	// Though enum class makes this hard, we check logic robustness
	// Here we test safe property deletion call on non-existent ID
	deletionManager->deletePropertyEntity(0, PropertyStorageType::PROPERTY_ENTITY);
	// Should return safely
}

TEST_F(DeletionManagerTest, DeletePropertyEntity_ExceptionHandling) {
	// Simulate dataManager throwing exception during retrieval
	// This is hard to mock without mocking DataManager,
	// but we can pass an ID that exists in allocation but not in DataManager logic (if inconsistent)
	// Or just rely on the try/catch block in source.

	// Test: Valid ID but DataManager::getProperty throws (e.g. not found)
	// DeletionManager catches and logs to stderr.
	deletionManager->deletePropertyEntity(99999, PropertyStorageType::PROPERTY_ENTITY);
}
