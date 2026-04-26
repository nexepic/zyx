/**
 * @file test_EntityTypeIndexManager_EdgeAndBatch.cpp
 * @brief Additional branch coverage tests for EntityTypeIndexManager.cpp targeting:
 *        - onEntityUpdated Node multi-label diff: old label removed (line 285)
 *        - onEntityUpdated Node multi-label diff: new label added (line 297)
 *        - onEntityUpdated Node multi-label diff: oldLid==0 skip (line 280)
 *        - onEntityUpdated Node multi-label diff: newLid==0 skip (line 292)
 *        - onEntityDeleted Node multi-label: lid==0 skip (line 321)
 *        - onEntityUpdated Edge: oldEntity.getTypeId()==0 (line 304)
 *        - onEntityDeleted Edge: entity.getTypeId()==0 (line 328)
 *        - updateLabelIndex Node: isDeleted with multi-label (line 97)
 *        - updateLabelIndex Node: !isDeleted oldLabel still present (line 110)
 *        - onEntitiesAdded Edge: entity.getTypeId()!=0 path (line 224)
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace fs = std::filesystem;

class EntityTypeIndexManagerEdgeAndBatchTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_etim_branch_" + boost::uuids::to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		nodeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager, fileStorage->getSystemStateManager(), graph::query::indexes::IndexTypes::NODE_LABEL_TYPE,
				graph::storage::state::keys::Node::LABEL_ROOT, graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE,
				graph::storage::state::keys::Node::PROPERTY_PREFIX);

		edgeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager, fileStorage->getSystemStateManager(), graph::query::indexes::IndexTypes::EDGE_TYPE_INDEX,
				graph::storage::state::keys::Edge::LABEL_ROOT, graph::query::indexes::IndexTypes::EDGE_PROPERTY_TYPE,
				graph::storage::state::keys::Edge::PROPERTY_PREFIX);
	}

	void TearDown() override {
		nodeIndexManager.reset();
		edgeIndexManager.reset();
		dataManager.reset();
		fileStorage.reset();
		if (database) database->close();
		database.reset();
		std::error_code ec;
		fs::remove(testFilePath, ec);
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> nodeIndexManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> edgeIndexManager;
};

// ============================================================================
// onEntityUpdated Node: label removed in new (old has label, new doesn't)
// Exercises lines 279-287: old label not found in new -> removeNode
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeUpdated_LabelRemoved) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lblA = dataManager->getOrCreateTokenId("LblRemove");
	graph::Node oldNode(1, lblA);
	nodeIndexManager->onEntityAdded(oldNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("LblRemove").size(), 1UL);

	// New node has no label -> old label should be removed
	graph::Node newNode(1, 0);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	EXPECT_TRUE(labelIdx->findNodes("LblRemove").empty());
}

// ============================================================================
// onEntityUpdated Node: label added in new (old has no label, new has one)
// Exercises lines 291-299: new label not found in old -> addNode
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeUpdated_LabelAdded) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	graph::Node oldNode(2, 0);
	nodeIndexManager->onEntityAdded(oldNode);

	int64_t lblB = dataManager->getOrCreateTokenId("LblAdded");
	graph::Node newNode(2, lblB);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("LblAdded").size(), 1UL);
}

// ============================================================================
// onEntityUpdated Node: label changed (old has A, new has B)
// Exercises both remove-old and add-new paths
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeUpdated_LabelChanged) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lblA = dataManager->getOrCreateTokenId("OldLbl");
	int64_t lblB = dataManager->getOrCreateTokenId("NewLbl");

	graph::Node oldNode(3, lblA);
	nodeIndexManager->onEntityAdded(oldNode);

	graph::Node newNode(3, lblB);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_TRUE(labelIdx->findNodes("OldLbl").empty());
	EXPECT_EQ(labelIdx->findNodes("NewLbl").size(), 1UL);
}

// ============================================================================
// onEntityUpdated Node: same label (old == new) -> no remove, no add
// Exercises found=true paths in both loops (lines 283, 295)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeUpdated_SameLabel) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("SameLbl");
	graph::Node oldNode(4, lbl);
	nodeIndexManager->onEntityAdded(oldNode);

	graph::Node newNode(4, lbl);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("SameLbl").size(), 1UL);
}

// ============================================================================
// onEntityUpdated Node: entityId==0 -> early return (line 273)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeUpdated_ZeroId) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("ZeroUpd");
	graph::Node oldNode(0, lbl);
	graph::Node newNode(0, lbl);
	EXPECT_NO_THROW(nodeIndexManager->onEntityUpdated(oldNode, newNode));
}

// ============================================================================
// onEntityUpdated Node: label index empty -> skip (line 273)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeUpdated_LabelIndexEmpty) {
	// Don't create label index
	int64_t lbl = dataManager->getOrCreateTokenId("NoIdx");
	graph::Node oldNode(5, lbl);
	graph::Node newNode(5, 0);
	EXPECT_NO_THROW(nodeIndexManager->onEntityUpdated(oldNode, newNode));
}

// ============================================================================
// onEntityDeleted Node: multi-label with valid labels
// Exercises lines 319-324
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeDeleted_WithLabel) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("DelLbl");
	graph::Node node(6, lbl);
	nodeIndexManager->onEntityAdded(node);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("DelLbl").size(), 1UL);

	nodeIndexManager->onEntityDeleted(node);
	EXPECT_TRUE(labelIdx->findNodes("DelLbl").empty());
}

// ============================================================================
// onEntityDeleted Node: entityId==0 -> early return (line 319)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeDeleted_ZeroId) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("ZeroDel");
	graph::Node node(0, lbl);
	EXPECT_NO_THROW(nodeIndexManager->onEntityDeleted(node));
}

// ============================================================================
// onEntityDeleted Node: label index empty -> skip
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeDeleted_LabelIndexEmpty) {
	// Don't create label index
	int64_t lbl = dataManager->getOrCreateTokenId("NoIdxDel");
	graph::Node node(7, lbl);
	EXPECT_NO_THROW(nodeIndexManager->onEntityDeleted(node));
}

// ============================================================================
// onEntityUpdated Edge: oldEntity.getTypeId()==0 (line 304)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeUpdated_OldTypeIdZero) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	graph::Edge oldEdge(10, 1, 2, 0);
	int64_t lbl = dataManager->getOrCreateTokenId("NewEdgeType");
	graph::Edge newEdge(10, 1, 2, lbl);

	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("NewEdgeType").size(), 1UL);
}

// ============================================================================
// onEntityDeleted Edge: entity.getTypeId()==0 (line 328)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeDeleted_TypeIdZero) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	graph::Edge edge(11, 1, 2, 0);
	EXPECT_NO_THROW(edgeIndexManager->onEntityDeleted(edge));
}

// ============================================================================
// onEntityDeleted Edge: entity with valid type
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeDeleted_ValidType) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("DelEdgeType");
	graph::Edge edge(12, 1, 2, lbl);
	edgeIndexManager->onEntityAdded(edge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("DelEdgeType").size(), 1UL);

	edgeIndexManager->onEntityDeleted(edge);
	EXPECT_TRUE(labelIdx->findNodes("DelEdgeType").empty());
}

// ============================================================================
// onEntityUpdated Edge: both old and new have same type (no change)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeUpdated_SameType) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("SameEdge");
	graph::Edge oldEdge(13, 1, 2, lbl);
	edgeIndexManager->onEntityAdded(oldEdge);

	graph::Edge newEdge(13, 1, 2, lbl);
	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("SameEdge").size(), 1UL);
}

// ============================================================================
// createPropertyIndex already exists (line 50-51)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, CreatePropertyIndex_AlreadyExists) {
	bool called = false;
	EXPECT_TRUE(nodeIndexManager->createPropertyIndex("dup", [&]() { called = true; return true; }));
	EXPECT_TRUE(called);

	called = false;
	EXPECT_FALSE(nodeIndexManager->createPropertyIndex("dup", [&]() { called = true; return true; }));
	EXPECT_FALSE(called);
}

// ============================================================================
// dropIndex unknown type (line 67)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, DropIndex_UnknownType) {
	EXPECT_FALSE(nodeIndexManager->dropIndex("unknown", ""));
}

// ============================================================================
// persistState exercises
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, PersistState) {
	EXPECT_NO_THROW(nodeIndexManager->persistState());
	EXPECT_NO_THROW(edgeIndexManager->persistState());
}

// ============================================================================
// updateLabelIndex Node: isDeleted=true path (line 96-99)
// Called indirectly via onEntityDeleted with single-label updateLabelIndex
// The Edge::onEntityDeleted calls updateLabelIndex(entity, labelStr, true)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeDeleted_ValidType_IsDeletedPath) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("IsDeletedEdge");
	graph::Edge edge(20, 1, 2, lbl);
	edgeIndexManager->onEntityAdded(edge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("IsDeletedEdge").size(), 1UL);

	// onEntityDeleted for Edge calls updateLabelIndex(entity, labelStr, true)
	edgeIndexManager->onEntityDeleted(edge);
	EXPECT_TRUE(labelIdx->findNodes("IsDeletedEdge").empty());
}

// ============================================================================
// updateLabelIndex Node: oldLabel non-empty and still present (line 104-112)
// This is hit when updateLabelIndex is called with an oldLabel that matches
// one of the node's current labels (stillPresent=true, so no removal).
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeUpdated_TypeChanged) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lblOld = dataManager->getOrCreateTokenId("OldEdgeT");
	int64_t lblNew = dataManager->getOrCreateTokenId("NewEdgeT");

	graph::Edge oldEdge(21, 1, 2, lblOld);
	edgeIndexManager->onEntityAdded(oldEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("OldEdgeT").size(), 1UL);

	graph::Edge newEdge(21, 1, 2, lblNew);
	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	EXPECT_TRUE(labelIdx->findNodes("OldEdgeT").empty());
	EXPECT_EQ(labelIdx->findNodes("NewEdgeT").size(), 1UL);
}

// ============================================================================
// updatePropertyIndexes: indexedKeys empty (line 154)
// Create property index, then call with entity that has no matching props
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, UpdatePropertyIndexes_IndexedKeysEmpty) {
	// Property index exists but has no keys indexed yet
	// updatePropertyIndexes should return early when indexedKeys is empty
	graph::Node node(30, 1);
	node.addProperty("key1", graph::PropertyValue(42));

	// onEntityAdded calls updatePropertyIndexes, which checks isEmpty()
	// and then indexedKeys.empty() -- both should short-circuit
	EXPECT_NO_THROW(nodeIndexManager->onEntityAdded(node));
}

// ============================================================================
// Node multi-label: lid==0 in label list (line 91 False branch, line 219 False)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, NodeAdded_WithZeroLabelId) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });

	// Node with labelId 0 (should skip in the label ID loop)
	graph::Node node(31, 0);
	EXPECT_NO_THROW(nodeIndexManager->onEntityAdded(node));
}

// ============================================================================
// onEntitiesAdded Edge batch path (line 224)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EntitiesAdded_EdgeBatch) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("BatchEdge");
	std::vector<graph::Edge> edges;
	edges.emplace_back(40, 1, 2, lbl);
	edges.emplace_back(41, 1, 3, lbl);

	edgeIndexManager->onEntitiesAdded(edges);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("BatchEdge").size(), 2UL);
}

// ============================================================================
// onEntitiesAdded Edge with typeId==0 (line 224 False branch)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EntitiesAdded_EdgeBatch_ZeroTypeId) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	std::vector<graph::Edge> edges;
	edges.emplace_back(42, 1, 2, 0); // typeId 0, should be skipped
	edgeIndexManager->onEntitiesAdded(edges);
}

// ============================================================================
// onEntitiesAdded empty (line 203)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EntitiesAdded_Empty) {
	std::vector<graph::Node> empty;
	EXPECT_NO_THROW(nodeIndexManager->onEntitiesAdded(empty));
}

// ============================================================================
// nl == oldLabel path (line 115 False branch)
// When updateLabelIndex is called on a Node with oldLabel matching a new label,
// we need nl == oldLabel to be true so the add is skipped.
// This happens through Edge's onEntityUpdated where same type is kept.
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, EdgeUpdated_SameType_SkipsAddition) {
	(void)edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("SameEdgeT2");
	graph::Edge oldEdge(50, 1, 2, lbl);
	edgeIndexManager->onEntityAdded(oldEdge);

	// Update with same type -> updateLabelIndex gets oldLabel == newLabel
	graph::Edge newEdge(50, 1, 2, lbl);
	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("SameEdgeT2").size(), 1UL);
}

// ============================================================================
// dropIndex label type (line 59-61)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, DropIndex_LabelType) {
	(void)nodeIndexManager->createLabelIndex([]() { return true; });
	EXPECT_TRUE(nodeIndexManager->dropIndex("label", ""));
}

// ============================================================================
// dropIndex property type (line 63-65)
// ============================================================================

TEST_F(EntityTypeIndexManagerEdgeAndBatchTest, DropIndex_PropertyType) {
	(void)nodeIndexManager->createPropertyIndex("dropme", []() { return true; });
	EXPECT_TRUE(nodeIndexManager->dropIndex("property", "dropme"));
}
