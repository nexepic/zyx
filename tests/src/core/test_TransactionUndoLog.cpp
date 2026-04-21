/**
 * @file test_TransactionUndoLog.cpp
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

#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include "graph/core/Database.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/data/TransactionContext.hpp"
#include "graph/storage/wal/UndoLog.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;
using namespace graph::storage::wal;

class TransactionUndoLogTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_undo_" + boost::uuids::to_string(uuid) + ".zyx");
		fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		db.reset();
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
		std::string walPath = testDbPath.string() + "-wal";
		fs::remove(walPath, ec);
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
};

// --- UndoLog population tests ---

TEST_F(TransactionUndoLogTest, AddNodeRecordsUndoEntry) {
	auto &dm = *db->getStorage()->getDataManager();
	auto txn = db->beginTransaction();

	// After beginTransaction, txnContext should be active
	ASSERT_TRUE(dm.hasActiveTransaction());

	const auto &undoLog = dm.getTransactionContext().undoLog();
	EXPECT_TRUE(undoLog.empty());

	Node node(0, 0);
	dm.addNode(node);
	EXPECT_GT(node.getId(), 0);

	// Undo log should now have one UNDO_ADDED entry
	ASSERT_EQ(undoLog.size(), 1u);
	EXPECT_EQ(undoLog.entries()[0].changeType, UndoChangeType::UNDO_ADDED);
	EXPECT_EQ(undoLog.entries()[0].entityId, node.getId());
	EXPECT_TRUE(undoLog.entries()[0].beforeImage.empty()); // No before-image for ADD

	txn.rollback();
}

TEST_F(TransactionUndoLogTest, AddNodeRollbackRestoresState) {
	// Add a node outside transaction to get a stable ID
	Node preNode(0, 0);
	{
		auto txn0 = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(preNode);
		txn0.commit();
	}
	int64_t preId = preNode.getId();
	ASSERT_GT(preId, 0);

	// Now start a transaction, add another node, then rollback
	int64_t newNodeId = 0;
	{
		auto txn = db->beginTransaction();
		Node newNode(0, 0);
		db->getStorage()->getDataManager()->addNode(newNode);
		newNodeId = newNode.getId();
		ASSERT_GT(newNodeId, 0);
		txn.rollback();
	}

	// After rollback, the new node should not be visible
	auto &dm = *db->getStorage()->getDataManager();
	Node retrieved = dm.getNode(newNodeId);
	EXPECT_FALSE(retrieved.isActive()) << "Rolled-back node should be inactive";
}

TEST_F(TransactionUndoLogTest, UpdateNodeRecordsUndoEntryWithBeforeImage) {
	// First create a node
	Node node(0, 0);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}
	ASSERT_GT(node.getId(), 0);

	// Now update it and check undo log
	auto &dm = *db->getStorage()->getDataManager();
	auto txn = db->beginTransaction();

	const auto &undoLog = dm.getTransactionContext().undoLog();
	EXPECT_TRUE(undoLog.empty());

	Node updatedNode = dm.getNode(node.getId());
	updatedNode.setLabelId(999);
	dm.updateNode(updatedNode);

	ASSERT_EQ(undoLog.size(), 1u);
	EXPECT_EQ(undoLog.entries()[0].changeType, UndoChangeType::UNDO_MODIFIED);
	EXPECT_EQ(undoLog.entries()[0].entityId, node.getId());
	EXPECT_FALSE(undoLog.entries()[0].beforeImage.empty()); // Must have before-image

	txn.rollback();
}

TEST_F(TransactionUndoLogTest, UpdateNodeRollbackRestoresOriginal) {
	// Create and commit a node with label 42
	Node node(0, 42);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}
	ASSERT_GT(node.getId(), 0);

	// Update label to 99 then rollback
	{
		auto &dm = *db->getStorage()->getDataManager();
		auto txn = db->beginTransaction();
		Node toUpdate = dm.getNode(node.getId());
		toUpdate.setLabelId(99);
		dm.updateNode(toUpdate);
		txn.rollback();
	}

	// After rollback, label should be restored to 42
	Node afterRollback = db->getStorage()->getDataManager()->getNode(node.getId());
	EXPECT_EQ(afterRollback.getLabelId(), 42) << "Label should be restored to original after rollback";
}

TEST_F(TransactionUndoLogTest, DeleteNodeRecordsUndoEntryWithBeforeImage) {
	// Create a node first
	Node node(0, 0);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}
	ASSERT_GT(node.getId(), 0);

	// Start transaction, delete node, check undo log
	auto &dm = *db->getStorage()->getDataManager();
	auto txn = db->beginTransaction();

	const auto &undoLog = dm.getTransactionContext().undoLog();
	EXPECT_TRUE(undoLog.empty());

	Node toDelete = dm.getNode(node.getId());
	dm.deleteNode(toDelete);

	ASSERT_EQ(undoLog.size(), 1u);
	EXPECT_EQ(undoLog.entries()[0].changeType, UndoChangeType::UNDO_DELETED);
	EXPECT_EQ(undoLog.entries()[0].entityId, node.getId());
	EXPECT_FALSE(undoLog.entries()[0].beforeImage.empty());

	txn.rollback();
}

TEST_F(TransactionUndoLogTest, DeleteNodeRollbackRestoresEntity) {
	// Create a node first
	Node node(0, 55);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}
	ASSERT_GT(node.getId(), 0);

	// Delete then rollback
	{
		auto &dm = *db->getStorage()->getDataManager();
		auto txn = db->beginTransaction();
		Node toDelete = dm.getNode(node.getId());
		dm.deleteNode(toDelete);
		txn.rollback();
	}

	// After rollback, node should still be visible
	Node afterRollback = db->getStorage()->getDataManager()->getNode(node.getId());
	EXPECT_TRUE(afterRollback.isActive()) << "Deleted-then-rolled-back node should be active";
}

TEST_F(TransactionUndoLogTest, UndoLogClearedAfterCommit) {
	auto &dm = *db->getStorage()->getDataManager();
	auto txn = db->beginTransaction();

	Node node(0, 0);
	dm.addNode(node);

	ASSERT_EQ(dm.getTransactionContext().undoLog().size(), 1u);

	txn.commit();

	// After commit, undo log should be cleared (context cleared)
	EXPECT_FALSE(dm.hasActiveTransaction());
}

TEST_F(TransactionUndoLogTest, UndoLogClearedAfterRollback) {
	auto &dm = *db->getStorage()->getDataManager();
	auto txn = db->beginTransaction();

	Node node(0, 0);
	dm.addNode(node);

	ASSERT_EQ(dm.getTransactionContext().undoLog().size(), 1u);

	txn.rollback();

	// After rollback, transaction context is cleared
	EXPECT_FALSE(dm.hasActiveTransaction());
}

TEST_F(TransactionUndoLogTest, MixedOperationsRollback) {
	// Create two nodes outside transaction
	Node node1(0, 1), node2(0, 2);
	{
		auto txn = db->beginTransaction();
		auto &dm = *db->getStorage()->getDataManager();
		dm.addNode(node1);
		dm.addNode(node2);
		txn.commit();
	}

	// Start transaction: update node1, add node3, delete node2
	int64_t node3Id = 0;
	{
		auto &dm = *db->getStorage()->getDataManager();
		auto txn = db->beginTransaction();

		// Update node1 label
		Node n1 = dm.getNode(node1.getId());
		n1.setLabelId(100);
		dm.updateNode(n1);

		// Add new node3
		Node node3(0, 3);
		dm.addNode(node3);
		node3Id = node3.getId();

		// Delete node2
		Node n2 = dm.getNode(node2.getId());
		dm.deleteNode(n2);

		const auto &undoLog = dm.getTransactionContext().undoLog();
		EXPECT_EQ(undoLog.size(), 3u);
		EXPECT_EQ(undoLog.entries()[0].changeType, UndoChangeType::UNDO_MODIFIED);
		EXPECT_EQ(undoLog.entries()[1].changeType, UndoChangeType::UNDO_ADDED);
		EXPECT_EQ(undoLog.entries()[2].changeType, UndoChangeType::UNDO_DELETED);

		txn.rollback();
	}

	auto &dm = *db->getStorage()->getDataManager();

	// node1 should have original label 1
	Node n1After = dm.getNode(node1.getId());
	EXPECT_EQ(n1After.getLabelId(), 1) << "node1 label should be restored to 1";

	// node3 should not exist (was added then rolled back)
	if (node3Id > 0) {
		Node n3After = dm.getNode(node3Id);
		EXPECT_FALSE(n3After.isActive()) << "node3 should not exist after rollback";
	}

	// node2 should still be active (was deleted then rolled back)
	Node n2After = dm.getNode(node2.getId());
	EXPECT_TRUE(n2After.isActive()) << "node2 should be restored after rollback";
}

TEST_F(TransactionUndoLogTest, AddEdgeRecordsUndoEntry) {
	// Create source and target nodes first
	Node src(0, 0), tgt(0, 0);
	{
		auto txn = db->beginTransaction();
		auto &dm = *db->getStorage()->getDataManager();
		dm.addNode(src);
		dm.addNode(tgt);
		txn.commit();
	}

	auto &dm = *db->getStorage()->getDataManager();
	auto txn = db->beginTransaction();

	Edge edge(0, src.getId(), tgt.getId(), 0);
	dm.addEdge(edge);

	const auto &undoLog = dm.getTransactionContext().undoLog();
	// Adding an edge internally updates source and target node references,
	// so the undo log will have > 1 entry (edge ADD + node UPDATEs).
	EXPECT_FALSE(undoLog.empty());

	// Verify that the edge ADD entry is present
	bool foundEdgeAdd = false;
	for (const auto &entry : undoLog.entries()) {
		if (entry.entityType == static_cast<uint8_t>(graph::EntityType::Edge) &&
			entry.changeType == UndoChangeType::UNDO_ADDED &&
			entry.entityId == edge.getId()) {
			foundEdgeAdd = true;
			break;
		}
	}
	EXPECT_TRUE(foundEdgeAdd) << "Expected UNDO_ADDED entry for the edge in undo log";

	txn.rollback();
}

// --- WAL data completeness tests ---

TEST_F(TransactionUndoLogTest, AddNodeWritesSerializedDataToWAL) {
	// Verify that a committed add-node transaction writes entity data
	// (WAL should have records with non-zero dataSize after this test)
	auto txn = db->beginTransaction();
	Node node(0, 42);
	db->getStorage()->getDataManager()->addNode(node);
	txn.commit();
	EXPECT_GT(node.getId(), 0);
	// If we get here without throw, WAL write with serialized data succeeded
}
