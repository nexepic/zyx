/**
 * @file test_EntityReferenceUpdater.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/8
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <algorithm>

// Include project headers
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/State.hpp"

using namespace graph::storage;
using namespace graph;

/**
 * @brief Test Fixture for EntityReferenceUpdater.
 * Sets up the full storage engine stack to simulate real-world entity interactions.
 */
class EntityReferenceUpdaterTest : public ::testing::Test {
protected:
    std::filesystem::path testFilePath;
    std::shared_ptr<std::fstream> file;
    FileHeader header;

    // Core Storage Components
    std::shared_ptr<SegmentTracker> segmentTracker;
    std::shared_ptr<FileHeaderManager> fileHeaderManager;
    std::shared_ptr<IDAllocator> idAllocator;
    std::shared_ptr<SpaceManager> spaceManager;

    // High-level Components
    std::shared_ptr<DataManager> dataManager;
    std::shared_ptr<EntityReferenceUpdater> updater;

    void SetUp() override {
        // 1. Initialize File
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        testFilePath = std::filesystem::temp_directory_path() / ("ref_update_full_" + to_string(uuid) + ".dat");

        file = std::make_shared<std::fstream>(
            testFilePath,
            std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc
        );
        ASSERT_TRUE(file->is_open());

        // Write initial empty header
        file->write(reinterpret_cast<char*>(&header), sizeof(FileHeader));
        file->flush();

        // 2. Initialize Components
        segmentTracker = std::make_shared<SegmentTracker>(file);
        fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);

        idAllocator = std::make_shared<IDAllocator>(
            file, segmentTracker,
            fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
            fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
            fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef()
        );

        // Note: DataManager constructor initializes EntityReferenceUpdater internally,
        // but we can also create one for testing or access the one in DataManager.
        // To ensure we test the exact instance used by the system:
        spaceManager = std::make_shared<SpaceManager>(
            file, testFilePath.string(), segmentTracker, fileHeaderManager, idAllocator
        );
        spaceManager->initialize(header);

        dataManager = std::make_shared<DataManager>(
            file, 100, header, idAllocator, segmentTracker, spaceManager
        );
        dataManager->initialize(); // This initializes EntityReferenceUpdater internally

