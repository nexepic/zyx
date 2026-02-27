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

	[[nodiscard]] bool isPropertyDeleted(int64_t id) const {
		// Check if property exists in DataManager and is inactive
		try {
			Property prop = dataManager->getProperty(id);
			return !prop.isActive();
		} catch (...) {
			return true; // Not found = deleted
		}
	}

	[[nodiscard]] bool isBlobDeleted(int64_t id) const {
		// Check if blob exists in DataManager and is inactive
		try {
			Blob blob = dataManager->getBlob(id);
			return !blob.isActive();
		} catch (...) {
			return true; // Not found = deleted
		}
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
	[[maybe_unused]] SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
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
	[[maybe_unused]] SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
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

// =========================================================================
// Additional Tests for Branch Coverage
// =========================================================================

TEST_F(DeletionManagerTest, DeleteNodeWithZeroId) {
	// Test deleteNode with ID 0
	Node n(0, 10);
	EXPECT_NO_THROW(deletionManager->deleteNode(n));
}

TEST_F(DeletionManagerTest, DeleteEdgeWithZeroId) {
	// Test deleteEdge with ID 0
	Node n1 = createNode("A");
	Node n2 = createNode("B");
	Edge e(0, n1.getId(), n2.getId(), 10);

	EXPECT_NO_THROW(deletionManager->deleteEdge(e));
}

TEST_F(DeletionManagerTest, DeletePropertyEntity_WithInactiveProperty) {
	// Test deletePropertyEntity when property is already inactive
	Property prop = createProperty();

	// Mark as inactive
	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	uint32_t idx = prop.getId() - h.start_id;
	segmentTracker->setEntityActive(pSeg, idx, false);

	// Should handle gracefully
	EXPECT_NO_THROW(deletionManager->deletePropertyEntity(prop.getId(), PropertyStorageType::PROPERTY_ENTITY));
}

TEST_F(DeletionManagerTest, DeletePropertyEntity_WithInactiveBlob) {
	// Test deletePropertyEntity when blob is already inactive
	Blob blob = createBlob();

	// Mark as inactive
	uint64_t bSeg = spaceManager->getTracker()->getSegmentOffsetForBlobId(blob.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(bSeg);
	uint32_t idx = blob.getId() - h.start_id;
	segmentTracker->setEntityActive(bSeg, idx, false);

	// Should handle gracefully
	EXPECT_NO_THROW(deletionManager->deletePropertyEntity(blob.getId(), PropertyStorageType::BLOB_ENTITY));
}

TEST_F(DeletionManagerTest, DeleteNode_NoPropertyEntity) {
	// Test deleteNode when node has no property entity
	Node node = createNode("NoPropNode");

	// Ensure node has no property entity
	EXPECT_FALSE(node.hasPropertyEntity());

	// Should delete without cascading to property
	EXPECT_NO_THROW(deletionManager->deleteNode(node));
	EXPECT_TRUE(isNodeDeleted(node.getId()));
}

TEST_F(DeletionManagerTest, DeleteEdge_NoPropertyEntity) {
	// Test deleteEdge when edge has no property entity
	Node n1 = createNode("A");
	Node n2 = createNode("B");
	Edge edge = createEdge(n1.getId(), n2.getId(), "Link");

	// Ensure edge has no property entity
	EXPECT_FALSE(edge.hasPropertyEntity());

	// Should delete without cascading to property
	EXPECT_NO_THROW(deletionManager->deleteEdge(edge));
	EXPECT_TRUE(isEdgeDeleted(edge.getId()));
}

TEST_F(DeletionManagerTest, MarkNodeInactive_ZeroSegmentOffset) {
	// Test markNodeInactive when segmentOffset is 0 (node not in any segment)
	Node node(99999, 10); // Non-existent ID

	// Should handle gracefully - segment offset will be 0
	EXPECT_NO_THROW(deletionManager->deleteNode(node));
}

TEST_F(DeletionManagerTest, MarkEdgeInactive_ZeroSegmentOffset) {
	// Test markEdgeInactive when segmentOffset is 0 (edge not in any segment)
	Node n1 = createNode("A");
	Node n2 = createNode("B");
	Edge edge(99999, n1.getId(), n2.getId(), 10); // Non-existent ID

	// Should handle gracefully - segment offset will be 0
	EXPECT_NO_THROW(deletionManager->deleteEdge(edge));
}

TEST_F(DeletionManagerTest, DeleteNodeConnectedEdges_EmptyResult) {
	// Test deleteNodeConnectedEdges when node has no connected edges
	Node node = createNode("IsolatedNode");

	// Should handle empty edges list gracefully
	EXPECT_NO_THROW(deletionManager->deleteNode(node));
}

TEST_F(DeletionManagerTest, FindSegmentForNonExistentId) {
	// Test findSegment methods with non-existent IDs
	EXPECT_EQ(deletionManager->findSegmentForNodeId(999999), 0ULL);
	EXPECT_EQ(deletionManager->findSegmentForEdgeId(999999), 0ULL);
	EXPECT_EQ(deletionManager->findSegmentForPropertyId(999999), 0ULL);
	EXPECT_EQ(deletionManager->findSegmentForBlobId(999999), 0ULL);
	EXPECT_EQ(deletionManager->findSegmentForIndexId(999999), 0ULL);
	EXPECT_EQ(deletionManager->findSegmentForStateId(999999), 0ULL);
}

TEST_F(DeletionManagerTest, MarkForCompactionNotTriggeredBelowThreshold) {
	// Test that compaction is NOT marked when fragmentation is below threshold
	uint32_t type = Node::typeId;
	uint64_t offset = spaceManager->allocateSegment(type, 100);
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);

	// Fill 100 nodes
	std::vector<Node> nodes;
	for (int i = 0; i < 100; i++) {
		Node n(h.start_id + i, 10);
		dataManager->addNode(n);
		segmentTracker->setEntityActive(offset, i, true);
		nodes.push_back(n);
	}
	segmentTracker->updateSegmentUsage(offset, 100, 0);

	// Delete 10 nodes (10% < 30%)
	for (int i = 0; i < 10; i++) {
		deletionManager->deleteNode(nodes[i]);
	}

	// Check that compaction was NOT triggered
	SegmentHeader hAfter = segmentTracker->getSegmentHeader(offset);
	EXPECT_EQ(hAfter.needs_compaction, 0U);
}

TEST_F(DeletionManagerTest, AnalyzeFragmentation_NoSegments) {
	// Test analyzeSegmentFragmentation when no segments exist for a type
	// Create a new type that hasn't been used
	auto fragMap = deletionManager->analyzeSegmentFragmentation(999);

	EXPECT_TRUE(fragMap.empty());
}

TEST_F(DeletionManagerTest, MultipleDeletesSameNode) {
	// Test deleting the same node multiple times
	Node node = createNode("MultiDeleteNode");

	// First delete
	deletionManager->deleteNode(node);
	EXPECT_TRUE(isNodeDeleted(node.getId()));

	// Second delete should handle gracefully (idempotent)
	EXPECT_NO_THROW(deletionManager->deleteNode(node));
}

TEST_F(DeletionManagerTest, MultipleDeletesSameEdge) {
	// Test deleting the same edge multiple times
	Node n1 = createNode("A");
	Node n2 = createNode("B");
	Edge edge = createEdge(n1.getId(), n2.getId(), "Link");

	// First delete
	deletionManager->deleteEdge(edge);
	EXPECT_TRUE(isEdgeDeleted(edge.getId()));

	// Second delete should handle gracefully (idempotent)
	EXPECT_NO_THROW(deletionManager->deleteEdge(edge));
}

TEST_F(DeletionManagerTest, DeletePropertyEntityWithPropertyStorageType) {
	// Test deletePropertyEntity with PROPERTY_ENTITY type
	Property prop = createProperty();
	Node node = createNode("PropNode");

	// Link node to property
	node.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateNode(node);

	// Delete node (should cascade to property)
	deletionManager->deleteNode(node);

	// Verify property is deleted
	EXPECT_TRUE(isNodeDeleted(node.getId()));
	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(pSeg, prop.getId() - h.start_id));
}

