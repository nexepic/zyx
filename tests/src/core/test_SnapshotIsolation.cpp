/**
 * @file test_SnapshotIsolation.cpp
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
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SnapshotManager.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;

class SnapshotIsolationTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_snapshot_" + boost::uuids::to_string(uuid) + ".zyx");
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

TEST_F(SnapshotIsolationTest, CommittedSnapshotContainsNodeData) {
	// Add a node and commit
	Node node(0, 42);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}
	ASSERT_GT(node.getId(), 0);

	// The PersistenceManager's captureCommittedSnapshot was called during commit.
	// Verify indirectly: start a read-only transaction and check the snapshot has data.
	auto txn = db->beginReadOnlyTransaction();

	// The snapshot should be non-null and populated
	auto *dm = db->getStorage()->getDataManager().get();
	const auto *snapshot = dm->getCurrentSnapshot();
	ASSERT_NE(snapshot, nullptr);

	// The snapshot should contain the committed node
	EXPECT_FALSE(snapshot->nodes.empty()) << "Snapshot should contain committed node data";

	auto it = snapshot->nodes.find(node.getId());
	EXPECT_NE(it, snapshot->nodes.end()) << "Snapshot should have the added node";
	if (it != snapshot->nodes.end()) {
		EXPECT_EQ(it->second.changeType, EntityChangeType::CHANGE_ADDED);
		EXPECT_TRUE(it->second.backup.has_value());
		EXPECT_EQ(it->second.backup->getId(), node.getId());
	}

	txn.commit();
}

TEST_F(SnapshotIsolationTest, CommittedSnapshotContainsEdgeData) {
	// Create nodes and an edge
	Node src(0, 0), tgt(0, 0);
	{
		auto txn = db->beginTransaction();
		auto &dm = *db->getStorage()->getDataManager();
		dm.addNode(src);
		dm.addNode(tgt);
		txn.commit();
	}

	Edge edge(0, src.getId(), tgt.getId(), 0);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addEdge(edge);
		txn.commit();
	}
	ASSERT_GT(edge.getId(), 0);

	auto txn = db->beginReadOnlyTransaction();
	auto *dm = db->getStorage()->getDataManager().get();
	const auto *snapshot = dm->getCurrentSnapshot();
	ASSERT_NE(snapshot, nullptr);

	// Snapshot should contain the edge
	EXPECT_FALSE(snapshot->edges.empty()) << "Snapshot should contain committed edge data";
	auto it = snapshot->edges.find(edge.getId());
	EXPECT_NE(it, snapshot->edges.end()) << "Snapshot should have the added edge";

	txn.commit();
}

TEST_F(SnapshotIsolationTest, SnapshotReflectsModifiedNode) {
	// Create node with label 10
	Node node(0, 10);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}

	// Update label to 99
	{
		auto &dm = *db->getStorage()->getDataManager();
		auto txn = db->beginTransaction();
		Node toUpdate = dm.getNode(node.getId());
		toUpdate.setLabelId(99);
		dm.updateNode(toUpdate);
		txn.commit();
	}

	auto txn = db->beginReadOnlyTransaction();
	auto *dm = db->getStorage()->getDataManager().get();
	const auto *snapshot = dm->getCurrentSnapshot();
	ASSERT_NE(snapshot, nullptr);

	// Snapshot should have the modified node
	auto it = snapshot->nodes.find(node.getId());
	EXPECT_NE(it, snapshot->nodes.end());
	if (it != snapshot->nodes.end()) {
		EXPECT_EQ(it->second.changeType, EntityChangeType::CHANGE_MODIFIED);
		EXPECT_TRUE(it->second.backup.has_value());
		EXPECT_EQ(it->second.backup->getLabelId(), 99);
	}

	txn.commit();
}

TEST_F(SnapshotIsolationTest, SnapshotReflectsDeletedNode) {
	// Create and delete a node
	Node node(0, 0);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}

	{
		auto &dm = *db->getStorage()->getDataManager();
		auto txn = db->beginTransaction();
		Node toDelete = dm.getNode(node.getId());
		dm.deleteNode(toDelete);
		txn.commit();
	}

	auto txn = db->beginReadOnlyTransaction();
	auto *dm = db->getStorage()->getDataManager().get();
	const auto *snapshot = dm->getCurrentSnapshot();
	ASSERT_NE(snapshot, nullptr);

	// Snapshot should mark the node as deleted
	auto it = snapshot->nodes.find(node.getId());
	EXPECT_NE(it, snapshot->nodes.end());
	if (it != snapshot->nodes.end()) {
		EXPECT_EQ(it->second.changeType, EntityChangeType::CHANGE_DELETED);
	}

	txn.commit();
}

TEST_F(SnapshotIsolationTest, ReadOnlyTransactionReadsFromSnapshot) {
	// Add a node
	Node node(0, 77);
	{
		auto txn = db->beginTransaction();
		db->getStorage()->getDataManager()->addNode(node);
		txn.commit();
	}

	// Start a read-only transaction and read the node through normal API
	auto txn = db->beginReadOnlyTransaction();
	Node readNode = db->getStorage()->getDataManager()->getNode(node.getId());
	EXPECT_EQ(readNode.getId(), node.getId());
	EXPECT_TRUE(readNode.isActive());
	EXPECT_EQ(readNode.getLabelId(), 77);
	txn.commit();
}

TEST_F(SnapshotIsolationTest, PersistenceManagerCaptureCommittedSnapshot) {
	// Direct unit test of captureCommittedSnapshot
	auto &dm = *db->getStorage()->getDataManager();
	auto pm = dm.getPersistenceManager();

	// Add a node in a write transaction (this marks it dirty)
	auto txn = db->beginTransaction();
	Node node(0, 55);
	dm.addNode(node);
	ASSERT_GT(node.getId(), 0);

	// Capture snapshot while dirty data exists
	auto snap = pm->captureCommittedSnapshot();
	ASSERT_NE(snap, nullptr);
	EXPECT_FALSE(snap->nodes.empty());
	EXPECT_TRUE(snap->nodes.contains(node.getId()));

	txn.rollback();
}

TEST_F(SnapshotIsolationTest, EmptyTransactionProducesEmptySnapshot) {
	// Commit a no-op transaction
	{
		auto txn = db->beginTransaction();
		txn.commit();
	}

	auto txn = db->beginReadOnlyTransaction();
	auto *dm = db->getStorage()->getDataManager().get();
	const auto *snapshot = dm->getCurrentSnapshot();
	ASSERT_NE(snapshot, nullptr);
	EXPECT_TRUE(snapshot->nodes.empty());
	EXPECT_TRUE(snapshot->edges.empty());
	txn.commit();
}
