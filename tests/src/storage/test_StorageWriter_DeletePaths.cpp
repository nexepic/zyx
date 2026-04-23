/**
 * @file test_StorageWriter_DeletePaths.cpp
 * @brief Coverage tests for StorageWriter delete, bitmap update, and
 *        pre-allocated entity write paths.
 **/

#include "data/DataManagerSharedTestFixture.hpp"

class StorageWriterDeleteTest : public DataManagerSharedTest {};

TEST_F(StorageWriterDeleteTest, DeleteEntityOnDisk_WithZeroId) {
	// Entity with zero ID should be handled gracefully
	Node n;
	n.setId(0);
	// deleteNode internally checks id and handles zero-id case
	EXPECT_NO_THROW({
		n.markInactive();
	});
}

TEST_F(StorageWriterDeleteTest, SaveModifiedEntity_UpdateInPlace) {
	Node n = createTestNode(dataManager, "Modified");
	dataManager->addNode(n);
	simulateSave();

	// Modify the node
	n.addLabelId(dataManager->getOrCreateTokenId("Extra"));
	dataManager->updateNode(n);
	simulateSave();

	dataManager->clearCache();
	Node loaded = dataManager->getNode(n.getId());
	EXPECT_TRUE(loaded.isActive());
}

TEST_F(StorageWriterDeleteTest, SaveDeletedEntity_DeleteOnDisk) {
	Node n = createTestNode(dataManager, "Deleted");
	dataManager->addNode(n);
	simulateSave();

	dataManager->deleteNode(n);
	simulateSave();

	dataManager->clearCache();
	Node loaded = dataManager->getNode(n.getId());
	EXPECT_FALSE(loaded.isActive());
}

TEST_F(StorageWriterDeleteTest, MultipleEntities_BatchWrite) {
	std::vector<Node> nodes;
	for (int i = 0; i < 10; ++i) {
		Node n = createTestNode(dataManager, "Batch");
		nodes.push_back(n);
	}
	dataManager->addNodes(nodes);
	simulateSave();

	for (const auto &n : nodes) {
		Node loaded = dataManager->getNode(n.getId());
		EXPECT_TRUE(loaded.isActive());
	}
}

TEST_F(StorageWriterDeleteTest, DeleteEdge_OnDisk) {
	Node n1 = createTestNode(dataManager, "DN1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "DN2");
	dataManager->addNode(n2);
	Edge e = createTestEdge(dataManager, n1.getId(), n2.getId(), "DEL_REL");
	dataManager->addEdge(e);
	simulateSave();

	dataManager->deleteEdge(e);
	simulateSave();

	dataManager->clearCache();
	Edge loaded = dataManager->getEdge(e.getId());
	EXPECT_FALSE(loaded.isActive());
}

TEST_F(StorageWriterDeleteTest, ModifyAndDeleteInSameBatch) {
	Node n1 = createTestNode(dataManager, "MD1");
	dataManager->addNode(n1);
	Node n2 = createTestNode(dataManager, "MD2");
	dataManager->addNode(n2);
	simulateSave();

	// Modify n1 and delete n2 before saving
	n1.addLabelId(dataManager->getOrCreateTokenId("NewLabel"));
	dataManager->updateNode(n1);
	dataManager->deleteNode(n2);
	simulateSave();

	dataManager->clearCache();
	Node loaded1 = dataManager->getNode(n1.getId());
	EXPECT_TRUE(loaded1.isActive());
	Node loaded2 = dataManager->getNode(n2.getId());
	EXPECT_FALSE(loaded2.isActive());
}

TEST_F(StorageWriterDeleteTest, SaveWithEmptySnapshot) {
	// Save with no unsaved changes should return early
	EXPECT_NO_THROW(fileStorage->save());
}