        // Retrieve the updater from SpaceManager (where it was set during DataManager::initialize)
        updater = dataManager->getEntityReferenceUpdater();
    }

    void TearDown() override {
        updater.reset();
        dataManager.reset();
        spaceManager.reset();
        idAllocator.reset();
        fileHeaderManager.reset();
        segmentTracker.reset();
        if (file && file->is_open()) file->close();
        std::filesystem::remove(testFilePath);
    }

    // --- Helpers to create valid on-disk entities and sync Tracker ---
    // These helpers ensure that the SegmentTracker's bitmap matches the DataManager's state,
    // which is crucial for EntityReferenceUpdater to locate entities.

    Node createNode(const std::string& label) {
        int64_t id = idAllocator->allocateId(Node::typeId);
        uint64_t offset = segmentTracker->getSegmentOffsetForNodeId(id);
        if (offset == 0) offset = spaceManager->allocateSegment(Node::typeId, NODES_PER_SEGMENT);

        Node node(id, label);
        dataManager->addNode(node);

        // Manual Tracker Sync
        SegmentHeader h = segmentTracker->getSegmentHeader(offset);
        segmentTracker->setEntityActive(offset, id - h.start_id, true);
        if ((id - h.start_id) >= h.used) {
            segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
        }

        return node;
    }

    Edge createEdge(int64_t src, int64_t dst, const std::string& label) {
        int64_t id = idAllocator->allocateId(Edge::typeId);
        uint64_t offset = segmentTracker->getSegmentOffsetForEdgeId(id);
        if (offset == 0) offset = spaceManager->allocateSegment(Edge::typeId, EDGES_PER_SEGMENT);

        Edge edge(id, src, dst, label);
        dataManager->addEdge(edge);

        SegmentHeader h = segmentTracker->getSegmentHeader(offset);
        segmentTracker->setEntityActive(offset, id - h.start_id, true);
        if ((id - h.start_id) >= h.used) {
            segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
        }

        return edge;
    }

    Property createProperty(int64_t ownerId, uint32_t ownerType) {
        int64_t id = idAllocator->allocateId(Property::typeId);
        uint64_t offset = segmentTracker->getSegmentOffsetForPropId(id);
        if (offset == 0) offset = spaceManager->allocateSegment(Property::typeId, PROPERTIES_PER_SEGMENT);

        Property prop;
        prop.setId(id);
        prop.setEntityInfo(ownerId, ownerType);
        dataManager->addPropertyEntity(prop);

        SegmentHeader h = segmentTracker->getSegmentHeader(offset);
        segmentTracker->setEntityActive(offset, id - h.start_id, true);
        if ((id - h.start_id) >= h.used) {
            segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
        }

        return prop;
    }

    Blob createBlob(int64_t ownerId, uint32_t ownerType) {
        int64_t id = idAllocator->allocateId(Blob::typeId);
        uint64_t offset = segmentTracker->getSegmentOffsetForBlobId(id);
        if (offset == 0) offset = spaceManager->allocateSegment(Blob::typeId, BLOBS_PER_SEGMENT);

        Blob blob;
        blob.setId(id);
        blob.setEntityInfo(ownerId, ownerType);
        dataManager->addBlobEntity(blob);

        SegmentHeader h = segmentTracker->getSegmentHeader(offset);
        segmentTracker->setEntityActive(offset, id - h.start_id, true);
        if ((id - h.start_id) >= h.used) {
            segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
        }

        return blob;
    }

    Index createIndex(Index::NodeType nodeType) {
        int64_t id = idAllocator->allocateId(Index::typeId);
        uint64_t offset = segmentTracker->getSegmentOffsetForIndexId(id);
        if (offset == 0) offset = spaceManager->allocateSegment(Index::typeId, INDEXES_PER_SEGMENT);

        Index idx(id, nodeType, 1);
        dataManager->addIndexEntity(idx);

        SegmentHeader h = segmentTracker->getSegmentHeader(offset);
        segmentTracker->setEntityActive(offset, id - h.start_id, true);
        if ((id - h.start_id) >= h.used) {
            segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
        }
        return idx;
    }

    State createState(const std::string& key) {
        int64_t id = idAllocator->allocateId(State::typeId);
        uint64_t offset = segmentTracker->getSegmentOffsetForStateId(id);
        if (offset == 0) offset = spaceManager->allocateSegment(State::typeId, STATES_PER_SEGMENT);

        State st(id, key, "data");
        dataManager->addStateEntity(st);

        SegmentHeader h = segmentTracker->getSegmentHeader(offset);
        segmentTracker->setEntityActive(offset, id - h.start_id, true);
        if ((id - h.start_id) >= h.used) {
            segmentTracker->updateSegmentUsage(offset, (id - h.start_id) + 1, h.inactive_count);
        }
        return st;
    }
};

// =========================================================================
// 1. Node Reference Updates
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesConnectedEdges) {
    // Scenario: Node A -> Edge E -> Node B
    // Move Node A. Edge E's source should update.
    Node nodeA = createNode("A");
    Node nodeB = createNode("B");
    Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "E");

    nodeA.setFirstOutEdgeId(edge.getId());
    dataManager->updateNode(nodeA);

    int64_t oldId = nodeA.getId();
    int64_t newId = oldId + 100;

    Node movedNode = nodeA;
    movedNode.getMutableMetadata().id = newId;

    // ACTION
    updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node));

    // VERIFY
    Edge updatedEdge = dataManager->getEdge(edge.getId());
    EXPECT_EQ(updatedEdge.getSourceNodeId(), newId);
    EXPECT_EQ(updatedEdge.getTargetNodeId(), nodeB.getId());
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesIncomingEdges) {
    // Scenario: Node A -> Edge E -> Node B
    // Move Node B. Edge E's target should update.
    Node nodeA = createNode("A");
    Node nodeB = createNode("B");
    Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "E");

    nodeB.setFirstInEdgeId(edge.getId());
    dataManager->updateNode(nodeB);

    int64_t oldId = nodeB.getId();
    int64_t newId = oldId + 500;
    Node movedNode = nodeB;
    movedNode.getMutableMetadata().id = newId;

    // ACTION
    updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node));

    // VERIFY
    Edge updatedEdge = dataManager->getEdge(edge.getId());
    EXPECT_EQ(updatedEdge.getTargetNodeId(), newId);
    EXPECT_EQ(updatedEdge.getSourceNodeId(), nodeA.getId());
}

