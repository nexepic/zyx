/**
 * @file test_EntityTypeIndexManager_EdgeUpdate.cpp
 * @brief Coverage tests for EntityTypeIndexManager edge type updates,
 *        property removal diffs, node multi-label diff on update/delete,
 *        and batch operations.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage;
using namespace graph::query::indexes;

class EntityTypeIndexEdgeUpdateTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<DataManager> dm;
	std::shared_ptr<IndexManager> im;
	fs::path dbPath;

	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_etim_edge_" + boost::uuids::to_string(uuid) + ".dat");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		fs::remove(dbPath, ec);
	}

	Node createNode(const std::string &label) const {
		Node node;
		node.setLabelId(dm->getOrCreateTokenId(label));
		dm->addNode(node);
		return node;
	}

	Edge createEdge(int64_t src, int64_t tgt, const std::string &type) const {
		Edge edge;
		edge.setSourceNodeId(src);
		edge.setTargetNodeId(tgt);
		edge.setTypeId(dm->getOrCreateTokenId(type));
		dm->addEdge(edge);
		return edge;
	}
};

TEST_F(EntityTypeIndexEdgeUpdateTest, EdgeTypeIndex_AddAndDelete) {
	// Empty property = label/type index
	ASSERT_TRUE(im->createIndex("edge_type_idx", "edge", "", ""));

	Node n1 = createNode("Person");
	Node n2 = createNode("Person");
	Edge e = createEdge(n1.getId(), n2.getId(), "KNOWS");

	db->getStorage()->flush();

	// Delete the edge
	dm->deleteEdge(e);

	// The edge should be removed from the type index
	auto result = im->findEdgeIdsByType("KNOWS");
	for (auto id : result) {
		EXPECT_NE(id, e.getId());
	}
}

TEST_F(EntityTypeIndexEdgeUpdateTest, EdgeTypeChange_UpdateIndex) {
	ASSERT_TRUE(im->createIndex("edge_type_idx", "edge", "", ""));

	Node n1 = createNode("Person");
	Node n2 = createNode("Person");
	Edge e = createEdge(n1.getId(), n2.getId(), "KNOWS");
	db->getStorage()->flush();

	// Update the edge type
	e.setTypeId(dm->getOrCreateTokenId("LIKES"));
	dm->updateEdge(e);

	// Edge should be removed from KNOWS
	auto knowsResult = im->findEdgeIdsByType("KNOWS");
	bool inKnows = false;
	for (auto id : knowsResult) {
		if (id == e.getId()) inKnows = true;
	}
	EXPECT_FALSE(inKnows);

	// Edge should appear in LIKES
	auto likesResult = im->findEdgeIdsByType("LIKES");
	bool inLikes = false;
	for (auto id : likesResult) {
		if (id == e.getId()) inLikes = true;
	}
	EXPECT_TRUE(inLikes);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, NodeMultiLabel_AddAndRemoveLabel) {
	// Empty property = label index
	ASSERT_TRUE(im->createIndex("node_label_idx", "node", "", ""));

	Node n;
	n.addLabelId(dm->getOrCreateTokenId("Person"));
	dm->addNode(n);
	db->getStorage()->flush();

	// Add a second label
	n.addLabelId(dm->getOrCreateTokenId("Student"));
	dm->updateNode(n);

	// Node should appear in both label lookups
	auto personResult = im->findNodeIdsByLabel("Person");
	bool inPerson = false;
	for (auto id : personResult) if (id == n.getId()) inPerson = true;
	EXPECT_TRUE(inPerson);

	auto studentResult = im->findNodeIdsByLabel("Student");
	bool inStudent = false;
	for (auto id : studentResult) if (id == n.getId()) inStudent = true;
	EXPECT_TRUE(inStudent);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, NodeMultiLabel_Delete) {
	ASSERT_TRUE(im->createIndex("node_label_del", "node", "", ""));

	Node n;
	n.addLabelId(dm->getOrCreateTokenId("Worker"));
	dm->addNode(n);
	db->getStorage()->flush();

	// Delete the node
	dm->deleteNode(n);

	auto result = im->findNodeIdsByLabel("Worker");
	bool found = false;
	for (auto id : result) {
		if (id == n.getId()) found = true;
	}
	EXPECT_FALSE(found);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, PropertyIndex_ValueChange) {
	ASSERT_TRUE(im->createIndex("prop_name", "node", "Item", "name"));

	Node n;
	n.setLabelId(dm->getOrCreateTokenId("Item"));
	dm->addNode(n);
	dm->addNodeProperties(n.getId(), {{"name", PropertyValue("old_name")}});
	db->getStorage()->flush();

	// Update property value
	dm->addNodeProperties(n.getId(), {{"name", PropertyValue("new_name")}});

	// Old value should be removed, new value should be indexed
	auto oldResult = im->findNodeIdsByProperty("name", PropertyValue("old_name"));
	bool inOld = false;
	for (auto id : oldResult) if (id == n.getId()) inOld = true;
	EXPECT_FALSE(inOld);

	auto newResult = im->findNodeIdsByProperty("name", PropertyValue("new_name"));
	bool inNew = false;
	for (auto id : newResult) if (id == n.getId()) inNew = true;
	EXPECT_TRUE(inNew);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, PropertyIndex_PropertyRemoval) {
	ASSERT_TRUE(im->createIndex("prop_rem", "node", "Doc", "title"));

	Node n;
	n.setLabelId(dm->getOrCreateTokenId("Doc"));
	dm->addNode(n);
	dm->addNodeProperties(n.getId(), {{"title", PropertyValue("Hello")}});
	db->getStorage()->flush();

	// Remove the property
	dm->removeNodeProperty(n.getId(), "title");

	auto result = im->findNodeIdsByProperty("title", PropertyValue("Hello"));
	bool found = false;
	for (auto id : result) if (id == n.getId()) found = true;
	EXPECT_FALSE(found);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, EdgePropertyIndex_AddAndRemove) {
	ASSERT_TRUE(im->createIndex("edge_weight", "edge", "CONNECTS", "weight"));

	Node n1 = createNode("Device");
	Node n2 = createNode("Device");
	Edge e = createEdge(n1.getId(), n2.getId(), "CONNECTS");
	dm->addEdgeProperties(e.getId(), {{"weight", PropertyValue(10)}});
	db->getStorage()->flush();

	// Remove edge property
	dm->removeEdgeProperty(e.getId(), "weight");

	auto result = im->findEdgeIdsByProperty("weight", PropertyValue(10));
	bool found = false;
	for (auto id : result) if (id == e.getId()) found = true;
	EXPECT_FALSE(found);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, DropIndex_Label) {
	ASSERT_TRUE(im->createIndex("drop_lbl", "node", "", ""));
	EXPECT_TRUE(im->dropIndexByName("drop_lbl"));
}

TEST_F(EntityTypeIndexEdgeUpdateTest, DropIndex_Property) {
	ASSERT_TRUE(im->createIndex("drop_prop", "node", "Temp2", "attr"));
	EXPECT_TRUE(im->dropIndexByName("drop_prop"));
}

TEST_F(EntityTypeIndexEdgeUpdateTest, CreatePropertyIndex_AlreadyExists) {
	ASSERT_TRUE(im->createIndex("dup_idx", "node", "Dup", "field"));
	// Creating same index name again should return false
	EXPECT_FALSE(im->createIndex("dup_idx", "node", "Dup2", "field2"));
}

TEST_F(EntityTypeIndexEdgeUpdateTest, BatchNodeAddition_MultipleLabels) {
	ASSERT_TRUE(im->createIndex("batch_lbl", "node", "", ""));

	std::vector<Node> nodes;
	for (int i = 0; i < 5; ++i) {
		Node n;
		n.addLabelId(dm->getOrCreateTokenId("Cat"));
		nodes.push_back(n);
	}
	dm->addNodes(nodes);
	db->getStorage()->flush();

	auto result = im->findNodeIdsByLabel("Cat");
	EXPECT_GE(result.size(), 5u);
}

TEST_F(EntityTypeIndexEdgeUpdateTest, DropIndex_NonExistent) {
	EXPECT_FALSE(im->dropIndexByName("no_such_index"));
}

TEST_F(EntityTypeIndexEdgeUpdateTest, DropIndexByDefinition) {
	ASSERT_TRUE(im->createIndex("def_idx", "node", "DefLabel", "defprop"));
	EXPECT_TRUE(im->dropIndexByDefinition("DefLabel", "defprop"));
}

TEST_F(EntityTypeIndexEdgeUpdateTest, ListIndexes) {
	ASSERT_TRUE(im->createIndex("list_idx", "node", "ListLabel", "listprop"));
	auto indexes = im->listIndexesDetailed();
	bool found = false;
	for (const auto &[name, etype, label, prop] : indexes) {
		if (name == "list_idx") found = true;
	}
	EXPECT_TRUE(found);
}
