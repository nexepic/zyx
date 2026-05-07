/**
 * @file test_ShowIndexesOperator.cpp
 * @brief Unit tests for ShowIndexesOperator covering NODE/EDGE branches.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class ShowIndexesOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_show_indexes_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

TEST_F(ShowIndexesOperatorTest, ShowNoIndexesReturnsNullopt) {
	ShowIndexesOperator op(im);
	op.open();
	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());
	op.close();
}

TEST_F(ShowIndexesOperatorTest, ShowNodeIndexes) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n(0, labelId);
	dm->addNode(n);
	dm->addNodeProperties(n.getId(), {{"name", PropertyValue(std::string("Alice"))}});
	ASSERT_TRUE(im->createIndex("idx_person_name", "node", "Person", "name"));

	ShowIndexesOperator op(im);
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto entity = (*batch)[0].getValue("entity");
	ASSERT_TRUE(entity.has_value());
	auto *strPtr = std::get_if<std::string>(&entity->getVariant());
	ASSERT_NE(strPtr, nullptr);
	EXPECT_EQ(*strPtr, "NODE");

	// Second call returns nullopt
	EXPECT_FALSE(op.next().has_value());
	op.close();
}

TEST_F(ShowIndexesOperatorTest, ShowEdgeIndexes) {
	// Create an edge type and edge index
	ASSERT_TRUE(im->createIndex("idx_edge_weight", "edge", "KNOWS", "weight"));

	ShowIndexesOperator op(im);
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto entity = (*batch)[0].getValue("entity");
	ASSERT_TRUE(entity.has_value());
	auto *strPtr = std::get_if<std::string>(&entity->getVariant());
	ASSERT_NE(strPtr, nullptr);
	EXPECT_EQ(*strPtr, "EDGE");

	op.close();
}

TEST_F(ShowIndexesOperatorTest, ShowMixedIndexes) {
	ASSERT_TRUE(im->createIndex("idx_node_age", "node", "Person", "age"));
	ASSERT_TRUE(im->createIndex("idx_edge_since", "edge", "KNOWS", "since"));

	ShowIndexesOperator op(im);
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2UL);

	bool hasNode = false, hasEdge = false;
	for (const auto &r : *batch) {
		auto entity = r.getValue("entity");
		ASSERT_TRUE(entity.has_value());
		auto *strPtr = std::get_if<std::string>(&entity->getVariant());
		ASSERT_NE(strPtr, nullptr);
		if (*strPtr == "NODE") hasNode = true;
		if (*strPtr == "EDGE") hasEdge = true;
	}
	EXPECT_TRUE(hasNode);
	EXPECT_TRUE(hasEdge);
	op.close();
}

TEST_F(ShowIndexesOperatorTest, NextReturnsFalseAfterExhaustion) {
	ASSERT_TRUE(im->createIndex("idx_x", "node", "X", "y"));

	ShowIndexesOperator op(im);
	op.open();
	ASSERT_TRUE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value()); // idempotent
	op.close();
}

TEST_F(ShowIndexesOperatorTest, GetOutputVariablesAndToString) {
	ShowIndexesOperator op(im);
	auto vars = op.getOutputVariables();
	EXPECT_EQ(vars.size(), 4UL);
	EXPECT_EQ(op.toString(), "ShowIndexes()");
}

TEST_F(ShowIndexesOperatorTest, OpenResetsState) {
	ASSERT_TRUE(im->createIndex("idx_reset", "node", "A", "b"));

	ShowIndexesOperator op(im);
	op.open();
	ASSERT_TRUE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());

	// Re-open resets
	op.open();
	EXPECT_TRUE(op.next().has_value());
	op.close();
}