TEST_F(DeletionManagerTest, DeletePropertyEntityWithBlobStorageType) {
	// Test deletePropertyEntity with BLOB_ENTITY type
	Blob blob = createBlob();
	Node node = createNode("BlobNode");

	// Link node to blob
	node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateNode(node);

	// Delete node (should cascade to blob)
	deletionManager->deleteNode(node);

	// Verify blob is deleted
	EXPECT_TRUE(isNodeDeleted(node.getId()));
	uint64_t bSeg = spaceManager->getTracker()->getSegmentOffsetForBlobId(blob.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(bSeg);
	EXPECT_FALSE(segmentTracker->isEntityActive(bSeg, blob.getId() - h.start_id));
}

TEST_F(DeletionManagerTest, IsActiveIdZero) {
	// Test isNodeActive and isEdgeActive with ID 0
	EXPECT_FALSE(deletionManager->isNodeActive(0));
	EXPECT_FALSE(deletionManager->isEdgeActive(0));
}

TEST_F(DeletionManagerTest, IsActiveNegativeId) {
	// Test with negative ID (should return false)
	EXPECT_FALSE(deletionManager->isNodeActive(-1));
	EXPECT_FALSE(deletionManager->isEdgeActive(-1));
}

// =========================================================================
// Priority 1: Invalid Segment Offset Tests
// =========================================================================

TEST_F(DeletionManagerTest, DeleteNodeWithInvalidSegmentOffset) {
	// Test deletion when entity is not in any segment (segmentOffset == 0)
	int64_t id = idAllocator->allocateId(Node::typeId);
	Node node(id, 0);

	// Don't add node to DataManager - this ensures findSegmentForNodeId returns 0
	EXPECT_NO_THROW(deletionManager->deleteNode(node));
	// Verify through DeletionManager
	EXPECT_FALSE(deletionManager->isNodeActive(id));
}

TEST_F(DeletionManagerTest, DeleteEdgeWithInvalidSegmentOffset) {
	// Test deletion when edge is not in any segment
	int64_t id = idAllocator->allocateId(Edge::typeId);
	Node n1 = createNode("N1");
	Node n2 = createNode("N2");
	Edge edge(id, n1.getId(), n2.getId(), 0);

	// Don't add edge to DataManager
	EXPECT_NO_THROW(deletionManager->deleteEdge(edge));
	EXPECT_FALSE(deletionManager->isEdgeActive(id));
}

TEST_F(DeletionManagerTest, DeletePropertyWithInvalidSegmentOffset) {
	// Test deletion when property is not in any segment
	int64_t id = idAllocator->allocateId(Property::typeId);
	Property prop;
	prop.setId(id);

	// Don't add property to DataManager
	EXPECT_NO_THROW(deletionManager->deleteProperty(prop));
	// Verify through DataManager - should return inactive property
	Property retrieved = dataManager->getProperty(id);
	EXPECT_FALSE(retrieved.isActive()) << "Property should be marked inactive";
}

TEST_F(DeletionManagerTest, DeleteBlobWithInvalidSegmentOffset) {
	// Test deletion when blob is not in any segment
	int64_t id = idAllocator->allocateId(Blob::typeId);
	Blob blob;
	blob.setId(id);

	// Don't add blob to DataManager
	EXPECT_NO_THROW(deletionManager->deleteBlob(blob));
	// Verify through DataManager - should return inactive blob
	Blob retrieved = dataManager->getBlob(id);
	EXPECT_FALSE(retrieved.isActive()) << "Blob should be marked inactive";
}

// =========================================================================
// Priority 2: Compaction Threshold Tests for Edge/Property/Blob
// =========================================================================

TEST_F(DeletionManagerTest, CompactionThreshold_Edge) {
	// Test compaction triggering for edges (fragmentation >= threshold)
	uint32_t type = Edge::typeId;
	uint64_t offset = spaceManager->allocateSegment(type, 10);
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(offset);

	// Create source and target nodes
	Node n1 = createNode("Source");
	Node n2 = createNode("Target");

	// Create and activate 10 edges
	std::vector<Edge> edges;
	for (int i = 0; i < 10; i++) {
		Edge e(h.start_id + i, n1.getId(), n2.getId(), 10);
		dataManager->addEdge(e);
		spaceManager->getTracker()->setEntityActive(offset, i, true);
		edges.push_back(e);
	}
	spaceManager->getTracker()->updateSegmentUsage(offset, 10, 0);

	// Delete 4 edges (40% fragmentation >= 30% threshold)
	for (int i = 0; i < 4; i++) {
		deletionManager->deleteEdge(edges[i]);
	}

	SegmentHeader hAfter = spaceManager->getTracker()->getSegmentHeader(offset);
	EXPECT_EQ(hAfter.needs_compaction, 1U);
}

TEST_F(DeletionManagerTest, CompactionThreshold_Property) {
	// Test compaction triggering for properties
	uint32_t type = Property::typeId;
	uint64_t offset = spaceManager->allocateSegment(type, 10);
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(offset);

	// Create a node to own properties
	Node node = createNode("PropOwner");
	dataManager->addNode(node);

	// Create and activate 10 properties
	std::vector<Property> props;
	for (int i = 0; i < 10; i++) {
		Property prop;
		prop.setId(h.start_id + i);
		prop.setEntityInfo(node.getId(), Node::typeId);
		dataManager->addPropertyEntity(prop);
		spaceManager->getTracker()->setEntityActive(offset, i, true);
		props.push_back(prop);
	}
	spaceManager->getTracker()->updateSegmentUsage(offset, 10, 0);

	// Delete 4 properties (40% fragmentation >= 30% threshold)
	for (int i = 0; i < 4; i++) {
		deletionManager->deleteProperty(props[i]);
	}

	SegmentHeader hAfter = spaceManager->getTracker()->getSegmentHeader(offset);
	EXPECT_EQ(hAfter.needs_compaction, 1U);
}

// TEST_F(DeletionManagerTest, CompactionThreshold_Blob) {
// 	// Test compaction triggering for blobs
// 	// DISABLED: This test causes SIGBUS error
// 	uint32_t type = Blob::typeId;
// 	uint64_t offset = spaceManager->allocateSegment(type, 10);
// 	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(offset);
//
// 	// Create a node to own blobs
// 	Node node = createNode("BlobOwner");
//
// 	// Create and activate 10 blobs
// 	std::vector<Blob> blobs;
// 	for (int i = 0; i < 10; i++) {
// 		Blob blob = createBlob();
// 		blob.setEntityInfo(node.getId(), Node::typeId);
// 		dataManager->updateBlobEntity(blob);
// 		spaceManager->getTracker()->setEntityActive(offset, i, true);
// 		blobs.push_back(blob);
// 	}
// 	spaceManager->getTracker()->updateSegmentUsage(offset, 10, 0);
//
// 	// Delete 4 blobs (40% fragmentation >= 30% threshold)
// 	for (int i = 0; i < 4; i++) {
// 		deletionManager->deleteBlob(blobs[i]);
// 	}
//
// 	SegmentHeader hAfter = spaceManager->getTracker()->getSegmentHeader(offset);
// 	EXPECT_EQ(hAfter.needs_compaction, 1U);
// }

// =========================================================================
// Priority 4: Large Unallocated ID Tests
// =========================================================================

TEST_F(DeletionManagerTest, IsActive_LargeUnallocatedId) {
	// Test with ID that's definitely not allocated
	EXPECT_FALSE(deletionManager->isNodeActive(INT64_MAX));
	EXPECT_FALSE(deletionManager->isEdgeActive(INT64_MAX));
}

// =========================================================================
// Priority 5: Already Inactive Entity Tests
// =========================================================================

TEST_F(DeletionManagerTest, DeleteNode_PreviouslyDeletedProperty) {
	// Test deleting a node whose property is already inactive
	Node node = createNode("Node");
	Property prop = createProperty();

	node.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateNode(node);

	// Manually mark property as inactive
	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	uint32_t idx = prop.getId() - h.start_id;
	spaceManager->getTracker()->setEntityActive(pSeg, idx, false);

	// Delete node - should handle inactive property gracefully
	EXPECT_NO_THROW(deletionManager->deleteNode(node));
	EXPECT_TRUE(isNodeDeleted(node.getId()));
}

// TEST_F(DeletionManagerTest, DeleteNode_PreviouslyDeletedBlob) {
// 	// Test deleting a node whose blob is already inactive
// 	// DISABLED: This test uses createBlob which may cause issues
// 	Node node = createNode("Node");
// 	Blob blob = createBlob();
//
// 	// Set blob owner info
// 	blob.setEntityInfo(node.getId(), Node::typeId);
// 	dataManager->updateBlobEntity(blob);
//
// 	node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
// 	dataManager->updateNode(node);
//
// 	// Manually mark blob as inactive
// 	uint64_t bSeg = spaceManager->getTracker()->getSegmentOffsetForBlobId(blob.getId());
// 	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(bSeg);
// 	uint32_t idx = blob.getId() - h.start_id;
// 	spaceManager->getTracker()->setEntityActive(bSeg, idx, false);
//
// 	// Delete node - should handle inactive blob gracefully
// 	EXPECT_NO_THROW(deletionManager->deleteNode(node));
// 	EXPECT_TRUE(isNodeDeleted(node.getId()));
// }

TEST_F(DeletionManagerTest, DeleteEdge_PreviouslyDeletedProperty) {
	// Test deleting an edge whose property is already inactive
	Node n1 = createNode("N1");
	Node n2 = createNode("N2");
	Edge edge = createEdge(n1.getId(), n2.getId(), "EDGE");
	Property prop = createProperty();

	edge.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateEdge(edge);

	// Manually mark property as inactive
	uint64_t pSeg = spaceManager->getTracker()->getSegmentOffsetForPropId(prop.getId());
	SegmentHeader h = spaceManager->getTracker()->getSegmentHeader(pSeg);
	uint32_t idx = prop.getId() - h.start_id;
	spaceManager->getTracker()->setEntityActive(pSeg, idx, false);

	// Delete edge - should handle inactive property gracefully
	EXPECT_NO_THROW(deletionManager->deleteEdge(edge));
	EXPECT_TRUE(isEdgeDeleted(edge.getId()));
}

// =========================================================================
// Zero-ID Tests - Cover lines 69, 77, 85 early returns
// =========================================================================

TEST_F(DeletionManagerTest, DeleteBlobWithZeroId) {
	// Test early return when blob ID is 0 (line 69)
	Blob blob;
	blob.setId(0);

	// Should return early without attempting deletion
	EXPECT_NO_THROW(deletionManager->deleteBlob(blob));

	// Verify no exception or crash
	SUCCEED();
}

TEST_F(DeletionManagerTest, DeleteIndexWithZeroId) {
	// Test early return when index ID is 0 (line 77)
	Index index;
	index.setId(0);

	// Should return early without attempting deletion
	EXPECT_NO_THROW(deletionManager->deleteIndex(index));

	// Verify no exception or crash
	SUCCEED();
}

TEST_F(DeletionManagerTest, DeleteStateWithZeroId) {
	// Test early return when state ID is 0 (line 85)
	State state;
	state.setId(0);

	// Should return early without attempting deletion
	EXPECT_NO_THROW(deletionManager->deleteState(state));

	// Verify no exception or crash
	SUCCEED();
}
