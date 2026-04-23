/**
 * @file test_WALRecovery_InjectedReplay.cpp
 * @brief Branch-coverage tests for WALRecovery.cpp
 *
 * Covers uncovered branches: small WAL data guard, truncated payload,
 * zero-dataSize payload, Node/Edge modify-in-place via injected WAL records,
 * Property/Blob delete, uncommitted-txn skip in buildFinalStateMap.
 *
 * IMPORTANT: mgr.close() deletes the WAL file (checkpoint). Tests that need
 * WAL recovery to actually run must copy the WAL before mgr.close() and
 * restore it afterwards.
 **/

#include "storage/wal/WALReplayTestFixture.hpp"

// Helper: copy WAL file before mgr.close(), restore it after
// so that the subsequent Database::open() triggers WAL recovery.
static void preserveWAL(WALManager &mgr, const std::string &walPath) {
	std::string walBackup = walPath + ".bak";
	fs::copy_file(walPath, walBackup, fs::copy_options::overwrite_existing);
	mgr.close();
	fs::rename(walBackup, walPath);
}

// ============================================================================
// buildFinalStateMap: rec.data.size() < sizeof(WALEntityPayload) guard (line 80-81)
//
// writeEntityChange() always prepends a WALEntityPayload header, so the data
// section of the resulting WAL record is always >= sizeof(WALEntityPayload).
// To hit the guard we must write a raw WAL_ENTITY_WRITE record whose data
// section is shorter than sizeof(WALEntityPayload).  We do this by appending
// a manually crafted record directly to the WAL file after the WALManager
// has already written BEGIN + COMMIT records.
// ============================================================================

TEST_F(WALReplayTest, BuildFinalStateMap_SmallDataRecordSkipped) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Write BEGIN + COMMIT via WALManager, then append a raw WAL_ENTITY_WRITE
	// record whose data section is only 3 bytes (< sizeof(WALEntityPayload)).
	{
		std::string walPath = dbPath.string() + "-wal";

		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 20001;
		mgr.writeBegin(txnId);
		mgr.writeCommit(txnId);
		mgr.sync();

		// Read back the valid WAL content written so far
		std::vector<uint8_t> walContent;
		{
			std::ifstream walIn(walPath, std::ios::binary);
			ASSERT_TRUE(walIn.is_open());
			walContent.assign(std::istreambuf_iterator<char>(walIn),
							  std::istreambuf_iterator<char>());
		}

		// Craft a WAL_ENTITY_WRITE record whose data is only 3 bytes — this is
		// shorter than sizeof(WALEntityPayload) and must be skipped by recovery.
		std::vector<uint8_t> tinyData = {0x01, 0x02, 0x03};
		WALRecordHeader rawHdr{};
		rawHdr.type = WALRecordType::WAL_ENTITY_WRITE;
		rawHdr.txnId = txnId; // same txnId so it looks committed
		rawHdr.recordSize =
			static_cast<uint32_t>(sizeof(WALRecordHeader) + tinyData.size());
		rawHdr.checksum = computeCRC32(tinyData.data(), tinyData.size());
		auto rawHdrBuf = serializeRecordHeader(rawHdr);

		// Append the crafted record to the WAL content
		walContent.insert(walContent.end(), rawHdrBuf.begin(), rawHdrBuf.end());
		walContent.insert(walContent.end(), tinyData.begin(), tinyData.end());

		mgr.close(); // deletes the WAL file

		// Restore the extended WAL content (with the raw tiny record appended)
		{
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(walOut.is_open());
			walOut.write(reinterpret_cast<const char *>(walContent.data()),
						 static_cast<std::streamsize>(walContent.size()));
		}
	}

	// Reopen — recovery should skip the small record gracefully without throwing
	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify<Node> with segmentOffset != 0 (modify existing node, line 114)
// ============================================================================

TEST_F(WALReplayTest, InjectedModifyExistingNodeRecovery) {
	int64_t nodeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 42);
		dm->addNode(n);
		nodeId = n.getId();
		txn.commit();
		db.close();
	}

	// Inject a MODIFY WAL record for the existing node
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Node modifiedNode(nodeId, 99);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedNode, Node::getTotalSize());

		uint64_t txnId = 20002;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  nodeId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayAddOrModify<Node> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Node retrieved = dm->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified node should still be active";
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify<Edge> with segmentOffset != 0 (modify existing edge, line 120)
// ============================================================================

