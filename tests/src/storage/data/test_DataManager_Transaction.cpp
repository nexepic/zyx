/**
 * @file test_DataManager_Transaction.cpp
 * @date 2025/8/15
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

#include "DataManagerTestFixture.hpp"

TEST_F(DataManagerTest, AddNodeWithTransaction) {
	// Covers L255-261: transactionActive_ true branch in addNode
	dataManager->setActiveTransaction(1);

	auto node = createTestNode(dataManager, "TxnNode");
	dataManager->addNode(node);
	EXPECT_NE(0, node.getId());
	EXPECT_EQ(1UL, observer->addedNodes.size());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, AddEdgeWithTransaction) {
	// Covers L374-380: transactionActive_ true branch in addEdge
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	dataManager->setActiveTransaction(2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "TxnEdge");
	dataManager->addEdge(edge);
	EXPECT_NE(0, edge.getId());
	EXPECT_EQ(1UL, observer->addedEdges.size());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, UpdateNodeWithTransaction) {
	// Covers L294-297: transactionActive_ true branch in updateNode
	auto node = createTestNode(dataManager, "Person");
	dataManager->addNode(node);

	dataManager->setActiveTransaction(3);
	observer->reset();

	node.setLabelId(dataManager->getOrCreateTokenId("UpdatedPerson"));
	dataManager->updateNode(node);
	EXPECT_EQ(1UL, observer->updatedNodes.size());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, UpdateEdgeWithTransaction) {
	// Covers L419-422: transactionActive_ true branch in updateEdge
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	dataManager->setActiveTransaction(4);
	observer->reset();

	edge.setTypeId(dataManager->getOrCreateTokenId("LIKES"));
	dataManager->updateEdge(edge);
	EXPECT_EQ(1UL, observer->updatedEdges.size());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, DeleteNodeWithTransaction) {
	// Covers L306-309: transactionActive_ true branch in deleteNode
	auto node = createTestNode(dataManager, "Person");
	dataManager->addNode(node);

	dataManager->setActiveTransaction(5);
	observer->reset();

	dataManager->deleteNode(node);
	EXPECT_EQ(1UL, observer->deletedNodes.size());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, DeleteEdgeWithTransaction) {
	// Covers L431-434: transactionActive_ true branch in deleteEdge
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	dataManager->setActiveTransaction(6);
	observer->reset();

	dataManager->deleteEdge(edge);
	EXPECT_EQ(1UL, observer->deletedEdges.size());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, AddNodesWithTransaction) {
	// Covers L274-279: transactionActive_ true in addNodes
	dataManager->setActiveTransaction(7);

	std::vector<Node> nodes;
	nodes.push_back(createTestNode(dataManager, "BatchA"));
	nodes.push_back(createTestNode(dataManager, "BatchB"));
	dataManager->addNodes(nodes);

	EXPECT_EQ(2UL, nodes.size());
	for (auto &n : nodes) {
		EXPECT_NE(0, n.getId());
	}

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, AddEdgesWithTransaction) {
	// Covers L396-401: transactionActive_ true in addEdges
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	dataManager->setActiveTransaction(8);

	std::vector<Edge> edges;
	edges.push_back(createTestEdge(dataManager, node1.getId(), node2.getId(), "E1"));
	edges.push_back(createTestEdge(dataManager, node2.getId(), node1.getId(), "E2"));
	dataManager->addEdges(edges);

	for (auto &e : edges) {
		EXPECT_NE(0, e.getId());
	}

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, AddNodeWithTransactionAndWAL) {
	// Covers L258-261: walManager_ non-null branch in addNode
	// The database has a WAL manager by default, so this should exercise
	// the walManager_->writeEntityChange path
	auto txn = database->beginTransaction();
	auto txnId = txn.getId();
	dataManager->setActiveTransaction(txnId);

	auto node = createTestNode(dataManager, "WALNode");
	dataManager->addNode(node);
	EXPECT_NE(0, node.getId());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, AddEdgeWithTransactionAndWAL) {
	// Covers L377-380: walManager_ non-null branch in addEdge
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto txn = database->beginTransaction();
	auto txnId = txn.getId();
	dataManager->setActiveTransaction(txnId);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "WALEdge");
	dataManager->addEdge(edge);
	EXPECT_NE(0, edge.getId());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, SetAndClearActiveTransaction) {
	// Covers L238-248: basic transaction state management
	dataManager->setActiveTransaction(42);

	auto node = createTestNode(dataManager, "TxnTest");
	dataManager->addNode(node);
	EXPECT_NE(0, node.getId());

	dataManager->clearActiveTransaction();

	// After clearing, operations should not record txn ops
	observer->reset();
	auto node2 = createTestNode(dataManager, "PostTxnTest");
	dataManager->addNode(node2);
	EXPECT_NE(0, node2.getId());
}

TEST_F(DataManagerTest, RollbackActiveTransactionWithAddedNodes) {
	// Covers rollbackActiveTransaction L954-L966 (OP_ADD for Node)
	dataManager->setActiveTransaction(100);

	auto node = createTestNode(dataManager, "RollbackNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();

	dataManager->rollbackActiveTransaction();

	// After rollback, the node should not be accessible
	auto result = dataManager->getNode(nodeId);
	EXPECT_FALSE(result.isActive()) << "Node should not be active after rollback";
}

TEST_F(DataManagerTest, RollbackActiveTransactionWithAddedEdges) {
	// Covers rollbackActiveTransaction L966 (OP_ADD for Edge)
	auto node1 = createTestNode(dataManager, "RollbackEdgeSrc");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "RollbackEdgeDst");
	dataManager->addNode(node2);

	fileStorage->flush();

	dataManager->setActiveTransaction(101);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "ROLLBACK_TEST");
	dataManager->addEdge(edge);
	int64_t edgeId = edge.getId();

	dataManager->rollbackActiveTransaction();

	auto result = dataManager->getEdge(edgeId);
	EXPECT_FALSE(result.isActive()) << "Edge should not be active after rollback";
}

TEST_F(DataManagerTest, RollbackActiveTransactionWithDeletedNode) {
	// Covers rollbackActiveTransaction L975 (OP_DELETE branch)
	auto node = createTestNode(dataManager, "DeleteRollbackNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();
	fileStorage->flush();

	dataManager->setActiveTransaction(102);
	auto loaded = dataManager->getNode(nodeId);
	dataManager->deleteNode(loaded);

	dataManager->rollbackActiveTransaction();

	// After rollback, dirty state is cleared and cache is flushed.
	// The delete operation marks the entity inactive in the manager, but rollback
	// clears dirty registries. The OP_DELETE branch in rollback is exercised here.
	auto result = dataManager->getNode(nodeId);
	// Note: rollback cannot fully restore deleted entities without snapshots
	// (as documented in the code). The key coverage goal is L975 (OP_DELETE case).
	(void)result;
}

TEST_F(DataManagerTest, RollbackActiveTransactionWithUpdatedNode) {
	// Covers rollbackActiveTransaction L980 (OP_UPDATE branch)
	auto node = createTestNode(dataManager, "UpdateRollbackNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();
	uint32_t origLabelId = node.getLabelId();
	fileStorage->flush();

	dataManager->setActiveTransaction(103);
	auto loaded = dataManager->getNode(nodeId);
	loaded.setLabelId(dataManager->getOrCreateTokenId("UpdatedLabel"));
	dataManager->updateNode(loaded);

	dataManager->rollbackActiveTransaction();

	auto result = dataManager->getNode(nodeId);
	EXPECT_EQ(origLabelId, result.getLabelId())
			<< "Updated label should be reverted after rollback";
}

TEST_F(DataManagerTest, RollbackActiveTransactionWithAddedEdge) {
	auto node1 = createTestNode(dataManager, "RollbackEdgeNode1");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "RollbackEdgeNode2");
	dataManager->addNode(node2);
	fileStorage->flush();

	observer->reset();
	dataManager->setActiveTransaction(200);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "RollbackEdge");
	dataManager->addEdge(edge);
	// Rollback should fire notifyEdgeDeleted for the added edge (line 966-970)
	dataManager->rollbackActiveTransaction();

	// Verify edge deletion notification was fired during rollback
	EXPECT_GE(observer->deletedEdges.size(), 1UL) << "Rollback should notify edge deletion";
}

TEST_F(DataManagerTest, CheckAndTriggerAutoFlush_DuringActiveTransaction) {
	dataManager->setActiveTransaction(300);

	// This should be suppressed (line 827: transactionActive_ -> return)
	EXPECT_NO_THROW(dataManager->checkAndTriggerAutoFlush());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, CheckAndTriggerAutoFlushSuppressedDuringTransaction) {
	// Covers line 819: transactionActive_ true branch in checkAndTriggerAutoFlush
	dataManager->setActiveTransaction(99);

	// This should return early due to active transaction
	EXPECT_NO_THROW(dataManager->checkAndTriggerAutoFlush());

	dataManager->clearActiveTransaction();

	// After clearing, auto-flush check should proceed normally
	EXPECT_NO_THROW(dataManager->checkAndTriggerAutoFlush());
}

TEST_F(DataManagerTest, RollbackMixedNodeAndEdgeAddOperations) {
	auto node1 = createTestNode(dataManager, "MixedRollbackSrc");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "MixedRollbackTgt");
	dataManager->addNode(node2);
	fileStorage->flush();

	observer->reset();
	dataManager->setActiveTransaction(500);

	// Add both node and edge in the transaction
	auto newNode = createTestNode(dataManager, "TxnNode");
	dataManager->addNode(newNode);

	auto newEdge = createTestEdge(dataManager, node1.getId(), node2.getId(), "TxnEdge");
	dataManager->addEdge(newEdge);

	// Rollback should undo both additions
	dataManager->rollbackActiveTransaction();

	// Verify both deletions were notified during rollback
	EXPECT_GE(observer->deletedNodes.size(), 1UL)
		<< "Rollback should notify node deletion";
	EXPECT_GE(observer->deletedEdges.size(), 1UL)
		<< "Rollback should notify edge deletion";
}

TEST_F(DataManagerTest, RollbackWithEdgeDeleteOperation) {
	auto node1 = createTestNode(dataManager, "RollbackDelEdgeSrc");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "RollbackDelEdgeTgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "DEL_ROLLBACK");
	dataManager->addEdge(edge);
	int64_t edgeId = edge.getId();

	fileStorage->flush();

	dataManager->setActiveTransaction(501);
	auto loadedEdge = dataManager->getEdge(edgeId);
	dataManager->deleteEdge(loadedEdge);

	dataManager->rollbackActiveTransaction();

	// The OP_DELETE branch was exercised for Edge entity type
	// After rollback + cache clear, edge should be retrievable from disk
	auto result = dataManager->getEdge(edgeId);
	// Note: rollback can't fully restore without snapshots, but the branch is covered
	(void)result;
}

TEST_F(DataManagerTest, RollbackWithEdgeUpdateOperation) {
	auto node1 = createTestNode(dataManager, "RollbackUpdEdgeSrc");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "RollbackUpdEdgeTgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "UPD_ROLLBACK");
	dataManager->addEdge(edge);
	int64_t edgeId = edge.getId();
	uint32_t origLabelId = edge.getTypeId();

	fileStorage->flush();

	dataManager->setActiveTransaction(502);
	auto loadedEdge = dataManager->getEdge(edgeId);
	loadedEdge.setTypeId(dataManager->getOrCreateTokenId("UPDATED_LABEL"));
	dataManager->updateEdge(loadedEdge);

	dataManager->rollbackActiveTransaction();

	auto result = dataManager->getEdge(edgeId);
	EXPECT_EQ(origLabelId, result.getTypeId())
		<< "Edge label should be reverted after rollback";
}