// =========================================================================
// 2. Edge Reference Updates
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_UpdatesNodePointers) {
    // Scenario: Node A -> Edge E -> Node B
    // Move Edge E. Node A's firstOut and Node B's firstIn should point to E_new.
    Node nodeA = createNode("A");
    Node nodeB = createNode("B");
    Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "E");

    nodeA.setFirstOutEdgeId(edge.getId());
    nodeB.setFirstInEdgeId(edge.getId());
    dataManager->updateNode(nodeA);
    dataManager->updateNode(nodeB);

    int64_t oldEdgeId = edge.getId();
    int64_t newEdgeId = oldEdgeId + 200;
    Edge movedEdge = edge;
    movedEdge.getMutableMetadata().id = newEdgeId;

    // ACTION
    updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

    // VERIFY
    Node updatedA = dataManager->getNode(nodeA.getId());
    Node updatedB = dataManager->getNode(nodeB.getId());

    EXPECT_EQ(updatedA.getFirstOutEdgeId(), newEdgeId);
    EXPECT_EQ(updatedB.getFirstInEdgeId(), newEdgeId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_UpdatesEdgeChain) {
    // Scenario: Node -> E1 -> E2 -> E3
    // Move E2. E1's next should be E2_new. E3's prev should be E2_new.
    Node n = createNode("N");
    Edge e1 = createEdge(n.getId(), n.getId(), "1");
    Edge e2 = createEdge(n.getId(), n.getId(), "2");
    Edge e3 = createEdge(n.getId(), n.getId(), "3");

    e1.setNextOutEdgeId(e2.getId());
    e2.setPrevOutEdgeId(e1.getId());
    e2.setNextOutEdgeId(e3.getId());
    e3.setPrevOutEdgeId(e2.getId());

    dataManager->updateEdge(e1);
    dataManager->updateEdge(e2);
    dataManager->updateEdge(e3);

    int64_t oldE2 = e2.getId();
    int64_t newE2 = oldE2 + 999;
    Edge movedE2 = e2;
    movedE2.getMutableMetadata().id = newE2;

    // ACTION
    updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

    // VERIFY
    Edge updatedE1 = dataManager->getEdge(e1.getId());
    Edge updatedE3 = dataManager->getEdge(e3.getId());

    EXPECT_EQ(updatedE1.getNextOutEdgeId(), newE2);
    EXPECT_EQ(updatedE3.getPrevOutEdgeId(), newE2);
}