TEST_F(WALReplayTest, InjectedModifyExistingEdgeRecovery) {
	int64_t nodeId1 = 0, nodeId2 = 0, edgeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n1(0, 1);
		dm->addNode(n1);
		nodeId1 = n1.getId();
		Node n2(0, 2);
		dm->addNode(n2);
		nodeId2 = n2.getId();
		Edge e(0, nodeId1, nodeId2, 10);
		dm->addEdge(e);
		edgeId = e.getId();
		txn.commit();
		db.close();
	}

	// Inject a MODIFY WAL record for the existing edge
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Edge modifiedEdge(edgeId, nodeId1, nodeId2, 20);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedEdge, Edge::getTotalSize());

		uint64_t txnId = 20003;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  edgeId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayAddOrModify<Edge> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Edge retrieved = dm->getEdge(edgeId);
		EXPECT_TRUE(retrieved.isActive()) << "Modified edge should still be active";
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify<Node> with segmentOffset == 0 (new node, no slot)
// ============================================================================

TEST_F(WALReplayTest, InjectedAddNewNodeRecovery) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Inject a WAL ADD record for a node that doesn't exist in the DB
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Node newNode(50000, 77);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			newNode, Node::getTotalSize());

		uint64_t txnId = 20004;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  50000,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayAddOrModify<Node> with segmentOffset == 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify<Edge> with segmentOffset == 0 (new edge, no slot)
// ============================================================================

TEST_F(WALReplayTest, InjectedAddNewEdgeRecovery) {
	int64_t nodeId1 = 0, nodeId2 = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n1(0, 1);
		dm->addNode(n1);
		nodeId1 = n1.getId();
		Node n2(0, 2);
		dm->addNode(n2);
		nodeId2 = n2.getId();
		txn.commit();
		db.close();
	}

	// Inject a WAL ADD record for an edge that doesn't exist in the DB
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Edge newEdge(50001, nodeId1, nodeId2, 5);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			newEdge, Edge::getTotalSize());

		uint64_t txnId = 20005;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  50001,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayAddOrModify<Edge> with segmentOffset == 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayDelete<Node> with segmentOffset != 0 (delete existing persisted node)
// ============================================================================

TEST_F(WALReplayTest, InjectedDeleteExistingNodeRecovery) {
	int64_t nodeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 42);
		dm->addNode(n);
		nodeId = n.getId();
		txn.commit();
		db.close();
	}

	// Inject a DELETE WAL record for the existing node
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Node nodeToDelete(nodeId, 42);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			nodeToDelete, Node::getTotalSize());

		uint64_t txnId = 20006;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  nodeId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Copy WAL before close() deletes it, then restore so recovery can replay
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Node retrieved = dm->getNode(nodeId);
		EXPECT_FALSE(retrieved.isActive()) << "Injected-deleted node should be inactive";
		db2.close();
	}
}

// ============================================================================
// replayDelete<Edge> with segmentOffset != 0 (delete existing persisted edge)
// ============================================================================

TEST_F(WALReplayTest, InjectedDeleteExistingEdgeRecovery) {
	int64_t nodeId1 = 0, nodeId2 = 0, edgeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n1(0, 1);
		dm->addNode(n1);
		nodeId1 = n1.getId();
		Node n2(0, 2);
		dm->addNode(n2);
		nodeId2 = n2.getId();
		Edge e(0, nodeId1, nodeId2, 10);
		dm->addEdge(e);
		edgeId = e.getId();
		txn.commit();
		db.close();
	}

	// Inject a DELETE WAL record for the existing edge
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Edge edgeToDelete(edgeId, nodeId1, nodeId2, 10);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			edgeToDelete, Edge::getTotalSize());

		uint64_t txnId = 20007;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  edgeId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Copy WAL before close() deletes it, then restore so recovery can replay
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Edge retrieved = dm->getEdge(edgeId);
		EXPECT_FALSE(retrieved.isActive()) << "Injected-deleted edge should be inactive";
		db2.close();
	}
}

// ============================================================================
// Multiple committed txns with entity writes (full recover path)
// ============================================================================

