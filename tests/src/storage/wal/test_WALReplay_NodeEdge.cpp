/**
 * @file test_WALReplay_NodeEdge.cpp
 * @date 2026/4/14
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include "storage/wal/WALReplayTestFixture.hpp"

// --- Delete recovery: committed DELETE in WAL → entity inactive after replay ---

TEST_F(WALReplayTest, DeleteRecoverySurvivesReopen) {
	int64_t nodeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		// Create and commit a node
		{
			auto txn = db.beginTransaction();
			Node n(0, 42);
			dm->addNode(n);
			nodeId = n.getId();
			txn.commit();
		}

		// Delete the node in a second transaction
		{
			auto txn = db.beginTransaction();
			Node toDelete = dm->getNode(nodeId);
			dm->deleteNode(toDelete);
			txn.commit();
		}

		db.close();
	}

	// Reopen — WAL replay should apply both the add and the delete
	{
		Database db2(dbPath.string());
		db2.open();

		Node retrieved = db2.getStorage()->getDataManager()->getNode(nodeId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted node should be inactive after WAL replay";

		db2.close();
	}
}

// --- Edge recovery: committed edge survives reopen ---

TEST_F(WALReplayTest, EdgeRecoverySurvivesReopen) {
	int64_t nodeId1 = 0, nodeId2 = 0, edgeId = 0;

	{
		Database db(dbPath.string());
		db.open();

		auto txn = db.beginTransaction();
		auto dm = db.getStorage()->getDataManager();

		Node n1(0, 1);
		dm->addNode(n1);
		nodeId1 = n1.getId();

		Node n2(0, 2);
		dm->addNode(n2);
		nodeId2 = n2.getId();

		Edge e(0, nodeId1, nodeId2, 1);
		dm->addEdge(e);
		edgeId = e.getId();

		txn.commit();
		db.close();
	}

	// Reopen — WAL replay should recover the edge
	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		Edge retrieved = dm->getEdge(edgeId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed edge should be visible after WAL replay";
		EXPECT_EQ(retrieved.getSourceNodeId(), nodeId1);
		EXPECT_EQ(retrieved.getTargetNodeId(), nodeId2);

		db2.close();
	}
}

// --- Edge deletion recovery ---

TEST_F(WALReplayTest, EdgeDeletionRecoverySurvivesReopen) {
	int64_t nodeId1 = 0, nodeId2 = 0, edgeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		// Create nodes and edge
		{
			auto txn = db.beginTransaction();
			Node n1(0, 1);
			dm->addNode(n1);
			nodeId1 = n1.getId();
			Node n2(0, 2);
			dm->addNode(n2);
			nodeId2 = n2.getId();
			Edge e(0, nodeId1, nodeId2, 1);
			dm->addEdge(e);
			edgeId = e.getId();
			txn.commit();
		}

		// Delete the edge
		{
			auto txn = db.beginTransaction();
			Edge toDelete = dm->getEdge(edgeId);
			dm->deleteEdge(toDelete);
			txn.commit();
		}

		db.close();
	}

	// Reopen with WAL recovery
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Edge retrieved = dm->getEdge(edgeId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted edge should be inactive after WAL replay";

		db2.close();
	}
}

// --- Node modification with property update recovery ---

TEST_F(WALReplayTest, NodeModificationRecoverySurvivesReopen) {
	int64_t nodeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			Node n(0, 50);
			dm->addNode(n);
			nodeId = n.getId();
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			dm->addNodeProperties(nodeId, {{"status", PropertyValue(std::string("modified"))}});
			txn.commit();
		}

		db.close();
	}

	// Reopen with WAL recovery
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Node retrieved = dm->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified node should be recovered";

		auto props = dm->getNodeProperties(nodeId);
		auto it = props.find("status");
		EXPECT_NE(it, props.end()) << "Modified property should be present";
		if (it != props.end()) {
			EXPECT_EQ(std::get<std::string>(it->second.getVariant()), "modified");
		}

		db2.close();
	}
}

// --- Delete node with properties: cascading delete recovery ---

TEST_F(WALReplayTest, DeleteNodeWithPropertiesRecovery) {
	int64_t nodeId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			Node n(0, 1);
			dm->addNode(n);
			nodeId = n.getId();
			dm->addNodeProperties(nodeId, {{"prop1", PropertyValue(std::string("val1"))},
											{"prop2", PropertyValue(static_cast<int64_t>(99))}});
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			Node toDelete = dm->getNode(nodeId);
			dm->deleteNode(toDelete);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Node retrieved = dm->getNode(nodeId);
		EXPECT_FALSE(retrieved.isActive()) << "Deleted node should be inactive after WAL replay";

		db2.close();
	}
}

// --- Complex graph: nodes + edges + properties + deletions across multiple transactions ---

TEST_F(WALReplayTest, ComplexGraphRecovery) {
	int64_t n1Id = 0, n2Id = 0, n3Id = 0;
	int64_t e1Id = 0, e2Id = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		// Transaction 1: Create 3 nodes and 2 edges with properties
		{
			auto txn = db.beginTransaction();

			Node n1(0, 1);
			dm->addNode(n1);
			n1Id = n1.getId();

			Node n2(0, 2);
			dm->addNode(n2);
			n2Id = n2.getId();

			Node n3(0, 3);
			dm->addNode(n3);
			n3Id = n3.getId();

			Edge e1(0, n1Id, n2Id, 10);
			dm->addEdge(e1);
			e1Id = e1.getId();

			Edge e2(0, n2Id, n3Id, 20);
			dm->addEdge(e2);
			e2Id = e2.getId();

			dm->addNodeProperties(n1Id, {{"name", PropertyValue(std::string("A"))}});
			dm->addEdgeProperties(e1Id, {{"type", PropertyValue(std::string("connects"))}});

			txn.commit();
		}

		// Transaction 2: Delete one edge
		{
			auto txn = db.beginTransaction();
			Edge toDel = dm->getEdge(e2Id);
			dm->deleteEdge(toDel);
			txn.commit();
		}

		// Transaction 3: Delete one node
		{
			auto txn = db.beginTransaction();
			Node toDel = dm->getNode(n3Id);
			dm->deleteNode(toDel);
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		EXPECT_TRUE(dm->getNode(n1Id).isActive()) << "Node 1 should be active";
		EXPECT_TRUE(dm->getNode(n2Id).isActive()) << "Node 2 should be active";
		EXPECT_FALSE(dm->getNode(n3Id).isActive()) << "Node 3 should be deleted";

		EXPECT_TRUE(dm->getEdge(e1Id).isActive()) << "Edge 1 should be active";
		EXPECT_FALSE(dm->getEdge(e2Id).isActive()) << "Edge 2 should be deleted";

		auto n1Props = dm->getNodeProperties(n1Id);
		EXPECT_NE(n1Props.find("name"), n1Props.end()) << "Node 1 property should be recovered";

		auto e1Props = dm->getEdgeProperties(e1Id);
		EXPECT_NE(e1Props.find("type"), e1Props.end()) << "Edge 1 property should be recovered";

		db2.close();
	}
}

// --- Interleaved committed and rolled-back nodes: only committed survive ---

TEST_F(WALReplayTest, InterleavedCommitRollbackNodeRecovery) {
	int64_t committedId1 = 0, committedId2 = 0, rolledBackId = 0;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		{
			auto txn = db.beginTransaction();
			Node n(0, 1);
			dm->addNode(n);
			committedId1 = n.getId();
			txn.commit();
		}

		{
			auto txn = db.beginTransaction();
			Node n(0, 99);
			dm->addNode(n);
			rolledBackId = n.getId();
			txn.rollback();
		}

		{
			auto txn = db.beginTransaction();
			Node n(0, 2);
			dm->addNode(n);
			committedId2 = n.getId();
			txn.commit();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		auto dm = db2.getStorage()->getDataManager();

		Node r1 = dm->getNode(committedId1);
		EXPECT_TRUE(r1.isActive()) << "First committed node should survive";
		EXPECT_EQ(r1.getLabelId(), 1);

		Node r2 = dm->getNode(committedId2);
		EXPECT_TRUE(r2.isActive()) << "Second committed node should survive";
		EXPECT_EQ(r2.getLabelId(), 2);

		if (rolledBackId > 0) {
			Node rb = dm->getNode(rolledBackId);
			EXPECT_FALSE(rb.isActive()) << "Rolled-back node should not survive";
		}

		db2.close();
	}
}

// --- Inject entity with empty data (dataSize=0) → replayAddOrModify early return ---

TEST_F(WALReplayTest, InjectedEmptyDataEntitySkippedGracefully) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Inject WAL records with empty serialized data for each entity type
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 10008;
		mgr.writeBegin(txnId);

		// Empty data for each entity type - ADDED change type → replayAddOrModify with empty data
		std::vector<uint8_t> emptyData;
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, emptyData);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, emptyData);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, emptyData);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, emptyData);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, emptyData);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  100, emptyData);

		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	// Reopen — recovery should handle empty data gracefully (early return)
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// --- Inject entity with empty data for DELETE → replayDelete early return ---

TEST_F(WALReplayTest, InjectedEmptyDataDeleteSkippedGracefully) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Inject WAL DELETE records with empty serialized data
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 10009;
		mgr.writeBegin(txnId);

		std::vector<uint8_t> emptyData;
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  200, emptyData);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  200, emptyData);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  200, emptyData);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  200, emptyData);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  200, emptyData);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  200, emptyData);

		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// --- Inject DELETE for entity that was never persisted (segmentOffset==0) ---

TEST_F(WALReplayTest, InjectedDeleteForNonPersistedEntitySkipped) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Inject DELETE WAL records for entities that don't exist in the DB
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 10010;
		mgr.writeBegin(txnId);

		// Create serialized entities with IDs that don't exist in DB
		Node fakeNode(99999, 1);
		auto nodeSer = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeNode, Node::getTotalSize());
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99999, std::vector<uint8_t>(nodeSer.begin(), nodeSer.end()));

		Edge fakeEdge(99999, 1, 2, 1);
		auto edgeSer = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeEdge, Edge::getTotalSize());
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99999, std::vector<uint8_t>(edgeSer.begin(), edgeSer.end()));

		Property fakeProp(99999, 1, 0);
		auto propSer = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeProp, Property::getTotalSize());
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99999, std::vector<uint8_t>(propSer.begin(), propSer.end()));

		Blob fakeBlob(99999, "fake");
		auto blobSer = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeBlob, Blob::getTotalSize());
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99999, std::vector<uint8_t>(blobSer.begin(), blobSer.end()));

		Index fakeIdx(99999, Index::NodeType::LEAF, 1);
		auto idxSer = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeIdx, Index::getTotalSize());
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99999, std::vector<uint8_t>(idxSer.begin(), idxSer.end()));

		State fakeSt(99999, "fake_key", "fake_val");
		auto stSer = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeSt, State::getTotalSize());
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99999, std::vector<uint8_t>(stSer.begin(), stSer.end()));

		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// --- Inject unknown entity type → default case in replayEntity ---

TEST_F(WALReplayTest, InjectedUnknownEntityTypeSkipped) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Inject a WAL record with an invalid entity type (255)
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		// Use raw data - serialize a fake node but with wrong entity type
		Node fakeNode(1, 1);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(fakeNode, Node::getTotalSize());

		uint64_t txnId = 10011;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, 255, // unknown entity type
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  1, std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// --- Inject modify for existing entity (segmentOffset != 0) for all types ---

TEST_F(WALReplayTest, InjectedModifyExistingBlobRecoveryNodeEdge) {
	int64_t blobId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Blob blob(0, "original blob");
		dm->addBlobEntity(blob);
		blobId = blob.getId();
		txn.commit();
		db.close();
	}

	// Inject a MODIFY WAL record for the existing blob
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Blob modifiedBlob(blobId, "modified blob data");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(modifiedBlob, Blob::getTotalSize());

		uint64_t txnId = 10013;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  blobId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Blob retrieved = dm->getBlob(blobId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified blob should still be active";
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedModifyExistingIndexRecovery) {
	int64_t indexId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Index idx(0, Index::NodeType::LEAF, 1);
		dm->addIndexEntity(idx);
		indexId = idx.getId();
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Index modifiedIdx(indexId, Index::NodeType::INTERNAL, 2);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(modifiedIdx, Index::getTotalSize());

		uint64_t txnId = 10014;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  indexId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Index retrieved = dm->getIndex(indexId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified index should still be active";
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedModifyExistingStateRecovery) {
	int64_t stateId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		State st(0, "orig_key", "orig_val");
		dm->addStateEntity(st);
		stateId = st.getId();
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		State modifiedSt(stateId, "mod_key", "mod_val");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(modifiedSt, State::getTotalSize());

		uint64_t txnId = 10015;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  stateId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		State retrieved = dm->getState(stateId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified state should still be active";
		db2.close();
	}
}

TEST_F(WALReplayTest, InjectedModifyExistingPropertyRecoveryNodeEdge) {
	int64_t propId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);
		Property prop(0, n.getId(), 0);
		dm->addPropertyEntity(prop);
		propId = prop.getId();
		txn.commit();
		db.close();
	}

	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Property modifiedProp(propId, 1, 0);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(modifiedProp, Property::getTotalSize());

		uint64_t txnId = 10016;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  propId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();
		mgr.close();
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Property retrieved = dm->getProperty(propId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified property should still be active";
		db2.close();
	}
}
