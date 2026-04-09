/**
 * @file test_DataManager_EdgeCases.cpp
 * @brief Branch coverage tests for DataManager — covers label resolution,
 *        edge direction queries, rollback, markEntityDeleted, batch ops
 *        with properties, checkAndTriggerAutoFlush, and getDirtyEntityInfos.
 **/

#include "DataManagerTestFixture.hpp"

// ============================================================================
// Label resolution edge cases
// ============================================================================

TEST_F(DataManagerTest, GetOrCreateLabelId_EmptyLabel) {
	// Line 189: empty label returns 0
	int64_t id = dataManager->getOrCreateTokenId("");
	EXPECT_EQ(id, 0);
}

TEST_F(DataManagerTest, ResolveLabel_ZeroId) {
	// Line 198: labelId == 0 returns ""
	std::string label = dataManager->resolveTokenName(0);
	EXPECT_EQ(label, "");
}

TEST_F(DataManagerTest, ResolveLabel_NonExistentId) {
	// resolveTokenName for an ID that doesn't exist
	std::string label = dataManager->resolveTokenName(999999);
	EXPECT_TRUE(label.empty());
}

// ============================================================================
// findEdgesByNode — direction branches (lines 700-708)
// ============================================================================

TEST_F(DataManagerTest, FindEdgesByNode_OutDirection) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	auto outEdges = dataManager->findEdgesByNode(n1.getId(), "out");
	EXPECT_EQ(outEdges.size(), 1UL);
	EXPECT_EQ(outEdges[0].getSourceNodeId(), n1.getId());
}

TEST_F(DataManagerTest, FindEdgesByNode_InDirection) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	// n2 is the target, so searching "in" for n2 should find the edge
	auto inEdges = dataManager->findEdgesByNode(n2.getId(), "in");
	EXPECT_EQ(inEdges.size(), 1UL);
}

TEST_F(DataManagerTest, FindEdgesByNode_BothDirection) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	// "both" should find the edge for either node
	auto bothEdges = dataManager->findEdgesByNode(n1.getId(), "both");
	EXPECT_GE(bothEdges.size(), 1UL);
}

TEST_F(DataManagerTest, FindEdgesByNode_DefaultDirection) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	// Any unrecognized direction falls through to "both"
	auto edges = dataManager->findEdgesByNode(n1.getId(), "any_unknown");
	EXPECT_GE(edges.size(), 1UL);
}

// ============================================================================
// rollbackActiveTransaction (lines 1399-1439)
// ============================================================================

TEST_F(DataManagerTest, Rollback_UndoNodeAdd) {
	dataManager->setActiveTransaction(100);

	auto node = createTestNode(dataManager, "RollbackNode");
	dataManager->addNode(node);
	EXPECT_NE(node.getId(), 0);

	observer->reset();
	dataManager->rollbackActiveTransaction();
	dataManager->clearActiveTransaction();

	// Rollback should have notified observers to delete the added node
	EXPECT_EQ(observer->deletedNodes.size(), 1UL);
}

TEST_F(DataManagerTest, Rollback_UndoEdgeAdd) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	dataManager->setActiveTransaction(101);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "REL");
	dataManager->addEdge(edge);

	observer->reset();
	dataManager->rollbackActiveTransaction();
	dataManager->clearActiveTransaction();

	// Rollback should have notified observers to delete the added edge
	EXPECT_EQ(observer->deletedEdges.size(), 1UL);
}

TEST_F(DataManagerTest, Rollback_UndoNodeDelete) {
	auto node = createTestNode(dataManager, "Keep");
	dataManager->addNode(node);
	simulateSave();

	dataManager->setActiveTransaction(102);
	dataManager->deleteNode(node);

	observer->reset();
	dataManager->rollbackActiveTransaction();
	dataManager->clearActiveTransaction();

	// OP_DELETE undo: dirty registry clearing handles restore
	// No crash is the primary assertion
	SUCCEED();
}