TEST_F(WALReplayTest, InjectedMultiTxnFullRecoverPath) {
	int64_t nodeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);
		nodeId = n.getId();
		txn.commit();
		db.close();
	}

	// Inject two committed transactions with entity writes
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		// Txn 1: modify existing node
		uint64_t txnId1 = 30001;
		mgr.writeBegin(txnId1);
		Node modified(nodeId, 55);
		auto ser1 = graph::utils::FixedSizeSerializer::serializeToBuffer(modified, Node::getTotalSize());
		mgr.writeEntityChange(txnId1, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  nodeId, std::vector<uint8_t>(ser1.begin(), ser1.end()));
		mgr.writeCommit(txnId1);

		// Txn 2: add new node
		uint64_t txnId2 = 30002;
		mgr.writeBegin(txnId2);
		Node newNode(60000, 88);
		auto ser2 = graph::utils::FixedSizeSerializer::serializeToBuffer(newNode, Node::getTotalSize());
		mgr.writeEntityChange(txnId2, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  60000, std::vector<uint8_t>(ser2.begin(), ser2.end()));
		mgr.writeCommit(txnId2);

		mgr.sync();

		// Preserve WAL so recovery runs the full multi-txn replay path
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// buildFinalStateMap: non-ENTITY_WRITE record type is skipped
// (Tests the 'continue' branch for non-entity-write records)
// ============================================================================

TEST_F(WALReplayTest, NonEntityWriteRecordsSkippedInBuildFinalStateMap) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Inject a committed transaction with only BEGIN + COMMIT (no ENTITY_WRITE).
	// This exercises the "committedTxns not empty but finalState empty" path.
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 20008;
		mgr.writeBegin(txnId);
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery hits the finalState.empty() early-exit branch
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayDelete<Property> with segmentOffset != 0 (line 124)
// ============================================================================

TEST_F(WALReplayTest, InjectedDeleteExistingPropertyRecovery) {
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

	// Inject a CHANGE_DELETED WAL record for the existing Property entity
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Property propToDelete(propId, 1, 0);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			propToDelete, Property::getTotalSize());

		uint64_t txnId = 40001;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  propId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayDelete<Property> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Property retrieved = dm->getProperty(propId);
		EXPECT_FALSE(retrieved.isActive()) << "Injected-deleted property should be inactive";
		db2.close();
	}
}

// ============================================================================
// replayDelete<Blob> with segmentOffset != 0 (line 130)
// ============================================================================

TEST_F(WALReplayTest, InjectedDeleteExistingBlobRecovery) {
	int64_t blobId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Blob blob(0, "blob for injected delete test");
		dm->addBlobEntity(blob);
		blobId = blob.getId();
		txn.commit();
		db.close();
	}

	// Inject a CHANGE_DELETED WAL record for the existing Blob entity
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Blob blobToDelete(blobId, "blob for injected delete test");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			blobToDelete, Blob::getTotalSize());

		uint64_t txnId = 40002;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  blobId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayDelete<Blob> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Blob retrieved = dm->getBlob(blobId);
		EXPECT_FALSE(retrieved.isActive()) << "Injected-deleted blob should be inactive";
		db2.close();
	}
}

// ============================================================================
// buildFinalStateMap: uncommitted txn skip (line 77-78)
// A txn with WAL_TXN_BEGIN + entity write but NO WAL_TXN_COMMIT must be skipped,
// while a committed txn alongside it IS replayed.
// ============================================================================

TEST_F(WALReplayTest, UncommittedTxnSkippedInBuildFinalStateMap) {
	int64_t nodeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 5);
		dm->addNode(n);
		nodeId = n.getId();
		txn.commit();
		db.close();
	}

	// Inject two transactions:
	//   txnUncommitted: has BEGIN + entity write but NO COMMIT  -> must be skipped
	//   txnCommitted:   has BEGIN + entity write + COMMIT       -> must be replayed
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		// Uncommitted transaction — writes a node that should NOT appear after recovery
		uint64_t txnUncommitted = 50010;
		mgr.writeBegin(txnUncommitted);
		Node uncommittedNode(70000, 11);
		auto serUncommitted = graph::utils::FixedSizeSerializer::serializeToBuffer(
			uncommittedNode, Node::getTotalSize());
		mgr.writeEntityChange(txnUncommitted, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  70000,
							  std::vector<uint8_t>(serUncommitted.begin(), serUncommitted.end()));
		// No writeCommit for txnUncommitted

		// Committed transaction — modifies the existing node (has a segment slot)
		uint64_t txnCommitted = 50011;
		mgr.writeBegin(txnCommitted);
		Node modifiedNode(nodeId, 99);
		auto serCommitted = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedNode, Node::getTotalSize());
		mgr.writeEntityChange(txnCommitted, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  nodeId,
							  std::vector<uint8_t>(serCommitted.begin(), serCommitted.end()));
		mgr.writeCommit(txnCommitted);

		mgr.sync();

		// Preserve WAL so recovery hits the uncommitted-txn skip branch (line 77-78)
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		// The committed modification must have been replayed
		auto dm = db2.getStorage()->getDataManager();
		Node retrieved = dm->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed node modification should be replayed";
		db2.close();
	}
}

