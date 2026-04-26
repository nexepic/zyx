/**
 * @file test_WALRecovery_EmptyAndSlotUpdate.cpp
 * @brief Branch coverage tests for WALRecovery.cpp targeting:
 *        - replayAddOrModify with empty data for all entity types (line 153)
 *        - replayDelete with empty data for all entity types (line 178)
 *        - replayAddOrModify<Index> existing slot update (line 168)
 *        - replayAddOrModify<State> existing slot update (line 168)
 *
 * IMPORTANT: Database::open() does NOT trigger WAL recovery.
 * Recovery is deferred to beginTransaction() / beginReadOnlyTransaction().
 * Each test must call beginReadOnlyTransaction() to trigger recovery.
 **/

#include "storage/wal/WALReplayTestFixture.hpp"

static void preserveWAL(WALManager &mgr, const std::string &walPath) {
	std::string walBackup = walPath + ".bak";
	fs::copy_file(walPath, walBackup, fs::copy_options::overwrite_existing);
	mgr.close();
	fs::rename(walBackup, walPath);
}

// ============================================================================
// replayAddOrModify<Index> with existing segment slot — update in place
// ============================================================================

TEST_F(WALReplayTest, ReplayModifyIndex_ExistingSlot) {
	int64_t indexId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		auto dm = db.getStorage()->getDataManager();

		Node n(0, 1);
		dm->addNode(n);

		Index idx(0, Index::NodeType::LEAF, 5);
		dm->addIndexEntity(idx);
		indexId = idx.getId();

		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		Index modifiedIdx(indexId, Index::NodeType::LEAF, 10);
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedIdx, Index::getTotalSize());

		uint64_t txnId = 90001;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  indexId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify<State> with existing segment slot — update in place
// ============================================================================

TEST_F(WALReplayTest, ReplayModifyState_ExistingSlot) {
	int64_t stateId = 0;
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		auto dm = db.getStorage()->getDataManager();

		Node n(0, 1);
		dm->addNode(n);

		State st(0, "wal_state_key", "initial_value");
		dm->addStateEntity(st);
		stateId = st.getId();

		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		State modifiedState(stateId, "wal_state_key", "modified_value");
		auto serialized = graph::utils::FixedSizeSerializer::serializeToBuffer(
			modifiedState, State::getTotalSize());

		uint64_t txnId = 90002;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_MODIFIED),
							  stateId,
							  std::vector<uint8_t>(serialized.begin(), serialized.end()));
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayAddOrModify with empty data for each entity type — returns early
// ============================================================================

TEST_F(WALReplayTest, ReplayAddIndex_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90003;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  99000, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayAddState_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90004;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  99001, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayAddBlob_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90007;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  99004, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayAddProperty_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90008;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  99005, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayAddEdge_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90009;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  99006, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayAddNode_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90010;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_ADDED),
							  99007, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

// ============================================================================
// replayDelete with empty data for each entity type — returns early
// ============================================================================

TEST_F(WALReplayTest, ReplayDeleteIndex_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90005;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Index::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99002, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayDeleteState_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90006;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, State::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99003, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayDeleteNode_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90011;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Node::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99008, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayDeleteEdge_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90012;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Edge::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99009, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayDeleteBlob_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90013;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Blob::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99010, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}

TEST_F(WALReplayTest, ReplayDeleteProperty_EmptyData_ReturnsEarly) {
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 1);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	{
		std::string walPath = dbPath.string() + "-wal";
		WALManager mgr;
		mgr.open(dbPath.string());

		uint64_t txnId = 90014;
		mgr.writeBegin(txnId);
		mgr.writeEntityChange(txnId, Property::typeId,
							  static_cast<uint8_t>(storage::EntityChangeType::CHANGE_DELETED),
							  99011, std::vector<uint8_t>());
		mgr.writeCommit(txnId);
		mgr.sync();

		preserveWAL(mgr, walPath);
	}

	{
		Database db2(dbPath.string());
		db2.open();
		{ auto txn = db2.beginReadOnlyTransaction(); txn.commit(); }
		db2.close();
	}
}