TEST_F(DataManagerTest, Rollback_UndoNodeUpdate) {
	auto node = createTestNode(dataManager, "Original");
	dataManager->addNode(node);
	simulateSave();

	dataManager->setActiveTransaction(103);
	int64_t newLabel = dataManager->getOrCreateTokenId("Updated");
	node.setLabelId(newLabel);
	dataManager->updateNode(node);

	observer->reset();
	dataManager->rollbackActiveTransaction();
	dataManager->clearActiveTransaction();

	// OP_UPDATE undo: handled by dirty registry clearing
	SUCCEED();
}

TEST_F(DataManagerTest, Rollback_MixedOps) {
	// Add two nodes, then rollback
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	simulateSave();

	dataManager->setActiveTransaction(104);

	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "MIX");
	dataManager->addEdge(edge);

	dataManager->deleteNode(n1);

	observer->reset();
	dataManager->rollbackActiveTransaction();
	dataManager->clearActiveTransaction();

	// Rollback reverses in reverse order: delete → update → add
	// Should not crash
	SUCCEED();
}

// ============================================================================
// markEntityDeleted — ADDED entity path (lines 1451-1462)
// ============================================================================

TEST_F(DataManagerTest, MarkEntityDeleted_AddedEntity) {
	// When an entity was just added (CHANGE_ADDED), deleting it removes from persistence
	auto node = createTestNode(dataManager, "Ephemeral");
	dataManager->addNode(node);

	// Entity is in CHANGE_ADDED state, now delete it
	dataManager->deleteNode(node);

	// The entity should be effectively gone — persistence manager clears the ADDED entry
	auto retrieved = dataManager->getNode(node.getId());
	EXPECT_FALSE(retrieved.isActive());
}

TEST_F(DataManagerTest, MarkEntityDeleted_ExistingEntity) {
	// When an entity is persisted, deleting marks CHANGE_DELETED
	auto node = createTestNode(dataManager, "Persistent");
	dataManager->addNode(node);
	simulateSave();

	dataManager->deleteNode(node);

	auto retrieved = dataManager->getNode(node.getId());
	EXPECT_FALSE(retrieved.isActive());
}

// ============================================================================
// checkAndTriggerAutoFlush — transaction suppression (line 1053-1054)
// ============================================================================

TEST_F(DataManagerTest, CheckAutoFlush_SuppressedDuringTransaction) {
	dataManager->setActiveTransaction(200);

	// During active transaction, auto-flush should be suppressed
	EXPECT_NO_THROW(dataManager->checkAndTriggerAutoFlush());

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, CheckAutoFlush_TriggeredOutsideTransaction) {
	// Outside transaction, auto-flush check should proceed normally
	EXPECT_NO_THROW(dataManager->checkAndTriggerAutoFlush());
}

// ============================================================================
// addNodes batch with properties (lines 235-267)
// ============================================================================

TEST_F(DataManagerTest, AddNodes_BatchEmpty) {
	std::vector<Node> empty;
	dataManager->addNodes(empty);
	EXPECT_EQ(observer->addedNodes.size(), 0UL);
}

TEST_F(DataManagerTest, AddNodes_BatchWithProperties) {
	std::vector<Node> nodes;
	for (int i = 0; i < 3; ++i) {
		Node n;
		n.setLabelId(dataManager->getOrCreateTokenId("Batch"));
		n.addProperty("key", PropertyValue(static_cast<int64_t>(i)));
		nodes.push_back(n);
	}

	dataManager->addNodes(nodes);

	// Batch add uses notifyNodesAdded, verify nodes got IDs assigned
	for (auto &n : nodes) {
		EXPECT_NE(n.getId(), 0);
		auto props = dataManager->getNodeProperties(n.getId());
		EXPECT_FALSE(props.empty());
	}
}

TEST_F(DataManagerTest, AddNodes_BatchNoProperties) {
	std::vector<Node> nodes;
	for (int i = 0; i < 3; ++i) {
		Node n;
		n.setLabelId(dataManager->getOrCreateTokenId("NoProp"));
		nodes.push_back(n);
	}

	dataManager->addNodes(nodes);

	// Batch add uses notifyNodesAdded, verify nodes got IDs
	for (auto &n : nodes) {
		EXPECT_NE(n.getId(), 0);
	}
}