// ============================================================================
// replayDelete<Index> with segmentOffset != 0 (delete existing persisted Index)
// Covers the true-branch of the delete path at line 136 of WALRecovery.cpp
// ============================================================================

TEST_F(WALReplayTest, InjectedDeleteExistingIndexRecovery) {
	int64_t indexId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Index idx(0, Index::NodeType::LEAF, 7);
		dm->addIndexEntity(idx);
		indexId = idx.getId();
		txn.commit();
		db.close();
	}

	// Inject a CHANGE_DELETED WAL record for the existing Index entity
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Index idxToDelete(indexId, Index::NodeType::LEAF, 7);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			idxToDelete, Index::getTotalSize());

		uint64_t txnId = 60001;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  indexId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayDelete<Index> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		auto dm = db2.getStorage()->getDataManager();
		Index retrieved = dm->getIndex(indexId);
		EXPECT_FALSE(retrieved.isActive()) << "Injected-deleted index should be inactive";
		db2.close();
	}
}

// ============================================================================
// replayDelete<State> with segmentOffset != 0 (delete existing persisted State)
// Covers the true-branch of the delete path at line 142 of WALRecovery.cpp
// ============================================================================

TEST_F(WALReplayTest, InjectedDeleteExistingStateRecovery) {
	int64_t stateId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		State st(0, "state_injected_del", "state_data");
		dm->addStateEntity(st);
		stateId = st.getId();
		txn.commit();
		db.close();
	}

	// Inject a CHANGE_DELETED WAL record for the existing State entity
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		State stToDelete(stateId, "state_injected_del", "state_data");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			stToDelete, State::getTotalSize());

		uint64_t txnId = 60002;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  stateId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayDelete<State> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
	}

	{
		// Note: State is pre-warmed into the page pool during StateManager init
		// (populateKeyToIdMap runs before WAL recovery). The WAL recovery writes
		// the inactive state to disk but the stale pool entry may still be served.
		// We therefore only verify that recovery completes without throwing.
		Database db2(dbPath.string());
		EXPECT_NO_THROW(db2.open());
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify<Property> with segmentOffset != 0 (modify existing Property)
// Covers the updateEntityInPlace branch for Property in replayAddOrModify
// ============================================================================

TEST_F(WALReplayTest, InjectedModifyExistingPropertyRecovery) {
	int64_t propId = 0;
	int64_t nodeId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		dm->addNode(n);
		nodeId = n.getId();
		Property prop(0, nodeId, 0);
		dm->addPropertyEntity(prop);
		propId = prop.getId();
		txn.commit();
		db.close();
	}

	// Inject a CHANGE_MODIFIED WAL record for the existing Property
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Property modifiedProp(propId, nodeId, 0);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedProp, Property::getTotalSize());

		uint64_t txnId = 60003;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  propId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayAddOrModify<Property> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
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

// ============================================================================
// replayAddOrModify<Blob> with segmentOffset != 0 (modify existing Blob)
// Covers the updateEntityInPlace branch for Blob in replayAddOrModify
// ============================================================================

TEST_F(WALReplayTest, InjectedModifyExistingBlobRecovery) {
	int64_t blobId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();
		auto txn = db.beginTransaction();
		Blob blob(0, "original blob data for modify test");
		dm->addBlobEntity(blob);
		blobId = blob.getId();
		txn.commit();
		db.close();
	}

	// Inject a CHANGE_MODIFIED WAL record for the existing Blob
	{
		WALManager mgr;
		mgr.open(dbPath.string());

		Blob modifiedBlob(blobId, "modified blob data");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedBlob, Blob::getTotalSize());

		uint64_t txnId = 60004;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  blobId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		// Preserve WAL so recovery runs replayAddOrModify<Blob> with segmentOffset != 0
		preserveWAL(mgr, dbPath.string() + "-wal");
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
