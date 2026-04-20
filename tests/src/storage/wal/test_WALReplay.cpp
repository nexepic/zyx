/**
 * @file test_WALReplay.cpp
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

#include "storage/wal/WALReplayTestFixture.hpp"

// ============================================================================
// Transaction-Level Recovery Tests
// ============================================================================

TEST_F(WALReplayTest, CommittedTransactionSurvivesReopen) {
	int64_t nodeId = 0;

	// Phase 1: Open DB, add a node, commit — then close WITHOUT checkpointing
	{
		Database db(dbPath.string());
		db.open();

		auto txn = db.beginTransaction();
		Node node(0, 77);
		db.getStorage()->getDataManager()->addNode(node);
		nodeId = node.getId();
		ASSERT_GT(nodeId, 0);
		txn.commit();

		// Close DB (WAL is NOT manually checkpointed so WAL file persists)
		db.close();
	}

	// Verify WAL file has data
	std::string walPath = dbPath.string() + "-wal";
	ASSERT_TRUE(fs::exists(walPath));
	EXPECT_GT(fs::file_size(walPath), sizeof(WALFileHeader));

	// Phase 2: Reopen DB — replayWAL() should re-apply the committed transaction
	{
		Database db2(dbPath.string());
		db2.open();

		// Check that the node committed in phase 1 is still visible
		Node retrieved = db2.getStorage()->getDataManager()->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive()) << "Committed node should be visible after WAL replay";
		EXPECT_EQ(retrieved.getLabelId(), 77);

		db2.close();
	}
}

TEST_F(WALReplayTest, RolledBackTransactionNotReplayed) {
	int64_t rolledBackNodeId = 0;

	{
		Database db(dbPath.string());
		db.open();

		// Commit a node so the DB is non-empty
		{
			auto txn = db.beginTransaction();
			Node n(0, 1);
			db.getStorage()->getDataManager()->addNode(n);
			txn.commit();
		}

		// Roll back another node
		{
			auto txn = db.beginTransaction();
			Node n(0, 2);
			db.getStorage()->getDataManager()->addNode(n);
			rolledBackNodeId = n.getId();
			txn.rollback();
		}

		db.close();
	}

	// Reopen and verify rolled-back node is absent
	{
		Database db2(dbPath.string());
		db2.open();

		if (rolledBackNodeId > 0) {
			Node retrieved = db2.getStorage()->getDataManager()->getNode(rolledBackNodeId);
			EXPECT_FALSE(retrieved.isActive()) << "Rolled-back node must not appear after reopen";
		}

		db2.close();
	}
}

TEST_F(WALReplayTest, OnlyCommittedTransactionsReplayed) {
	int64_t committedId = 0;
	int64_t uncommittedId = 0;

	{
		Database db(dbPath.string());
		db.open();

		// Committed
		{
			auto txn = db.beginTransaction();
			Node n(0, 10);
			db.getStorage()->getDataManager()->addNode(n);
			committedId = n.getId();
			txn.commit();
		}

		// Rolled back
		{
			auto txn = db.beginTransaction();
			Node n(0, 20);
			db.getStorage()->getDataManager()->addNode(n);
			uncommittedId = n.getId();
			txn.rollback();
		}

		db.close();
	}

	{
		Database db2(dbPath.string());
		db2.open();

		auto &dm = *db2.getStorage()->getDataManager();

		Node committed = dm.getNode(committedId);
		EXPECT_TRUE(committed.isActive()) << "Committed node must be visible";
		EXPECT_EQ(committed.getLabelId(), 10);

		if (uncommittedId > 0 && uncommittedId != committedId) {
			Node rolledBack = dm.getNode(uncommittedId);
			EXPECT_FALSE(rolledBack.isActive()) << "Rolled-back node must not be visible";
		}

		db2.close();
	}
}

TEST_F(WALReplayTest, CorruptedWALStopsAtCorruption) {
	std::string walPath;

	// Write a valid WAL with one committed transaction
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 5);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
		walPath = dbPath.string() + "-wal";
	}

	// Corrupt the tail of the WAL file by appending random bytes
	{
		std::ofstream wal(walPath, std::ios::binary | std::ios::app);
		std::vector<uint8_t> garbage = {0xFF, 0xFE, 0xFD, 0xAB, 0x00, 0x01};
		wal.write(reinterpret_cast<const char *>(garbage.data()),
				  static_cast<std::streamsize>(garbage.size()));
	}

	// Reopen with corrupted WAL — should not throw; corrupted records are skipped
	EXPECT_NO_THROW({
		Database db2(dbPath.string());
		db2.open();
		db2.close();
	});
}

TEST_F(WALReplayTest, WALEntityRecordsHaveNonZeroDataSize) {
	std::string walPath = dbPath.string() + "-wal";

	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		Node n(0, 88);
		db.getStorage()->getDataManager()->addNode(n);
		txn.commit();
		db.close();
	}

	// Open WAL directly and verify ENTITY_WRITE records have data
	WALManager mgr;
	mgr.open(dbPath.string());

	auto result = mgr.readRecords();
	mgr.close();

	bool foundEntityWrite = false;
	for (const auto &rec : result.records) {
		if (rec.header.type == WALRecordType::WAL_ENTITY_WRITE) {
			foundEntityWrite = true;
			ASSERT_GE(rec.data.size(), sizeof(WALEntityPayload));
			auto payload = deserializeEntityPayload(rec.data.data());
			EXPECT_GT(payload.dataSize, 0u) << "Entity WAL record must contain serialized data";
		}
	}

	EXPECT_TRUE(foundEntityWrite) << "Expected at least one WAL_ENTITY_WRITE record";
}

TEST_F(WALReplayTest, EmptyWALNoOpRecovery) {
	// Create and immediately close — produces empty WAL
	{
		Database db(dbPath.string());
		db.open();
		db.close();
	}

	// Reopen — should not throw
	EXPECT_NO_THROW({
		Database db2(dbPath.string());
		db2.open();
		db2.close();
	});
}

TEST_F(WALReplayTest, MultipleCommittedTransactionsAllRecovered) {
	std::vector<int64_t> nodeIds;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		for (int i = 0; i < 5; ++i) {
			auto txn = db.beginTransaction();
			Node n(0, static_cast<int64_t>(100 + i));
			dm->addNode(n);
			nodeIds.push_back(n.getId());
			txn.commit();
		}

		db.close();
	}

	// Reopen — all 5 nodes should be recovered
	{
		Database db2(dbPath.string());
		db2.open();
		auto dm = db2.getStorage()->getDataManager();

		for (size_t i = 0; i < nodeIds.size(); ++i) {
			Node retrieved = dm->getNode(nodeIds[i]);
			EXPECT_TRUE(retrieved.isActive()) << "Node " << i << " should be visible after replay";
			EXPECT_EQ(retrieved.getLabelId(), static_cast<int64_t>(100 + i));
		}

		db2.close();
	}
}

TEST_F(WALReplayTest, RecoveryResultStatisticsAndCheckpoint) {
	{
		Database db(dbPath.string());
		db.open();

		// One committed transaction with a node
		{
			auto txn = db.beginTransaction();
			Node n(0, 55);
			db.getStorage()->getDataManager()->addNode(n);
			txn.commit();
		}

		// One rolled-back transaction
		{
			auto txn = db.beginTransaction();
			Node n(0, 66);
			db.getStorage()->getDataManager()->addNode(n);
			txn.rollback();
		}

		db.close();
	}

	// Reopen and trigger recovery via beginTransaction (which calls ensureWALAndTransactionManager)
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery by starting (and immediately committing) a read-only transaction
		{
			auto txn = db2.beginReadOnlyTransaction();
			txn.commit();
		}

		// After recovery, WAL should be checkpointed (truncated to header-only)
		std::string walPath = dbPath.string() + "-wal";
		if (fs::exists(walPath)) {
			auto walSize = fs::file_size(walPath);
			EXPECT_EQ(walSize, sizeof(WALFileHeader))
					<< "WAL should be truncated to header after recovery + checkpoint";
		}

		db2.close();
	}
}

TEST_F(WALReplayTest, IdempotentReplay) {
	int64_t nodeId = 0;

	{
		Database db(dbPath.string());
		db.open();

		auto txn = db.beginTransaction();
		Node n(0, 33);
		db.getStorage()->getDataManager()->addNode(n);
		nodeId = n.getId();
		txn.commit();
		db.close();
	}

	// First reopen — triggers recovery
	{
		Database db2(dbPath.string());
		db2.open();
		Node retrieved = db2.getStorage()->getDataManager()->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive());
		EXPECT_EQ(retrieved.getLabelId(), 33);
		db2.close();
	}

	// Second reopen — recovery should be a no-op (WAL was checkpointed)
	{
		Database db3(dbPath.string());
		db3.open();
		Node retrieved = db3.getStorage()->getDataManager()->getNode(nodeId);
		EXPECT_TRUE(retrieved.isActive());
		EXPECT_EQ(retrieved.getLabelId(), 33);
		db3.close();
	}
}

TEST_F(WALReplayTest, EdgePropertyRecoverySurvivesReopen) {
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

		Edge e(0, nodeId1, nodeId2, 1);
		dm->addEdge(e);
		edgeId = e.getId();

		dm->addEdgeProperties(edgeId, {{"since", PropertyValue(std::string("2024"))},
										{"weight", PropertyValue(1.5)}});

		txn.commit();
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

		Edge retrieved = dm->getEdge(edgeId);
		EXPECT_TRUE(retrieved.isActive());

		auto props = dm->getEdgeProperties(edgeId);
		EXPECT_NE(props.find("since"), props.end()) << "Edge property 'since' should be recovered";
		EXPECT_NE(props.find("weight"), props.end()) << "Edge property 'weight' should be recovered";

		db2.close();
	}
}

TEST_F(WALReplayTest, MultipleNodesWithPropertiesRecovery) {
	std::vector<int64_t> nodeIds;
	constexpr int nodeCount = 3;

	{
		Database db(dbPath.string());
		db.open();
		auto dm = db.getStorage()->getDataManager();

		for (int i = 0; i < nodeCount; ++i) {
			auto txn = db.beginTransaction();
			Node n(0, static_cast<int64_t>(100 + i));
			dm->addNode(n);
			nodeIds.push_back(n.getId());
			dm->addNodeProperties(n.getId(),
								  {{"index", PropertyValue(static_cast<int64_t>(i))},
								   {"label", PropertyValue(std::string("node_" + std::to_string(i)))}});
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

		for (int i = 0; i < nodeCount; ++i) {
			Node retrieved = dm->getNode(nodeIds[i]);
			EXPECT_TRUE(retrieved.isActive()) << "Node " << i << " should be recovered";

			auto props = dm->getNodeProperties(nodeIds[i]);
			auto indexIt = props.find("index");
			EXPECT_NE(indexIt, props.end()) << "Property 'index' should be recovered for node " << i;
			if (indexIt != props.end()) {
				EXPECT_EQ(std::get<int64_t>(indexIt->second.getVariant()), static_cast<int64_t>(i));
			}

			auto labelIt = props.find("label");
			EXPECT_NE(labelIt, props.end()) << "Property 'label' should be recovered for node " << i;
			if (labelIt != props.end()) {
				EXPECT_EQ(std::get<std::string>(labelIt->second.getVariant()), "node_" + std::to_string(i));
			}
		}

		db2.close();
	}
}

TEST_F(WALReplayTest, EmptyWALTriggersEarlyCheckpoint) {
	// Create DB and write a transaction to create the WAL file
	{
		Database db(dbPath.string());
		db.open();
		auto txn = db.beginTransaction();
		txn.commit();
		db.close();
	}

	// The WAL file should exist. Truncate it to header + a few invalid bytes
	// so needsRecovery() returns true but readRecords() returns empty records
	std::string walPath = dbPath.string() + "-wal";
	if (fs::exists(walPath)) {
		std::vector<uint8_t> headerData(sizeof(WALFileHeader));
		{
			std::ifstream walIn(walPath, std::ios::binary);
			walIn.read(reinterpret_cast<char *>(headerData.data()),
					   static_cast<std::streamsize>(sizeof(WALFileHeader)));
		}
		{
			std::ofstream walOut(walPath, std::ios::binary | std::ios::trunc);
			walOut.write(reinterpret_cast<const char *>(headerData.data()),
						 static_cast<std::streamsize>(sizeof(WALFileHeader)));
			// Write a few bytes that don't form a valid record
			uint8_t partial[] = {0x01, 0x02};
			walOut.write(reinterpret_cast<const char *>(partial), 2);
		}
	}

	// Reopen — recovery should handle empty/invalid records gracefully
	{
		Database db2(dbPath.string());
		db2.open();
		auto txn = db2.beginReadOnlyTransaction();
		txn.commit();
		db2.close();
	}
}

TEST_F(WALReplayTest, RollbackOnlyWALTriggersEarlyCheckpoint) {
	{
		Database db(dbPath.string());
		db.open();

		// Only rolled-back transactions — no commits
		{
			auto txn = db.beginTransaction();
			Node n(0, 1);
			db.getStorage()->getDataManager()->addNode(n);
			txn.rollback();
		}

		{
			auto txn = db.beginTransaction();
			Node n(0, 2);
			db.getStorage()->getDataManager()->addNode(n);
			txn.rollback();
		}

		db.close();
	}

	// Reopen — WAL has records but no committed txns
	{
		Database db2(dbPath.string());
		db2.open();

		// Trigger WAL recovery
		auto txn = db2.beginReadOnlyTransaction();
		txn.commit();

		db2.close();
	}
}

TEST_F(WALReplayTest, CommittedTxnNoEntityWritesFinalStateEmpty) {
	{
		Database db(dbPath.string());
		db.open();

		// Commit an empty transaction (no entity changes at all)
		{
			auto txn = db.beginTransaction();
			txn.commit();
		}

		db.close();
	}

	// Reopen — should handle empty finalState gracefully
	EXPECT_NO_THROW({
		Database db2(dbPath.string());
		db2.open();
		auto txn = db2.beginReadOnlyTransaction();
		txn.commit();
		db2.close();
	});

	// WAL should be checkpointed after recovery
	std::string walPath = dbPath.string() + "-wal";
	if (fs::exists(walPath)) {
		auto walSize = fs::file_size(walPath);
		EXPECT_EQ(walSize, sizeof(WALFileHeader))
				<< "WAL should be truncated to header after recovery of empty commit";
	}
}