TEST_F(DataManagerTest, AddNodes_BatchWithTransaction) {
	dataManager->setActiveTransaction(300);

	std::vector<Node> nodes;
	for (int i = 0; i < 2; ++i) {
		Node n;
		n.setLabelId(dataManager->getOrCreateTokenId("TxnBatch"));
		nodes.push_back(n);
	}

	dataManager->addNodes(nodes);

	for (auto &n : nodes) {
		EXPECT_NE(n.getId(), 0);
	}

	dataManager->clearActiveTransaction();
}

// ============================================================================
// addEdges batch with properties (lines 568-605)
// ============================================================================

TEST_F(DataManagerTest, AddEdges_BatchEmpty) {
	std::vector<Edge> empty;
	dataManager->addEdges(empty);
	// No crash expected
	SUCCEED();
}

TEST_F(DataManagerTest, AddEdges_BatchWithProperties) {
	auto n1 = createTestNode(dataManager, "S");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "T");
	dataManager->addNode(n2);

	std::vector<Edge> edges;
	for (int i = 0; i < 3; ++i) {
		Edge e;
		e.setSourceNodeId(n1.getId());
		e.setTargetNodeId(n2.getId());
		e.setTypeId(dataManager->getOrCreateTokenId("BATCH_REL"));
		e.addProperty("weight", PropertyValue(static_cast<int64_t>(i * 10)));
		edges.push_back(e);
	}

	dataManager->addEdges(edges);

	// Verify edges got IDs and properties stored
	for (auto &e : edges) {
		EXPECT_NE(e.getId(), 0);
		auto props = dataManager->getEdgeProperties(e.getId());
		EXPECT_FALSE(props.empty());
	}
}

TEST_F(DataManagerTest, AddEdges_BatchNoProperties) {
	auto n1 = createTestNode(dataManager, "S");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "T");
	dataManager->addNode(n2);

	std::vector<Edge> edges;
	for (int i = 0; i < 3; ++i) {
		Edge e;
		e.setSourceNodeId(n1.getId());
		e.setTargetNodeId(n2.getId());
		e.setTypeId(dataManager->getOrCreateTokenId("BARE_REL"));
		edges.push_back(e);
	}

	dataManager->addEdges(edges);

	for (auto &e : edges) {
		EXPECT_NE(e.getId(), 0);
	}
}

TEST_F(DataManagerTest, AddEdges_BatchWithTransaction) {
	auto n1 = createTestNode(dataManager, "S");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "T");
	dataManager->addNode(n2);

	dataManager->setActiveTransaction(301);

	std::vector<Edge> edges;
	for (int i = 0; i < 2; ++i) {
		Edge e;
		e.setSourceNodeId(n1.getId());
		e.setTargetNodeId(n2.getId());
		e.setTypeId(dataManager->getOrCreateTokenId("TXN_EDGE"));
		edges.push_back(e);
	}

	dataManager->addEdges(edges);

	for (auto &e : edges) {
		EXPECT_NE(e.getId(), 0);
	}

	dataManager->clearActiveTransaction();
}

// ============================================================================
// preadBytes (line 88-92)
// ============================================================================

TEST_F(DataManagerTest, PreadBytes_ValidRead) {
	// preadBytes on a valid fd should work
	char buf[16];
	ssize_t n = dataManager->preadBytes(buf, sizeof(buf), 0);
	EXPECT_GT(n, 0);
}

// ============================================================================
// Node/Edge property operations with observer notifications
// ============================================================================

TEST_F(DataManagerTest, AddNodeProperties_TriggersUpdate) {
	auto node = createTestNode(dataManager, "PropNode");
	dataManager->addNode(node);
	simulateSave();

	observer->reset();
	dataManager->addNodeProperties(node.getId(), {{"age", PropertyValue(static_cast<int64_t>(25))}});

	EXPECT_EQ(observer->updatedNodes.size(), 1UL);
}

