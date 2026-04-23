/**
 * @file test_NodeKeyConstraint_EdgeCases.cpp
 * @brief Branch coverage tests for NodeKeyConstraint — covers missing property
 *        in checkCompositeUniqueness, single-property buildCompositeKey,
 *        and validateUpdate with unchanged composite key.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/storage/constraints/NodeKeyConstraint.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage::constraints;

class NodeKeyConstraintEdgeCaseTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path dbPath;

	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_nkc_edge_" + boost::uuids::to_string(uuid) + ".dat");
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

	Node createNode(const std::string &label,
		const std::unordered_map<std::string, PropertyValue> &props) const {
		Node node;
		node.setLabelId(dm->getOrCreateTokenId(label));
		dm->addNode(node);
		if (!props.empty())
			dm->addNodeProperties(node.getId(), props);
		return node;
	}
};

TEST_F(NodeKeyConstraintEdgeCaseTest, ValidateInsert_MissingPropertyInCheck) {
	ASSERT_TRUE(im->createIndex("idx_a", "node", "Thing", "a"));
	ASSERT_TRUE(im->createIndex("idx_b", "node", "Thing", "b"));

	NodeKeyConstraint c("nk_thing", "Thing", {"a", "b"}, im);

	// Insert with only property "a" — "b" is missing from props.
	// checkCompositeUniqueness should return early (missing property)
	// BUT checkNotNull will throw because "b" is required
	EXPECT_THROW(c.validateInsert(1, {{"a", PropertyValue("x")}}), std::runtime_error);
}

TEST_F(NodeKeyConstraintEdgeCaseTest, ValidateUpdate_UnchangedCompositeKey) {
	ASSERT_TRUE(im->createIndex("idx_c", "node", "Widget", "c"));

	createNode("Widget", {{"c", PropertyValue("val1")}});

	NodeKeyConstraint c("nk_widget", "Widget", {"c"}, im);

	// Update where old and new composite keys are the same
	EXPECT_NO_THROW(c.validateUpdate(1,
		{{"c", PropertyValue("val1")}},
		{{"c", PropertyValue("val1")}}));
}

TEST_F(NodeKeyConstraintEdgeCaseTest, ValidateInsert_EmptyCandidatesAfterFirstProperty) {
	ASSERT_TRUE(im->createIndex("idx_p1", "node", "Gadget", "p1"));
	ASSERT_TRUE(im->createIndex("idx_p2", "node", "Gadget", "p2"));

	createNode("Gadget", {{"p1", PropertyValue("a")}, {"p2", PropertyValue("b")}});

	NodeKeyConstraint c("nk_gadget", "Gadget", {"p1", "p2"}, im);

	// Insert with p1 that has no matches (empty candidates after first lookup)
	EXPECT_NO_THROW(c.validateInsert(100,
		{{"p1", PropertyValue("unique_val")}, {"p2", PropertyValue("any")}}));
}

TEST_F(NodeKeyConstraintEdgeCaseTest, ValidateInsert_DuplicateMultiProperty) {
	ASSERT_TRUE(im->createIndex("idx_x", "node", "Multi", "x"));
	ASSERT_TRUE(im->createIndex("idx_y", "node", "Multi", "y"));

	auto existing = createNode("Multi", {{"x", PropertyValue(1)}, {"y", PropertyValue(2)}});

	NodeKeyConstraint c("nk_multi", "Multi", {"x", "y"}, im);

	// Same composite key as existing — should throw
	EXPECT_THROW(c.validateInsert(999,
		{{"x", PropertyValue(1)}, {"y", PropertyValue(2)}}), std::runtime_error);

	// Same entity id — should not throw (updating self)
	EXPECT_NO_THROW(c.validateInsert(existing.getId(),
		{{"x", PropertyValue(1)}, {"y", PropertyValue(2)}}));
}

TEST_F(NodeKeyConstraintEdgeCaseTest, ValidateInsert_NullPropertyThrows) {
	ASSERT_TRUE(im->createIndex("idx_nn", "node", "Strict", "name"));

	NodeKeyConstraint c("nk_strict", "Strict", {"name"}, im);

	EXPECT_THROW(c.validateInsert(1,
		{{"name", PropertyValue()}}), std::runtime_error);
}

TEST_F(NodeKeyConstraintEdgeCaseTest, ValidateUpdate_ChangedCompositeKey_NoDuplicate) {
	ASSERT_TRUE(im->createIndex("idx_upd", "node", "Updatable", "key"));

	createNode("Updatable", {{"key", PropertyValue("old")}});

	NodeKeyConstraint c("nk_upd", "Updatable", {"key"}, im);

	EXPECT_NO_THROW(c.validateUpdate(1,
		{{"key", PropertyValue("old")}},
		{{"key", PropertyValue("new_unique")}}));
}
