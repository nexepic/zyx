/**
 * @file test_UniqueAndNodeKeyConstraint.cpp
 * @brief Unit tests for UniqueConstraint and NodeKeyConstraint.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/storage/constraints/NodeKeyConstraint.hpp"
#include "graph/storage/constraints/UniqueConstraint.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::storage::constraints;

class ConstraintImplementationTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path dbPath;

	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_constraints_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(dbPath)) {
			fs::remove(dbPath);
		}

		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		if (fs::exists(dbPath)) {
			fs::remove(dbPath, ec);
		}
	}

	Node createNode(const std::string &label,
		const std::unordered_map<std::string, PropertyValue> &props) const {
		Node node;
		node.setLabelId(dm->getOrCreateLabelId(label));
		dm->addNode(node);
		dm->addNodeProperties(node.getId(), props);
		return node;
	}
};

TEST_F(ConstraintImplementationTest, UniqueConstraintMetadataAndValidation) {
	ASSERT_TRUE(im->createIndex("idx_person_email", "node", "Person", "email"));
	auto ada = createNode("Person", {{"email", PropertyValue("ada@example.com")}});
	auto grace = createNode("Person", {{"email", PropertyValue("grace@example.com")}});

	UniqueConstraint c("uq_person_email", "Person", "email", im);
	EXPECT_EQ(c.getType(), ConstraintType::CT_UNIQUE);
	EXPECT_EQ(c.getName(), "uq_person_email");
	EXPECT_EQ(c.getLabel(), "Person");
	EXPECT_EQ(c.getProperties(), std::vector<std::string>({"email"}));

	EXPECT_NO_THROW(c.validateInsert(100, std::unordered_map<std::string, PropertyValue>{}));
	EXPECT_NO_THROW(c.validateInsert(100, {{"email", PropertyValue()}}));
	EXPECT_NO_THROW(c.validateInsert(100, {{"email", PropertyValue("new@example.com")}}));
	EXPECT_THROW(c.validateInsert(100, {{"email", PropertyValue("ada@example.com")}}), std::runtime_error);

	EXPECT_NO_THROW(c.validateUpdate(
		grace.getId(),
		{{"email", PropertyValue("grace@example.com")}},
		{{"email", PropertyValue("grace@example.com")}}));
	EXPECT_NO_THROW(c.validateUpdate(
		grace.getId(),
		{{"email", PropertyValue("grace@example.com")}},
		{{"email", PropertyValue("new2@example.com")}}));
	EXPECT_THROW(c.validateUpdate(
		grace.getId(),
		{{"email", PropertyValue("grace@example.com")}},
		{{"email", PropertyValue("ada@example.com")}}), std::runtime_error);

	EXPECT_NO_THROW(c.validateUpdate(
		ada.getId(),
		{{"email", PropertyValue("ada@example.com")}},
		{{"email", PropertyValue("ada@example.com")}}));
}

TEST_F(ConstraintImplementationTest, NodeKeyConstraintMetadataAndValidation) {
	ASSERT_TRUE(im->createIndex("idx_person_first", "node", "Person", "first"));
	ASSERT_TRUE(im->createIndex("idx_person_last", "node", "Person", "last"));

	createNode("Person", {{"first", PropertyValue("Ada")}, {"last", PropertyValue("Lovelace")}});
	auto grace = createNode("Person", {{"first", PropertyValue("Grace")}, {"last", PropertyValue("Hopper")}});

	NodeKeyConstraint c("nk_person_name", "Person", {"first", "last"}, im);
	EXPECT_EQ(c.getType(), ConstraintType::CT_NODE_KEY);
	EXPECT_EQ(c.getName(), "nk_person_name");
	EXPECT_EQ(c.getLabel(), "Person");
	EXPECT_EQ(c.getProperties(), std::vector<std::string>({"first", "last"}));

	EXPECT_THROW(c.validateInsert(100, {{"last", PropertyValue("Lovelace")}}), std::runtime_error);
	EXPECT_THROW(c.validateInsert(100, {{"first", PropertyValue("Ada")}, {"last", PropertyValue()}}), std::runtime_error);
	EXPECT_THROW(c.validateInsert(100, {{"first", PropertyValue("Ada")}, {"last", PropertyValue("Lovelace")}}), std::runtime_error);
	EXPECT_NO_THROW(c.validateInsert(100, {{"first", PropertyValue("Alan")}, {"last", PropertyValue("Turing")}}));

	EXPECT_NO_THROW(c.validateUpdate(
		grace.getId(),
		{{"first", PropertyValue("Grace")}, {"last", PropertyValue("Hopper")}},
		{{"first", PropertyValue("Grace")}, {"last", PropertyValue("Hopper")}}));
	EXPECT_THROW(c.validateUpdate(
		grace.getId(),
		{{"first", PropertyValue("Grace")}, {"last", PropertyValue("Hopper")}},
		{{"first", PropertyValue("Ada")}, {"last", PropertyValue("Lovelace")}}), std::runtime_error);
}