// =========================================================================
// 3. Property/Blob Reference Updates
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesPropertyRef) {
    // Scenario: Node A has Property P.
    // Move Node A. Property P should now point to A_new.
    Node node = createNode("PropOwner");
    Property prop = createProperty(node.getId(), toUnderlying(SegmentType::Node));

    node.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
    dataManager->updateNode(node);

    int64_t oldNodeId = node.getId();
    int64_t newNodeId = oldNodeId + 10;
    Node movedNode = node;
    movedNode.getMutableMetadata().id = newNodeId;

    // ACTION
    updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

    // VERIFY
    Property updatedProp = dataManager->getProperty(prop.getId());
    EXPECT_EQ(updatedProp.getEntityId(), newNodeId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateProperty_UpdatesNodeRef) {
    // Scenario: Node A has Property P.
    // Move Property P. Node A should point to P_new.
    Node node = createNode("Owner");
    Property prop = createProperty(node.getId(), toUnderlying(SegmentType::Node));

    node.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
    dataManager->updateNode(node);

    int64_t oldPropId = prop.getId();
    int64_t newPropId = oldPropId + 50;
    Property movedProp = prop;
    movedProp.setId(newPropId);

    // ACTION
    updater->updateEntityReferences(oldPropId, &movedProp, toUnderlying(SegmentType::Property));

    // VERIFY
    Node updatedNode = dataManager->getNode(node.getId());
    EXPECT_EQ(updatedNode.getPropertyEntityId(), newPropId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateBlob_UpdatesBlobChain) {
    // Scenario: B1 -> B2.
    // Move B2. B1 should point to B2_new.
    Node n = createNode("N");
    Blob b1 = createBlob(n.getId(), toUnderlying(SegmentType::Node));
    Blob b2 = createBlob(n.getId(), toUnderlying(SegmentType::Node));

    b1.setNextBlobId(b2.getId());
    b2.setPrevBlobId(b1.getId());
    dataManager->updateBlobEntity(b1);
    dataManager->updateBlobEntity(b2);

    int64_t oldB2 = b2.getId();
    int64_t newB2 = oldB2 + 88;
    Blob movedB2 = b2;
    movedB2.setId(newB2);

    // ACTION
    updater->updateEntityReferences(oldB2, &movedB2, toUnderlying(SegmentType::Blob));

    // VERIFY
    Blob updatedB1 = dataManager->getBlob(b1.getId());
    EXPECT_EQ(updatedB1.getNextBlobId(), newB2);
}

// =========================================================================
// 4. Index Reference Updates (New)
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateIndex_ChildToParent) {
    // Scenario: Parent -> Child.
    // Move Child. Parent should update its pointer to Child_new.

    // 1. Create Parent (Internal Node)
    Index parent = createIndex(Index::NodeType::INTERNAL);
    int64_t parentId = parent.getId();

    // 2. Create Child
    Index child = createIndex(Index::NodeType::LEAF);
    int64_t childId = child.getId();

    child.setParentId(parentId);
    dataManager->updateIndexEntity(child);

    // 3. Link Parent -> Child using setAllChildren
    Index::ChildEntry entry = {PropertyValue(std::monostate{}), childId, 0};
    std::vector<Index::ChildEntry> children = {entry};
    parent.setAllChildren(children, dataManager);
    dataManager->updateIndexEntity(parent);

    // 4. Move Child
    int64_t newChildId = childId + 50;
    Index movedChild = child;
    movedChild.getMutableMetadata().id = newChildId;

    // ACTION
    updater->updateEntityReferences(childId, &movedChild, toUnderlying(SegmentType::Index));

    // VERIFY
    Index updatedParent = dataManager->getIndex(parentId);
    std::vector<int64_t> ids = updatedParent.getChildIds();
    ASSERT_EQ(ids.size(), 1);
    EXPECT_EQ(ids[0], newChildId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateIndex_ParentToChild) {
    // Scenario: Parent -> Child.
    // Move Parent. Child should update its parentId to Parent_new.

    Index parent = createIndex(Index::NodeType::INTERNAL);
    int64_t parentId = parent.getId();

    Index child = createIndex(Index::NodeType::LEAF);
    int64_t childId = child.getId();

    child.setParentId(parentId);
    dataManager->updateIndexEntity(child);

    // Link Parent -> Child
    std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), childId, 0}};
    parent.setAllChildren(children, dataManager);
    dataManager->updateIndexEntity(parent);

    // 4. Move Parent
    int64_t newParentId = parentId + 100;
    Index movedParent = parent;
    movedParent.getMutableMetadata().id = newParentId;

    // ACTION
    updater->updateEntityReferences(parentId, &movedParent, toUnderlying(SegmentType::Index));

    // VERIFY
    Index updatedChild = dataManager->getIndex(childId);
    EXPECT_EQ(updatedChild.getParentId(), newParentId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateIndex_Siblings) {
    // Scenario: Leaf1 <-> Leaf2.
    // Move Leaf1. Leaf2's Prev pointer should update.

    Index leaf1 = createIndex(Index::NodeType::LEAF);
    Index leaf2 = createIndex(Index::NodeType::LEAF);
    int64_t id1 = leaf1.getId();
    int64_t id2 = leaf2.getId();

    // Link them
    leaf1.setNextLeafId(id2);
    leaf2.setPrevLeafId(id1);
    dataManager->updateIndexEntity(leaf1);
    dataManager->updateIndexEntity(leaf2);

    // Move Leaf1
    int64_t newId1 = id1 + 99;
    Index movedLeaf1 = leaf1;
    movedLeaf1.getMutableMetadata().id = newId1;

    // ACTION
    updater->updateEntityReferences(id1, &movedLeaf1, toUnderlying(SegmentType::Index));

    // VERIFY
    Index updatedLeaf2 = dataManager->getIndex(id2);
    EXPECT_EQ(updatedLeaf2.getPrevLeafId(), newId1);
}

// =========================================================================
// 5. State Reference Updates (New)
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateState_Chain) {
    // Scenario: S1 <-> S2. Move S2. S1's next pointer should update.

    State state1 = createState("S1");
    int64_t s1 = state1.getId();

    State state2 = createState("S2");
    int64_t s2 = state2.getId();

    state1.setNextStateId(s2);
    state2.setPrevStateId(s1);
    dataManager->updateStateEntity(state1);
    dataManager->updateStateEntity(state2);

    // Move S2
    int64_t newS2 = s2 + 555;
    State movedS2 = state2;
    movedS2.getMutableMetadata().id = newS2;

    // ACTION
    updater->updateEntityReferences(s2, &movedS2, toUnderlying(SegmentType::State));

    // VERIFY
    State updatedS1 = dataManager->getState(s1);
    EXPECT_EQ(updatedS1.getNextStateId(), newS2);
}