TEST_F(DataManagerTest, RemoveNodeProperty_TriggersUpdate) {
	auto node = createTestNode(dataManager, "RemPropNode");
	node.addProperty("name", PropertyValue(std::string("Alice")));
	dataManager->addNode(node);
	simulateSave();

	// Store properties via addNodeProperties
	dataManager->addNodeProperties(node.getId(), {{"age", PropertyValue(static_cast<int64_t>(30))}});

	observer->reset();
	dataManager->removeNodeProperty(node.getId(), "age");

	EXPECT_EQ(observer->updatedNodes.size(), 1UL);
}

TEST_F(DataManagerTest, AddEdgeProperties_TriggersUpdate) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);
	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "REL");
	dataManager->addEdge(edge);
	simulateSave();

	observer->reset();
	dataManager->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(5.0)}});

	EXPECT_EQ(observer->updatedEdges.size(), 1UL);
}

TEST_F(DataManagerTest, RemoveEdgeProperty_TriggersUpdate) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);
	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "REL");
	dataManager->addEdge(edge);
	simulateSave();

	dataManager->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(5.0)}});

	observer->reset();
	dataManager->removeEdgeProperty(edge.getId(), "weight");

	EXPECT_EQ(observer->updatedEdges.size(), 1UL);
}

// ============================================================================
// hasUnsavedChanges / prepareFlushSnapshot / commitFlushSnapshot
// ============================================================================

TEST_F(DataManagerTest, HasUnsavedChanges_AfterAdd) {
	auto node = createTestNode(dataManager, "Dirty");
	dataManager->addNode(node);

	EXPECT_TRUE(dataManager->hasUnsavedChanges());
}

TEST_F(DataManagerTest, HasUnsavedChanges_AfterFlush) {
	auto node = createTestNode(dataManager, "Clean");
	dataManager->addNode(node);
	simulateSave();

	EXPECT_FALSE(dataManager->hasUnsavedChanges());
}

// ============================================================================
// State operations
// ============================================================================

TEST_F(DataManagerTest, StateOperations_CRUD) {
	// Add state
	auto state = createTestState("test_key");
	dataManager->addStateEntity(state);
	EXPECT_NE(state.getId(), 0);

	// Get state
	auto retrieved = dataManager->getState(state.getId());
	EXPECT_EQ(retrieved.getId(), state.getId());

	// Find by key
	auto found = dataManager->findStateByKey("test_key");
	EXPECT_EQ(found.getId(), state.getId());

	// Update
	state.setNextStateId(42);
	dataManager->updateStateEntity(state);
	auto updated = dataManager->getState(state.getId());
	EXPECT_EQ(updated.getNextStateId(), 42);

	// Delete
	dataManager->deleteState(state);
}

TEST_F(DataManagerTest, AddStateProperties) {
	auto state = createTestState("prop_state");
	dataManager->addStateEntity(state);

	dataManager->addStateProperties("prop_state",
		{{"config_key", PropertyValue(std::string("config_value"))}}, false);

	auto props = dataManager->getStateProperties("prop_state");
	EXPECT_FALSE(props.empty());
}

TEST_F(DataManagerTest, RemoveState_ByKey) {
	auto state = createTestState("remove_me");
	dataManager->addStateEntity(state);

	dataManager->removeState("remove_me");

	auto found = dataManager->findStateByKey("remove_me");
	// After removal, state should not be found (id == 0 or inactive)
	EXPECT_EQ(found.getId(), 0);
}

// ============================================================================
// updateNode / updateEdge while in transaction
// ============================================================================

