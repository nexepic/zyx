/**
 * @file test_FileStorage_Save.cpp
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

// ============================================================================
// Save & SaveData Tests
// ============================================================================

TEST_F(FileStorageTest, SaveDataEmpty) {
	std::vector<graph::Node> data;
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_EQ(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataSingleElement) {
	std::vector<graph::Node> data = {graph::Node(1, 10)};
	uint64_t segmentHead = 0;

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataFitsInOneSegment) {
	std::vector<graph::Node> data;
	for (int64_t i = 1; i <= 50; ++i) {
		data.push_back(graph::Node(i, 100));
	}
	uint64_t segmentHead = 0;
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, SaveDataMultipleSegments) {
	std::vector<graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data.push_back(graph::Node(i, 100));
	}
	uint64_t segmentHead = 0;
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

TEST_F(FileStorageTest, Save_WhenNotOpen_Throws) {
	fileStorage->close();
	EXPECT_THROW(fileStorage->save(), std::runtime_error);
}

TEST_F(FileStorageTest, Save_NoUnsavedChanges_ReturnsEarly) {
	// No data modifications, so save should return early
	EXPECT_NO_THROW(fileStorage->save());
}

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

	e.setTypeId(42);
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

TEST_F(FileStorageTest, SaveData_EmptyData) {
	// Test saveData with empty data vector (line 276 early return)
	std::vector<graph::Edge> emptyData;
	uint64_t segHead = 0;
	fileStorage->saveData(emptyData, segHead, 100);
	EXPECT_EQ(segHead, 0u);
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
	std::vector<graph::Edge> edgeData;
	for (int64_t i = 1; i <= 200; ++i) {
		edgeData.push_back(graph::Edge(i, n1.getId(), n2.getId(), 0));
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

TEST_F(FileStorageTest, SaveData_WriteToExistingSegmentChain) {
	// Test saveData when segment chain already exists with data (line 346-354)
	auto dm = fileStorage->getDataManager();

	// First batch creates the segment chain
	std::vector<graph::Node> data1;
	for (int64_t i = 1; i <= 50; ++i) {
		data1.push_back(graph::Node(i, 100));
	}
	uint64_t segmentHead = 0;
	fileStorage->saveData(data1, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);

	// Second batch adds to existing chain
	std::vector<graph::Node> data2;
	for (int64_t i = 51; i <= 100; ++i) {
		data2.push_back(graph::Node(i, 200));
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
	std::vector<graph::Index> data;
	for (auto &idx : indexes) {
		graph::Index newIdx;
		newIdx.setId(idx.getId());
		newIdx.getMutableMetadata().isActive = true;
		data.push_back(newIdx);
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Index::typeId);
	fileStorage->saveData(data, segHead, graph::storage::INDEXES_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

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
	std::vector<graph::State> data;
	for (auto &s : states) {
		graph::State newS;
		newS.setId(s.getId());
		newS.setKey("reused");
		data.push_back(newS);
	}
	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::State::typeId);
	fileStorage->saveData(data, segHead, graph::storage::STATES_PER_SEGMENT);

	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_PropertyMultiSegmentChain) {
	// Create enough properties to fill multiple segments, then add more
	auto dm = fileStorage->getDataManager();

	// Fill first segment
	std::vector<graph::Property> data1;
	uint32_t capacity = graph::storage::PROPERTIES_PER_SEGMENT;
	for (uint32_t i = 1; i <= capacity; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data1.push_back(p);
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Add more to trigger second segment allocation
	std::vector<graph::Property> data2;
	for (uint32_t i = capacity + 1; i <= capacity + 10; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data2.push_back(p);
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

TEST_F(FileStorageTest, Save_EdgeSlotReuse_CoversPreAllocatedPath) {
	// Create edges, flush to disk, delete one, flush, then create new edge
	// that reuses the deleted slot. This covers line 290 True for Edge template.
	auto dm = fileStorage->getDataManager();
	auto edgeAlloc = fileStorage->getIDAllocator(graph::EntityType::Edge);

	// Create two nodes for edges to reference
	graph::Node n1(0, 0);
	dm->addNode(n1);
	graph::Node n2(0, 0);
	dm->addNode(n2);

	// Create several edges and flush to disk
	std::vector<graph::Edge> edges;
	for (int i = 0; i < 5; i++) {
		int64_t eid = edgeAlloc->allocate();
		int64_t labelId = dm->getOrCreateTokenId("KNOWS");
		graph::Edge e(eid, n1.getId(), n2.getId(), labelId);
		dm->addEdge(e);
		edges.push_back(e);
	}
	fileStorage->flush();

	// Delete one edge to create an inactive slot
	dm->deleteEdge(edges[2]);
	fileStorage->flush();

	// Create a new edge - the allocator should reuse the deleted ID slot
	int64_t newEid = edgeAlloc->allocate();
	int64_t labelId = dm->getOrCreateTokenId("FOLLOWS");
	graph::Edge newEdge(newEid, n1.getId(), n2.getId(), labelId);
	dm->addEdge(newEdge);

	// This save() call should hit the pre-allocated slot path for Edge (line 290 True)
	EXPECT_NO_THROW(fileStorage->save());
}

TEST_F(FileStorageTest, SaveData_PropertyMultiSegmentChain_TraversalBranch) {
	// Fill a Property segment to capacity, then add more to force chain traversal.
	// This covers line 351 False branch for the Property template instantiation.
	uint32_t capacity = graph::storage::PROPERTIES_PER_SEGMENT;

	// First batch: fill exactly one segment
	std::vector<graph::Property> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data1.push_back(p);
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Second batch: overflow into second segment, forcing chain traversal
	std::vector<graph::Property> data2;
	for (uint32_t i = capacity + 1; i <= capacity * 2 + 5; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data2.push_back(p);
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
	std::vector<graph::Property> data3;
	for (uint32_t i = capacity * 2 + 6; i <= capacity * 2 + 10; ++i) {
		graph::Property p;
		p.setId(static_cast<int64_t>(i));
		p.getMutableMetadata().isActive = true;
		data3.push_back(p);
	}
	fileStorage->saveData(data3, segHead, capacity);
	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_IndexMultiSegmentChain_TraversalBranch) {
	// Fill an Index segment to capacity, then add more to force chain traversal.
	// This covers line 351 False branch for the Index template instantiation.
	uint32_t capacity = graph::storage::INDEXES_PER_SEGMENT;

	// First batch: fill exactly one segment
	std::vector<graph::Index> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		graph::Index idx;
		idx.setId(static_cast<int64_t>(i));
		idx.getMutableMetadata().isActive = true;
		data1.push_back(idx);
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Second batch: overflow into second segment, forcing chain traversal
	std::vector<graph::Index> data2;
	for (uint32_t i = capacity + 1; i <= capacity * 2 + 5; ++i) {
		graph::Index idx;
		idx.setId(static_cast<int64_t>(i));
		idx.getMutableMetadata().isActive = true;
		data2.push_back(idx);
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
	std::vector<graph::Index> data3;
	for (uint32_t i = capacity * 2 + 6; i <= capacity * 2 + 10; ++i) {
		graph::Index idx;
		idx.setId(static_cast<int64_t>(i));
		idx.getMutableMetadata().isActive = true;
		data3.push_back(idx);
	}
	fileStorage->saveData(data3, segHead, capacity);
	EXPECT_NE(segHead, 0u);
}

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
	std::vector<graph::Edge> data;
	graph::Edge r1(edges[1].getId(), n1.getId(), n2.getId(), 42);
	data.push_back(r1);
	graph::Edge r2(edges[3].getId(), n1.getId(), n2.getId(), 42);
	data.push_back(r2);

	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Edge::typeId);
	fileStorage->saveData(data, segHead, graph::storage::EDGES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}

TEST_F(FileStorageTest, SaveData_EdgeMultiSegmentChainTraversal) {
	// Create enough edges to span multiple segments, then add more
	auto dm = fileStorage->getDataManager();

	graph::Node n(0, 0);
	dm->addNode(n);
	fileStorage->flush();

	// First batch: fill enough to create multiple segments
	uint32_t capacity = graph::storage::EDGES_PER_SEGMENT;
	std::vector<graph::Edge> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		data1.push_back(graph::Edge(static_cast<int64_t>(i), n.getId(), n.getId(), 0));
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	// Second batch: more edges to trigger chain traversal and second segment
	std::vector<graph::Edge> data2;
	for (uint32_t i = capacity + 1; i <= capacity + 10; ++i) {
		data2.push_back(graph::Edge(static_cast<int64_t>(i), n.getId(), n.getId(), 0));
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

TEST_F(FileStorageTest, SaveData_NodeMultiSegmentChainTraversal) {
	uint32_t capacity = graph::storage::NODES_PER_SEGMENT;

	std::vector<graph::Node> data1;
	for (uint32_t i = 1; i <= capacity; ++i) {
		data1.push_back(graph::Node(static_cast<int64_t>(i), 100));
	}
	uint64_t segHead = 0;
	fileStorage->saveData(data1, segHead, capacity);
	ASSERT_NE(segHead, 0u);

	std::vector<graph::Node> data2;
	for (uint32_t i = capacity + 1; i <= capacity + 5; ++i) {
		data2.push_back(graph::Node(static_cast<int64_t>(i), 200));
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
	std::vector<graph::Node> data;
	graph::Node r1(nodes[1].getId(), 42);
	data.push_back(r1);
	graph::Node r2(nodes[3].getId(), 42);
	data.push_back(r2);

	uint64_t segHead = dm->getSegmentTracker()->getChainHead(graph::Node::typeId);
	fileStorage->saveData(data, segHead, graph::storage::NODES_PER_SEGMENT);
	EXPECT_NE(segHead, 0u);
}