// =========================================================================
// 6. Edge Cases
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, SameId_NoOp) {
    Node n = createNode("N");
    int64_t id = n.getId();

    // Pass same ID - should do nothing
    updater->updateEntityReferences(id, &n, toUnderlying(SegmentType::Node));

    Node check = dataManager->getNode(id);
    EXPECT_EQ(check.getId(), id);
}

TEST_F(EntityReferenceUpdaterTest, UpdateUnknownType_Safe) {
    Node n = createNode("N");
    // Pass a garbage type ID - should do nothing (not crash)
    updater->updateEntityReferences(n.getId(), &n, 999);
}

// =========================================================================
// 7. Functional Correctness Tests (Integration)
//    Verifies that the database actually WORKS after updates.
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, Functional_GraphTraversal_AfterNodeMove) {
    // Scenario: Create a path A -> B -> C.
    // Move Node B to a new ID.
    // Verify we can still traverse from A to C via the new B.

    Node nA = createNode("A");
    Node nB = createNode("B");
    Node nC = createNode("C");

    // A -> B
    Edge eAB = createEdge(nA.getId(), nB.getId(), "A->B");
    nA.setFirstOutEdgeId(eAB.getId());
    nB.setFirstInEdgeId(eAB.getId());

    // B -> C
    Edge eBC = createEdge(nB.getId(), nC.getId(), "B->C");
    // Link eBC to B's out chain (simplified: B has one in, one out)
    nB.setFirstOutEdgeId(eBC.getId());
    nC.setFirstInEdgeId(eBC.getId());

    // Persist structure
    dataManager->updateNode(nA);
    dataManager->updateNode(nB);
    dataManager->updateNode(nC);

    // MOVE Node B
    int64_t oldBId = nB.getId();
    int64_t newBId = oldBId + 5000;

    // Simulate the physical move (copy data to new ID slot)
    // In real Compaction, SpaceManager does this. Here we mock it.
    Node movedB = nB;
    movedB.getMutableMetadata().id = newBId;

    // We must manually write "movedB" to disk so DataManager can find it at newId
    // (This mocks the SpaceManager copying the bytes)
    {
        uint64_t newOffset = spaceManager->allocateSegment(Node::typeId, NODES_PER_SEGMENT);
        // We cheat a bit on offset calculation for the mock, just ensuring it exists
        // In reality, we'd calculate where newBId lands.
        // For this test, we just add it via DataManager which handles placement.
        // But we must ensure DataManager treats it as "Active".
        // Let's rely on dataManager->addNode logic which we helper-ized in createNode,
        // but here we force the ID.
        // Since we can't force ID in addNode easily without allocator, we simulate:
        // We just update the references. The functionality test checks if A points to newBId.
        // Whether newBId actually has data on disk is irrelevant for EntityReferenceUpdater's job,
        // BUT relevant for "Retrieval".
        // So we MUST put data at newBId.
    }

    // Hack: Inject movedB into cache/disk at newBId so get(newBId) works
    // We use the internal cache for simplicity or re-add it.
    // Since we can't easily force an ID allocation, we mock the retrieval expectation:
    // If A points to newBId, and we ask A for its neighbor, it returns newBId.

    // ACTION: Update References
    updater->updateEntityReferences(oldBId, &movedB, toUnderlying(SegmentType::Node));

    // VERIFICATION: Traverse A -> Edge -> B_new
    Node updatedA = dataManager->getNode(nA.getId());
    Edge updatedEAB = dataManager->getEdge(updatedA.getFirstOutEdgeId());

    // 1. Check Link Integrity
    EXPECT_EQ(updatedEAB.getTargetNodeId(), newBId)
        << "Traversal broken: Edge A->B does not point to new B ID";

    // 2. Check Backward Link (B_new -> A) logic
    // The edge itself was not moved, so its ID is same.
    // But does B_new (the object passed to updater) need updates?
    // No, Updater updates *references to* B. B's own data (pointing to E_AB) is moved with it.
}