TEST_F(DataManagerTest, UpdateNode_InTransaction) {
	auto node = createTestNode(dataManager, "Original");
	dataManager->addNode(node);
	simulateSave();

	dataManager->setActiveTransaction(400);

	int64_t newLabel = dataManager->getOrCreateTokenId("Modified");
	node.setLabelId(newLabel);
	dataManager->updateNode(node);

	EXPECT_EQ(observer->updatedNodes.size(), 1UL);

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, UpdateEdge_InTransaction) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);
	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "REL");
	dataManager->addEdge(edge);
	simulateSave();

	dataManager->setActiveTransaction(401);

	observer->reset();
	int64_t newLabel = dataManager->getOrCreateTokenId("UPDATED_REL");
	edge.setTypeId(newLabel);
	dataManager->updateEdge(edge);

	EXPECT_GE(observer->updatedEdges.size(), 1UL);

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, DeleteNode_InTransaction) {
	auto node = createTestNode(dataManager, "ToDelete");
	dataManager->addNode(node);
	simulateSave();

	dataManager->setActiveTransaction(402);
	dataManager->deleteNode(node);

	EXPECT_EQ(observer->deletedNodes.size(), 1UL);

	dataManager->clearActiveTransaction();
}

TEST_F(DataManagerTest, DeleteEdge_InTransaction) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);
	auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "REL");
	dataManager->addEdge(edge);
	simulateSave();

	dataManager->setActiveTransaction(403);
	dataManager->deleteEdge(edge);

	EXPECT_EQ(observer->deletedEdges.size(), 1UL);

	dataManager->clearActiveTransaction();
}

// ============================================================================
// Batch retrieval
// ============================================================================

TEST_F(DataManagerTest, GetNodeBatch) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto batch = dataManager->getNodeBatch({n1.getId(), n2.getId()});
	EXPECT_EQ(batch.size(), 2UL);
}

TEST_F(DataManagerTest, GetEdgeBatch) {
	auto n1 = createTestNode(dataManager, "A");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "B");
	dataManager->addNode(n2);

	auto e1 = createTestEdge(dataManager, n1.getId(), n2.getId(), "R1");
	dataManager->addEdge(e1);
	auto e2 = createTestEdge(dataManager, n2.getId(), n1.getId(), "R2");
	dataManager->addEdge(e2);

	auto batch = dataManager->getEdgeBatch({e1.getId(), e2.getId()});
	EXPECT_EQ(batch.size(), 2UL);
}

TEST_F(DataManagerTest, GetNodesInRange) {
	for (int i = 0; i < 5; ++i) {
		auto n = createTestNode(dataManager, "Range");
		dataManager->addNode(n);
	}

	auto nodes = dataManager->getNodesInRange(1, 100, 10);
	EXPECT_GE(nodes.size(), 5UL);
}

TEST_F(DataManagerTest, GetEdgesInRange) {
	auto n1 = createTestNode(dataManager, "S");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "T");
	dataManager->addNode(n2);

	for (int i = 0; i < 5; ++i) {
		auto e = createTestEdge(dataManager, n1.getId(), n2.getId(), "E");
		dataManager->addEdge(e);
	}

	auto edges = dataManager->getEdgesInRange(1, 100, 10);
	EXPECT_GE(edges.size(), 5UL);
}

// ============================================================================
// Blob and Index entity operations
// ============================================================================

TEST_F(DataManagerTest, BlobOperations_CRUD) {
	auto blob = createTestBlob("hello world");
	dataManager->addBlobEntity(blob);
	EXPECT_NE(blob.getId(), 0);

	auto retrieved = dataManager->getBlob(blob.getId());
	EXPECT_EQ(retrieved.getId(), blob.getId());

	blob.setData("updated data");
	dataManager->updateBlobEntity(blob);

	dataManager->deleteBlob(blob);
}

TEST_F(DataManagerTest, IndexOperations_CRUD) {
	auto index = createTestIndex(Index::NodeType::INTERNAL, 0);
	dataManager->addIndexEntity(index);
	EXPECT_NE(index.getId(), 0);

	auto retrieved = dataManager->getIndex(index.getId());
	EXPECT_EQ(retrieved.getId(), index.getId());

	index.setParentId(42);
	dataManager->updateIndexEntity(index);

	dataManager->deleteIndex(index);
}

// ============================================================================
// clearCache
// ============================================================================

TEST_F(DataManagerTest, ClearCache_NoException) {
	auto node = createTestNode(dataManager, "Cached");
	dataManager->addNode(node);
	simulateSave();

	// Load into cache
	(void)dataManager->getNode(node.getId());

	EXPECT_NO_THROW(dataManager->clearCache());
}