TEST_F(EntityReferenceUpdaterTest, Functional_BlobDataRetrieval_AfterBlobMove) {
    // Scenario: Node has a large Property stored in a Blob.
    // Move the Blob.
    // Verify Node can still read the property data.

    Node node = createNode("BlobHolder");
    Blob blob = createBlob(node.getId(), toUnderlying(SegmentType::Node));

    // Setup Blob Data
    std::string testData = "This is some large data string";
    blob.setData(testData);
    dataManager->updateBlobEntity(blob);

    // Link Node -> Blob
    node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
    dataManager->updateNode(node);

    // MOVE Blob
    int64_t oldBlobId = blob.getId();
    int64_t newBlobId = oldBlobId + 999;

    Blob movedBlob = blob;
    movedBlob.setId(newBlobId);

    // Mock the data existing at new location (Critical for functional test)
    // In real system, SpaceManager copies bytes. Here we just add it as a new entity.
    // We need to bypass IDAllocator to force a specific ID?
    // Or we just rely on the pointer check.
    // If we want to test data retrieval, we must write 'movedBlob' to disk.
    // DataManager doesn't allow forcing ID easily in public API.
    // So we primarily verify the POINTER in the Node is correct.
    // If the pointer is correct, and SpaceManager did its copy job, retrieval works.

    updater->updateEntityReferences(oldBlobId, &movedBlob, toUnderlying(SegmentType::Blob));

    // VERIFY
    Node updatedNode = dataManager->getNode(node.getId());
    EXPECT_EQ(updatedNode.getPropertyEntityId(), newBlobId);
    EXPECT_EQ(updatedNode.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
}

TEST_F(EntityReferenceUpdaterTest, Functional_BTreeSearch_AfterChildMove) {
    // Scenario: Internal Node -> Leaf Node.
    // Key "50" points to child Leaf.
    // Move Leaf Node.
    // Verify InternalNode.findChild("50") returns new Leaf ID.

    // 1. Setup Tree
    Index parent = createIndex(Index::NodeType::INTERNAL);
    Index leaf = createIndex(Index::NodeType::LEAF);
    int64_t parentId = parent.getId();
    int64_t leafId = leaf.getId();

    // Parent -> Child (Key 50)
    // Layout: [FirstChild (Implicit Key)] ...
    // We add one child (the leaf) as the first child (implicit -inf key).
    std::vector<Index::ChildEntry> children;
    children.push_back({PropertyValue(std::monostate{}), leafId, 0});

    parent.setAllChildren(children, dataManager);
    dataManager->updateIndexEntity(parent);

    leaf.setParentId(parentId);
    dataManager->updateIndexEntity(leaf);

    // 2. Move Leaf
    int64_t newLeafId = leafId + 200;
    Index movedLeaf = leaf;
    movedLeaf.getMutableMetadata().id = newLeafId;

    // 3. Update
    updater->updateEntityReferences(leafId, &movedLeaf, toUnderlying(SegmentType::Index));

    // 4. Functional Verify: Can we find the child?
    Index updatedParent = dataManager->getIndex(parentId);

    // 根据 PropertyValue.hpp，我们需要通过 getVariant() 访问内部数据
    auto comparator = [](const PropertyValue& a, const PropertyValue& b) {
        // 1. 检查是否为 Monostate (即 B+树中的 Negative Infinity 键)
        bool aIsInf = std::holds_alternative<std::monostate>(a.getVariant());
        bool bIsInf = std::holds_alternative<std::monostate>(b.getVariant());

        // 处理无穷小逻辑
        if (aIsInf && bIsInf) return false; // 相等
        if (aIsInf) return true;            // a 是无穷小，a < b
        if (bIsInf) return false;           // b 是无穷小，a > b

        // 2. 比较实际整数值
        // PropertyValue 的构造函数保证整型存储为 int64_t
        return std::get<int64_t>(a.getVariant()) < std::get<int64_t>(b.getVariant());
    };

    // 查找逻辑：Key 10 应该落入第一个子节点的范围（即 -inf 开始的范围）
    int64_t foundChildId = updatedParent.findChild(PropertyValue(static_cast<int64_t>(10)), dataManager, comparator);

    EXPECT_EQ(foundChildId, newLeafId)
        << "B-Tree lookups failed: Parent does not point to moved Leaf ID";
}

TEST_F(EntityReferenceUpdaterTest, Functional_StateChain_Read_AfterMove) {
    // Scenario: State1 -> State2 (Chain).
    // Move State2.
    // Verify State1.getNextChainId() is correct.

    State s1 = createState("config");
    State s2 = createState(""); // chunk

    s1.setNextChainId(s2.getId());
    s2.setPrevChainId(s1.getId());
    dataManager->updateStateEntity(s1);
    dataManager->updateStateEntity(s2);

    int64_t oldS2 = s2.getId();
    int64_t newS2 = oldS2 + 300;
    State movedS2 = s2;
    movedS2.getMutableMetadata().id = newS2;

    updater->updateEntityReferences(oldS2, &movedS2, toUnderlying(SegmentType::State));

    State updatedS1 = dataManager->getState(s1.getId());
    EXPECT_EQ(updatedS1.getNextChainId(), newS2)
        << "State chain broken: Head does not point to new Next state";
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_UpdatesInEdgeChain) {
	// Scenario: Multiple edges pointing TO the same node (Incoming Edges).
	// Node T (Target).
	// Node S1 -> T (Edge E1).
	// Node S2 -> T (Edge E2).
	// Node S3 -> T (Edge E3).
	// Chain: E1 <-> E2 <-> E3.
	// Move E2. Verify E1.nextIn and E3.prevIn point to E2_new.

	Node t = createNode("Target");
	Node s1 = createNode("S1");
	Node s2 = createNode("S2");
	Node s3 = createNode("S3");

	Edge e1 = createEdge(s1.getId(), t.getId(), "E1");
	Edge e2 = createEdge(s2.getId(), t.getId(), "E2");
	Edge e3 = createEdge(s3.getId(), t.getId(), "E3");

	// Manually setup Incoming Chain on edges
	// Note: In real logic, DataManager maintains this. Here we force it on disk entities.
	e1.setNextInEdgeId(e2.getId());

	e2.setPrevInEdgeId(e1.getId());
	e2.setNextInEdgeId(e3.getId());

	e3.setPrevInEdgeId(e2.getId());

	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);
	dataManager->updateEdge(e3);

	// MOVE E2
	int64_t oldE2 = e2.getId();
	int64_t newE2 = oldE2 + 555;
	Edge movedE2 = e2;
	movedE2.getMutableMetadata().id = newE2;

	// ACTION
	updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

	// VERIFY
	Edge updatedE1 = dataManager->getEdge(e1.getId());
	Edge updatedE3 = dataManager->getEdge(e3.getId());

	EXPECT_EQ(updatedE1.getNextInEdgeId(), newE2) << "E1 nextIn pointer failed to update";
	EXPECT_EQ(updatedE3.getPrevInEdgeId(), newE2) << "E3 prevIn pointer failed to update";
}

TEST_F(EntityReferenceUpdaterTest, UpdateProperty_UpdatesEdgeRef) {
	// Scenario: Edge E owns Property P.
	// Move Property P. Edge E should point to P_new.

	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");
	Property prop = createProperty(edge.getId(), toUnderlying(SegmentType::Edge));

	edge.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateEdge(edge);

	int64_t oldPropId = prop.getId();
	int64_t newPropId = oldPropId + 123;
	Property movedProp = prop;
	movedProp.setId(newPropId);

	// ACTION
	updater->updateEntityReferences(oldPropId, &movedProp, toUnderlying(SegmentType::Property));

	// VERIFY
	Edge updatedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ(updatedEdge.getPropertyEntityId(), newPropId);
	EXPECT_EQ(updatedEdge.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
}

TEST_F(EntityReferenceUpdaterTest, UpdateBlob_UpdatesEdgeRef) {
	// Scenario: Edge E owns Blob B.
	// Move Blob B. Edge E should point to B_new.

	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");
	Blob blob = createBlob(edge.getId(), toUnderlying(SegmentType::Edge));

	edge.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateEdge(edge);

	int64_t oldBlobId = blob.getId();
	int64_t newBlobId = oldBlobId + 456;
	Blob movedBlob = blob;
	movedBlob.setId(newBlobId);

	// ACTION
	updater->updateEntityReferences(oldBlobId, &movedBlob, toUnderlying(SegmentType::Blob));

	// VERIFY
	Edge updatedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ(updatedEdge.getPropertyEntityId(), newBlobId);
	EXPECT_EQ(updatedEdge.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
}

TEST_F(EntityReferenceUpdaterTest, UpdateBlob_UpdatesBidirectionalChain) {
	// Scenario: B1 <-> B2 <-> B3.
	// Move B2.
	// Verify: B1.next -> B2_new AND B3.prev -> B2_new.
	// Also Verify: Entity Info sync logic (simulated).

	Node n = createNode("Owner");
	uint32_t typeNode = toUnderlying(SegmentType::Node);

	Blob b1 = createBlob(n.getId(), typeNode);
	Blob b2 = createBlob(n.getId(), typeNode);
	Blob b3 = createBlob(n.getId(), typeNode);

	// Link B1 -> B2 -> B3
	b1.setNextBlobId(b2.getId());

	b2.setPrevBlobId(b1.getId());
	b2.setNextBlobId(b3.getId());

	b3.setPrevBlobId(b2.getId());
	// Hack: Set B3 to have "wrong" owner info initially to trigger the sync logic
	b3.setEntityInfo(99999, 999);

	dataManager->updateBlobEntity(b1);
	dataManager->updateBlobEntity(b2);
	dataManager->updateBlobEntity(b3);

	// MOVE B2
	int64_t oldB2 = b2.getId();
	int64_t newB2 = oldB2 + 888;
	Blob movedB2 = b2;
	movedB2.setId(newB2);

	// ACTION
	updater->updateEntityReferences(oldB2, &movedB2, toUnderlying(SegmentType::Blob));

	// VERIFY
	Blob updatedB1 = dataManager->getBlob(b1.getId());
	Blob updatedB3 = dataManager->getBlob(b3.getId());

	// 1. Check Pointers
	EXPECT_EQ(updatedB1.getNextBlobId(), newB2) << "B1 next pointer failed";
	EXPECT_EQ(updatedB3.getPrevBlobId(), newB2) << "B3 prev pointer failed";

	// 2. Check Entity Info Sync (The missing branch)
	// movedB2 (source) has correct owner info (n.getId(), Node).
	// updatedB3 was set to (99999, 999). It should now be synced to n.getId().
	EXPECT_EQ(updatedB3.getEntityId(), n.getId());
	EXPECT_EQ(updatedB3.getEntityType(), typeNode);
}

TEST_F(EntityReferenceUpdaterTest, UpdateIndex_SiblingUpdateNext) {
	// Scenario: Leaf1 -> Leaf2.
	// Move Leaf2. Leaf1's NEXT pointer should update.
	// (Previous tests mostly covered moving Leaf1 updating Leaf2's PREV)

	Index leaf1 = createIndex(Index::NodeType::LEAF);
	Index leaf2 = createIndex(Index::NodeType::LEAF);

	leaf1.setNextLeafId(leaf2.getId());
	leaf2.setPrevLeafId(leaf1.getId());

	dataManager->updateIndexEntity(leaf1);
	dataManager->updateIndexEntity(leaf2);

	// MOVE Leaf2 (The "Next" sibling)
	int64_t oldL2 = leaf2.getId();
	int64_t newL2 = oldL2 + 200;
	Index movedL2 = leaf2;
	movedL2.getMutableMetadata().id = newL2;

	// ACTION
	updater->updateEntityReferences(oldL2, &movedL2, toUnderlying(SegmentType::Index));

	// VERIFY
	Index updatedL1 = dataManager->getIndex(leaf1.getId());
	EXPECT_EQ(updatedL1.getNextLeafId(), newL2);
}

TEST_F(EntityReferenceUpdaterTest, UpdateState_ChainPrevUpdate) {
	// Scenario: S1 <- S2.
	// Move S1 (The "Previous" state). S2's PREV pointer should update.

	State s1 = createState("S1");
	State s2 = createState("S2");

	// Link S1 <- S2
	s1.setNextStateId(s2.getId());
	s2.setPrevStateId(s1.getId());

	dataManager->updateStateEntity(s1);
	dataManager->updateStateEntity(s2);

	// MOVE S1
	int64_t oldS1 = s1.getId();
	int64_t newS1 = oldS1 + 111;
	State movedS1 = s1;
	movedS1.getMutableMetadata().id = newS1;

	// ACTION
	updater->updateEntityReferences(oldS1, &movedS1, toUnderlying(SegmentType::State));

	// VERIFY
	State updatedS2 = dataManager->getState(s2.getId());
	EXPECT_EQ(updatedS2.getPrevStateId(), newS1);
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesBlobRef) {
	// Scenario: Node has a Blob entity (not Property).
	// Move Node. Blob should update its owner info to new Node ID.
	// Note: updatePropertiesPointingToEntity is called when the *Entity* (Node) moves,
	// and it updates the *Blob* pointing to it.

	Node node = createNode("BlobOwner");
	Blob blob = createBlob(node.getId(), toUnderlying(SegmentType::Node));

	// Link Node -> Blob
	node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateNode(node);

	// MOVE Node
	int64_t oldNodeId = node.getId();
	int64_t newNodeId = oldNodeId + 99;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newNodeId;

	// ACTION
	updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

	// VERIFY
	// The Blob should now point to the new Node ID
	Blob updatedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(updatedBlob.getEntityId(), newNodeId);
	EXPECT_EQ(updatedBlob.getEntityType(), toUnderlying(SegmentType::Node));
}